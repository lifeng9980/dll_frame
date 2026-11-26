#include "metric_registry.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <optional>
#include <vector>

namespace ir::procedures::recipe {
using namespace ir::api;

namespace {

PreparedMetric prepGbEvaluate(const std::string &name, const Json &inputs, const Json &params,
                                const Json &outputs, std::map<std::string, SlotKind> &slots,
                                const std::string &path)
{
    allowFields(inputs, {"v1_map", "v2_map", "vn_map"}, path + ".inputs");
    allowFields(params, {
        "t0_c", "t1_c", "pixel_pitch_um", "emissivity", "system_gain", "adc_swing_v",
        "f_number", "vmax_per_count", "integration_time_us", "invalid_integration_us", "digital_full",
        "response_upper", "response_lower", "noise_upper", "netd_upper",
        "short_bad_threshold", "open_bad_threshold", "cycle_upper"
    }, path + ".params");
    allowFields(outputs, {"valid_mask", "total_bad_mask"}, path + ".outputs");
    const auto v1Slot = requireInputSlot(inputs, "v1_map", SlotKind::double_map, slots, path);
    const auto v2Slot = requireInputSlot(inputs, "v2_map", SlotKind::double_map, slots, path);
    const auto vnSlot = requireInputSlot(inputs, "vn_map", SlotKind::double_map, slots, path);
    const IRCalcParameters cp = parseCalcParams(params, path + ".params");
    IRGbCriteria crit{};
    crit.response_upper = optionalNumber(params, "response_upper", 1.5, path + ".params");
    crit.response_lower = optionalNumber(params, "response_lower", 0.5, path + ".params");
    crit.noise_upper = optionalNumber(params, "noise_upper", 1.5, path + ".params");
    crit.netd_upper = optionalNumber(params, "netd_upper", 2.0, path + ".params");
    crit.short_bad_threshold = optionalNumber(params, "short_bad_threshold", 0.0, path + ".params");
    crit.open_bad_threshold = optionalNumber(params, "open_bad_threshold", 0.0, path + ".params");
    crit.cycle_upper = optionalInteger(params, "cycle_upper", 3, 1, 1000, path + ".params");

    const auto validOut = bindOutputSlot(outputs, "valid_mask", SlotKind::byte_mask, false, slots, path);
    const auto badOut = bindOutputSlot(outputs, "total_bad_mask", SlotKind::byte_mask, false, slots, path);

    std::vector<MetricOutput> outs{
        {".before_avg_snr", "ratio", "gb_evaluate", 1},
        {".after_avg_netd_mk", "mK", "gb_evaluate", 1},
        {".after_response_nonuniformity_pct", "%", "gb_evaluate", 1},
        {".total_bad_count", "count", "gb_evaluate", 1},
        {".total_bad_rate_pct", "%", "gb_evaluate", 1},
        {".chip_grade", "grade", "gb_evaluate", 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &v1 = getMap(ctx, v1Slot);
            const auto &v2 = getMap(ctx, v2Slot);
            const auto &vn = getMap(ctx, vnSlot);
            const auto g = v1.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(v1, n, g, "gb_evaluate.v1_map");
            checkMapSize(v2, n, g, "gb_evaluate.v2_map");
            checkMapSize(vn, n, g, "gb_evaluate.vn_map");
            std::optional<SlotBufferWriter<ByteMask>> validWriter, badWriter;
            IRGbBadPixelMasks masks{};
            masks.pixel_capacity = n;
            if (!validOut.empty()) {
                validWriter.emplace(ctx.masks, validOut, g, n, validOut != badOut, true);
                masks.valid_mask = validWriter->data();
            }
            if (!badOut.empty()) {
                badWriter.emplace(ctx.masks, badOut, g, n, badOut != validOut, true);
                masks.total_bad_mask = badWriter->data();
            }
            IRGbSummary gb{};
            char err[1024]{};
            checkStatus(IR_GbEvaluateMaps(v1.data.data(), v2.data.data(), vn.data.data(),
                g.height, g.width, n, &cp, &crit, &gb,
                (!validOut.empty() || !badOut.empty()) ? &masks : nullptr, err, sizeof err), err);
            if (validWriter) validWriter->commit();
            if (badWriter) badWriter->commit();
            recordScalarOutputs(ctx, name, outs, {
                gb.before_avg_snr, gb.after_avg_netd_mk, gb.after_response_nonuniformity_pct,
                double(gb.bad_pixels.total_bad_count), gb.bad_pixels.total_bad_rate_pct,
                double(gb.bad_pixels.chip_grade)
            });
        }
    }};
}

PreparedMetric prepGjbEvaluate(const std::string &name, const Json &inputs, const Json &params,
                                 const Json &outputs, std::map<std::string, SlotKind> &slots,
                                 const std::string &path)
{
    allowFields(inputs, {"v1_map", "v2_map", "vn_map", "vm_map"}, path + ".inputs");
    allowFields(params, {
        "t0_c", "t1_c", "pixel_pitch_um", "emissivity", "system_gain", "adc_swing_v",
        "f_number", "vmax_per_count", "integration_time_us", "invalid_integration_us", "digital_full",
        "k1", "k2", "k3", "k4", "cycle_upper",
        "burst_frame_count", "lrnu_kernel_size", "noise_subtract_mode", "noise_subtract_factor"
    }, path + ".params");
    allowFields(outputs, {"valid_mask", "total_bad_mask", "drift_ctx"}, path + ".outputs");
    const auto v1Slot = requireInputSlot(inputs, "v1_map", SlotKind::double_map, slots, path);
    const auto v2Slot = requireInputSlot(inputs, "v2_map", SlotKind::double_map, slots, path);
    const auto vnSlot = requireInputSlot(inputs, "vn_map", SlotKind::double_map, slots, path);
    const auto vmSlot = optionalInputSlot(inputs, "vm_map", SlotKind::double_map, slots, path);
    const IRCalcParameters cp = parseCalcParams(params, path + ".params");
    IRGjbCriteria crit{};
    crit.k1 = optionalNumber(params, "k1", 1.5, path + ".params");
    crit.k2 = optionalNumber(params, "k2", 1.5, path + ".params");
    crit.k3 = optionalNumber(params, "k3", 0.4, path + ".params");
    crit.k4 = optionalNumber(params, "k4", 0.4, path + ".params");
    crit.cycle_upper = optionalInteger(params, "cycle_upper", 2, 1, 1000, path + ".params");
    const int32_t burstFrames = optionalInteger(params, "burst_frame_count", 8, 1, INT32_MAX, path + ".params");
    const int32_t lrnuKernel = optionalInteger(params, "lrnu_kernel_size", 15, 0, 1025, path + ".params");
    require(lrnuKernel <= 1 || (lrnuKernel >= 3 && (lrnuKernel % 2) == 1),
            IR_BAD_ARGUMENT, path + ".params.lrnu_kernel_size must be <= 1 or odd >= 3");
    const int32_t noiseSubMode = parseNoiseSubtractMode(
        optionalString(params, "noise_subtract_mode", "quadrature", path + ".params"), path + ".params");
    const double noiseSubFactor = optionalNumber(params, "noise_subtract_factor", 1.0, path + ".params");

    const auto validOut = bindOutputSlot(outputs, "valid_mask", SlotKind::byte_mask, false, slots, path);
    const auto badOut = bindOutputSlot(outputs, "total_bad_mask", SlotKind::byte_mask, false, slots, path);
    const auto ctxOut = bindOutputSlot(outputs, "drift_ctx", SlotKind::drift_context, false, slots, path);

    std::vector<MetricOutput> outs{
        {".before_avg_snr", "ratio", "gjb_evaluate", 1},
        {".after_avg_netd_mk", "mK", "gjb_evaluate", 1},
        {".after_response_nonuniformity_pct", "%", "gjb_evaluate", 1},
        {".after_rfpn_v", "V", "gjb_evaluate", 1},
        {".total_bad_count", "count", "gjb_evaluate", 1},
        {".total_bad_rate_pct", "%", "gjb_evaluate", 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &v1 = getMap(ctx, v1Slot);
            const auto &v2 = getMap(ctx, v2Slot);
            const auto &vn = getMap(ctx, vnSlot);
            const auto &vm = vmSlot.empty() ? v2 : getMap(ctx, vmSlot);
            const auto g = v1.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(v1, n, g, "gjb_evaluate.v1_map");
            checkMapSize(v2, n, g, "gjb_evaluate.v2_map");
            checkMapSize(vn, n, g, "gjb_evaluate.vn_map");
            checkMapSize(vm, n, g, "gjb_evaluate.vm_map");

            std::optional<SlotBufferWriter<ByteMask>> validWriter, badWriter;
            IRGjbBadPixelMasks masks{};
            masks.pixel_capacity = n;
            if (!validOut.empty()) {
                validWriter.emplace(ctx.masks, validOut, g, n, validOut != badOut, true);
                masks.valid_mask = validWriter->data();
            }
            if (!badOut.empty()) {
                badWriter.emplace(ctx.masks, badOut, g, n, badOut != validOut, true);
                masks.total_bad_mask = badWriter->data();
            }
            IRGjbSummary gjb{};
            char err[1024]{};
            checkStatus(IR_GjbEvaluateMaps(v1.data.data(), v2.data.data(), vn.data.data(), vm.data.data(),
                g.height, g.width, n, &cp, &crit, &gjb,
                (!validOut.empty() || !badOut.empty()) ? &masks : nullptr, err, sizeof err), err);
            if (validWriter) validWriter->commit();
            if (badWriter) badWriter->commit();
            if (!ctxOut.empty()) {
                IRGjbDriftContext dctx{};
                dctx.volts_per_count = gjb.volts_per_count;
                dctx.delta_t_k = std::max(1e-9, std::abs(cp.t1_c - cp.t0_c));
                dctx.ref_response_count = gjb.before_avg_pixel_response_count;
                dctx.ref_responsivity = gjb.after_avg_responsivity;
                dctx.vmax_per_count = cp.vmax_per_count;
                dctx.noise_subtract_factor = noiseSubFactor;
                dctx.burst_frame_count = burstFrames;
                dctx.noise_subtract_mode = noiseSubMode;
                dctx.lrnu_kernel_size = lrnuKernel;
                ctx.driftContexts[ctxOut] = dctx;
            }
            recordScalarOutputs(ctx, name, outs, {
                gjb.before_avg_snr, gjb.after_avg_netd_mk, gjb.after_response_nonuniformity_pct,
                gjb.after_rfpn_v, double(gjb.bad_pixels.total_bad_count), gjb.bad_pixels.total_bad_rate_pct
            });
        }
    }};
}

PreparedMetric prepGjbBurst(const std::string &name, const Json &inputs, const Json &params,
                              const Json &outputs, std::map<std::string, SlotKind> &slots,
                              const std::string &path)
{
    allowFields(inputs, {"mean_map", "vn_map", "valid_mask", "drift_ctx"}, path + ".inputs");
    allowFields(params, {
        "volts_per_count", "delta_t_k", "ref_response_count", "ref_responsivity", "vmax_per_count",
        "noise_subtract_factor", "burst_frame_count", "noise_subtract_mode", "lrnu_kernel_size"
    }, path + ".params");
    allowFields(outputs, {}, path + ".outputs");
    const auto meanSlot = requireInputSlot(inputs, "mean_map", SlotKind::double_map, slots, path);
    const auto vnSlot = requireInputSlot(inputs, "vn_map", SlotKind::double_map, slots, path);
    const auto maskSlot = optionalInputSlot(inputs, "valid_mask", SlotKind::byte_mask, slots, path);
    const auto ctxSlot = optionalInputSlot(inputs, "drift_ctx", SlotKind::drift_context, slots, path);

    if (ctxSlot.empty()) {
        for (const char *reqKey : {"volts_per_count", "delta_t_k", "ref_response_count",
                                   "ref_responsivity", "vmax_per_count", "burst_frame_count"}) {
            require(params.contains(reqKey), IR_BAD_ARGUMENT,
                    path + ".params." + reqKey + " is required when drift_ctx input is omitted");
        }
    }
    if (params.contains("noise_subtract_mode")) {
        (void)parseNoiseSubtractMode(stringField(params, "noise_subtract_mode", path + ".params"),
                                     path + ".params");
    }
    if (params.contains("lrnu_kernel_size")) {
        const int k = integerField(params, "lrnu_kernel_size", 0, 1025, path + ".params");
        require(k <= 1 || (k >= 3 && (k % 2) == 1), IR_BAD_ARGUMENT,
                path + ".params.lrnu_kernel_size must be <= 1 or odd >= 3");
    }
    const Json capturedParams = params;

    std::vector<MetricOutput> outs{
        {".mean_dn", "DN", "gjb_burst", 1},
        {".span_dn", "DN", "gjb_burst", 1},
        {".spatial_std_dn", "DN", "gjb_burst", 1},
        {".spatial_std_denoised_dn", "DN", "gjb_burst", 1},
        {".nonuniformity_pct", "%", "gjb_burst", 1},
        {".avg_temporal_noise_dn", "DN", "gjb_burst", 1},
        {".estimated_netd_mk", "mK", "gjb_burst", 1},
        {".lrnu_std_dn", "DN", "gjb_burst", 1},
        {".lrnu_pct", "%", "gjb_burst", 1},
        {".hrnu_std_dn", "DN", "gjb_burst", 1},
        {".hrnu_pct", "%", "gjb_burst", 1},
        {".valid_pixel_count", "count", "gjb_burst", 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &meanMap = getMap(ctx, meanSlot);
            const auto &vnMap = getMap(ctx, vnSlot);
            const auto g = meanMap.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(meanMap, n, g, "gjb_burst.mean_map");
            checkMapSize(vnMap, n, g, "gjb_burst.vn_map");
            const uint8_t *maskPtr = nullptr;
            if (!maskSlot.empty()) {
                const auto &vm = getMask(ctx, maskSlot);
                checkMaskSize(vm, n, g, "gjb_burst.valid_mask");
                maskPtr = vm.data.data();
            }

            IRGjbDriftContext dctx = ctxSlot.empty() ? IRGjbDriftContext{} : getDriftContext(ctx, ctxSlot);
            const std::string pPath = "params";
            dctx.volts_per_count = optionalNumber(capturedParams, "volts_per_count", dctx.volts_per_count, pPath);
            dctx.delta_t_k = optionalNumber(capturedParams, "delta_t_k", dctx.delta_t_k, pPath);
            dctx.ref_response_count = optionalNumber(capturedParams, "ref_response_count", dctx.ref_response_count, pPath);
            dctx.ref_responsivity = optionalNumber(capturedParams, "ref_responsivity", dctx.ref_responsivity, pPath);
            dctx.vmax_per_count = optionalNumber(capturedParams, "vmax_per_count", dctx.vmax_per_count, pPath);
            dctx.noise_subtract_factor = optionalNumber(capturedParams, "noise_subtract_factor",
                ctxSlot.empty() ? 1.0 : dctx.noise_subtract_factor, pPath);
            dctx.burst_frame_count = optionalInteger(capturedParams, "burst_frame_count",
                ctxSlot.empty() ? 8 : dctx.burst_frame_count, 1, INT32_MAX, pPath);
            if (capturedParams.contains("noise_subtract_mode")) {
                dctx.noise_subtract_mode = parseNoiseSubtractMode(
                    stringField(capturedParams, "noise_subtract_mode", pPath), pPath);
            } else if (ctxSlot.empty()) {
                dctx.noise_subtract_mode = IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE;
            }
            if (capturedParams.contains("lrnu_kernel_size")) {
                dctx.lrnu_kernel_size = integerField(capturedParams, "lrnu_kernel_size", 0, 1025, pPath);
            } else if (ctxSlot.empty()) {
                dctx.lrnu_kernel_size = 15;
            }

            IRGjbBurstMetrics m{};
            char err[1024]{};
            checkStatus(IR_GjbEvaluateBurstMetrics(meanMap.data.data(), vnMap.data.data(), maskPtr,
                g.height, g.width, n, &dctx, &m, err, sizeof err), err);
            recordScalarOutputs(ctx, name, outs, {
                m.mean_dn, m.span_dn, m.spatial_std_dn, m.spatial_std_denoised_dn,
                m.nonuniformity_pct, m.avg_temporal_noise_dn, m.estimated_netd_mk,
                m.lrnu_std_dn, m.lrnu_pct, m.hrnu_std_dn, m.hrnu_pct,
                static_cast<double>(m.valid_pixel_count)
            });
        }
    }};
}

PreparedMetric prepNoise3dCompute(const std::string &name, const Json &inputs, const Json &params,
                                    const Json &outputs, std::map<std::string, SlotKind> &slots,
                                    const std::string &path)
{
    allowFields(inputs, {"frames", "blind_mask", "k_map", "b_map"}, path + ".inputs");
    allowFields(params, {"mode", "b_avg", "hist_interval", "thread_count", "invalid_fill"}, path + ".params");
    allowFields(outputs, {}, path + ".outputs");
    const auto framesSlot = requireInputSlot(inputs, "frames", SlotKind::batch, slots, path);
    const auto blindSlot = requireInputSlot(inputs, "blind_mask", SlotKind::byte_mask, slots, path);
    const auto modeText = optionalString(params, "mode", "org", path + ".params");
    require(modeText == "org" || modeText == "nuc", IR_BAD_ARGUMENT,
            path + ".params.mode must be 'org' or 'nuc'");
    const int32_t mode = (modeText == "nuc") ? IR_NOISE3D_MODE_NUC : IR_NOISE3D_MODE_ORG;
    const auto kSlot = (mode == IR_NOISE3D_MODE_NUC)
        ? requireInputSlot(inputs, "k_map", SlotKind::double_map, slots, path)
        : optionalInputSlot(inputs, "k_map", SlotKind::double_map, slots, path);
    const auto bSlot = (mode == IR_NOISE3D_MODE_NUC)
        ? requireInputSlot(inputs, "b_map", SlotKind::double_map, slots, path)
        : optionalInputSlot(inputs, "b_map", SlotKind::double_map, slots, path);
    const bool hasExplicitBAvg = params.contains("b_avg");
    const double explicitBAvg = hasExplicitBAvg ? numberField(params, "b_avg", path + ".params") : 0.0;
    const int32_t histInterval = optionalInteger(params, "hist_interval", 4, 1, 4096, path + ".params");
    const int32_t threadCount = optionalInteger(params, "thread_count", 1, 1, 256, path + ".params");
    const double invalidFill = optionalNumber(params, "invalid_fill", -999.0, path + ".params");

    std::vector<MetricOutput> outs{
        {".delta_t_dn", "DN", "noise3d_compute", 1},
        {".delta_i_dn", "DN", "noise3d_compute", 1},
        {".delta_j_dn", "DN", "noise3d_compute", 1},
        {".delta_ij_dn", "DN", "noise3d_compute", 1},
        {".delta_ti_dn", "DN", "noise3d_compute", 1},
        {".delta_tj_dn", "DN", "noise3d_compute", 1},
        {".delta_tij_dn", "DN", "noise3d_compute", 1},
        {".delta_total_dn", "DN", "noise3d_compute", 1},
        {".global_mean_dn", "DN", "noise3d_compute", 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &b = getBatch(ctx, framesSlot);
            const auto &blind = getMask(ctx, blindSlot);
            const auto g = b.geometry;
            const auto n = pixels(g.height, g.width);
            checkMaskSize(blind, n, g, "noise3d_compute.blind_mask");
            const double *kPtr = nullptr, *bPtr = nullptr;
            double resolvedBAvg = explicitBAvg;
            if (mode == IR_NOISE3D_MODE_NUC) {
                const auto &km = getMap(ctx, kSlot);
                const auto &bm = getMap(ctx, bSlot);
                checkMapSize(km, n, g, "noise3d_compute.k_map");
                checkMapSize(bm, n, g, "noise3d_compute.b_map");
                kPtr = km.data.data();
                bPtr = bm.data.data();
                if (!hasExplicitBAvg) {
                    double sum = 0.0;
                    int64_t cnt = 0;
                    for (int64_t i = 0; i < n; ++i) {
                        if (blind.data[static_cast<size_t>(i)] == 0 && std::isfinite(bm.data[static_cast<size_t>(i)])) {
                            sum += bm.data[static_cast<size_t>(i)];
                            ++cnt;
                        }
                    }
                    require(cnt > 0, IR_COMPUTATION_FAILED, "No valid pixels in noise3d b_map");
                    resolvedBAvg = sum / static_cast<double>(cnt);
                }
            }

            const int32_t frameCount = int32_t(b.stamps.size());
            double scalars[IR_NOISE3D_SCALAR_COUNT]{};
            std::vector<double> hist(static_cast<size_t>(IR_NOISE3D_HIST_GROUP_COUNT) * histInterval);
            std::vector<double> histMeta(static_cast<size_t>(IR_NOISE3D_HIST_GROUP_COUNT) * 3);
            std::vector<double> mapIJ(static_cast<size_t>(n));
            std::vector<double> mapTI(static_cast<size_t>(frameCount) * g.height);
            std::vector<double> mapTJ(static_cast<size_t>(frameCount) * g.width);

            IRNoise3DComputeOutputs out{};
            out.scalars = scalars;
            out.hist = hist.data();
            out.hist_meta = histMeta.data();
            out.map_ij = mapIJ.data();
            out.map_ti = mapTI.data();
            out.map_tj = mapTJ.data();
            out.hist_capacity = int64_t(hist.size());
            out.map_ij_capacity = n;
            out.map_ti_capacity = int64_t(mapTI.size());
            out.map_tj_capacity = int64_t(mapTJ.size());
            out.scalars_capacity = IR_NOISE3D_SCALAR_COUNT;
            out.hist_meta_capacity = int32_t(histMeta.size());

            const IRNoise3DComputeParams cp{mode, threadCount, histInterval, 0, resolvedBAvg, invalidFill};
            char err[1024]{};
            const IRArrayView input = frameBatchArrayView(b);
            checkStatus(IR_Noise3DComputeFramesDn(&input,
                blind.data.data(), n, kPtr, bPtr, kPtr ? n : 0,
                &cp, &out, err, sizeof err), err);
            recordScalarOutputs(ctx, name, outs, {
                scalars[IR_NOISE3D_SCALAR_DELTA_T],
                scalars[IR_NOISE3D_SCALAR_DELTA_I],
                scalars[IR_NOISE3D_SCALAR_DELTA_J],
                scalars[IR_NOISE3D_SCALAR_DELTA_IJ],
                scalars[IR_NOISE3D_SCALAR_DELTA_TI],
                scalars[IR_NOISE3D_SCALAR_DELTA_TJ],
                scalars[IR_NOISE3D_SCALAR_DELTA_TIJ],
                scalars[IR_NOISE3D_SCALAR_DELTA_TOTAL],
                scalars[IR_NOISE3D_SCALAR_GLOBAL_MEAN]
            });
        }
    }};
}


PreparedMetric prepChannelSpectrum(const std::string &name, const Json &inputs, const Json &params,
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

    std::vector<MetricOutput> outs{
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
            const IRArrayView input = frameBatchArrayView(b);
            const int32_t bins = frameCount / 2 + 1;
            std::vector<double> freqs(static_cast<size_t>(bins));
            std::vector<double> amps(static_cast<size_t>(channels) * bins);
            std::vector<double> tMeans(static_cast<size_t>(channels));
            std::vector<double> tStds(static_cast<size_t>(channels));
            char err[1024]{};
            checkStatus(IR_ChannelSpectrum(&input, axis, channels, fsHz, winKind,
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

PreparedMetric prepFlickerDetect(const std::string &name, const Json &inputs, const Json &params,
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

    std::vector<MetricOutput> outs{
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
            const IRArrayView input = frameBatchArrayView(b);
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
            std::optional<SlotBufferWriter<ByteMask>> outWriter;
            uint8_t *outMaskPtr = nullptr;
            if (!flickerOut.empty()) {
                outWriter.emplace(ctx.masks, flickerOut, g, n, flickerOut != maskSlot);
                outMaskPtr = outWriter->data();
            }
            IRFlickerSummary summary{};
            char err[1024]{};
            checkStatus(IR_FlickerDetect(&input,
                kPtr, bPtr, maskPtr, mult, exceedThresh,
                &summary, nullptr, outMaskPtr, n, nullptr, nullptr, 0,
                err, sizeof err), err);
            if (outWriter)
                outWriter->commit();
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

PreparedMetric prepFft2d(const std::string &name, const Json &inputs, const Json &params,
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

    std::vector<MetricOutput> outs{
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
            std::optional<SlotBufferWriter<DoubleMap>> magWriter, pwrWriter;
            double *magPtr = nullptr;
            double *pwrPtr = nullptr;
            if (!magOut.empty()) {
                magWriter.emplace(ctx.maps, magOut, g, n, magOut != mapSlot && magOut != pwrOut);
                magPtr = magWriter->data();
            }
            if (!pwrOut.empty()) {
                pwrWriter.emplace(ctx.maps, pwrOut, g, n, pwrOut != mapSlot && pwrOut != magOut);
                pwrPtr = pwrWriter->data();
            }
            IRFft2dSummary summary{};
            char err[1024]{};
            checkStatus(IR_Fft2dMagnitudeF64(m.data.data(), maskPtr, g.height, g.width, n,
                winKind, detrend ? 1 : 0, shiftDc ? 1 : 0,
                &summary, magPtr, pwrPtr, err, sizeof err), err);
            if (magWriter)
                magWriter->commit();
            if (pwrWriter)
                pwrWriter->commit();
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


constexpr MetricSpec kStandardMethods[]{
    {"gb_evaluate", nullptr, prepGbEvaluate},
    {"gjb_evaluate", nullptr, prepGjbEvaluate},
    {"gjb_burst", nullptr, prepGjbBurst},
    {"noise3d_compute", "frames", prepNoise3dCompute},
    {"channel_spectrum", "frames", prepChannelSpectrum},
    {"flicker_detect", "frames", prepFlickerDetect},
    {"fft2d", "map", prepFft2d},
};

} // namespace

std::span<const MetricSpec> standardMetrics() noexcept
{
    return kStandardMethods;
}

} // namespace ir::procedures::recipe
