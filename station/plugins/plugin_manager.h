#pragma once

#include "IR_plugin_abi.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ir::plugins {

struct LibraryHandle;

struct LoadedMetricPlugin {
    std::shared_ptr<LibraryHandle> library;
    const IRMetricPluginV1 *descriptor{nullptr};
    std::string pluginId;
    std::string pluginVersion;
    std::string methodName;
    int32_t methodVersion{1};
    std::string pluginFile;
    std::string pluginSha256;
    uint64_t fileSizeBytes{0};
};

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager &) = delete;
    PluginManager &operator=(const PluginManager &) = delete;
    PluginManager(PluginManager &&) noexcept;
    PluginManager &operator=(PluginManager &&) noexcept;

    int32_t loadLibrary(const std::filesystem::path &libraryPath,
                        std::string *errOut = nullptr);

    int32_t loadFromDirectory(const std::filesystem::path &pluginDir,
                              std::string *errOut = nullptr);

    int32_t loadConfiguredPaths(const std::vector<std::string> &utf8Paths,
                                std::string *errOut = nullptr);

    std::optional<LoadedMetricPlugin> findMetric(std::string_view methodName) const;
    std::vector<LoadedMetricPlugin> listMetrics() const;
    void unloadAll();

    static bool isValidPluginMethodName(std::string_view methodName) noexcept;

private:
    static int32_t validateDescriptor(const IRMetricPluginV1 &desc,
                                      std::string *errOut);

    mutable std::shared_mutex mutex_;
    std::vector<std::shared_ptr<LibraryHandle>> libraries_;
    std::unordered_map<std::string, LoadedMetricPlugin> metricsByMethod_;
};

} // namespace ir::plugins
