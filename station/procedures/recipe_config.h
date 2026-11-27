#pragma once

#include "IR_analysis.h"
#include "IR_recipe.h"
#include "measurement.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <initializer_list>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <variant>
#include <vector>

namespace ir::procedures::recipe {
using Json = nlohmann::json;

inline uint64_t pixelElementBytes(int32_t dataType) noexcept
{
    return dataType == IR_TYPE_U16 ? sizeof(uint16_t)
        : dataType == IR_TYPE_U32 ? sizeof(uint32_t) : 0;
}

inline const char *pixelDataTypeName(int32_t dataType) noexcept
{
    return dataType == IR_TYPE_U16 ? "U16"
        : dataType == IR_TYPE_U32 ? "U32" : "unknown";
}

inline const char *pixelFormatName(int32_t dataType) noexcept
{
    return dataType == IR_TYPE_U16 ? "RAW16_LE"
        : dataType == IR_TYPE_U32 ? "RAW32_LE" : "unknown";
}

struct DeviceConfig {
    int kind{};
    std::string connectionJson;
    uint32_t powerCapabilities{0};
    int32_t powerMinChannel{0};
    int32_t powerMaxChannel{0};
};

struct StationConfig {
    int backend{};
    int validBits{};
    int64_t maxBatchBytes{256 * 1024 * 1024};
    uint64_t maxInflightBytes{1024ULL * 1024ULL * 1024ULL};
    int32_t queueCapacity{64};
    std::string source;
    std::map<std::string, DeviceConfig> devices;
    std::vector<std::string> pluginPaths;
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

using PixelStorage = std::variant<std::vector<uint16_t>, std::vector<uint32_t>>;

// 只有采集写入像素。归档持有只读批次，后续采集按需替换槽位。
struct FrameBatch {
    IRGeometry geometry{};
    PixelStorage pixels{std::vector<uint16_t>{}};
    std::vector<IRFrameStamp> stamps;

    int32_t dataType() const noexcept
    {
        return pixels.index() == 0 ? IR_TYPE_U16
            : pixels.index() == 1 ? IR_TYPE_U32 : 0;
    }

    void resizePixels(int32_t type, std::size_t elements)
    {
        switch (type) {
        case IR_TYPE_U16:
            if (!std::holds_alternative<std::vector<uint16_t>>(pixels))
                pixels.emplace<std::vector<uint16_t>>();
            std::get<std::vector<uint16_t>>(pixels).resize(elements);
            return;
        case IR_TYPE_U32:
            if (!std::holds_alternative<std::vector<uint32_t>>(pixels))
                pixels.emplace<std::vector<uint32_t>>();
            std::get<std::vector<uint32_t>>(pixels).resize(elements);
            return;
        default:
            throw std::invalid_argument("Recipe capture supports U16 or U32 pixel storage");
        }
    }

    uint64_t pixelCapacityBytes() const noexcept
    {
        if (const auto *values = std::get_if<std::vector<uint16_t>>(&pixels))
            return static_cast<uint64_t>(values->capacity()) * sizeof(uint16_t);
        if (const auto *values = std::get_if<std::vector<uint32_t>>(&pixels))
            return static_cast<uint64_t>(values->capacity()) * sizeof(uint32_t);
        return 0;
    }

    uint64_t capacityBytes() const noexcept
    {
        return pixelCapacityBytes() + static_cast<uint64_t>(stamps.capacity()) * sizeof(IRFrameStamp);
    }
};

inline IRArrayView frameBatchArrayView(const FrameBatch &batch) noexcept
{
    if (const auto *values = std::get_if<std::vector<uint16_t>>(&batch.pixels)) {
        return {values->data(), static_cast<uint64_t>(values->size()) * sizeof(uint16_t),
                IR_TYPE_U16, batch.geometry.width, batch.geometry.height,
                static_cast<int32_t>(batch.stamps.size())};
    }
    if (const auto *values = std::get_if<std::vector<uint32_t>>(&batch.pixels))
        return {values->data(), static_cast<uint64_t>(values->size()) * sizeof(uint32_t),
                IR_TYPE_U32, batch.geometry.width, batch.geometry.height,
                static_cast<int32_t>(batch.stamps.size())};
    return {nullptr, 0, 0, batch.geometry.width, batch.geometry.height,
            static_cast<int32_t>(batch.stamps.size())};
}

inline IRMutableArrayView mutableFrameBatchArrayView(FrameBatch &batch, int32_t frames) noexcept
{
    if (auto *values = std::get_if<std::vector<uint16_t>>(&batch.pixels)) {
        return {values->data(), static_cast<uint64_t>(values->size()) * sizeof(uint16_t),
                IR_TYPE_U16, batch.geometry.width, batch.geometry.height, frames};
    }
    if (auto *values = std::get_if<std::vector<uint32_t>>(&batch.pixels))
        return {values->data(), static_cast<uint64_t>(values->size()) * sizeof(uint32_t),
                IR_TYPE_U32, batch.geometry.width, batch.geometry.height, frames};
    return {nullptr, 0, 0, batch.geometry.width, batch.geometry.height, frames};
}

struct Limit {
    std::string metric;
    std::string unit;
    bool isMaximum;
    double threshold;
};

// ======================== 统一配方数据流与可组合算子模型 (schemav) ========================

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
    std::map<std::string, std::shared_ptr<FrameBatch>> batches;
    std::map<std::string, DoubleMap> maps;
    std::map<std::string, ByteMask> masks;
    std::map<std::string, IRGjbDriftContext> driftContexts;
    std::map<std::string, Json> metrics;
    std::map<std::string, double> scalarValues;

    uint64_t retainedBytes() const noexcept
    {
        uint64_t bytes = 0;
        const auto add = [&bytes](uint64_t count, uint64_t itemBytes) {
            if (itemBytes != 0 && count > (std::numeric_limits<uint64_t>::max() - bytes) / itemBytes) {
                bytes = std::numeric_limits<uint64_t>::max();
                return;
            }
            bytes += count * itemBytes;
        };
        for (const auto &[name, batch] : batches) {
            (void)name;
            add(batch->capacityBytes(), 1);
        }
        for (const auto &[name, map] : maps) {
            (void)name;
            add(map.data.capacity(), sizeof(double));
        }
        for (const auto &[name, mask] : masks) {
            (void)name;
            add(mask.data.capacity(), sizeof(uint8_t));
        }
        add(driftContexts.size(), sizeof(IRGjbDriftContext));
        return bytes;
    }
};

struct FlushStep {
    StepRef ref;
    int32_t discardFrames{};
    int32_t timeoutMs{};
};

struct CaptureStep {
    StepRef ref;
    int32_t frames{};
    int32_t timeoutMs{};
    std::string outputSlot;
};

struct SaveRawStep {
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

struct DetectorRegisterStep {
    StepRef ref;
    std::string device;
    uint8_t commandCode{};
    std::vector<uint8_t> payload;
    std::string payloadHex;
};

struct DetectorCurrentStep {
    StepRef ref;
    std::string device;
    std::string name;
    int32_t gainCode{};
    uint16_t channelCode{};
};

struct CoolerReadStep {
    StepRef ref;
    std::string device;
    std::string name;
};

struct PowerWaitStableStep {
    StepRef ref;
    std::string device;
    int32_t channel{};
    IRPowerCurrentStableSpec spec{};
};

struct PluginProvenance {
    std::string id;
    std::string file;
};

struct MetricOutput {
    std::string suffix;
    std::string unit;
    std::string method;
    int32_t version{1};
    std::optional<PluginProvenance> plugin;
};

struct AnalysisRunContext {
    ExecutionContext &ctx;
    std::function<bool()> isStopRequested;
    uint64_t inflightBaseBytes{0};
    uint64_t maxInflightBytes{0};
};

struct MetricOperations {
    std::function<void(IRGeometry)> checkGeometry;
    std::function<void(AnalysisRunContext &)> execute;
};

struct AnalysisStep {
    StepRef ref;
    std::string name;
    std::string method;
    int32_t version{1};
    std::vector<MetricOutput> outputs;
    MetricOperations operations;
};

using InnerStep = std::variant<
    BlackbodyStep, PowerStep, PowerWaitStableStep, OvenStabilizeStep, OvenStandbyStep, DmmReadStep,
    DetectorTimingStep, DetectorGpolStep, DetectorRegisterStep, DetectorCurrentStep,
    CoolerReadStep, FlushStep, CaptureStep, AnalysisStep>;

struct RepeatBurstStep {
    StepRef ref;
    int32_t maxIterations{0};
    double durationSec{0.0};
    double intervalSec{0.0};
    std::string csvFile{"burst_summary.csv"};
    std::vector<std::string> recordMetrics;
    std::vector<InnerStep> steps;
};

using RecipeStep = std::variant<
    BlackbodyStep, PowerStep, PowerWaitStableStep, OvenStabilizeStep, OvenStandbyStep, DmmReadStep,
    DetectorTimingStep, DetectorGpolStep, DetectorRegisterStep, DetectorCurrentStep,
    CoolerReadStep, FlushStep, CaptureStep, AnalysisStep, SaveRawStep,
    RepeatBurstStep>;

struct RecipeConfig {
    std::string id;
    std::vector<RecipeStep> steps;
    std::vector<Limit> limits;
    std::vector<std::string> deviceAliases;
    std::vector<StandbyBlackbody> standbyBlackbodies;
    std::vector<std::string> standbyOvens;
    std::map<std::string, std::string> metricUnits;
    Json definition;
    bool hasRepeatBurst{false};
    size_t doubleMapSlotCount{};
    size_t byteMaskSlotCount{};
    size_t driftContextSlotCount{};
};

struct ExternalMetricResolver {
    std::function<bool(std::string_view)> handlesMethod;
    std::function<AnalysisStep(StepRef, const std::string &, const std::string &, int32_t,
                               const Json &, const std::string &,
                               std::map<std::string, SlotKind> &)> prepareMetric;
};

StationConfig parseStation(const char *text);
RecipeConfig parseRecipe(const char *text, const StationConfig &station,
                         const ExternalMetricResolver *externalMetrics = nullptr);
void runRecipe(const char *stationJson,
               const std::function<RecipeConfig(const StationConfig &)> &prepareRecipe,
               const char *directory, IRRecipeResult &result, const IRRecipeRunOptions *options);

uint64_t addBytes(uint64_t total, uint64_t additional);
uint64_t multiplyBytes(uint64_t count, uint64_t itemBytes);
uint64_t batchStorageBytes(IRGeometry geometry, int32_t frames, int32_t dataType,
                           int64_t maxBatchBytes);
void requireWithinMemoryLimit(uint64_t required, uint64_t limit);

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
AnalysisStep prepareAnalysis(StepRef ref, const Json &fields, const std::string &path,
                             std::map<std::string, SlotKind> &knownSlots,
                             const ExternalMetricResolver *externalMetrics = nullptr);
} // namespace ir::procedures::recipe
