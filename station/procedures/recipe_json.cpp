#include "IR_recipe.h"
#include "IR_analysis.h"
#include "api_common.h"
#include "recipe_config.h"
#include "recipe_runtime_common.h"
#include "save_raw.h"
#include "station/acquisition/frame_source.h"
#include "station/instruments/device_lease.h"
#include "station/plugins/plugin_metric_provider.h"
#include "station/version.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>

namespace {
using namespace ir::api;
using namespace ir::procedures;
using namespace ir::procedures::recipe;
namespace fs = std::filesystem;

std::unique_ptr<ir::plugins::PluginMetricProvider> loadPluginProviderIfNeeded(
    const StationConfig &station)
{
    if (station.pluginPaths.empty())
        return nullptr;
    auto provider = std::make_unique<ir::plugins::PluginMetricProvider>();
    std::string pluginErr;
    const int32_t st = provider->loadConfiguredPaths(station.pluginPaths, &pluginErr);
    if (st != IR_OK)
        throw Error(st, pluginErr);
    return provider;
}
}

extern "C" int32_t IR_CheckRecipeJson(const char *stationJson, const char *recipeJson,
    char *err, int32_t errBytes)
{
    return guarded(err, errBytes, [&] {
        const auto station = parseStation(stationJson);
        const auto provider = loadPluginProviderIfNeeded(station);
        (void)parseRecipeV2(recipeJson, station, provider.get());
    });
}

extern "C" int32_t IR_RunRecipeJsonEx(const char *stationJson, const char *recipeJson,
    const char *directory, const IRRecipeRunOptions *options,
    IRRecipeResult *result, char *err, int32_t errBytes)
{
    IRRecipeResult report{0, IR_RECIPE_STAGE_VALIDATE, -1, IR_OK};
    RecipeProgress progress{report};
    const auto status = guarded(err, errBytes, [&] {
        if (options != nullptr) {
            require(options->struct_size == sizeof(IRRecipeRunOptions), IR_BAD_ARGUMENT,
                    "Invalid IRRecipeRunOptions struct_size");
            require(options->flags == 0, IR_BAD_ARGUMENT,
                    "IRRecipeRunOptions.flags must be 0");
        }
        progress.enter(IR_RECIPE_STAGE_RESERVE);
        Measurement measurement;
        progress.enter(IR_RECIPE_STAGE_VALIDATE);
        const auto station = parseStation(stationJson);
        measurement.frames().checkStop();
        const auto provider = loadPluginProviderIfNeeded(station);
        const auto recipe = parseRecipeV2(recipeJson, station, provider.get());
        measurement.frames().checkStop();
        runRecipeV2(measurement.frames(), station, recipe, directory, report, options);
    });
    if (result) *result = report;
    return status;
}

extern "C" int32_t IR_RunRecipeJson(const char *stationJson, const char *recipeJson,
    const char *directory, IRRecipeResult *result, char *err, int32_t errBytes)
{
    return IR_RunRecipeJsonEx(stationJson, recipeJson, directory, nullptr, result, err, errBytes);
}
