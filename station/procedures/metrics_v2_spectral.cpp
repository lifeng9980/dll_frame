#include "metric_registry_v2.h"
#include <climits>
#include <cmath>
#include <vector>

namespace ir::procedures::recipe {
using namespace ir::api;

namespace {

PreparedMetricV2 prepChannelSpectrum(const std::string &name, const Json &inputs, const Json &params,
                                      const Json &outputs, std::map<std::string, SlotKind> &slots,
                                      const std::string &path)
{
    allowFields(inputs, {"frames"}, path + ".inputs");
    allowFields(params, {"axis", "channel_count", "sample_rate_hz", "window_kind"}, path + ".params");
    allowFields(outputs, {}, path + ".outputs");
    const auto framesSlot = requireInputSlot(inputs, "frames", SlotKind::batch, slots, path);
    const auto axisText = optionalString(params, "axis", "columns", path + ".params");
    require(axisText == "columns" || axisText == "rows", IR_BAD_ARGUMENT,
            path + ".params.axis must be 'columns' or 'rows'");
    const int32_t axis = (axisText == "rows") ? IR_SPECTRUM_AXIS_ROWS : IR_SPECTRUM_AXIS_COLUMNS;
    const int32_t channels = optionalInteger(params, "channel_count", 4, 1, 1024, path + ".params");
    const double fsHz = optionalNumber(params, "sample_rate_hz", 50.0, path + ".params");
    require(fsHz > 0.0, IR_BAD_ARGUMENT, path + ".params.sample_rate_hz must be > 0");
    const int32_t winKind = parseWindowKind(
        optionalString(params, "window_kind", "rectangular", path + ".params"), path + ".params");

    std::vector<MetricOutputV2> outs{
        {".ch0_peak_freq_hz", "Hz", "channel_spectrum", 1},
        {".ch0_peak_amp_dn", "DN", "channel_spectrum", 1},
        {".ch0_temporal_mean_dn", "DN", "channel_spectrum", 1},
        {".ch0_temporal_std_dn", "DN", "channel_spectrum", 1}
    };
    return {1, outs, {
        [=](IRGeometry g) {
            const int32_t axisLen = (axis == IR_SPECTRUM_AXIS_COLUMNS) ? g.width : g.height;
            require(axisLen % channels == 0, IR_BAD_ARGUMENT,
                    "channel_spectrum channel_count must divide image axis length");
        },
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &b = getBatch(ctx, framesSlot);
            const auto g = b.geometry;
            const int32_t frameCount = int32_t(b.stamps.size());
            const int32_t bins = frameCount / 2 + 1;
            std::vector<double> freqs(static_cast<size_t>(bins));
            std::vector<double> amps(static_cast<size_t>(channels) * bins);
            std::vector<double> tMeans(static_cast<size_t>(channels));
            std::vector<double> tStds(static_cast<size_t>(channels));
            char err[1024]{};
            checkStatus(IR_ChannelSpectrumU16(b.pixels.data(), int64_t(b.pixels.size()),
                frameCount, g.height, g.width, axis, channels, fsHz, winKind,
                freqs.data(), bins, nullptr, 0, amps.data(), int64_t(amps.size()),
                tMeans.data(), tStds.data(), channels, err, sizeof err), err);
            int32_t bestK = 0;
            for (int32_t k = 1; k < bins; ++k) {
                if (bestK == 0 || amps[static_cast<size_t>(k)] > amps[static_cast<size_t>(bestK)])
                    bestK = k;
            }
            recordScalarOutputs(ctx, name, outs, {
                freqs[static_cast<size_t>(bestK)],
                amps[static_cast<size_t>(bestK)],
                tMeans[0], tStds[0]
            });
        }
    }};
}

PreparedMetricV2 prepFlickerDetect(const std::string &name, const Json &inputs, const Json &params,
                                   const Json &outputs, std::map<std::string, SlotKind> &slots,
                                   const std::string &path)
{
    allowFields(inputs, {"frames", "k_map", "b_map", "valid_mask"}, path + ".inputs");
    allowFields(params, {"std_dev_multiplier", "exceed_count_threshold"}, path + ".params");
    allowFields(outputs, {"flicker_mask"}, path + ".outputs");
    const auto framesSlot = requireInputSlot(inputs, "frames", SlotKind::batch, slots, path);
    const auto kSlot = optionalInputSlot(inputs, "k_map", SlotKind::double_map, slots, path);
    const auto bSlot = optionalInputSlot(inputs, "b_map", SlotKind::double_map, slots, path);
    require(kSlot.empty() == bSlot.empty(), IR_BAD_ARGUMENT,
            path + ".inputs: k_map and b_map must both be provided or both omitted");
    const auto maskSlot = optionalInputSlot(inputs, "valid_mask", SlotKind::byte_mask, slots, path);
    const double mult = optionalNumber(params, "std_dev_multiplier", 3.0, path + ".params");
    require(std::isfinite(mult) && mult > 0.0, IR_BAD_ARGUMENT,
            path + ".params.std_dev_multiplier must be finite and > 0");
    const int32_t exceedThresh = optionalInteger(params, "exceed_count_threshold", 0, 0, INT32_MAX, path + ".params");
    const auto flickerOut = bindOutputSlot(outputs, "flicker_mask", SlotKind::byte_mask, false, slots, path);

    std::vector<MetricOutputV2> outs{
        {".flicker_count", "count", "flicker_detect", 1},
        {".valid_pixel_count", "count", "flicker_detect", 1},
        {".flicker_ratio_pct", "%", "flicker_detect", 1},
        {".mean_frame_mean_dn", "DN", "flicker_detect", 1},
        {".mean_frame_sigma_dn", "DN", "flicker_detect", 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &b = getBatch(ctx, framesSlot);
            const auto g = b.geometry;
            const auto n = pixels(g.height, g.width);
            const double *kPtr = nullptr;
            const double *bPtr = nullptr;
            const uint8_t *maskPtr = nullptr;
            if (!kSlot.empty()) {
                const auto &km = getMap(ctx, kSlot);
                const auto &bm = getMap(ctx, bSlot);
                checkMapSize(km, n, g, "flicker_detect.k_map");
                checkMapSize(bm, n, g, "flicker_detect.b_map");
                kPtr = km.data.data();
                bPtr = bm.data.data();
            }
            if (!maskSlot.empty()) {
                const auto &vm = getMask(ctx, maskSlot);
                checkMaskSize(vm, n, g, "flicker_detect.valid_mask");
                maskPtr = vm.data.data();
            }
            std::vector<uint8_t> outMask;
            uint8_t *outMaskPtr = nullptr;
            if (!flickerOut.empty()) {
                outMask.resize(static_cast<size_t>(n));
                outMaskPtr = outMask.data();
            }
            IRFlickerSummary summary{};
            char err[1024]{};
            checkStatus(IR_FlickerDetectU16(b.pixels.data(), int64_t(b.pixels.size()),
                int32_t(b.stamps.size()), g.height, g.width,
                kPtr, bPtr, maskPtr, mult, exceedThresh,
                &summary, nullptr, outMaskPtr, n, nullptr, nullptr, 0,
                err, sizeof err), err);
            if (!flickerOut.empty())
                ctx.masks[flickerOut] = {g, std::move(outMask)};
            recordScalarOutputs(ctx, name, outs, {
                static_cast<double>(summary.flicker_count),
                static_cast<double>(summary.valid_pixel_count),
                summary.flicker_ratio_pct,
                summary.mean_frame_mean_dn,
                summary.mean_frame_sigma_dn
            });
        }
    }};
}

PreparedMetricV2 prepFft2d(const std::string &name, const Json &inputs, const Json &params,
                           const Json &outputs, std::map<std::string, SlotKind> &slots,
                           const std::string &path)
{
    allowFields(inputs, {"map", "valid_mask"}, path + ".inputs");
    allowFields(params, {"window", "detrend_mean", "shift_dc_center"}, path + ".params");
    allowFields(outputs, {"magnitude_map", "power_map"}, path + ".outputs");
    const auto mapSlot = requireInputSlot(inputs, "map", SlotKind::double_map, slots, path);
    const auto maskSlot = optionalInputSlot(inputs, "valid_mask", SlotKind::byte_mask, slots, path);
    const auto winText = optionalString(params, "window", "hann", path + ".params");
    require(winText == "rect" || winText == "rectangular" || winText == "hann",
            IR_BAD_ARGUMENT, path + ".params.window must be 'rect', 'rectangular', or 'hann'");
    const int32_t winKind = (winText == "hann") ? IR_WINDOW_HANN : IR_WINDOW_RECTANGULAR;
    const bool detrend = optionalBoolean(params, "detrend_mean", true, path + ".params");
    const bool shiftDc = optionalBoolean(params, "shift_dc_center", true, path + ".params");
    const auto magOut = bindOutputSlot(outputs, "magnitude_map", SlotKind::double_map, false, slots, path);
    const auto pwrOut = bindOutputSlot(outputs, "power_map", SlotKind::double_map, false, slots, path);

    std::vector<MetricOutputV2> outs{
        {".dc_magnitude", "DN", "fft2d", 1},
        {".peak_ac_magnitude", "DN", "fft2d", 1},
        {".total_ac_power", "DN^2", "fft2d", 1},
        {".low_freq_power_ratio", "ratio", "fft2d", 1},
        {".high_freq_power_ratio", "ratio", "fft2d", 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &m = getMap(ctx, mapSlot);
            const auto g = m.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(m, n, g, "fft2d.map");
            const uint8_t *maskPtr = nullptr;
            if (!maskSlot.empty()) {
                const auto &vm = getMask(ctx, maskSlot);
                checkMaskSize(vm, n, g, "fft2d.valid_mask");
                maskPtr = vm.data.data();
            }
            std::vector<double> magMap;
            std::vector<double> pwrMap;
            double *magPtr = nullptr;
            double *pwrPtr = nullptr;
            if (!magOut.empty()) {
                magMap.resize(static_cast<size_t>(n));
                magPtr = magMap.data();
            }
            if (!pwrOut.empty()) {
                pwrMap.resize(static_cast<size_t>(n));
                pwrPtr = pwrMap.data();
            }
            IRFft2dSummary summary{};
            char err[1024]{};
            checkStatus(IR_Fft2dMagnitudeF64(m.data.data(), maskPtr, g.height, g.width, n,
                winKind, detrend ? 1 : 0, shiftDc ? 1 : 0,
                &summary, magPtr, pwrPtr, err, sizeof err), err);
            if (!magOut.empty())
                ctx.maps[magOut] = {g, std::move(magMap)};
            if (!pwrOut.empty())
                ctx.maps[pwrOut] = {g, std::move(pwrMap)};
            recordScalarOutputs(ctx, name, outs, {
                summary.dc_magnitude,
                summary.peak_ac_magnitude,
                summary.total_ac_power,
                summary.low_freq_power_ratio,
                summary.high_freq_power_ratio
            });
        }
    }};
}

constexpr MetricSpecV2 kSpectralMethods[]{
    {"channel_spectrum", "frames", prepChannelSpectrum},
    {"flicker_detect", "frames", prepFlickerDetect},
    {"fft2d", "map", prepFft2d},
};

} // namespace

std::span<const MetricSpecV2> spectralMetricsV2() noexcept
{
    return kSpectralMethods;
}

} // namespace ir::procedures::recipe
