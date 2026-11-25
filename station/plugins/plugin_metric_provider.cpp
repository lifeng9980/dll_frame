#include "plugin_metric_provider.h"
#include "plugin_host_adapter.h"
#include "api_common.h"

#include <memory>

namespace ir::plugins {

int32_t PluginMetricProvider::loadConfiguredPaths(const std::vector<std::string> &utf8Paths,
                                                  std::string *errOut)
{
    return manager_.loadConfiguredPaths(utf8Paths, errOut);
}

bool PluginMetricProvider::handlesMethod(std::string_view methodName) const noexcept
{
    return methodName.rfind("plugin:", 0) == 0;
}

ir::procedures::recipe::AnalysisStepV2 PluginMetricProvider::prepareMetric(
    ir::procedures::recipe::StepRef ref,
    const std::string &stepName,
    const std::string &method,
    int32_t reqVersion,
    const ir::procedures::recipe::Json &fields,
    const std::string &path,
    std::map<std::string, ir::procedures::recipe::SlotKind> &knownSlots) const
{
    using namespace ir::api;
    using namespace ir::procedures::recipe;

    require(PluginManager::isValidPluginMethodName(method), IR_BAD_ARGUMENT,
            path + ".method has invalid plugin name: " + method);
    const auto found = manager_.findMetric(method);
    require(found.has_value(), IR_BAD_ARGUMENT,
            path + ".method plugin is not loaded: " + method);
    const auto &plugin = *found;
    require(reqVersion == plugin.methodVersion, IR_BAD_ARGUMENT,
            path + ".version=" + std::to_string(reqVersion) + " does not match plugin method version " +
                std::to_string(plugin.methodVersion));

    const Json normalizedInputs = normalizeInputsObject(
        fields, plugin.descriptor->default_input_key, method, path);
    const Json params = fields.contains("params") ? objectField(fields, "params", path) : Json::object();
    const Json outputs = fields.contains("outputs") ? objectField(fields, "outputs", path) : Json::object();

    PrepareCallbackBridge prepBridge(plugin, normalizedInputs, outputs, knownSlots, path);
    auto binding = std::make_shared<PluginStepBinding>(prepBridge.prepare(params));
    const std::string paramsText = params.dump();

    std::function<void(IRGeometry)> checkGeometryFn;
    if (plugin.descriptor->check_geometry != nullptr) {
        checkGeometryFn = [plugin, paramsText](IRGeometry geom) {
            char errBuf[1024]{};
            int32_t st = IR_COMPUTATION_FAILED;
            try {
                st = plugin.descriptor->check_geometry(
                    paramsText.c_str(), geom, errBuf, static_cast<int32_t>(sizeof(errBuf)));
            } catch (...) {
                throw Error(IR_COMPUTATION_FAILED,
                            "C++ exception escaped plugin check_geometry(): " + plugin.methodName);
            }
            errBuf[sizeof(errBuf) - 1] = '\0';
            if (st != IR_OK) {
                throw Error(st, errBuf[0] != '\0' ? std::string(errBuf)
                                                  : ("Plugin geometry check failed: " + plugin.methodName));
            }
        };
    }

    auto outsCopy = binding->scalarOutputs;
    auto executeFn = [plugin, binding, stepName, paramsText](AnalysisRunContext &runCtx) {
        HostCallbackBridge hostBridge(plugin, *binding, stepName, runCtx);
        hostBridge.execute(paramsText);
    };

    return {
        std::move(ref),
        stepName,
        method,
        plugin.methodVersion,
        std::move(outsCopy),
        {std::move(checkGeometryFn), std::move(executeFn)}
    };
}

} // namespace ir::plugins
