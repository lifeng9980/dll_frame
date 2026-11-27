#include "IR_plugin_cxx.hpp"

#include <cmath>
#include <cstdint>

namespace {

int32_t prepare(const IRPrepareCallbacks *callbacks, const char *, char *err,
                int32_t errBytes) noexcept
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!callbacks || !callbacks->prepare_ctx || !callbacks->require_input_slot ||
            !callbacks->declare_scalar_output)
            return IR_NULL_POINTER;

        int32_t bound = 0;
        int32_t status = callbacks->require_input_slot(
            callbacks->prepare_ctx, "frames", IR_PLUGIN_SLOT_BATCH, 1, &bound);
        if (status != IR_OK)
            return status;
        if (!bound)
            return IR_BAD_ARGUMENT;
        return callbacks->declare_scalar_output(callbacks->prepare_ctx, ".mean_dn", "DN");
    });
}

int32_t execute(const IRHostCallbacks *callbacks, IRGeometry, const char *,
                char *err, int32_t errBytes) noexcept
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!callbacks || !callbacks->host_ctx || !callbacks->get_slot ||
            !callbacks->emit_scalar)
            return IR_NULL_POINTER;

        IRSlotView view{};
        int32_t status = callbacks->get_slot(callbacks->host_ctx, "frames", &view);
        if (status != IR_OK)
            return status;
        if (view.slot_kind != IR_PLUGIN_SLOT_BATCH || !view.array.data ||
            view.array.frames <= 0 || view.array.width <= 0 || view.array.height <= 0)
            return IR_BAD_ARGUMENT;

        const auto width = static_cast<uint64_t>(view.array.width);
        const auto height = static_cast<uint64_t>(view.array.height);
        const auto frames = static_cast<uint64_t>(view.array.frames);
        if (height > UINT64_MAX / width || frames > UINT64_MAX / (width * height))
            return IR_BAD_ARGUMENT;
        const uint64_t count = frames * width * height;
        if (view.array.data_type != IR_TYPE_U16 && view.array.data_type != IR_TYPE_U32)
            return IR_UNSUPPORTED;
        const uint64_t elementBytes = view.array.data_type == IR_TYPE_U16
            ? sizeof(uint16_t) : sizeof(uint32_t);
        if (count > UINT64_MAX / elementBytes ||
            view.array.byte_size < count * elementBytes)
            return IR_BAD_ARGUMENT;

        long double sum = 0.0L;
        if (view.array.data_type == IR_TYPE_U16) {
            const auto *pixels = static_cast<const uint16_t *>(view.array.data);
            for (uint64_t i = 0; i < count; ++i) {
                if ((i & 0xFFFFu) == 0 && callbacks->is_cancelled &&
                    callbacks->is_cancelled(callbacks->host_ctx))
                    return IR_CANCELLED;
                sum += pixels[i];
            }
        } else if (view.array.data_type == IR_TYPE_U32) {
            const auto *pixels = static_cast<const uint32_t *>(view.array.data);
            for (uint64_t i = 0; i < count; ++i) {
                if ((i & 0xFFFFu) == 0 && callbacks->is_cancelled &&
                    callbacks->is_cancelled(callbacks->host_ctx))
                    return IR_CANCELLED;
                sum += pixels[i];
            }
        }

        const double mean = static_cast<double>(sum / count);
        if (!std::isfinite(mean))
            return IR_COMPUTATION_FAILED;
        return callbacks->emit_scalar(callbacks->host_ctx, ".mean_dn", mean);
    });
}

} // namespace

extern "C" IR_PLUGIN_EXPORT int32_t ir_plugin_query(
    const IRMetricPlugin **outList, uint32_t *outCount) noexcept
{
    if (!outList || !outCount)
        return IR_NULL_POINTER;

    static const IRMetricPlugin plugin{
        IR_PLUGIN_ABI_VERSION,
        "example.mean_dn",
        "plugin:example.mean_dn",
        "frames",
        &prepare,
        &execute
    };
    *outList = &plugin;
    *outCount = 1;
    return IR_OK;
}
