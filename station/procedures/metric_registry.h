#pragma once

#include "recipe_config.h"
#include "IR_analysis.h"
#include "api_common.h"
#include <algorithm>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ir::procedures::recipe {

struct PreparedMetric {
    int32_t version{1};
    std::vector<MetricOutput> outputs;
    MetricOperations ops;
};

struct MetricSpec {
    const char *name;
    const char *defaultInputKey;
    PreparedMetric (*prepare)(const std::string &stepName, const Json &inputs,
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
                         const std::vector<MetricOutput> &outputs,
                         std::span<const double> values);

inline void recordScalarOutputs(ExecutionContext &ctx, const std::string &name,
                                const std::vector<MetricOutput> &outputs,
                                std::initializer_list<double> values)
{
    recordScalarOutputs(ctx, name, outputs, std::span<const double>(values.begin(), values.size()));
}

template <typename MapT>
class SlotBufferWriter {
public:
    SlotBufferWriter(std::map<std::string, MapT> &container, const std::string &slotName,
                     IRGeometry g, int64_t pixelCount, bool directReuse, bool zeroFill = false)
        : container_(container), slotName_(slotName), geometry_(g),
          direct_(directReuse && !slotName.empty())
    {
        if (direct_) {
            auto &slot = container_[slotName_];
            slot.geometry = g;
            slot.data.resize(static_cast<size_t>(pixelCount));
            if (zeroFill)
                std::fill(slot.data.begin(), slot.data.end(), typename decltype(slot.data)::value_type{});
            ptr_ = slot.data.data();
        } else {
            scratch_.resize(static_cast<size_t>(pixelCount));
            ptr_ = scratch_.data();
        }
    }

    auto *data() noexcept { return ptr_; }

    void commit()
    {
        if (!direct_ && !slotName_.empty())
            container_[slotName_] = {geometry_, std::move(scratch_)};
    }

private:
    std::map<std::string, MapT> &container_;
    std::string slotName_;
    IRGeometry geometry_{};
    bool direct_{false};
    decltype(MapT::data) scratch_;
    decltype(MapT::data.data()) ptr_{nullptr};
};

IRCalcParameters parseCalcParams(const Json &params, const std::string &path);
int32_t parseNoiseSubtractMode(const std::string &text, const std::string &path);
int32_t parseWindowKind(const std::string &text, const std::string &path);
Json normalizeInputsObject(const Json &fields, const char *defaultInputKey,
                           const std::string &method, const std::string &path);

std::span<const MetricSpec> basicMetrics() noexcept;
std::span<const MetricSpec> standardMetrics() noexcept;

} // namespace ir::procedures::recipe
