#pragma once

#include "measurement.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace ir::procedures::recipe {
using Json = nlohmann::json;

struct DeviceConfig {
    int kind{};
    std::string connectionJson;
};

struct StationConfig {
    int backend{};
    int validBits{};
    int64_t maxBatchBytes{256 * 1024 * 1024};
    std::string source;
    std::map<std::string, DeviceConfig> devices;
};

struct StepRef {
    int32_t index{-1};
    std::string id;
};

struct BlackbodyStep {
    StepRef ref;
    std::string device;
    BlackbodyTarget target;
};

struct PowerStep {
    enum class Operation { set, output, read };
    StepRef ref;
    std::string device;
    int32_t channel{};
    Operation operation{};
    double voltageV{};
    double currentLimitA{};
    bool enabled{};
    bool offAfterRun{};
};

struct CaptureStep {
    StepRef ref;
    int32_t frames{};
    int32_t timeoutMs{};
};

// 只有采集写入像素。指标和归档都以 const 引用使用完整批次。
struct FrameBatch {
    IRGeometry geometry{};
    std::vector<uint16_t> pixels;
    std::vector<IRFrameStamp> stamps;
};

struct MetricOutput {
    const char *suffix;
    const char *unit;
    const char *method;
    int32_t version;
};

struct MetricOperations {
    std::function<void(IRGeometry)> checkGeometry;
    std::function<std::vector<double>(const FrameBatch &)> calculate;
};

struct AnalysisStep {
    StepRef ref;
    std::string name;
    std::span<const MetricOutput> outputs;
    MetricOperations operations;
};

struct Limit {
    std::string metric;
    std::string unit;
    bool isMaximum;
    double threshold;
};

struct RecipeConfig {
    std::string id;
    std::vector<std::variant<BlackbodyStep, PowerStep>> controls;
    CaptureStep capture;
    std::vector<AnalysisStep> analysis;
    StepRef archive;
    std::vector<Limit> limits;
    std::vector<std::string> deviceAliases;
    Json definition;  // 已通过预检的配方内容，用于保存实际计算参数。
};

StationConfig parseStation(const char *text);
RecipeConfig parseRecipe(const char *text, const StationConfig &station);

// 设备和指标配置共用字段读取与错误位置。
std::string stringField(const Json &object, const char *key, const std::string &path);
int integerField(const Json &object, const char *key, int low, int high, const std::string &path);
void allowFields(const Json &object, std::initializer_list<const char *> allowed, const std::string &path);
AnalysisStep prepareAnalysis(StepRef ref, const Json &fields, const std::string &path, int32_t frames);
}
