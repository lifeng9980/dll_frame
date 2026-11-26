#include "IR_recipe.h"
#include "api_common.h"
#include "recipe_config.h"
#include "station/plugins/plugin_runtime.h"
#include <memory>

namespace {
using namespace ir::api;
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

RecipeConfig prepareRecipe(const char *recipeJson, const StationConfig &station)
{
    const auto provider = loadPluginProviderIfNeeded(station);
    const auto resolver = makeExternalResolver(provider.get());
    return parseRecipe(recipeJson, station, provider ? &resolver : nullptr);
}
} // namespace

extern "C" int32_t IR_CheckRecipeJson(const char *stationJson, const char *recipeJson,
    char *err, int32_t errBytes)
noexcept {
    return guarded(err, errBytes, [&] {
        (void)prepareRecipe(recipeJson, parseStation(stationJson));
    });
}

extern "C" int32_t IR_RunRecipeJson(const char *stationJson, const char *recipeJson,
    const char *directory, const IRRecipeRunOptions *options,
    IRRecipeResult *result, char *err, int32_t errBytes)
noexcept {
    IRRecipeResult report{0, IR_RECIPE_STAGE_VALIDATE, -1, IR_OK};
    const auto status = guarded(err, errBytes, [&] {
        runRecipe(stationJson, [recipeJson](const StationConfig &station) {
            return prepareRecipe(recipeJson, station);
        }, directory, report, options);
    });
    if (result) *result = report;
    return status;
}
