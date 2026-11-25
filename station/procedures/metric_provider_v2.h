#pragma once

#include "recipe_config.h"
#include <map>
#include <string>
#include <string_view>

namespace ir::procedures::recipe {

class IMetricProviderV2 {
public:
    virtual ~IMetricProviderV2() = default;
    virtual bool handlesMethod(std::string_view methodName) const noexcept = 0;
    virtual AnalysisStepV2 prepareMetric(
        StepRef ref, const std::string &stepName, const std::string &method,
        int32_t reqVersion, const Json &fields, const std::string &path,
        std::map<std::string, SlotKind> &knownSlots) const = 0;
};

} // namespace ir::procedures::recipe
