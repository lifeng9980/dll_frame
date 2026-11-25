#pragma once

#include "recipe_config.h"
#include "IR_analysis.h"
#include "api_common.h"
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace ir::procedures::recipe {

struct PreparedMetricV2 {
    int32_t version{1};
    std::vector<MetricOutputV2> outputs;
    MetricV2Operations ops;
};

struct MetricSpecV2 {
    const char *name;
    const char *defaultInputKey;
    PreparedMetricV2 (*prepare)(const std::string &stepName, const Json &inputs,
                                const Json &params, const Json &outputs,
                                std::map<std::string, SlotKind> &knownSlots,
                                const std::string &path);
};

const char *slotKindName(SlotKind kind) noexcept;

std::string requireInputSlot(const Json &inputs, const char *key, SlotKind expected,
                             const std::map<std::string, SlotKind> &knownSlots,
                             const std::string &path);

std::string optionalInputSlot(const Json &inputs, const char *key, SlotKind expected,
                              const std::map<std::string, SlotKind> &knownSlots,
                              const std::string &path);

std::string bindOutputSlot(const Json &outputs, const char *key, SlotKind kind, bool required,
                           std::map<std::string, SlotKind> &knownSlots, const std::string &path);

const FrameBatch &getBatch(const ExecutionContext &ctx, const std::string &slot);
const DoubleMap &getMap(const ExecutionContext &ctx, const std::string &slot);
const ByteMask &getMask(const ExecutionContext &ctx, const std::string &slot);
const IRGjbDriftContext &getDriftContext(const ExecutionContext &ctx, const std::string &slot);

void ensureSameGeometry(IRGeometry a, IRGeometry b, const char *label);
void checkMapSize(const DoubleMap &m, int64_t expectedPixels, IRGeometry g, const char *label);
void checkMaskSize(const ByteMask &m, int64_t expectedPixels, IRGeometry g, const char *label);

void recordScalarOutputs(ExecutionContext &ctx, const std::string &name,
                         const std::vector<MetricOutputV2> &outputs,
                         std::span<const double> values);

inline void recordScalarOutputs(ExecutionContext &ctx, const std::string &name,
                                const std::vector<MetricOutputV2> &outputs,
                                std::initializer_list<double> values)
{
    recordScalarOutputs(ctx, name, outputs, std::span<const double>(values.begin(), values.size()));
}

IRCalcParameters parseCalcParams(const Json &params, const std::string &path);
int32_t parseNoiseSubtractMode(const std::string &text, const std::string &path);
int32_t parseWindowKind(const std::string &text, const std::string &path);
Json normalizeInputsObject(const Json &fields, const char *defaultInputKey,
                           const std::string &method, const std::string &path);

std::span<const MetricSpecV2> basicMetricsV2() noexcept;
std::span<const MetricSpecV2> standardMetricsV2() noexcept;
std::span<const MetricSpecV2> spectralMetricsV2() noexcept;

} // namespace ir::procedures::recipe
