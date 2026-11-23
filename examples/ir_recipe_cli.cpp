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
    return fs::u8path(value);
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
    if (acqIt == station.end() || !acqIt->is_object()) return stationText;
    auto &acquisition = *acqIt;
    const auto backendIt = acquisition.find("backend");
    const auto sourceIt = acquisition.find("source");
    if (backendIt != acquisition.end() && backendIt->is_string() && *backendIt == "raw_replay" &&
        sourceIt != acquisition.end() && sourceIt->is_string()) {
        const auto rawSource = sourceIt->get<std::string>();
        if (!rawSource.empty()) {
            const auto source = pathFromUtf8(rawSource);
            const auto absolute = fs::absolute(stationPath.parent_path() / source).lexically_normal();
            acquisition["source"] = pathUtf8(absolute);
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

Arguments parseArguments(const std::vector<std::string> &values)
{
    if (values.size() == 1 && values[0] == "--help")
        throw std::runtime_error("Usage: ir_run_recipe --check|--run --station station.json --recipe recipe.json --output new-directory");

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

std::atomic<bool> stopWindowActive{false};

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD ctrlType)
{
    if (!stopWindowActive.load(std::memory_order_acquire))
        return FALSE;
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
        IR_RequestStop();
        return TRUE;
    }
    return FALSE;
}

class StopSignalGuard {
public:
    StopSignalGuard()
    {
        IR_RequestStop();  // Safe no-op while idle; initializes stop state before handler registration.
        stopWindowActive.store(true, std::memory_order_release);
        installed_ = SetConsoleCtrlHandler(consoleCtrlHandler, TRUE) != 0;
        if (!installed_) stopWindowActive.store(false, std::memory_order_release);
    }
    ~StopSignalGuard()
    {
        stopWindowActive.store(false, std::memory_order_release);
        if (installed_) SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
    }
private:
    bool installed_{false};
};
#else
using StopSignalGuard = ir::examples::PosixStopSignalGuard;
#endif

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
        auto status = IR_CheckRecipeJson(stationText.c_str(), recipeText.c_str(), error, sizeof error);
        if (status != IR_OK) {
            if (isRunMode) {
                statusPrinted = true;
                std::cout << "run_status=ERROR\ncompleted_frames=0\n" << std::flush;
            }
            std::fprintf(stderr, "recipe check status=%d %s\n", status, error);
            return status;
        }
        checkOutputDirectory(outputPath);
        if (arguments.mode == "--check") {
            std::puts("recipe=VALID");
            return 0;
        }

        const auto outputText = pathUtf8(outputPath);
        IRRecipeResult run{};
        {
            StopSignalGuard stopGuard;
            status = IR_RunRecipeJson(stationText.c_str(), recipeText.c_str(), outputText.c_str(),
                                      &run, error, sizeof error);
        }
        if (status != IR_OK) {
            const char *runStatus = status == IR_CANCELLED ? "CANCELLED" : "ERROR";
            statusPrinted = true;
            std::cout << "run_status=" << runStatus << '\n'
                      << "completed_frames=" << run.completed_frames << '\n'
                      << std::flush;
            std::fprintf(stderr, "recipe run status=%d stage=%d step_index=%d completed_frames=%d cleanup_status=%d %s\n",
                status, run.failed_stage, run.failed_step_index, run.completed_frames, run.cleanup_status, error);
            return status;
        }

        const auto record = Json::parse(readText(outputPath / "result.json"));
        const auto runStatusText = record.at("run_status").get<std::string>();
        const auto completedFrames = record.at("completed_frames").get<int>();
        statusPrinted = true;
        std::cout << "run_status=" << runStatusText << '\n'
                  << "completed_frames=" << completedFrames << '\n';
        for (auto metric = record.at("metrics").begin(); metric != record.at("metrics").end(); ++metric)
            std::cout << "metric." << metric.key() << '=' << metric.value().at("value")
                      << ' ' << metric.value().at("unit").get<std::string>() << '\n';
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
        std::puts("Usage: ir_run_recipe --check|--run --station station.json --recipe recipe.json --output new-directory");
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
        std::puts("Usage: ir_run_recipe --check|--run --station station.json --recipe recipe.json --output new-directory");
        return 0;
    }
    return execute(values);
}
#endif
