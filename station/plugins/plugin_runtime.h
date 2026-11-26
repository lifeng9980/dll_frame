#pragma once

#include "IR_plugin_abi.h"
#include "station/procedures/metric_registry.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ir::plugins {

struct LibraryHandle;

struct LoadedMetricPlugin {
    std::shared_ptr<LibraryHandle> library;
    const IRMetricPlugin *descriptor{nullptr};
    std::string pluginId;
    std::string methodName;
    std::string pluginFile;
};

struct PluginStepBinding {
    std::map<std::string, std::pair<std::string, ir::procedures::recipe::SlotKind>, std::less<>> inputs;
    std::map<std::string, std::pair<std::string, ir::procedures::recipe::SlotKind>, std::less<>> outputs;
    std::vector<ir::procedures::recipe::MetricOutput> scalarOutputs;
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

    static bool isValidPluginMethodName(std::string_view methodName) noexcept;

private:
    int32_t loadLibrary(const std::filesystem::path &libraryPath, bool requireQuerySymbol,
                        std::string *errOut);
    int32_t loadFromDirectory(const std::filesystem::path &pluginDir, std::string *errOut);
    std::optional<LoadedMetricPlugin> findMetric(std::string_view methodName) const;

    std::unordered_map<std::string, LoadedMetricPlugin> metrics_;
};

} // namespace ir::plugins
