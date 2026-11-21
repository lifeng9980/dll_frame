#include "IR_recipe.h"
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {
void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string pathUtf8(const fs::path &path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / fs::path("IR-SDK-recipe-") / std::to_string(suffix);
        fs::create_directories(path_);
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    const fs::path &path() const { return path_; }
private:
    fs::path path_;
};

void writeRaw(const fs::path &path)
{
    const std::array<uint16_t, 8> pixels{20, 40, 60, 80, 22, 42, 62, 82};
    std::ofstream raw(path / "frames.raw", std::ios::binary);
    raw.write(reinterpret_cast<const char *>(pixels.data()), sizeof(pixels));
    check(raw.good(), "write fixture RAW");
    std::ofstream index(path / "frames.csv", std::ios::binary);
    index << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
          << "0,2,2,4,RAW16_LE,16,little\n"
          << "1,2,2,4,RAW16_LE,16,little\n";
    check(index.good(), "write fixture index");
}

Json makeRecipe(double noiseLimit)
{
    return {
        {"schema_version", 1},
        {"recipe_id", "roi-and-noise-test-v1"},
        {"steps", Json::array({
            {{"id", "batch"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}},
            {{"id", "noise"}, {"type", "analyze"}, {"method", "temporal_noise"},
             {"name", "noise"}, {"input", "batch"}},
            {{"id", "left"}, {"type", "analyze"}, {"method", "roi_mean"},
             {"name", "left"}, {"input", "batch"}, {"x", 0}, {"y", 0}, {"width", 1}, {"height", 2}},
            {{"id", "archive"}, {"type", "save_raw"}, {"input", "batch"}}
        })},
        {"limits", Json::array({{{"metric", "noise.temporal_noise_dn"}, {"operator", "max"},
                                  {"value", noiseLimit}, {"unit", "DN"}}})}
    };
}
}

int main()
{
    try {
        TemporaryDirectory temporary;
        const auto rawPath = temporary.path() / "raw16";
        fs::create_directories(rawPath);
        writeRaw(rawPath);

        Json station{
            {"schema_version", 1},
            {"acquisition", {{"backend", "raw_replay"}, {"source", pathUtf8(rawPath)},
                              {"valid_bits", 16}, {"max_batch_bytes", 64}}},
            {"devices", Json::object()}
        };
        auto recipe = makeRecipe(2.0);
        const auto stationText = station.dump();
        const auto recipeText = recipe.dump();
        char error[1024]{};
        check(IR_CheckRecipeJson(stationText.c_str(), recipeText.c_str(), error, sizeof error) == IR_OK,
              error);

        auto malformed = recipe;
        malformed["steps"][1]["not_a_field"] = true;
        const auto malformedText = malformed.dump();
        check(IR_CheckRecipeJson(stationText.c_str(), malformedText.c_str(), error, sizeof error) == IR_BAD_ARGUMENT,
              "unknown recipe fields must be rejected");

        auto tooSmall = station;
        tooSmall["acquisition"]["max_batch_bytes"] = 8;
        const auto tooSmallText = tooSmall.dump();
        const auto rejectedOutput = pathUtf8(temporary.path() / "too-small");
        IRRecipeResult result{};
        check(IR_RunRecipeJson(tooSmallText.c_str(), recipeText.c_str(), rejectedOutput.c_str(),
                               &result, error, sizeof error) == IR_BAD_ARGUMENT,
              "batch above configured byte limit must be rejected");
        check(result.failed_stage == IR_RECIPE_STAGE_PREPARE && result.failed_step_index == 0 &&
              result.completed_frames == 0 && result.cleanup_status == IR_OK, "batch preparation result");
        check(!fs::exists(temporary.path() / "too-small"), "failed batch must not publish output");

        auto shortSource = recipe;
        shortSource["steps"][0]["frames"] = 3;
        const auto shortSourceText = shortSource.dump();
        const auto shortOutput = temporary.path() / "short-source";
        const auto shortPath = pathUtf8(shortOutput);
        check(IR_RunRecipeJson(stationText.c_str(), shortSourceText.c_str(), shortPath.c_str(),
                               &result, error, sizeof error) == IR_END_OF_STREAM, error);
        check(result.completed_frames == 2 && result.failed_stage == IR_RECIPE_STAGE_CAPTURE &&
              result.failed_step_index == 0 && result.cleanup_status == IR_OK, "partial batch result");
        check(!fs::exists(shortOutput), "incomplete capture must not publish output");

        const auto passOutput = temporary.path() / "pass-run";
        const auto passPath = pathUtf8(passOutput);
        check(IR_RunRecipeJson(stationText.c_str(), recipeText.c_str(), passPath.c_str(),
                               &result, error, sizeof error) == IR_OK, error);
        check(result.completed_frames == 2 && result.failed_stage == IR_RECIPE_STAGE_NONE &&
              result.failed_step_index == -1 && result.cleanup_status == IR_OK, "complete batch result");
        std::ifstream passFile(passOutput / "result.json", std::ios::binary);
        const auto pass = Json::parse(passFile);
        check(pass.at("run_status") == "PASS" && pass.at("completed_frames") == 2,
              "pass recipe status and frame count");
        check(pass.at("recipe") == recipe && pass.at("cleanup_status") == IR_OK, "archived recipe and cleanup status");
        check(std::abs(pass.at("metrics").at("left.mean_dn").at("value").get<double>() - 41.0) < 1e-12,
              "ROI mean result");
        check(std::abs(pass.at("metrics").at("noise.mean_dn").at("value").get<double>() - 51.0) < 1e-12,
              "whole image mean result");
        check(std::abs(pass.at("metrics").at("noise.temporal_noise_dn").at("value").get<double>() -
                       std::sqrt(2.0)) < 1e-12, "temporal noise result");

        recipe = makeRecipe(1.0);
        const auto ngText = recipe.dump();
        const auto ngOutput = pathUtf8(temporary.path() / "ng-run");
        check(IR_RunRecipeJson(stationText.c_str(), ngText.c_str(), ngOutput.c_str(),
                               nullptr, error, sizeof error) == IR_OK, error);
        std::ifstream ngFile(temporary.path() / "ng-run" / "result.json", std::ios::binary);
        check(Json::parse(ngFile).at("run_status") == "NG", "limit miss is product NG, not SDK failure");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
