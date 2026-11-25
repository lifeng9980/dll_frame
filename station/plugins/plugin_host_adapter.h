#pragma once

#include "IR_plugin_abi.h"
#include "plugin_manager.h"
#include "station/procedures/metric_registry.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ir::plugins {

struct PluginStepBinding {
    std::map<std::string, std::pair<std::string, ir::procedures::recipe::SlotKind>, std::less<>> inputs;
    std::map<std::string, std::pair<std::string, ir::procedures::recipe::SlotKind>, std::less<>> outputs;
    std::vector<ir::procedures::recipe::MetricOutput> scalarOutputs;
};

class PrepareCallbackBridge {
public:
    PrepareCallbackBridge(const LoadedMetricPlugin &plugin,
                          const ir::procedures::recipe::Json &inputsJson,
                          const ir::procedures::recipe::Json &outputsJson,
                          std::map<std::string, ir::procedures::recipe::SlotKind> &knownSlots,
                          const std::string &stepPath);

    PluginStepBinding prepare(const ir::procedures::recipe::Json &paramsJson);

    int32_t handleRequireInputSlot(const char *inputKey, int32_t expectedSlotKind,
                                   int32_t required, int32_t *outIsBound) noexcept;
    int32_t handleBindOutputSlot(const char *outputKey, int32_t slotKind,
                                 int32_t required, int32_t *outIsBound) noexcept;
    int32_t handleDeclareScalarOutput(const char *suffixUtf8, const char *unitUtf8) noexcept;

private:
    int32_t recordCallbackError(int32_t code, const std::string &msg) noexcept;
    int32_t recordCallbackError(int32_t code, const char *msg) noexcept;

    const LoadedMetricPlugin &plugin_;
    const ir::procedures::recipe::Json &inputsJson_;
    const ir::procedures::recipe::Json &outputsJson_;
    std::map<std::string, ir::procedures::recipe::SlotKind> &knownSlots_;
    const std::string &stepPath_;
    PluginStepBinding binding_;
    std::set<std::string> queriedInputs_;
    std::set<std::string> queriedOutputs_;
    std::set<std::string> boundOutputSlotNames_;
    std::set<std::string> declaredSuffixes_;
    int32_t firstCallbackErrorCode_{IR_OK};
    char lastError_[1024]{};
    IRPrepareCallbacksV1 vtable_{};
};

class HostCallbackBridge {
public:
    HostCallbackBridge(const LoadedMetricPlugin &plugin,
                       const PluginStepBinding &binding,
                       const std::string &stepName,
                       ir::procedures::recipe::AnalysisRunContext &runCtx);

    void execute(const std::string &paramsText);

    int32_t handleHasSlot(const char *ioKey) noexcept;
    int32_t handleGetSlot(const char *inputKey, IRSlotView *outView) noexcept;
    int32_t handleAllocDoubleMap(const char *outputKey, int32_t width, int32_t height, double **outData) noexcept;
    int32_t handleAllocByteMask(const char *outputKey, int32_t width, int32_t height, uint8_t **outData) noexcept;
    int32_t handleEmitScalar(const char *suffixUtf8, double value) noexcept;
    int32_t handleIsCancelled() const noexcept;

private:
    void setLastError(const char *msg) noexcept;
    uint64_t stagedBytes() const;
    bool withinInflightBudget(uint64_t deltaBytes) noexcept;

    const LoadedMetricPlugin &plugin_;
    const PluginStepBinding &binding_;
    const std::string &stepName_;
    ir::procedures::recipe::AnalysisRunContext &runCtx_;
    std::map<std::string, ir::procedures::recipe::DoubleMap, std::less<>> stagedMaps_;
    std::map<std::string, ir::procedures::recipe::ByteMask, std::less<>> stagedMasks_;
    std::map<std::string, double, std::less<>> emittedScalars_;
    char lastError_[1024]{};
    IRHostCallbacksV1 vtable_{};
};

class PluginMetricProvider final {
public:
    int32_t loadConfiguredPaths(const std::vector<std::string> &utf8Paths, std::string *errOut);
    bool handlesMethod(std::string_view methodName) const noexcept;
    ir::procedures::recipe::AnalysisStep prepareMetric(
        ir::procedures::recipe::StepRef ref,
        const std::string &stepName,
        const std::string &method,
        int32_t reqVersion,
        const ir::procedures::recipe::Json &fields,
        const std::string &path,
        std::map<std::string, ir::procedures::recipe::SlotKind> &knownSlots) const;

private:
    PluginManager manager_;
};

} // namespace ir::plugins
