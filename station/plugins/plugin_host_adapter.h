#pragma once

#include "IR_plugin_abi.h"
#include "plugin_manager.h"
#include "api_common.h"
#include "station/acquisition/frame_source.h"
#include "station/procedures/metric_registry_v2.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
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
    std::vector<ir::procedures::recipe::MetricOutputV2> scalarOutputs;
};

namespace detail {

inline bool toRecipeSlotKind(int32_t abiKind, ir::procedures::recipe::SlotKind &out) noexcept
{
    using SlotKind = ir::procedures::recipe::SlotKind;
    switch (abiKind) {
    case IR_PLUGIN_SLOT_BATCH:
        out = SlotKind::batch;
        return true;
    case IR_PLUGIN_SLOT_DOUBLE_MAP:
        out = SlotKind::double_map;
        return true;
    case IR_PLUGIN_SLOT_BYTE_MASK:
        out = SlotKind::byte_mask;
        return true;
    case IR_PLUGIN_SLOT_DRIFT_CONTEXT:
        out = SlotKind::drift_context;
        return true;
    default:
        return false;
    }
}

inline int32_t toAbiSlotKind(ir::procedures::recipe::SlotKind kind) noexcept
{
    using SlotKind = ir::procedures::recipe::SlotKind;
    switch (kind) {
    case SlotKind::batch: return IR_PLUGIN_SLOT_BATCH;
    case SlotKind::double_map: return IR_PLUGIN_SLOT_DOUBLE_MAP;
    case SlotKind::byte_mask: return IR_PLUGIN_SLOT_BYTE_MASK;
    case SlotKind::drift_context: return IR_PLUGIN_SLOT_DRIFT_CONTEXT;
    }
    return 0;
}

extern "C" {
static int32_t irPrepRequireInputSlot(void *ctx, const char *inputKey,
                                      int32_t expectedSlotKind, int32_t required,
                                      int32_t *outIsBound) noexcept;
static int32_t irPrepBindOutputSlot(void *ctx, const char *outputKey,
                                    int32_t slotKind, int32_t required,
                                    int32_t *outIsBound) noexcept;
static int32_t irPrepDeclareScalarOutput(void *ctx, const char *suffixUtf8,
                                         const char *unitUtf8) noexcept;

static int32_t irHostHasSlot(void *ctx, const char *ioKey) noexcept;
static int32_t irHostGetSlot(void *ctx, const char *inputKey, IRSlotView *outView) noexcept;
static int32_t irHostAllocDoubleMap(void *ctx, const char *outputKey,
                                    int32_t width, int32_t height, double **outData) noexcept;
static int32_t irHostAllocByteMask(void *ctx, const char *outputKey,
                                   int32_t width, int32_t height, uint8_t **outData) noexcept;
static int32_t irHostEmitScalar(void *ctx, const char *suffixUtf8, double value) noexcept;
static int32_t irHostIsCancelled(void *ctx) noexcept;
static void irHostLogMessage(void *ctx, int32_t level, const char *msgUtf8) noexcept;
}

} // namespace detail

class PrepareCallbackBridge {
public:
    PrepareCallbackBridge(const LoadedMetricPlugin &plugin,
                          const ir::procedures::recipe::Json &inputsJson,
                          const ir::procedures::recipe::Json &outputsJson,
                          std::map<std::string, ir::procedures::recipe::SlotKind> &knownSlots,
                          const std::string &stepPath)
        : plugin_(plugin),
          inputsJson_(inputsJson),
          outputsJson_(outputsJson),
          knownSlots_(knownSlots),
          stepPath_(stepPath)
    {
        vtable_.struct_size = sizeof(IRPrepareCallbacksV1);
        vtable_.prepare_ctx = this;
        vtable_.require_input_slot = &detail::irPrepRequireInputSlot;
        vtable_.bind_output_slot = &detail::irPrepBindOutputSlot;
        vtable_.declare_scalar_output = &detail::irPrepDeclareScalarOutput;
    }

    PluginStepBinding prepare(const ir::procedures::recipe::Json &paramsJson)
    {
        const std::string paramsText = paramsJson.dump();
        char errBuf[1024]{};
        int32_t st = IR_COMPUTATION_FAILED;
        try {
            st = plugin_.descriptor->prepare(
                &vtable_, paramsText.c_str(), errBuf, static_cast<int32_t>(sizeof(errBuf)));
        } catch (...) {
            throw ir::api::Error(IR_COMPUTATION_FAILED,
                                 "C++ exception escaped plugin prepare(): " + plugin_.methodName);
        }
        errBuf[sizeof(errBuf) - 1] = '\0';
        if (st != IR_OK) {
            std::string msg = errBuf[0] != '\0'
                ? std::string(errBuf)
                : (lastError_[0] != '\0' ? std::string(lastError_)
                                         : ("Plugin prepare failed: " + plugin_.methodName));
            throw ir::api::Error(st, msg);
        }
        if (firstCallbackErrorCode_ != IR_OK) {
            std::string msg = lastError_[0] != '\0'
                ? std::string(lastError_)
                : ("Plugin prepare callback failed: " + plugin_.methodName);
            throw ir::api::Error(firstCallbackErrorCode_, msg);
        }
        for (auto it = inputsJson_.begin(); it != inputsJson_.end(); ++it) {
            ir::api::require(queriedInputs_.contains(it.key()), IR_BAD_ARGUMENT,
                             stepPath_ + ".inputs has unknown field: " + it.key());
        }
        for (auto it = outputsJson_.begin(); it != outputsJson_.end(); ++it) {
            ir::api::require(queriedOutputs_.contains(it.key()), IR_BAD_ARGUMENT,
                             stepPath_ + ".outputs has unknown field: " + it.key());
        }
        return std::move(binding_);
    }

    int32_t handleRequireInputSlot(const char *inputKey, int32_t expectedSlotKind,
                                   int32_t required, int32_t *outIsBound) noexcept
    {
        if (!inputKey || inputKey[0] == '\0')
            return recordCallbackError(IR_NULL_POINTER, "Plugin inputKey must not be null or empty");
        if (outIsBound)
            *outIsBound = 0;
        ir::procedures::recipe::SlotKind kind{};
        if (!detail::toRecipeSlotKind(expectedSlotKind, kind))
            return recordCallbackError(IR_BAD_ARGUMENT, "Plugin requested unknown input slot kind");
        try {
            const std::string key(inputKey);
            queriedInputs_.insert(key);
            const std::string slotName = (required != 0)
                ? ir::procedures::recipe::requireInputSlot(
                      inputsJson_, key.c_str(), kind, knownSlots_, stepPath_)
                : ir::procedures::recipe::optionalInputSlot(
                      inputsJson_, key.c_str(), kind, knownSlots_, stepPath_);
            if (!slotName.empty()) {
                binding_.inputs[key] = {slotName, kind};
                if (outIsBound)
                    *outIsBound = 1;
            }
            return IR_OK;
        } catch (const ir::api::Error &e) {
            return recordCallbackError(e.code, e.what());
        } catch (...) {
            return recordCallbackError(IR_COMPUTATION_FAILED, "Internal error validating plugin input slot");
        }
    }

    int32_t handleBindOutputSlot(const char *outputKey, int32_t slotKind,
                                 int32_t required, int32_t *outIsBound) noexcept
    {
        if (!outputKey || outputKey[0] == '\0')
            return recordCallbackError(IR_NULL_POINTER, "Plugin outputKey must not be null or empty");
        if (outIsBound)
            *outIsBound = 0;
        ir::procedures::recipe::SlotKind kind{};
        if (!detail::toRecipeSlotKind(slotKind, kind) ||
            (kind != ir::procedures::recipe::SlotKind::double_map &&
             kind != ir::procedures::recipe::SlotKind::byte_mask)) {
            return recordCallbackError(IR_BAD_ARGUMENT, "Plugin output slots only support double_map or byte_mask");
        }
        try {
            const std::string key(outputKey);
            queriedOutputs_.insert(key);
            const std::string slotName = ir::procedures::recipe::bindOutputSlot(
                outputsJson_, key.c_str(), kind, required != 0, knownSlots_, stepPath_);
            if (!slotName.empty()) {
                if (!boundOutputSlotNames_.insert(slotName).second) {
                    return recordCallbackError(
                        IR_BAD_ARGUMENT,
                        stepPath_ + ".outputs binds duplicate slot name '" + slotName + "'");
                }
                binding_.outputs[key] = {slotName, kind};
                if (outIsBound)
                    *outIsBound = 1;
            }
            return IR_OK;
        } catch (const ir::api::Error &e) {
            return recordCallbackError(e.code, e.what());
        } catch (...) {
            return recordCallbackError(IR_COMPUTATION_FAILED, "Internal error binding plugin output slot");
        }
    }

    int32_t handleDeclareScalarOutput(const char *suffixUtf8, const char *unitUtf8) noexcept
    {
        if (!suffixUtf8 || !unitUtf8)
            return recordCallbackError(IR_NULL_POINTER, "Plugin scalar suffix and unit must not be null");
        try {
            const std::string suffix(suffixUtf8);
            const std::string unit(unitUtf8);
            if (suffix.size() < 2 || suffix[0] != '.' || unit.empty()) {
                return recordCallbackError(
                    IR_BAD_ARGUMENT, "Plugin scalar output requires '.suffix' and non-empty unit");
            }
            if (!declaredSuffixes_.insert(suffix).second) {
                return recordCallbackError(
                    IR_BAD_ARGUMENT, ("Plugin declared duplicate scalar suffix: " + suffix).c_str());
            }
            binding_.scalarOutputs.push_back({
                suffix,
                unit,
                plugin_.methodName,
                plugin_.methodVersion,
                plugin_.pluginId,
                plugin_.pluginVersion,
                plugin_.pluginFile,
                plugin_.pluginSha256
            });
            return IR_OK;
        } catch (const ir::api::Error &e) {
            return recordCallbackError(e.code, e.what());
        } catch (...) {
            return recordCallbackError(IR_COMPUTATION_FAILED, "Internal error declaring plugin scalar output");
        }
    }

private:
    int32_t recordCallbackError(int32_t code, const std::string &msg) noexcept
    {
        if (code != IR_OK && firstCallbackErrorCode_ == IR_OK)
            firstCallbackErrorCode_ = code;
        ir::api::copyText(lastError_, sizeof(lastError_), msg.c_str());
        return code;
    }

    int32_t recordCallbackError(int32_t code, const char *msg) noexcept
    {
        if (code != IR_OK && firstCallbackErrorCode_ == IR_OK)
            firstCallbackErrorCode_ = code;
        ir::api::copyText(lastError_, sizeof(lastError_), msg ? msg : "");
        return code;
    }

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
                       ir::procedures::recipe::AnalysisRunContext &runCtx)
        : plugin_(plugin),
          binding_(binding),
          stepName_(stepName),
          runCtx_(runCtx)
    {
        vtable_.struct_size = sizeof(IRHostCallbacksV1);
        vtable_.host_ctx = this;
        vtable_.has_slot = &detail::irHostHasSlot;
        vtable_.get_slot = &detail::irHostGetSlot;
        vtable_.alloc_double_map = &detail::irHostAllocDoubleMap;
        vtable_.alloc_byte_mask = &detail::irHostAllocByteMask;
        vtable_.emit_scalar = &detail::irHostEmitScalar;
        vtable_.is_cancelled = &detail::irHostIsCancelled;
        vtable_.log_message = &detail::irHostLogMessage;
    }

    void execute(const std::string &paramsText)
    {
        char errBuf[1024]{};
        int32_t st = IR_COMPUTATION_FAILED;
        try {
            st = plugin_.descriptor->execute(
                &vtable_, paramsText.c_str(), errBuf, static_cast<int32_t>(sizeof(errBuf)));
        } catch (...) {
            throw ir::api::Error(IR_COMPUTATION_FAILED,
                                 "C++ exception escaped plugin execute(): " + plugin_.methodName);
        }
        errBuf[sizeof(errBuf) - 1] = '\0';
        if (st == IR_CANCELLED || runCtx_.capture.isStopRequested()) {
            const std::string msg = errBuf[0] != '\0'
                ? std::string(errBuf)
                : "Measurement or acquisition cancelled";
            throw ir::api::Error(IR_CANCELLED, msg);
        }
        if (st != IR_OK) {
            std::string msg = errBuf[0] != '\0'
                ? std::string(errBuf)
                : (lastError_[0] != '\0' ? std::string(lastError_)
                                         : ("Plugin execution failed (" + plugin_.methodName +
                                            ", status=" + std::to_string(st) + ")"));
            throw ir::api::Error(st, msg);
        }

        for (const auto &[outKey, slotPair] : binding_.outputs) {
            const auto &[slotName, slotKind] = slotPair;
            if (slotKind == ir::procedures::recipe::SlotKind::double_map) {
                ir::api::require(stagedMaps_.contains(slotName), IR_COMPUTATION_FAILED,
                                 "Plugin did not allocate bound output double_map: " + outKey);
            } else if (slotKind == ir::procedures::recipe::SlotKind::byte_mask) {
                ir::api::require(stagedMasks_.contains(slotName), IR_COMPUTATION_FAILED,
                                 "Plugin did not allocate bound output byte_mask: " + outKey);
            }
        }

        std::vector<double> orderedValues;
        orderedValues.reserve(binding_.scalarOutputs.size());
        for (const auto &out : binding_.scalarOutputs) {
            const auto it = emittedScalars_.find(out.suffix);
            ir::api::require(it != emittedScalars_.end(), IR_COMPUTATION_FAILED,
                             "Plugin did not emit declared scalar output: " + out.suffix);
            orderedValues.push_back(it->second);
        }

        for (auto &[slotName, mapVal] : stagedMaps_)
            runCtx_.ctx.maps[slotName] = std::move(mapVal);
        for (auto &[slotName, maskVal] : stagedMasks_)
            runCtx_.ctx.masks[slotName] = std::move(maskVal);

        ir::procedures::recipe::recordScalarOutputs(
            runCtx_.ctx, stepName_, binding_.scalarOutputs, orderedValues);
    }

    int32_t handleHasSlot(const char *ioKey) noexcept
    {
        if (!ioKey || ioKey[0] == '\0')
            return 0;
        const std::string_view key(ioKey);
        return (binding_.inputs.contains(key) || binding_.outputs.contains(key)) ? 1 : 0;
    }

    int32_t handleGetSlot(const char *inputKey, IRSlotView *outView) noexcept
    {
        if (!inputKey || !outView)
            return IR_NULL_POINTER;
        try {
            const auto bindIt = binding_.inputs.find(std::string_view(inputKey));
            if (bindIt == binding_.inputs.end()) {
                setLastError(("Unbound plugin input key: " + std::string(inputKey)).c_str());
                return IR_BAD_ARGUMENT;
            }
            const auto &[slotName, slotKind] = bindIt->second;
            *outView = IRSlotView{};
            outView->struct_size = sizeof(IRSlotView);
            outView->slot_kind = detail::toAbiSlotKind(slotKind);
            using SlotKind = ir::procedures::recipe::SlotKind;
            switch (slotKind) {
            case SlotKind::batch: {
                const auto &b = ir::procedures::recipe::getBatch(runCtx_.ctx, slotName);
                outView->width = b.geometry.width;
                outView->height = b.geometry.height;
                outView->frames = static_cast<int32_t>(b.stamps.size());
                outView->valid_bits = b.geometry.valid_bits;
                outView->data = b.pixels.data();
                outView->byte_size = b.pixels.size() * sizeof(uint16_t);
                outView->stamps = b.stamps.data();
                break;
            }
            case SlotKind::double_map: {
                const auto &m = ir::procedures::recipe::getMap(runCtx_.ctx, slotName);
                outView->width = m.geometry.width;
                outView->height = m.geometry.height;
                outView->frames = 1;
                outView->valid_bits = m.geometry.valid_bits;
                outView->data = m.data.data();
                outView->byte_size = m.data.size() * sizeof(double);
                break;
            }
            case SlotKind::byte_mask: {
                const auto &m = ir::procedures::recipe::getMask(runCtx_.ctx, slotName);
                outView->width = m.geometry.width;
                outView->height = m.geometry.height;
                outView->frames = 1;
                outView->valid_bits = m.geometry.valid_bits;
                outView->data = m.data.data();
                outView->byte_size = m.data.size() * sizeof(uint8_t);
                break;
            }
            case SlotKind::drift_context: {
                const auto &d = ir::procedures::recipe::getDriftContext(runCtx_.ctx, slotName);
                outView->frames = 1;
                outView->data = &d;
                outView->byte_size = sizeof(IRGjbDriftContext);
                break;
            }
            }
            return IR_OK;
        } catch (const ir::api::Error &e) {
            setLastError(e.what());
            return e.code;
        } catch (...) {
            setLastError("Failed reading runtime slot");
            return IR_COMPUTATION_FAILED;
        }
    }

    int32_t handleAllocDoubleMap(const char *outputKey, int32_t width, int32_t height, double **outData) noexcept
    {
        if (!outputKey || !outData)
            return IR_NULL_POINTER;
        *outData = nullptr;
        try {
            const auto bindIt = binding_.outputs.find(std::string_view(outputKey));
            if (bindIt == binding_.outputs.end() ||
                bindIt->second.second != ir::procedures::recipe::SlotKind::double_map) {
                setLastError(("Unbound double_map output key: " + std::string(outputKey)).c_str());
                return IR_BAD_ARGUMENT;
            }
            const auto geom = runCtx_.ctx.geometry;
            if (width <= 0 || height <= 0 || width != geom.width || height != geom.height) {
                setLastError("Plugin output double_map dimensions do not match frame geometry");
                return IR_BAD_ARGUMENT;
            }
            const uint64_t pixels = static_cast<uint64_t>(ir::api::pixels(height, width));
            const uint64_t requestedBytes = ir::procedures::recipe::multiplyBytes(pixels, sizeof(double));
            if (pixels > std::numeric_limits<size_t>::max() / sizeof(double)) {
                setLastError("Plugin output double_map size overflow");
                return IR_BAD_ARGUMENT;
            }
            const std::string &slotName = bindIt->second.first;
            uint64_t existingStagedBytes = 0;
            if (const auto it = stagedMaps_.find(slotName); it != stagedMaps_.end()) {
                existingStagedBytes = ir::procedures::recipe::multiplyBytes(
                    it->second.data.capacity(), sizeof(double));
            }
            const uint64_t deltaBytes = (requestedBytes > existingStagedBytes)
                ? (requestedBytes - existingStagedBytes)
                : 0;
            if (!withinInflightBudget(deltaBytes))
                return IR_BAD_ARGUMENT;

            auto &slot = stagedMaps_[slotName];
            slot.geometry = geom;
            slot.data.assign(static_cast<size_t>(pixels), 0.0);
            *outData = slot.data.data();
            return IR_OK;
        } catch (const ir::api::Error &e) {
            setLastError(e.what());
            return e.code;
        } catch (...) {
            setLastError("Failed allocating plugin double_map");
            return IR_COMPUTATION_FAILED;
        }
    }

    int32_t handleAllocByteMask(const char *outputKey, int32_t width, int32_t height, uint8_t **outData) noexcept
    {
        if (!outputKey || !outData)
            return IR_NULL_POINTER;
        *outData = nullptr;
        try {
            const auto bindIt = binding_.outputs.find(std::string_view(outputKey));
            if (bindIt == binding_.outputs.end() ||
                bindIt->second.second != ir::procedures::recipe::SlotKind::byte_mask) {
                setLastError(("Unbound byte_mask output key: " + std::string(outputKey)).c_str());
                return IR_BAD_ARGUMENT;
            }
            const auto geom = runCtx_.ctx.geometry;
            if (width <= 0 || height <= 0 || width != geom.width || height != geom.height) {
                setLastError("Plugin output byte_mask dimensions do not match frame geometry");
                return IR_BAD_ARGUMENT;
            }
            const uint64_t pixels = static_cast<uint64_t>(ir::api::pixels(height, width));
            if (pixels > std::numeric_limits<size_t>::max()) {
                setLastError("Plugin output byte_mask size overflow");
                return IR_BAD_ARGUMENT;
            }
            const std::string &slotName = bindIt->second.first;
            uint64_t existingStagedBytes = 0;
            if (const auto it = stagedMasks_.find(slotName); it != stagedMasks_.end())
                existingStagedBytes = static_cast<uint64_t>(it->second.data.capacity());
            const uint64_t deltaBytes = (pixels > existingStagedBytes) ? (pixels - existingStagedBytes) : 0;
            if (!withinInflightBudget(deltaBytes))
                return IR_BAD_ARGUMENT;

            auto &slot = stagedMasks_[slotName];
            slot.geometry = geom;
            slot.data.assign(static_cast<size_t>(pixels), 0);
            *outData = slot.data.data();
            return IR_OK;
        } catch (const ir::api::Error &e) {
            setLastError(e.what());
            return e.code;
        } catch (...) {
            setLastError("Failed allocating plugin byte_mask");
            return IR_COMPUTATION_FAILED;
        }
    }

    int32_t handleEmitScalar(const char *suffixUtf8, double value) noexcept
    {
        if (!suffixUtf8)
            return IR_NULL_POINTER;
        if (!std::isfinite(value)) {
            setLastError("Plugin emitted non-finite scalar value");
            return IR_COMPUTATION_FAILED;
        }
        try {
            const std::string suffix(suffixUtf8);
            bool declared = false;
            for (const auto &out : binding_.scalarOutputs) {
                if (out.suffix == suffix) {
                    declared = true;
                    break;
                }
            }
            if (!declared) {
                setLastError(("Plugin emitted undeclared scalar suffix: " + suffix).c_str());
                return IR_BAD_ARGUMENT;
            }
            if (!emittedScalars_.emplace(suffix, value).second) {
                setLastError(("Plugin emitted scalar suffix multiple times: " + suffix).c_str());
                return IR_BAD_ARGUMENT;
            }
            return IR_OK;
        } catch (...) {
            setLastError("Failed recording plugin scalar output");
            return IR_COMPUTATION_FAILED;
        }
    }

    int32_t handleIsCancelled() const noexcept
    {
        return runCtx_.capture.isStopRequested() ? 1 : 0;
    }

private:
    void setLastError(const char *msg) noexcept
    {
        ir::api::copyText(lastError_, sizeof(lastError_), msg ? msg : "");
    }

    uint64_t stagedBytes() const
    {
        uint64_t total = 0;
        for (const auto &[_, m] : stagedMaps_) {
            total = ir::procedures::recipe::addBytes(
                total, ir::procedures::recipe::multiplyBytes(m.data.capacity(), sizeof(double)));
        }
        for (const auto &[_, m] : stagedMasks_)
            total = ir::procedures::recipe::addBytes(total, static_cast<uint64_t>(m.data.capacity()));
        return total;
    }

    bool withinInflightBudget(uint64_t deltaBytes) noexcept
    {
        if (runCtx_.maxInflightBytes == 0)
            return true;
        try {
            const uint64_t used = ir::procedures::recipe::addBytes(
                ir::procedures::recipe::addBytes(runCtx_.inflightBaseBytes, runCtx_.ctx.retainedBytes()),
                stagedBytes());
            const uint64_t nextTotal = ir::procedures::recipe::addBytes(used, deltaBytes);
            ir::procedures::recipe::requireWithinMemoryLimit(nextTotal, runCtx_.maxInflightBytes);
            return true;
        } catch (const ir::api::Error &e) {
            setLastError(e.what());
            return false;
        } catch (...) {
            setLastError("Failed evaluating plugin memory budget");
            return false;
        }
    }

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

namespace detail {
extern "C" {
static int32_t irPrepRequireInputSlot(void *ctx, const char *inputKey,
                                      int32_t expectedSlotKind, int32_t required,
                                      int32_t *outIsBound) noexcept
{
    auto *self = static_cast<PrepareCallbackBridge *>(ctx);
    return self ? self->handleRequireInputSlot(inputKey, expectedSlotKind, required, outIsBound) : IR_NULL_POINTER;
}

static int32_t irPrepBindOutputSlot(void *ctx, const char *outputKey,
                                    int32_t slotKind, int32_t required,
                                    int32_t *outIsBound) noexcept
{
    auto *self = static_cast<PrepareCallbackBridge *>(ctx);
    return self ? self->handleBindOutputSlot(outputKey, slotKind, required, outIsBound) : IR_NULL_POINTER;
}

static int32_t irPrepDeclareScalarOutput(void *ctx, const char *suffixUtf8,
                                         const char *unitUtf8) noexcept
{
    auto *self = static_cast<PrepareCallbackBridge *>(ctx);
    return self ? self->handleDeclareScalarOutput(suffixUtf8, unitUtf8) : IR_NULL_POINTER;
}

static int32_t irHostHasSlot(void *ctx, const char *ioKey) noexcept
{
    auto *self = static_cast<HostCallbackBridge *>(ctx);
    return self ? self->handleHasSlot(ioKey) : 0;
}

static int32_t irHostGetSlot(void *ctx, const char *inputKey, IRSlotView *outView) noexcept
{
    auto *self = static_cast<HostCallbackBridge *>(ctx);
    return self ? self->handleGetSlot(inputKey, outView) : IR_NULL_POINTER;
}

static int32_t irHostAllocDoubleMap(void *ctx, const char *outputKey,
                                    int32_t width, int32_t height, double **outData) noexcept
{
    auto *self = static_cast<HostCallbackBridge *>(ctx);
    return self ? self->handleAllocDoubleMap(outputKey, width, height, outData) : IR_NULL_POINTER;
}

static int32_t irHostAllocByteMask(void *ctx, const char *outputKey,
                                   int32_t width, int32_t height, uint8_t **outData) noexcept
{
    auto *self = static_cast<HostCallbackBridge *>(ctx);
    return self ? self->handleAllocByteMask(outputKey, width, height, outData) : IR_NULL_POINTER;
}

static int32_t irHostEmitScalar(void *ctx, const char *suffixUtf8, double value) noexcept
{
    auto *self = static_cast<HostCallbackBridge *>(ctx);
    return self ? self->handleEmitScalar(suffixUtf8, value) : IR_NULL_POINTER;
}

static int32_t irHostIsCancelled(void *ctx) noexcept
{
    auto *self = static_cast<HostCallbackBridge *>(ctx);
    return self ? self->handleIsCancelled() : 0;
}

static void irHostLogMessage(void *, int32_t, const char *) noexcept
{
    // Reserved hook for future structured host logging; safe no-op.
}
}
} // namespace detail

} // namespace ir::plugins
