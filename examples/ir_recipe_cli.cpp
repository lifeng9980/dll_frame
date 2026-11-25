#include "IR_recipe.h"
#include "posix_stop_signal.h"
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <Windows.h>
#else
#include <csignal>
#endif
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {
std::string pathUtf8(const fs::path &path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

fs::path pathFromUtf8(const std::string &value)
{
#ifdef _WIN32
    return fs::path(std::u8string(value.begin(), value.end()));
#else
    return fs::path(value);
#endif
}

#ifdef _WIN32
std::string wideToUtf8(const std::wstring &value)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) throw std::runtime_error("Cannot convert command-line text to UTF-8");
    std::string result(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr) != bytes)
        throw std::runtime_error("Cannot convert command-line text to UTF-8");
    return result;
}
#endif

std::string readText(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open JSON file: " + pathUtf8(path));
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string resolveStationSource(const fs::path &stationPath, const std::string &stationText)
{
    Json station;
    try {
        station = Json::parse(stationText);
    } catch (const Json::parse_error &) {
        return stationText;
    }
    if (!station.is_object()) return stationText;
    const auto acqIt = station.find("acquisition");
    if (acqIt != station.end() && acqIt->is_object()) {
        auto &acquisition = *acqIt;
        const auto backendIt = acquisition.find("backend");
        const auto sourceIt = acquisition.find("source");
        if (backendIt != acquisition.end() && backendIt->is_string() && *backendIt == "raw_replay" &&
            sourceIt != acquisition.end() && sourceIt->is_string()) {
            const auto rawSource = sourceIt->get<std::string>();
            if (!rawSource.empty()) {
                const auto source = pathFromUtf8(rawSource);
                if (source.is_relative()) {
                    const auto absolute = fs::absolute(stationPath.parent_path() / source).lexically_normal();
                    acquisition["source"] = pathUtf8(absolute);
                }
            }
        }
    }
    const auto pluginsIt = station.find("plugins");
    if (pluginsIt != station.end() && pluginsIt->is_array()) {
        for (auto &item : *pluginsIt) {
            if (!item.is_string()) continue;
            const auto rawPlugin = item.get<std::string>();
            if (rawPlugin.empty()) continue;
            const auto pluginPath = pathFromUtf8(rawPlugin);
            if (pluginPath.is_relative()) {
                const auto absolute = fs::absolute(stationPath.parent_path() / pluginPath).lexically_normal();
                item = pathUtf8(absolute);
            }
        }
    }
    return station.dump();
}

void checkOutputDirectory(const fs::path &path)
{
    if (path.empty()) throw std::runtime_error("Specify --output");
    std::error_code error;
    const auto target = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    if (error) throw std::runtime_error("Cannot inspect output directory");
    if (target.type() != fs::file_type::not_found)
        throw std::runtime_error("Output directory already exists");
    auto parent = path.parent_path();
    if (parent.empty()) parent = fs::current_path(error);
    if (error || !fs::is_directory(parent, error) || error)
        throw std::runtime_error("Output parent directory must already exist");
}

struct Arguments {
    std::string mode;
    fs::path station;
    fs::path recipe;
    fs::path output;
};

void printUsage()
{
    std::puts("Usage: ir_run_recipe --check|--run --station station.json --recipe recipe.json --output new-directory");
}

const char *statusName(int32_t status) noexcept
{
    switch (status) {
    case IR_OK: return "IR_OK";
    case IR_NULL_POINTER: return "IR_NULL_POINTER";
    case IR_BAD_ARGUMENT: return "IR_BAD_ARGUMENT";
    case IR_BUFFER_TOO_SMALL: return "IR_BUFFER_TOO_SMALL";
    case IR_COMPUTATION_FAILED: return "IR_COMPUTATION_FAILED";
    case IR_INVALID_HANDLE: return "IR_INVALID_HANDLE";
    case IR_IO_ERROR: return "IR_IO_ERROR";
    case IR_TIMEOUT: return "IR_TIMEOUT";
    case IR_CANCELLED: return "IR_CANCELLED";
    case IR_BUSY: return "IR_BUSY";
    case IR_UNSUPPORTED: return "IR_UNSUPPORTED";
    case IR_END_OF_STREAM: return "IR_END_OF_STREAM";
    case IR_FRAME_GAP: return "IR_FRAME_GAP";
    default: return "IR_UNKNOWN";
    }
}

const char *stageName(int32_t stage) noexcept
{
    switch (stage) {
    case IR_RECIPE_STAGE_NONE: return "none";
    case IR_RECIPE_STAGE_VALIDATE: return "validate";
    case IR_RECIPE_STAGE_RESERVE: return "reserve";
    case IR_RECIPE_STAGE_PREPARE: return "prepare";
    case IR_RECIPE_STAGE_INSTRUMENT: return "instrument";
    case IR_RECIPE_STAGE_CAPTURE: return "capture";
    case IR_RECIPE_STAGE_ANALYZE: return "analyze";
    case IR_RECIPE_STAGE_ARCHIVE: return "archive";
    case IR_RECIPE_STAGE_CLEANUP: return "cleanup";
    default: return "unknown";
    }
}

void onRecipeStepEvent(const IRRecipeStepEvent *ev, void *) noexcept
{
    if (!ev) return;
    const char *id = ev->step_id_utf8 ? ev->step_id_utf8 : "";
    const char *type = ev->step_type_utf8 ? ev->step_type_utf8 : "";
    if (ev->event_kind == IR_STEP_EVENT_COMPLETED) {
        std::fprintf(stderr, "[step #%d] %s (%s) ... OK (%.1f ms)\n",
                     ev->step_index + 1, id, type, ev->duration_ms);
    } else if (ev->event_kind == IR_STEP_EVENT_FAILED) {
        std::fprintf(stderr, "[step #%d] %s (%s) ... FAILED [%s] %s\n",
                     ev->step_index + 1, id, type, statusName(ev->status),
                     ev->detail_utf8 ? ev->detail_utf8 : "");
    }
}

Arguments parseArguments(const std::vector<std::string> &values)
{
    if (values.size() == 1 && values[0] == "--help") {
        printUsage();
        std::exit(0);
    }

    Arguments arguments;
    for (size_t index = 0; index < values.size(); ++index) {
        const auto &value = values[index];
        if (value == "--check" || value == "--run") {
            if (!arguments.mode.empty()) throw std::runtime_error("Choose exactly one of --check or --run");
            arguments.mode = value;
        } else if (value == "--station" || value == "--recipe" || value == "--output") {
            if (++index >= values.size()) throw std::runtime_error(value + " needs a path");
            const auto path = pathFromUtf8(values[index]);
            if (value == "--station") arguments.station = path;
            else if (value == "--recipe") arguments.recipe = path;
            else arguments.output = path;
        } else {
            throw std::runtime_error("Unknown argument: " + value);
        }
    }
    if (arguments.mode.empty() || arguments.station.empty() || arguments.recipe.empty() || arguments.output.empty())
        throw std::runtime_error("Usage: ir_run_recipe --check|--run --station station.json --recipe recipe.json --output new-directory");
    return arguments;
}

using ir::examples::StopSignalGuard;

int execute(const std::vector<std::string> &values)
{
    bool isRunMode = false;
    bool statusPrinted = false;
    try {
        const auto arguments = parseArguments(values);
        isRunMode = arguments.mode == "--run";
        const auto stationPath = fs::absolute(arguments.station).lexically_normal();
        const auto recipePath = fs::absolute(arguments.recipe).lexically_normal();
        auto outputPath = arguments.output;
        if (outputPath.is_relative()) outputPath = recipePath.parent_path() / outputPath;
        outputPath = fs::absolute(outputPath).lexically_normal();

        const auto stationText = resolveStationSource(stationPath, readText(stationPath));
        const auto recipeText = readText(recipePath);
        char error[2048]{};
        int32_t status = IR_OK;
        if (arguments.mode == "--check") {
            status = IR_CheckRecipeJson(stationText.c_str(), recipeText.c_str(), error, sizeof error);
            if (status != IR_OK) {
                std::fprintf(stderr, "recipe check status=%d (%s) %s\n", status, statusName(status), error);
                return status;
            }
            checkOutputDirectory(outputPath);
            std::puts("recipe=VALID");
            return 0;
        }
        checkOutputDirectory(outputPath);

        const auto outputText = pathUtf8(outputPath);
        IRRecipeRunOptions options{};
        options.struct_size = sizeof(IRRecipeRunOptions);
        options.flags = 0;
        options.on_step_event = onRecipeStepEvent;
        options.user_data = nullptr;
        IRRecipeResult run{};
        {
            StopSignalGuard stopGuard;
            status = IR_RunRecipeJson(stationText.c_str(), recipeText.c_str(), outputText.c_str(),
                                      &options, &run, error, sizeof error);
        }
        if (status != IR_OK) {
            const char *runStatus = status == IR_CANCELLED ? "CANCELLED" : "ERROR";
            statusPrinted = true;
            std::cout << "run_status=" << runStatus << '\n'
                      << "completed_frames=" << run.completed_frames << '\n'
                      << "output_dir=" << outputText << '\n'
                      << std::flush;
            std::fprintf(stderr, "recipe run status=%d (%s) stage=%d (%s) step_index=%d completed_frames=%d cleanup_status=%d (%s) %s\n",
                status, statusName(status), run.failed_stage, stageName(run.failed_stage),
                run.failed_step_index, run.completed_frames, run.cleanup_status,
                statusName(run.cleanup_status), error);
            return status;
        }

        const auto record = Json::parse(readText(outputPath / "result.json"));
        const auto runStatusText = record.at("run_status").get<std::string>();
        const auto completedFrames = record.at("completed_frames").get<int>();
        statusPrinted = true;
        std::cout << "run_status=" << runStatusText << '\n'
                  << "completed_frames=" << completedFrames << '\n'
                  << "output_dir=" << outputText << '\n';
        if (record.contains("total_duration_ms"))
            std::cout << "total_duration_ms=" << record.at("total_duration_ms") << '\n';
        for (auto metric = record.at("metrics").begin(); metric != record.at("metrics").end(); ++metric)
            std::cout << "metric." << metric.key() << '=' << metric.value().at("value")
                      << ' ' << metric.value().at("unit").get<std::string>() << '\n';
        if (record.contains("limits") && record.at("limits").is_array()) {
            for (const auto &lim : record.at("limits")) {
                std::cout << "limit." << lim.at("metric").get<std::string>()
                          << '=' << lim.at("result").get<std::string>()
                          << " (actual=" << lim.at("actual")
                          << ' ' << lim.at("operator").get<std::string>()
                          << ' ' << lim.at("value")
                          << ' ' << lim.at("unit").get<std::string>() << ")\n";
            }
        }
        if (runStatusText == "NG") return 20;
        return 0;
    } catch (const std::exception &error) {
        if (isRunMode && !statusPrinted)
            std::cout << "run_status=ERROR\ncompleted_frames=0\n" << std::flush;
        std::fprintf(stderr, "ir_run_recipe: %s\n", error.what());
        return 2;
    }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    std::vector<std::string> values;
    for (int index = 1; index < argc; ++index) values.push_back(wideToUtf8(argv[index]));
    if (values.size() == 1 && values.front() == "--help") {
        printUsage();
        return 0;
    }
    return execute(values);
}
#else
int main(int argc, char **argv)
{
    std::vector<std::string> values;
    for (int index = 1; index < argc; ++index) values.emplace_back(argv[index]);
    if (values.size() == 1 && values.front() == "--help") {
        printUsage();
        return 0;
    }
    return execute(values);
}
#endif
