#include "IR_recipe.h"
#include "api_common.h"
#include "recipe_config.h"
#include "recipe_runtime_common.h"
#include "station/acquisition/frame_source.h"
#include "station/plugins/plugin_runtime.h"
#include <memory>

namespace {
using namespace ir::api;
using namespace ir::procedures;
using namespace ir::procedures::recipe;

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

ExternalMetricResolver makeExternalResolver(const ir::plugins::PluginMetricProvider *provider)
{
    ExternalMetricResolver resolver;
    if (!provider)
        return resolver;
    resolver.handlesMethod = [provider](std::string_view method) {
        return provider->handlesMethod(method);
    };
    resolver.prepareMetric = [provider](StepRef ref, const std::string &stepName,
                                        const std::string &method, int32_t reqVersion,
                                        const Json &fields, const std::string &path,
                                        std::map<std::string, SlotKind> &knownSlots) {
        return provider->prepareMetric(std::move(ref), stepName, method, reqVersion,
                                       fields, path, knownSlots);
    };
    return resolver;
}
} // namespace

extern "C" int32_t IR_CheckRecipeJson(const char *stationJson, const char *recipeJson,
    char *err, int32_t errBytes)
{
    return guarded(err, errBytes, [&] {
        const auto station = parseStation(stationJson);
        const auto provider = loadPluginProviderIfNeeded(station);
        const auto resolver = makeExternalResolver(provider.get());
        (void)parseRecipe(recipeJson, station, provider ? &resolver : nullptr);
    });
}

extern "C" int32_t IR_RunRecipeJson(const char *stationJson, const char *recipeJson,
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
        const auto resolver = makeExternalResolver(provider.get());
        const auto recipe = parseRecipe(recipeJson, station, provider ? &resolver : nullptr);
        measurement.frames().checkStop();
        runRecipe(measurement.frames(), station, recipe, directory, report, options);
    });
    if (result) *result = report;
    return status;
}
