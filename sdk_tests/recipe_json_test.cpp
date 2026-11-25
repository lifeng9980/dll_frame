#include "IR_recipe.h"
#include "IR_analysis.h"
#include "IR_station.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

std::string readText(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    check(file.is_open(), ("Cannot open file: " + pathUtf8(path)).c_str());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool isIso8601UtcMillis(const std::string &value)
{
    if (value.size() != 24) return false;
    const char *pattern = "dddd-dd-ddTdd:dd:dd.dddZ";
    for (size_t index = 0; index < 24; ++index) {
        if (pattern[index] == 'd') {
            if (!std::isdigit(static_cast<unsigned char>(value[index]))) return false;
        } else if (value[index] != pattern[index]) {
            return false;
        }
    }
    return true;
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
        {"schemav", 1},
        {"recipe_id", "roi-and-noise-test"},
        {"steps", Json::array({
            {{"id", "batch"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "batch"}},
            {{"id", "noise"}, {"type", "analyze"}, {"method", "temporal_noise"},
             {"name", "noise"}, {"input", "batch"}},
            {{"id", "left"}, {"type", "analyze"}, {"method", "roi_mean"},
             {"name", "left"}, {"input", "batch"},
             {"params", {{"x", 0}, {"y", 0}, {"width", 1}, {"height", 2}}}},
            {{"id", "archive"}, {"type", "save_raw"}, {"input", "batch"}}
        })},
        {"limits", Json::array({{{"metric", "noise.temporal_noise_dn"}, {"operator", "max"},
                                  {"value", noiseLimit}, {"unit", "DN"}}})}
    };
}

void checkBundledTemplates(const fs::path &examplesDir)
{
    const fs::path stationsDir = examplesDir / "stations";
    const fs::path recipesDir = examplesDir / "recipes";
    const std::vector<std::pair<std::string, std::string>> pairs{
        {"offline.json", "noise_roi.json"},
        {"roi_2x2.json", "noise_roi_2x2.json"},
        {"roi_2x2.json", "nodisk_roi.json"},
        {"sim_tcp.json", "noise_blackbody.json"},
        {"sim_power_serial.json", "power_serial_smoke.json"},
        {"sim_power_serial.json", "power_serial_output_cycle.json"},
        {"sim_virtual_station_tcp.json", "virtual_station_hil.json"},
    };

    std::set<std::string> checkedStations;
    std::set<std::string> checkedRecipes;
    char error[1024]{};
    for (const auto &[stationName, recipeName] : pairs) {
        const auto stationPath = stationsDir / stationName;
        const auto recipePath = recipesDir / recipeName;
        const auto stationText = readText(stationPath);
        const auto recipeText = readText(recipePath);

        const auto stationJson = Json::parse(stationText);
        const auto recipeJson = Json::parse(recipeText);
        check(!stationJson.contains("$schema") && !recipeJson.contains("$schema"),
              "bundled templates must not declare $schema");
        check(!stationJson.contains("schema_version") && !recipeJson.contains("schema_version"),
              "bundled templates must not declare legacy schema_version");

        const auto status = IR_CheckRecipeJson(stationText.c_str(), recipeText.c_str(), error, sizeof error);
        if (status != IR_OK) {
            throw std::runtime_error("Template preflight failed for " + stationName + " + " +
                                     recipeName + ": " + error);
        }
        checkedStations.insert(stationName);
        checkedRecipes.insert(recipeName);
    }

    for (const auto &entry : fs::directory_iterator(stationsDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            const auto name = entry.path().filename().string();
            check(checkedStations.contains(name),
                  ("Uncovered station template in examples/stations: " + name).c_str());
        }
    }
    for (const auto &entry : fs::directory_iterator(recipesDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            const auto name = entry.path().filename().string();
            check(checkedRecipes.contains(name),
                  ("Uncovered recipe template in examples/recipes: " + name).c_str());
        }
    }
}
}

int main(int argc, char **argv)
{
    try {
        TemporaryDirectory temporary;
        const auto rawPath = temporary.path() / "raw16";
        fs::create_directories(rawPath);
        writeRaw(rawPath);

        Json station{
            {"schemav", 1},
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

        // Omitting schemav defaults cleanly to 1
        auto defaultStation = station;
        defaultStation.erase("schemav");
        auto defaultRecipe = recipe;
        defaultRecipe.erase("schemav");
        check(IR_CheckRecipeJson(defaultStation.dump().c_str(), defaultRecipe.dump().c_str(),
                                 error, sizeof error) == IR_OK,
              "omitting schemav must succeed by default");

        // Legacy $schema and schema_version must be rejected
        auto badStationSchema = station;
        badStationSchema["$schema"] = "../schemas/station.schema.json";
        check(IR_CheckRecipeJson(badStationSchema.dump().c_str(), recipeText.c_str(), error, sizeof error) == IR_BAD_ARGUMENT,
              "legacy station $schema must be rejected");

        auto badRecipeSchema = recipe;
        badRecipeSchema["$schema"] = "../schemas/recipe.schema.json";
        check(IR_CheckRecipeJson(stationText.c_str(), badRecipeSchema.dump().c_str(), error, sizeof error) == IR_BAD_ARGUMENT,
              "legacy recipe $schema must be rejected");

        auto legacyStationVer = defaultStation;
        legacyStationVer["schema_version"] = 1;
        check(IR_CheckRecipeJson(legacyStationVer.dump().c_str(), recipeText.c_str(), error, sizeof error) == IR_BAD_ARGUMENT,
              "legacy station schema_version must be rejected");

        auto legacyRecipeVer = defaultRecipe;
        legacyRecipeVer["schema_version"] = 2;
        check(IR_CheckRecipeJson(stationText.c_str(), legacyRecipeVer.dump().c_str(), error, sizeof error) == IR_BAD_ARGUMENT,
              "legacy recipe schema_version must be rejected");

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

        auto limitedStation = station;
        limitedStation["acquisition"]["queue_capacity"] = 2;
        limitedStation["acquisition"]["max_inflight_bytes"] = 150;
        const auto limitedStationText = limitedStation.dump();
        const auto budgetOutput = temporary.path() / "total-budget";
        check(IR_RunRecipeJson(limitedStationText.c_str(), recipeText.c_str(), pathUtf8(budgetOutput).c_str(),
                               &result, error, sizeof error) == IR_BAD_ARGUMENT,
              "queue and batch above total memory limit must be rejected");
        check(std::string(error).find("max_inflight_bytes") != std::string::npos &&
              result.failed_stage == IR_RECIPE_STAGE_PREPARE && result.completed_frames == 0 &&
              !fs::exists(budgetOutput), "total memory limit result");

        IRGeometry existingGeometry{};
        check(IR_SetFrameQueueCapacity(8, error, sizeof error) == IR_OK, error);
        check(IR_OpenFrames(IR_RAW_REPLAY, pathUtf8(rawPath).c_str(), 16, &existingGeometry,
                            error, sizeof error) == IR_OK, error);
        const auto busyOutput = pathUtf8(temporary.path() / "existing-source");
        check(IR_RunRecipeJson(limitedStationText.c_str(), recipeText.c_str(), busyOutput.c_str(),
                               &result, error, sizeof error) == IR_BUSY, "existing source must remain reserved");
        IRCaptureStats existingStats{};
        check(IR_GetCaptureStats(&existingStats, error, sizeof error) == IR_OK, error);
        check(existingStats.queue_capacity == 8, "busy recipe must preserve the caller queue");
        check(IR_StartFrames(error, sizeof error) == IR_OK, error);
        check(IR_StopFrames(error, sizeof error) == IR_OK, error);
        check(IR_CloseFrames(error, sizeof error) == IR_OK, error);

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
        check(pass.at("station_version") == IR_StationVersion(), "result.json station_version mismatch");
        check(pass.at("analysis_version") == IR_AnalysisVersion(), "result.json analysis_version mismatch");
        check(pass.at("source_commit").is_string() && !pass.at("source_commit").get<std::string>().empty(),
              "result.json source_commit must be a nonempty string");
        const auto startedUtc = pass.at("started_utc").get<std::string>();
        const auto finishedUtc = pass.at("finished_utc").get<std::string>();
        check(isIso8601UtcMillis(startedUtc), "result.json started_utc must be ISO-8601 UTC");
        check(isIso8601UtcMillis(finishedUtc), "result.json finished_utc must be ISO-8601 UTC");
        check(startedUtc <= finishedUtc, "result.json started_utc must not be after finished_utc");
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

        if (argc > 1) {
            checkBundledTemplates(fs::path(argv[1]));
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
