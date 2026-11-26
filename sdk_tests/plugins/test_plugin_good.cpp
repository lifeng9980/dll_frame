#include "IR_plugin_cxx.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

void writeErr(char *err, int32_t errBytes, const char *msg) noexcept
{
    if (!err || errBytes <= 0 || !msg)
        return;
    const size_t n = std::min(static_cast<size_t>(errBytes - 1), std::strlen(msg));
    std::memcpy(err, msg, n);
    err[n] = '\0';
}

int32_t extractIntParam(const char *paramsJson, const char *key, int32_t fallback, bool &ok)
{
    ok = true;
    if (!paramsJson)
        return fallback;
    const std::string text(paramsJson);
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = text.find(needle);
    if (pos == std::string::npos)
        return fallback;
    size_t idx = pos + needle.size();
    while (idx < text.size() && (text[idx] == ' ' || text[idx] == '\t'))
        ++idx;
    try {
        size_t consumed = 0;
        const int32_t val = std::stoi(text.substr(idx), &consumed);
        if (consumed == 0) {
            ok = false;
            return fallback;
        }
        return val;
    } catch (...) {
        ok = false;
        return fallback;
    }
}

int32_t prepPixelSpan(const IRPrepareCallbacks *prep, const char *paramsJson,
                      char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!prep || !prep->prepare_ctx)
            return IR_NULL_POINTER;
        bool ok = true;
        const int32_t minFrames = extractIntParam(paramsJson, "min_frames", 1, ok);
        if (!ok || minFrames < 1) {
            writeErr(err, errBytes, "min_frames must be an integer >= 1");
            return IR_BAD_ARGUMENT;
        }
        int32_t bound = 0;
        int32_t st = prep->require_input_slot(prep->prepare_ctx, "frames", IR_PLUGIN_SLOT_BATCH, 1, &bound);
        if (st != IR_OK)
            return st;
        st = prep->bind_output_slot(prep->prepare_ctx, "span_map", IR_PLUGIN_SLOT_DOUBLE_MAP, 0, &bound);
        if (st != IR_OK)
            return st;
        st = prep->bind_output_slot(prep->prepare_ctx, "span_mask", IR_PLUGIN_SLOT_BYTE_MASK, 0, &bound);
        if (st != IR_OK)
            return st;
        st = prep->declare_scalar_output(prep->prepare_ctx, ".min_dn", "DN");
        if (st != IR_OK)
            return st;
        st = prep->declare_scalar_output(prep->prepare_ctx, ".max_dn", "DN");
        if (st != IR_OK)
            return st;
        return prep->declare_scalar_output(prep->prepare_ctx, ".span_dn", "DN");
    });
}

int32_t execPixelSpan(const IRHostCallbacks *host, IRGeometry geometry,
                      const char *paramsJson, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!host || !host->host_ctx || !host->get_slot || !host->has_slot ||
            !host->alloc_double_map || !host->emit_scalar)
            return IR_NULL_POINTER;
        bool ok = true;
        const int32_t minWidth = extractIntParam(paramsJson, "min_width", 1, ok);
        if (!ok || geometry.width < minWidth || geometry.height < 1) {
            writeErr(err, errBytes, "Image width is smaller than plugin min_width");
            return IR_BAD_ARGUMENT;
        }
        const int32_t minFrames = extractIntParam(paramsJson, "min_frames", 1, ok);
        if (!ok || minFrames < 1) {
            writeErr(err, errBytes, "Invalid min_frames parameter");
            return IR_BAD_ARGUMENT;
        }

        IRSlotView view{};
        int32_t st = host->get_slot(host->host_ctx, "frames", &view);
        if (st != IR_OK) {
            writeErr(err, errBytes, "Failed to read frames slot");
            return st;
        }
        const IRArrayView &array = view.array;
        if (view.slot_kind != IR_PLUGIN_SLOT_BATCH || array.frames < minFrames ||
            array.width <= 0 || array.height <= 0 || !array.data || !view.stamps) {
            writeErr(err, errBytes, "Invalid frames slot view");
            return IR_BAD_ARGUMENT;
        }
        if (view.valid_bits <= 0) {
            writeErr(err, errBytes, "Expected positive valid_bits in IRSlotView");
            return IR_BAD_ARGUMENT;
        }

        const int32_t outW = extractIntParam(paramsJson, "out_width", array.width, ok);
        if (!ok || outW <= 0 || outW > array.width) {
            writeErr(err, errBytes, "Invalid out_width");
            return IR_BAD_ARGUMENT;
        }
        const int32_t outH = extractIntParam(paramsJson, "out_height", array.height, ok);
        if (!ok || outH <= 0 || outH > array.height) {
            writeErr(err, errBytes, "Invalid out_height");
            return IR_BAD_ARGUMENT;
        }

        const size_t elementSize = array.data_type == IR_TYPE_U16 ? sizeof(uint16_t)
            : array.data_type == IR_TYPE_U32 ? sizeof(uint32_t) : 0;
        if (elementSize == 0 || view.valid_bits > static_cast<int32_t>(elementSize * 8)) {
            writeErr(err, errBytes, "Unsupported pixel data type or valid_bits");
            return IR_UNSUPPORTED;
        }
        uint64_t sampleCount = 1;
        for (const int32_t dimension : {array.width, array.height, array.frames}) {
            if (dimension <= 0 || sampleCount > std::numeric_limits<uint64_t>::max() /
                                                   static_cast<uint64_t>(dimension)) {
                writeErr(err, errBytes, "Invalid frame array shape");
                return IR_BAD_ARGUMENT;
            }
            sampleCount *= static_cast<uint64_t>(dimension);
        }
        if (sampleCount > array.byte_size / elementSize) {
            writeErr(err, errBytes, "Frame array byte_size is smaller than its shape");
            return IR_BAD_ARGUMENT;
        }

        double *spanMap = nullptr;
        if (host->has_slot(host->host_ctx, "span_map")) {
            st = host->alloc_double_map(host->host_ctx, "span_map", outW, outH, &spanMap);
            if (st != IR_OK) {
                writeErr(err, errBytes, "Failed allocating span_map");
                return st;
            }
        }
        uint8_t *spanMask = nullptr;
        if (host->has_slot(host->host_ctx, "span_mask")) {
            if (!host->alloc_byte_mask)
                return IR_NULL_POINTER;
            st = host->alloc_byte_mask(host->host_ctx, "span_mask", outW, outH, &spanMask);
            if (st != IR_OK)
                return st;
        }

        double globalMin = std::numeric_limits<double>::infinity();
        double globalMax = -std::numeric_limits<double>::infinity();
        const auto calculate = [&](const auto *pixels) {
            using Pixel = std::remove_cv_t<std::remove_pointer_t<decltype(pixels)>>;
            const size_t nPix = static_cast<size_t>(array.width) * static_cast<size_t>(array.height);
            for (int32_t r = 0; r < array.height; ++r) {
                for (int32_t c = 0; c < array.width; ++c) {
                    const size_t i = static_cast<size_t>(r) * static_cast<size_t>(array.width) +
                                     static_cast<size_t>(c);
                    Pixel pMin = pixels[i];
                    Pixel pMax = pixels[i];
                    for (int32_t f = 1; f < array.frames; ++f) {
                        const Pixel value = pixels[static_cast<size_t>(f) * nPix + i];
                        if (value < pMin) pMin = value;
                        if (value > pMax) pMax = value;
                    }
                    globalMin = std::min(globalMin, static_cast<double>(pMin));
                    globalMax = std::max(globalMax, static_cast<double>(pMax));
                    if (r < outH && c < outW) {
                        const size_t out = static_cast<size_t>(r) * static_cast<size_t>(outW) +
                                           static_cast<size_t>(c);
                        if (spanMap)
                            spanMap[out] = static_cast<double>(pMax) - static_cast<double>(pMin);
                        if (spanMask)
                            spanMask[out] = uint8_t(pMax != pMin);
                    }
                }
            }
        };
        switch (array.data_type) {
        case IR_TYPE_U16: calculate(static_cast<const uint16_t *>(array.data)); break;
        case IR_TYPE_U32: calculate(static_cast<const uint32_t *>(array.data)); break;
        default: return IR_UNSUPPORTED;
        }

        st = host->emit_scalar(host->host_ctx, ".min_dn", globalMin);
        if (st != IR_OK) return st;
        st = host->emit_scalar(host->host_ctx, ".max_dn", globalMax);
        if (st != IR_OK) return st;
        return host->emit_scalar(host->host_ctx, ".span_dn", globalMax - globalMin);
    });
}

int32_t prepSimpleFrames(const IRPrepareCallbacks *prep, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!prep || !prep->prepare_ctx)
            return IR_NULL_POINTER;
        int32_t bound = 0;
        const int32_t st = prep->require_input_slot(prep->prepare_ctx, "frames", IR_PLUGIN_SLOT_BATCH, 1, &bound);
        if (st != IR_OK) return st;
        return prep->declare_scalar_output(prep->prepare_ctx, ".value", "count");
    });
}

int32_t execSlowCancel(const IRHostCallbacks *host, IRGeometry, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!host || !host->host_ctx || !host->is_cancelled || !host->emit_scalar)
            return IR_NULL_POINTER;
        for (int i = 0; i < 500; ++i) {
            if (host->is_cancelled(host->host_ctx)) {
                writeErr(err, errBytes, "Cancelled inside slow plugin");
                return IR_CANCELLED;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return host->emit_scalar(host->host_ctx, ".value", 500.0);
    });
}

int32_t execThrowCxx(const IRHostCallbacks *, IRGeometry, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, []() -> int32_t {
        throw std::runtime_error("intentional plugin C++ exception");
    });
}

int32_t prepOverBudget(const IRPrepareCallbacks *prep, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!prep || !prep->prepare_ctx)
            return IR_NULL_POINTER;
        int32_t bound = 0;
        int32_t st = prep->require_input_slot(prep->prepare_ctx, "frames", IR_PLUGIN_SLOT_BATCH, 1, &bound);
        if (st != IR_OK) return st;
        st = prep->bind_output_slot(prep->prepare_ctx, "big_map", IR_PLUGIN_SLOT_DOUBLE_MAP, 1, &bound);
        if (st != IR_OK) return st;
        st = prep->bind_output_slot(prep->prepare_ctx, "big_map2", IR_PLUGIN_SLOT_DOUBLE_MAP, 0, &bound);
        if (st != IR_OK) return st;
        return prep->declare_scalar_output(prep->prepare_ctx, ".ok", "count");
    });
}

int32_t execOverBudget(const IRHostCallbacks *host, IRGeometry, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!host || !host->host_ctx || !host->get_slot || !host->has_slot ||
            !host->alloc_double_map || !host->emit_scalar)
            return IR_NULL_POINTER;
        IRSlotView view{};
        int32_t st = host->get_slot(host->host_ctx, "frames", &view);
        if (st != IR_OK) return st;
        double *ptr = nullptr;
        st = host->alloc_double_map(host->host_ctx, "big_map", view.array.width, view.array.height, &ptr);
        if (st != IR_OK) {
            writeErr(err, errBytes, "Budget limit rejected big_map");
            return st;
        }
        if (host->has_slot(host->host_ctx, "big_map2")) {
            double *ptr2 = nullptr;
            st = host->alloc_double_map(host->host_ctx, "big_map2", view.array.width * 2, view.array.height * 2, &ptr2);
            if (st != IR_OK) {
                writeErr(err, errBytes, "Budget limit rejected big_map2");
                return st;
            }
        }
        return host->emit_scalar(host->host_ctx, ".ok", 1.0);
    });
}

int32_t prepMapAdd(const IRPrepareCallbacks *prep, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!prep || !prep->prepare_ctx)
            return IR_NULL_POINTER;
        int32_t bound = 0;
        int32_t st = prep->bind_output_slot(prep->prepare_ctx, "out_map", IR_PLUGIN_SLOT_DOUBLE_MAP, 1, &bound);
        if (st != IR_OK) return st;
        st = prep->require_input_slot(prep->prepare_ctx, "in_map", IR_PLUGIN_SLOT_DOUBLE_MAP, 1, &bound);
        if (st != IR_OK) return st;
        return prep->declare_scalar_output(prep->prepare_ctx, ".mean_out", "DN");
    });
}

int32_t execMapAdd(const IRHostCallbacks *host, IRGeometry, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!host || !host->host_ctx || !host->get_slot || !host->alloc_double_map || !host->emit_scalar)
            return IR_NULL_POINTER;
        IRSlotView inView{};
        int32_t st = host->get_slot(host->host_ctx, "in_map", &inView);
        if (st != IR_OK) return st;
        const IRArrayView &input = inView.array;
        if (input.data_type != IR_TYPE_F64 || !input.data || input.width <= 0 || input.height <= 0) {
            writeErr(err, errBytes, "Expected a valid F64 map slot");
            return IR_BAD_ARGUMENT;
        }
        double *outPtr = nullptr;
        st = host->alloc_double_map(host->host_ctx, "out_map", input.width, input.height, &outPtr);
        if (st != IR_OK) return st;
        const size_t nPix = static_cast<size_t>(input.width) * static_cast<size_t>(input.height);
        if (nPix > input.byte_size / sizeof(double)) {
            writeErr(err, errBytes, "Map slot byte_size is too small");
            return IR_BAD_ARGUMENT;
        }
        const auto *inPtr = static_cast<const double *>(input.data);
        double sum = 0.0;
        for (size_t i = 0; i < nPix; ++i) {
            outPtr[i] = inPtr[i] + 10.0;
            sum += outPtr[i];
        }
        return host->emit_scalar(host->host_ctx, ".mean_out", sum / static_cast<double>(nPix));
    });
}

int32_t prepDuplicateAlloc(const IRPrepareCallbacks *prep, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!prep || !prep->prepare_ctx)
            return IR_NULL_POINTER;
        int32_t bound = 0;
        int32_t st = prep->require_input_slot(prep->prepare_ctx, "frames", IR_PLUGIN_SLOT_BATCH, 1, &bound);
        if (st != IR_OK) return st;
        st = prep->bind_output_slot(prep->prepare_ctx, "map", IR_PLUGIN_SLOT_DOUBLE_MAP, 1, &bound);
        if (st != IR_OK) return st;
        return prep->declare_scalar_output(prep->prepare_ctx, ".value", "count");
    });
}

int32_t execDuplicateAlloc(const IRHostCallbacks *host, IRGeometry geometry,
                           const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!host || !host->host_ctx || !host->alloc_double_map || !host->emit_scalar)
            return IR_NULL_POINTER;
        double *first = nullptr;
        int32_t st = host->alloc_double_map(host->host_ctx, "map", geometry.width, geometry.height, &first);
        if (st != IR_OK) return st;
        double *second = nullptr;
        (void)host->alloc_double_map(host->host_ctx, "map", geometry.width, geometry.height, &second);
        return host->emit_scalar(host->host_ctx, ".value", 1.0);
    });
}

int32_t execEmitNan(const IRHostCallbacks *host, IRGeometry, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!host || !host->host_ctx || !host->emit_scalar)
            return IR_NULL_POINTER;
        return host->emit_scalar(host->host_ctx, ".value", std::numeric_limits<double>::quiet_NaN());
    });
}

const IRMetricPlugin kPlugins[]{
    {
        IR_PLUGIN_ABI_VERSION,
        "ir.test.suite",
        "plugin:test.pixel_span",
        "frames",
        prepPixelSpan,
        execPixelSpan
    },
    {
        IR_PLUGIN_ABI_VERSION,
        "ir.test.suite",
        "plugin:test.slow_cancel",
        "frames",
        prepSimpleFrames,
        execSlowCancel
    },
    {
        IR_PLUGIN_ABI_VERSION,
        "ir.test.suite",
        "plugin:test.throw_cxx",
        "frames",
        prepSimpleFrames,
        execThrowCxx
    },
    {
        IR_PLUGIN_ABI_VERSION,
        "ir.test.suite",
        "plugin:test.over_budget",
        "frames",
        prepOverBudget,
        execOverBudget
    },
    {
        IR_PLUGIN_ABI_VERSION,
        "ir.test.suite",
        "plugin:test.map_add",
        "in_map",
        prepMapAdd,
        execMapAdd
    },
    {
        IR_PLUGIN_ABI_VERSION,
        "ir.test.suite",
        "plugin:test.duplicate_alloc",
        "frames",
        prepDuplicateAlloc,
        execDuplicateAlloc
    },
    {
        IR_PLUGIN_ABI_VERSION,
        "ir.test.suite",
        "plugin:test.emit_nan",
        "frames",
        prepSimpleFrames,
        execEmitNan
    }
};

} // namespace

extern "C" IR_PLUGIN_EXPORT int32_t ir_plugin_query(
    const IRMetricPlugin **out_list, uint32_t *out_count)
{
    if (!out_list || !out_count)
        return IR_NULL_POINTER;
    *out_list = kPlugins;
    *out_count = static_cast<uint32_t>(sizeof(kPlugins) / sizeof(kPlugins[0]));
    return IR_OK;
}
