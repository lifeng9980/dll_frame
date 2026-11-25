#include "IR_plugin_cxx.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
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
        if (view.frames < minFrames || view.width <= 0 || view.height <= 0 || !view.data || !view.stamps) {
            writeErr(err, errBytes, "Invalid frames slot view");
            return IR_BAD_ARGUMENT;
        }
        if (view.valid_bits <= 0) {
            writeErr(err, errBytes, "Expected positive valid_bits in IRSlotView");
            return IR_BAD_ARGUMENT;
        }

        const int32_t outW = extractIntParam(paramsJson, "out_width", view.width, ok);
        if (!ok || outW <= 0 || outW > view.width) {
            writeErr(err, errBytes, "Invalid out_width");
            return IR_BAD_ARGUMENT;
        }
        const int32_t outH = extractIntParam(paramsJson, "out_height", view.height, ok);
        if (!ok || outH <= 0 || outH > view.height) {
            writeErr(err, errBytes, "Invalid out_height");
            return IR_BAD_ARGUMENT;
        }

        const size_t nPix = static_cast<size_t>(view.width) * static_cast<size_t>(view.height);
        const auto *pixels = static_cast<const uint16_t *>(view.data);

        double *spanMap = nullptr;
        if (host->has_slot(host->host_ctx, "span_map")) {
            st = host->alloc_double_map(host->host_ctx, "span_map", outW, outH, &spanMap);
            if (st != IR_OK) {
                writeErr(err, errBytes, "Failed allocating span_map");
                return st;
            }
        }

        double globalMin = static_cast<double>(pixels[0]);
        double globalMax = static_cast<double>(pixels[0]);
        for (int32_t r = 0; r < view.height; ++r) {
            for (int32_t c = 0; c < view.width; ++c) {
                const size_t i = static_cast<size_t>(r) * static_cast<size_t>(view.width) + static_cast<size_t>(c);
                uint16_t pMin = pixels[i];
                uint16_t pMax = pixels[i];
                for (int32_t f = 1; f < view.frames; ++f) {
                    const uint16_t v = pixels[static_cast<size_t>(f) * nPix + i];
                    if (v < pMin) pMin = v;
                    if (v > pMax) pMax = v;
                }
                if (static_cast<double>(pMin) < globalMin) globalMin = static_cast<double>(pMin);
                if (static_cast<double>(pMax) > globalMax) globalMax = static_cast<double>(pMax);
                if (spanMap != nullptr && r < outH && c < outW) {
                    spanMap[static_cast<size_t>(r) * static_cast<size_t>(outW) + static_cast<size_t>(c)] =
                        static_cast<double>(pMax - pMin);
                }
            }
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
        st = host->alloc_double_map(host->host_ctx, "big_map", view.width, view.height, &ptr);
        if (st != IR_OK) {
            writeErr(err, errBytes, "Budget limit rejected big_map");
            return st;
        }
        if (host->has_slot(host->host_ctx, "big_map2")) {
            double *ptr2 = nullptr;
            st = host->alloc_double_map(host->host_ctx, "big_map2", view.width * 2, view.height * 2, &ptr2);
            if (st != IR_OK) {
                writeErr(err, errBytes, "Budget limit rejected big_map2");
                return st;
            }
        }
        return host->emit_scalar(host->host_ctx, ".ok", 1.0);
    });
}

int32_t prepInPlaceAdd(const IRPrepareCallbacks *prep, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!prep || !prep->prepare_ctx)
            return IR_NULL_POINTER;
        int32_t bound = 0;
        int32_t st = prep->require_input_slot(prep->prepare_ctx, "in_map", IR_PLUGIN_SLOT_DOUBLE_MAP, 1, &bound);
        if (st != IR_OK) return st;
        st = prep->bind_output_slot(prep->prepare_ctx, "out_map", IR_PLUGIN_SLOT_DOUBLE_MAP, 1, &bound);
        if (st != IR_OK) return st;
        return prep->declare_scalar_output(prep->prepare_ctx, ".mean_out", "DN");
    });
}

int32_t execInPlaceAdd(const IRHostCallbacks *host, IRGeometry, const char *, char *err, int32_t errBytes)
{
    return ir_plugin_guard(err, errBytes, [&]() -> int32_t {
        if (!host || !host->host_ctx || !host->get_slot || !host->alloc_double_map || !host->emit_scalar)
            return IR_NULL_POINTER;
        IRSlotView inView{};
        int32_t st = host->get_slot(host->host_ctx, "in_map", &inView);
        if (st != IR_OK) return st;
        double *outPtr = nullptr;
        st = host->alloc_double_map(host->host_ctx, "out_map", inView.width, inView.height, &outPtr);
        if (st != IR_OK) return st;
        const size_t nPix = static_cast<size_t>(inView.width) * static_cast<size_t>(inView.height);
        const auto *inPtr = static_cast<const double *>(inView.data);
        double sum = 0.0;
        for (size_t i = 0; i < nPix; ++i) {
            outPtr[i] = inPtr[i] + 10.0;
            sum += outPtr[i];
        }
        return host->emit_scalar(host->host_ctx, ".mean_out", sum / static_cast<double>(nPix));
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
        "plugin:test.in_place_add",
        "in_map",
        prepInPlaceAdd,
        execInPlaceAdd
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
