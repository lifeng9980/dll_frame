#include "IR_device.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <Windows.h>
#endif

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

std::string requiredString(const Json &object, const char *key)
{
    const auto value = object.find(key);
    if (value == object.end() || !value->is_string())
        throw std::runtime_error(std::string(key) + " must be a string");
    const auto text = value->get<std::string>();
    if (text.empty() || text.find('\0') != std::string::npos)
        throw std::runtime_error(std::string(key) + " must be a nonempty UTF-8 string without NUL");
    return text;
}

const Json &requiredObject(const Json &object, const char *key)
{
    const auto value = object.find(key);
    if (value == object.end() || !value->is_object())
        throw std::runtime_error(std::string(key) + " must be an object");
    return *value;
}

int nonnegativeInteger(const Json &object, const char *key)
{
    const auto value = object.find(key);
    if (value == object.end() || !value->is_number())
        throw std::runtime_error(std::string(key) + " must be a nonnegative 32-bit integer");
    const auto number = value->get<double>();
    if (!std::isfinite(number) || number < 0 || number > INT32_MAX || std::trunc(number) != number)
        throw std::runtime_error(std::string(key) + " must be a nonnegative 32-bit integer");
    return static_cast<int>(number);
}

double requiredNumber(const Json &object, const char *key)
{
    const auto value = object.find(key);
    if (value == object.end() || !value->is_number())
        throw std::runtime_error(std::string(key) + " must be a number");
    const auto number = value->get<double>();
    if (!std::isfinite(number))
        throw std::runtime_error(std::string(key) + " must be finite");
    return number;
}

struct Config {
    int backend{};
    int validBits{};
    int blackbodyKind{};
    std::string source;
    std::string output;
    std::string recipeId;
    std::string connection;
    IRNoiseRecipe recipe{};
};

Config readConfig(const fs::path &fileName)
{
    const auto absoluteFile = fs::absolute(fileName).lexically_normal();
    std::ifstream file(absoluteFile, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open recipe JSON: " + pathUtf8(absoluteFile));

    Json root;
    try {
        root = Json::parse(file);
    } catch (const Json::parse_error &) {
        throw std::runtime_error("Recipe file contains invalid JSON");
    }
    if (!root.is_object())
        throw std::runtime_error("Recipe JSON must contain an object at the root");

    const auto &capture = requiredObject(root, "capture");
    const auto &noise = requiredObject(root, "noise");
    const auto backendName = requiredString(capture, "backend");
    const auto sourceName = requiredString(capture, "source");
    Config config;
    if (backendName == "raw_replay") {
        config.backend = IR_RAW_REPLAY;
        config.source = pathUtf8((absoluteFile.parent_path() / pathFromUtf8(sourceName)).lexically_normal());
    } else if (backendName == "directshow") {
        config.backend = IR_DIRECTSHOW;
        config.source = sourceName;
    } else if (backendName == "v4l2") {
        config.backend = IR_V4L2;
        config.source = sourceName;
    } else {
        throw std::runtime_error("capture.backend must be raw_replay, directshow, or v4l2");
    }
    config.validBits = nonnegativeInteger(capture, "valid_bits");
    if (config.validBits > 16)
        throw std::runtime_error("capture.valid_bits must be from 0 to 16");

    config.recipeId = requiredString(noise, "recipe_id");
    config.recipe.frames = nonnegativeInteger(noise, "frames");
    config.recipe.frame_timeout_ms = nonnegativeInteger(noise, "frame_timeout_ms");

    const auto output = root.find("output_directory");
    if (output != root.end() && !output->is_null()) {
        if (!output->is_string())
            throw std::runtime_error("output_directory must be a string or null");
        const auto outputName = output->get<std::string>();
        if (outputName.empty() || outputName.find('\0') != std::string::npos)
            throw std::runtime_error("output_directory must be a nonempty UTF-8 path");
        config.output = pathUtf8((absoluteFile.parent_path() / pathFromUtf8(outputName)).lexically_normal());
    }

    const auto blackbody = root.find("blackbody");
    if (blackbody != root.end()) {
        if (!blackbody->is_object())
            throw std::runtime_error("blackbody must be an object");
        const auto kind = requiredString(*blackbody, "kind");
        if (kind == "pcn7-lt")
            config.blackbodyKind = IR_PCN7LT;
        else if (kind == "dcn1000")
            config.blackbodyKind = IR_DCN1000;
        else
            throw std::runtime_error("blackbody.kind must be pcn7-lt or dcn1000");
        const auto &connection = requiredObject(*blackbody, "connection");
        config.connection = connection.dump();
        config.recipe.temperature_c = requiredNumber(*blackbody, "temperature_c");
        config.recipe.tolerance_c = requiredNumber(*blackbody, "tolerance_c");
        config.recipe.stable_window_ms = nonnegativeInteger(*blackbody, "stable_window_ms");
        config.recipe.stable_timeout_ms = nonnegativeInteger(*blackbody, "stable_timeout_ms");
        const auto requireStable = blackbody->find("require_device_stable");
        if (requireStable == blackbody->end() || !requireStable->is_boolean())
            throw std::runtime_error("blackbody.require_device_stable must be a boolean");
        config.recipe.require_device_stable = requireStable->get<bool>() ? 1 : 0;
    }
    return config;
}

void printError(const char *operation, int status, const char *detail)
{
    std::fprintf(stderr, "%s status=%d %s\n", operation, status, detail);
}

int run(const fs::path &recipePath)
{
    try {
        const auto config = readConfig(recipePath);
        char error[1024]{};
        IRGeometry geometry{};
        int status = IR_OpenFrames(config.backend, config.source.c_str(), config.validBits,
                                   &geometry, error, sizeof error);
        if (status != IR_OK) {
            printError("IR_OpenFrames", status, error);
            return status;
        }
        std::printf("width=%d height=%d valid_bits=%d\n", geometry.width, geometry.height,
                    geometry.valid_bits);

        IRHandle blackbody = 0;
        if (config.blackbodyKind != 0) {
            status = IR_OpenDevice(config.blackbodyKind, config.connection.c_str(), &blackbody,
                                   error, sizeof error);
            if (status != IR_OK) printError("IR_OpenDevice", status, error);
        }

        if (status == IR_OK) {
            auto recipe = config.recipe;
            recipe.recipe_id = config.recipeId.c_str();
            recipe.blackbody = blackbody;
            IRNoiseResult result{};
            const char *output = config.output.empty() ? nullptr : config.output.c_str();
            // The opened capture source and the complete recipe run stay on this thread.
            status = IR_RunNoise(&recipe, output, &result, error, sizeof error);
            std::printf("status=%d completed=%d failed_step=%d cleanup_status=%d\n",
                        status, result.completed, result.failed_step, result.cleanup_status);
            if (status == IR_OK) {
                std::printf("mean_dn=%.17g\ntemporal_noise_dn=%.17g\n",
                            result.mean_dn, result.temporal_noise_dn);
            } else {
                printError("IR_RunNoise", status, error);
            }
        }

        if (blackbody != 0) {
            const auto closeStatus = IR_CloseDevice(blackbody, error, sizeof error);
            if (closeStatus != IR_OK) printError("IR_CloseDevice", closeStatus, error);
            if (status == IR_OK) status = closeStatus;
        }
        const auto closeStatus = IR_CloseFrames(error, sizeof error);
        if (closeStatus != IR_OK) printError("IR_CloseFrames", closeStatus, error);
        return status == IR_OK ? closeStatus : status;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Recipe JSON: %s\n", error.what());
        return IR_BAD_ARGUMENT;
    }
}

void printUsage()
{
    std::puts("Usage: ir_run_recipe <recipe.json>\n"
              "RAW and output paths resolve relative to the JSON file.\n"
              "A blackbody object opens the instrument and applies the configured setpoint.");
}
}

#ifdef _WIN32
namespace {
std::string wideToUtf8(const wchar_t *value)
{
    const int length = static_cast<int>(std::wcslen(value));
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) throw std::runtime_error("Cannot convert command-line path to UTF-8");
    std::string result(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
                            result.data(), bytes, nullptr, nullptr) != bytes)
        throw std::runtime_error("Cannot convert command-line path to UTF-8");
    return result;
}
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        printUsage();
        return 0;
    }
    if (argc != 2) {
        printUsage();
        return IR_BAD_ARGUMENT;
    }
    try {
        return run(pathFromUtf8(wideToUtf8(argv[1])));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Recipe path: %s\n", error.what());
        return IR_BAD_ARGUMENT;
    }
}
#else
int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") {
        printUsage();
        return 0;
    }
    if (argc != 2) {
        printUsage();
        return IR_BAD_ARGUMENT;
    }
    return run(pathFromUtf8(argv[1]));
}
#endif
