#pragma once

#include "IR_analysis.h"
#include "IR_recipe.h"
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
    int32_t queueCapacity{64};
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

// ======================== Schema v2 数据流与可组合算子模型 ========================

enum class SlotKind { batch, double_map, byte_mask, drift_context };

struct DoubleMap {
    IRGeometry geometry{};
    std::vector<double> data;
};

struct ByteMask {
    IRGeometry geometry{};
    std::vector<uint8_t> data;
};

struct ExecutionContext {
    IRGeometry geometry{};
    std::map<std::string, FrameBatch> batches;
    std::map<std::string, DoubleMap> maps;
    std::map<std::string, ByteMask> masks;
    std::map<std::string, IRGjbDriftContext> driftContexts;
    std::map<std::string, Json> metrics;
    std::map<std::string, double> scalarValues;
};

struct FlushStep {
    StepRef ref;
    int32_t discardFrames{};
    int32_t timeoutMs{};
};

struct CaptureStepV2 {
    StepRef ref;
    int32_t frames{};
    int32_t timeoutMs{};
    std::string outputSlot;
};

struct SaveRawStepV2 {
    StepRef ref;
    std::string inputSlot;
    std::string subdir;
};

struct StandbyBlackbody {
    std::string device;
    double targetC{};
};

struct OvenStabilizeStep {
    StepRef ref;
    std::string device;
    OvenTarget target;
    bool standbyAfterRun{false};
};

struct OvenStandbyStep {
    StepRef ref;
    std::string device;
};

struct DmmReadStep {
    StepRef ref;
    std::string device;
    std::string name;
    int32_t mode{IR_DMM_MODE_ANY};
    bool setMode{false};
    std::string unit{"reading"};
};

struct DetectorTimingStep {
    StepRef ref;
    std::string device;
    int32_t integrationUs{};
    int32_t frameHz{};
    int32_t emptyUs{};
};

struct DetectorGpolStep {
    StepRef ref;
    std::string device;
    double gpolV{};
};

struct MetricV2Operations {
    std::function<void(IRGeometry)> checkGeometry;
    std::function<void(ExecutionContext &)> execute;
};

struct AnalysisStepV2 {
    StepRef ref;
    std::string name;
    std::string method;
    int32_t version{1};
    std::vector<MetricOutput> outputs;
    MetricV2Operations operations;
};

using InnerStepV2 = std::variant<
    BlackbodyStep, PowerStep, OvenStabilizeStep, OvenStandbyStep, DmmReadStep,
    DetectorTimingStep, DetectorGpolStep, FlushStep, CaptureStepV2, AnalysisStepV2>;

struct RepeatBurstStep {
    StepRef ref;
    int32_t maxIterations{0};
    double durationSec{0.0};
    double intervalSec{0.0};
    std::string csvFile{"burst_summary.csv"};
    std::vector<std::string> recordMetrics;
    std::vector<InnerStepV2> steps;
};

using StepV2 = std::variant<
    BlackbodyStep, PowerStep, OvenStabilizeStep, OvenStandbyStep, DmmReadStep,
    DetectorTimingStep, DetectorGpolStep, FlushStep, CaptureStepV2,
    AnalysisStepV2, SaveRawStepV2, RepeatBurstStep>;

struct RecipeConfigV2 {
    std::string id;
    std::vector<StepV2> steps;
    std::vector<Limit> limits;
    std::vector<std::string> deviceAliases;
    std::vector<StandbyBlackbody> standbyBlackbodies;
    std::vector<std::string> standbyOvens;
    Json definition;
    bool hasRepeatBurst{false};
};

StationConfig parseStation(const char *text);
RecipeConfig parseRecipe(const char *text, const StationConfig &station);
int detectRecipeSchemaVersion(const char *text);
RecipeConfigV2 parseRecipeV2(const char *text, const StationConfig &station);
void runRecipeV2(ir::acquisition::Capture &capture, const StationConfig &station,
                 const RecipeConfigV2 &recipe, const char *directory, IRRecipeResult &result);

// 设备和指标配置共用字段读取与错误位置。
const Json &objectField(const Json &object, const char *key, const std::string &path);
std::string stringField(const Json &object, const char *key, const std::string &path);
std::string optionalString(const Json &object, const char *key, const std::string &fallback,
                           const std::string &path);
double numberField(const Json &object, const char *key, const std::string &path);
double optionalNumber(const Json &object, const char *key, double fallback, const std::string &path);
int integerField(const Json &object, const char *key, int low, int high, const std::string &path);
int optionalInteger(const Json &object, const char *key, int fallback, int low, int high,
                    const std::string &path);
bool booleanField(const Json &object, const char *key, const std::string &path);
bool optionalBoolean(const Json &object, const char *key, bool fallback, const std::string &path);
void allowFields(const Json &object, std::initializer_list<const char *> allowed, const std::string &path);
Json parseJson(const char *text, const char *label);

BlackbodyStep parseBlackbodyStep(StepRef ref, const Json &fields, const std::string &path,
                                 const StationConfig &station);
PowerStep parsePowerStep(StepRef ref, const std::string &type, const Json &fields,
                         const std::string &path, const StationConfig &station);
AnalysisStep prepareAnalysis(StepRef ref, const Json &fields, const std::string &path, int32_t frames);
AnalysisStepV2 prepareAnalysisV2(StepRef ref, const Json &fields, const std::string &path,
                                 std::map<std::string, SlotKind> &knownSlots);
} // namespace ir::procedures::recipe
