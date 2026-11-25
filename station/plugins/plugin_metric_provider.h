#pragma once

#include "plugin_manager.h"
#include "station/procedures/metric_provider_v2.h"
#include <string>
#include <string_view>
#include <vector>

namespace ir::plugins {

class PluginMetricProvider final : public ir::procedures::recipe::IMetricProviderV2 {
public:
    int32_t loadConfiguredPaths(const std::vector<std::string> &utf8Paths, std::string *errOut);
    bool handlesMethod(std::string_view methodName) const noexcept override;
    ir::procedures::recipe::AnalysisStepV2 prepareMetric(
        ir::procedures::recipe::StepRef ref,
        const std::string &stepName,
        const std::string &method,
        int32_t reqVersion,
        const ir::procedures::recipe::Json &fields,
        const std::string &path,
        std::map<std::string, ir::procedures::recipe::SlotKind> &knownSlots) const override;

private:
    PluginManager manager_;
};

} // namespace ir::plugins
