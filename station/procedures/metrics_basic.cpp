#include "metric_registry.h"
#include <climits>
#include <vector>

namespace ir::procedures::recipe {
using namespace ir::api;

namespace {

PreparedMetric prepTemporalNoise(const std::string &name, const Json &inputs, const Json &params,
                                   const Json &outputs, std::map<std::string, SlotKind> &slots,
                                   const std::string &path)
{
    allowFields(inputs, {"frames"}, path + ".inputs");
    allowFields(params, {}, path + ".params");
    allowFields(outputs, {}, path + ".outputs");
    const auto framesSlot = requireInputSlot(inputs, "frames", SlotKind::batch, slots, path);
    std::vector<MetricOutput> outs{
        {".mean_dn", "DN", "frame_mean", 1},
        {".temporal_noise_dn", "DN", "mean_pixel_sample_std", 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &b = getBatch(ctx, framesSlot);
            const auto n = pixels(b.geometry.height, b.geometry.width);
            std::vector<double> meanMap(static_cast<size_t>(n)), stdMap(static_cast<size_t>(n));
            double summary[2]{};
            char err[1024]{};
            checkStatus(IR_FrameStatisticsU16(b.pixels.data(), int64_t(b.pixels.size()),
                int32_t(b.stamps.size()), b.geometry.height, b.geometry.width, 1,
                meanMap.data(), stdMap.data(), n, summary, 2, err, sizeof err), err);
            recordScalarOutputs(ctx, name, outs, {summary[0], summary[1]});
        }
    }};
}

PreparedMetric prepRoiMean(const std::string &name, const Json &inputs, const Json &params,
                             const Json &outputs, std::map<std::string, SlotKind> &slots,
                             const std::string &path)
{
    allowFields(inputs, {"frames"}, path + ".inputs");
    allowFields(params, {"x", "y", "width", "height"}, path + ".params");
    allowFields(outputs, {}, path + ".outputs");
    const auto framesSlot = requireInputSlot(inputs, "frames", SlotKind::batch, slots, path);
    const auto x = integerField(params, "x", 0, INT32_MAX, path + ".params");
    const auto y = integerField(params, "y", 0, INT32_MAX, path + ".params");
    const auto w = integerField(params, "width", 1, INT32_MAX, path + ".params");
    const auto h = integerField(params, "height", 1, INT32_MAX, path + ".params");
    std::vector<MetricOutput> outs{{".mean_dn", "DN", "roi_mean", 1}};
    return {1, outs, {
        [=](IRGeometry g) {
            require(x <= g.width - w && y <= g.height - h,
                    IR_BAD_ARGUMENT, "ROI is outside the image geometry");
        },
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &b = getBatch(ctx, framesSlot);
            double mean{};
            char err[1024]{};
            checkStatus(IR_ROIMeanU16(b.pixels.data(), int64_t(b.pixels.size()),
                int32_t(b.stamps.size()), b.geometry.height, b.geometry.width,
                x, y, w, h, &mean, err, sizeof err), err);
            recordScalarOutputs(ctx, name, outs, {mean});
        }
    }};
}

PreparedMetric prepFrameStats(const std::string &name, const Json &inputs, const Json &params,
                                const Json &outputs, std::map<std::string, SlotKind> &slots,
                                const std::string &path)
{
    allowFields(inputs, {"frames", "k_map", "b_map", "dark_map", "valid_mask"}, path + ".inputs");
    allowFields(params, {"sample_std"}, path + ".params");
    allowFields(outputs, {"mean_map", "std_map"}, path + ".outputs");
    const auto framesSlot = requireInputSlot(inputs, "frames", SlotKind::batch, slots, path);
    const auto kSlot = optionalInputSlot(inputs, "k_map", SlotKind::double_map, slots, path);
    const auto bSlot = optionalInputSlot(inputs, "b_map", SlotKind::double_map, slots, path);
    require(kSlot.empty() == bSlot.empty(), IR_BAD_ARGUMENT,
            path + ".inputs: k_map and b_map must both be provided or both omitted");
    const auto darkSlot = optionalInputSlot(inputs, "dark_map", SlotKind::double_map, slots, path);
    const auto maskSlot = optionalInputSlot(inputs, "valid_mask", SlotKind::byte_mask, slots, path);
    const int sampleStd = optionalInteger(params, "sample_std", 0, 0, 1, path + ".params");
    const auto meanOut = bindOutputSlot(outputs, "mean_map", SlotKind::double_map, false, slots, path);
    const auto stdOut = bindOutputSlot(outputs, "std_map", SlotKind::double_map, false, slots, path);

    std::vector<MetricOutput> outs{
        {".mean_dn", "DN", "frame_mean", 1},
        {".noise_dn", "DN", sampleStd == 0 ? "mean_pixel_pop_std" : "mean_pixel_sample_std", 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &b = getBatch(ctx, framesSlot);
            const auto g = b.geometry;
            const auto n = pixels(g.height, g.width);
            const double *kPtr = nullptr, *bPtr = nullptr, *darkPtr = nullptr;
            const uint8_t *maskPtr = nullptr;
            if (!kSlot.empty()) {
                const auto &km = getMap(ctx, kSlot);
                const auto &bm = getMap(ctx, bSlot);
                checkMapSize(km, n, g, "frame_stats.k_map");
                checkMapSize(bm, n, g, "frame_stats.b_map");
                kPtr = km.data.data();
                bPtr = bm.data.data();
            }
            if (!darkSlot.empty()) {
                const auto &dm = getMap(ctx, darkSlot);
                checkMapSize(dm, n, g, "frame_stats.dark_map");
                darkPtr = dm.data.data();
            }
            if (!maskSlot.empty()) {
                const auto &vm = getMask(ctx, maskSlot);
                checkMaskSize(vm, n, g, "frame_stats.valid_mask");
                maskPtr = vm.data.data();
            }
            SlotBufferWriter meanWriter(ctx.maps, meanOut, g, n, meanOut != stdOut);
            SlotBufferWriter stdWriter(ctx.maps, stdOut, g, n, stdOut != meanOut);
            double summary[2]{};
            char err[1024]{};
            const int32_t frameCount = int32_t(b.stamps.size());
            if (!kPtr && !darkPtr && !maskPtr) {
                checkStatus(IR_FrameStatisticsU16(b.pixels.data(), int64_t(b.pixels.size()),
                    frameCount, g.height, g.width, sampleStd, meanWriter.data(), stdWriter.data(), n,
                    summary, 2, err, sizeof err), err);
            } else {
                checkStatus(IR_FrameStatisticsNucU16(b.pixels.data(), int64_t(b.pixels.size()),
                    frameCount, g.height, g.width, sampleStd, kPtr, bPtr, darkPtr, maskPtr,
                    meanWriter.data(), stdWriter.data(), n, summary, 2, err, sizeof err), err);
            }
            meanWriter.commit();
            stdWriter.commit();
            recordScalarOutputs(ctx, name, outs, {summary[0], summary[1]});
        }
    }};
}

PreparedMetric prepMapSummary(const std::string &name, const Json &inputs, const Json &params,
                                const Json &outputs, std::map<std::string, SlotKind> &slots,
                                const std::string &path)
{
    allowFields(inputs, {"map", "valid_mask"}, path + ".inputs");
    allowFields(params, {"roi_x", "roi_y", "roi_w", "roi_h", "p_low", "p_high"}, path + ".params");
    allowFields(outputs, {}, path + ".outputs");
    const auto mapSlot = requireInputSlot(inputs, "map", SlotKind::double_map, slots, path);
    const auto maskSlot = optionalInputSlot(inputs, "valid_mask", SlotKind::byte_mask, slots, path);
    const int rx = optionalInteger(params, "roi_x", 0, 0, INT32_MAX, path + ".params");
    const int ry = optionalInteger(params, "roi_y", 0, 0, INT32_MAX, path + ".params");
    const int rw = optionalInteger(params, "roi_w", 0, 0, INT32_MAX, path + ".params");
    const int rh = optionalInteger(params, "roi_h", 0, 0, INT32_MAX, path + ".params");
    const double pLow = optionalNumber(params, "p_low", 5.0, path + ".params");
    const double pHigh = optionalNumber(params, "p_high", 95.0, path + ".params");
    require((rw == 0 && rh == 0) || (rw > 0 && rh > 0), IR_BAD_ARGUMENT,
            path + ".params: roi_w and roi_h must both be 0 or both > 0");
    require(pLow >= 0.0 && pLow <= pHigh && pHigh <= 100.0, IR_BAD_ARGUMENT,
            path + ".params: invalid percentile range");

    std::vector<MetricOutput> outs{
        {".mean", "DN", "map_summary", 1},
        {".pop_std", "DN", "map_summary", 1},
        {".sample_std", "DN", "map_summary", 1},
        {".min", "DN", "map_summary", 1},
        {".max", "DN", "map_summary", 1},
        {".span", "DN", "map_summary", 1},
        {".p_low", "DN", "map_summary", 1},
        {".p_high", "DN", "map_summary", 1},
        {".p_span", "DN", "map_summary", 1},
        {".valid_count", "count", "map_summary", 1}
    };
    return {1, outs, {
        [=](IRGeometry g) {
            if (rw > 0) {
                require(rx <= g.width - rw && ry <= g.height - rh,
                        IR_BAD_ARGUMENT, "map_summary ROI is outside the image geometry");
            }
        },
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &m = getMap(ctx, mapSlot);
            const auto g = m.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(m, n, g, "map_summary.map");
            const uint8_t *maskPtr = nullptr;
            if (!maskSlot.empty()) {
                const auto &vm = getMask(ctx, maskSlot);
                checkMaskSize(vm, n, g, "map_summary.valid_mask");
                maskPtr = vm.data.data();
            }
            IRMapSummary ms{};
            char err[1024]{};
            checkStatus(IR_MapSummaryF64(m.data.data(), maskPtr, g.height, g.width, n,
                rx, ry, rw, rh, pLow, pHigh, &ms, err, sizeof err), err);
            recordScalarOutputs(ctx, name, outs, {
                ms.mean, ms.pop_std, ms.sample_std, ms.min_val, ms.max_val,
                ms.span, ms.percentile_low, ms.percentile_high, ms.percentile_span,
                static_cast<double>(ms.valid_count)
            });
        }
    }};
}

PreparedMetric prepTwoPointNuc(const std::string &name, const Json &inputs, const Json &params,
                                 const Json &outputs, std::map<std::string, SlotKind> &slots,
                                 const std::string &path)
{
    allowFields(inputs, {"v1_map", "v2_map", "valid_mask", "ref_map"}, path + ".inputs");
    allowFields(params, {}, path + ".params");
    allowFields(outputs, {"k_map", "b_map"}, path + ".outputs");
    const auto v1Slot = requireInputSlot(inputs, "v1_map", SlotKind::double_map, slots, path);
    const auto v2Slot = requireInputSlot(inputs, "v2_map", SlotKind::double_map, slots, path);
    const auto maskSlot = optionalInputSlot(inputs, "valid_mask", SlotKind::byte_mask, slots, path);
    const auto refSlot = optionalInputSlot(inputs, "ref_map", SlotKind::double_map, slots, path);
    const auto kOut = bindOutputSlot(outputs, "k_map", SlotKind::double_map, true, slots, path);
    const auto bOut = bindOutputSlot(outputs, "b_map", SlotKind::double_map, true, slots, path);

    std::vector<MetricOutput> outs{
        {".mean1_dn", "DN", "two_point_nuc", 1},
        {".mean2_dn", "DN", "two_point_nuc", 1},
        {".mean_ref_dn", "DN", "two_point_nuc", 1},
        {".valid_count", "count", "two_point_nuc", 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &v1 = getMap(ctx, v1Slot);
            const auto &v2 = getMap(ctx, v2Slot);
            const auto g = v1.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(v1, n, g, "two_point_nuc.v1_map");
            checkMapSize(v2, n, g, "two_point_nuc.v2_map");
            const uint8_t *maskPtr = nullptr;
            const double *refPtr = nullptr;
            if (!maskSlot.empty()) {
                const auto &vm = getMask(ctx, maskSlot);
                checkMaskSize(vm, n, g, "two_point_nuc.valid_mask");
                maskPtr = vm.data.data();
            }
            if (!refSlot.empty()) {
                const auto &rm = getMap(ctx, refSlot);
                checkMapSize(rm, n, g, "two_point_nuc.ref_map");
                refPtr = rm.data.data();
            }
            const bool kDirect = kOut != v1Slot && kOut != v2Slot && kOut != refSlot && kOut != bOut;
            const bool bDirect = bOut != v1Slot && bOut != v2Slot && bOut != refSlot && bOut != kOut;
            SlotBufferWriter kWriter(ctx.maps, kOut, g, n, kDirect);
            SlotBufferWriter bWriter(ctx.maps, bOut, g, n, bDirect);
            double summary[4]{};
            char err[1024]{};
            checkStatus(IR_BuildTwoPointNucF64(v1.data.data(), v2.data.data(), maskPtr, refPtr,
                g.height, g.width, n, kWriter.data(), bWriter.data(), summary, 4, err, sizeof err), err);
            kWriter.commit();
            bWriter.commit();
            recordScalarOutputs(ctx, name, outs, {summary[0], summary[1], summary[2], summary[3]});
        }
    }};
}

PreparedMetric prepSpatialFilter(const std::string &name, const Json &inputs, const Json &params,
                                   const Json &outputs, std::map<std::string, SlotKind> &slots,
                                   const std::string &path)
{
    allowFields(inputs, {"in_map", "valid_mask"}, path + ".inputs");
    allowFields(params, {"filter_kind", "kernel_w", "kernel_h"}, path + ".params");
    allowFields(outputs, {"low_map", "high_map"}, path + ".outputs");
    const auto inSlot = requireInputSlot(inputs, "in_map", SlotKind::double_map, slots, path);
    const auto maskSlot = optionalInputSlot(inputs, "valid_mask", SlotKind::byte_mask, slots, path);
    const auto kind = optionalString(params, "filter_kind", "box_mean", path + ".params");
    require(kind == "box_mean" || kind == "gaussian", IR_BAD_ARGUMENT,
            path + ".params.filter_kind must be box_mean or gaussian");
    const int32_t filterKind = (kind == "gaussian") ? IR_SPATIAL_FILTER_GAUSSIAN : IR_SPATIAL_FILTER_BOX_MEAN;
    const char *lowMethod = (kind == "gaussian") ? "spatial_gaussian_low" : "spatial_box_mean_low";
    const char *highMethod = (kind == "gaussian") ? "spatial_gaussian_high" : "spatial_box_mean_high";
    const int kw = optionalInteger(params, "kernel_w", 15, 1, 1025, path + ".params");
    const int kh = optionalInteger(params, "kernel_h", 15, 1, 1025, path + ".params");
    require((kw % 2) == 1 && (kh % 2) == 1, IR_BAD_ARGUMENT,
            path + ".params: kernel_w and kernel_h must be odd >= 1");
    const auto lowOut = bindOutputSlot(outputs, "low_map", SlotKind::double_map, false, slots, path);
    const auto highOut = bindOutputSlot(outputs, "high_map", SlotKind::double_map, false, slots, path);
    require(!lowOut.empty() || !highOut.empty(), IR_BAD_ARGUMENT,
            path + ".outputs: provide low_map and/or high_map");

    std::vector<MetricOutput> outs{
        {".low_std_dn", "DN", lowMethod, 1},
        {".high_std_dn", "DN", highMethod, 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &inMap = getMap(ctx, inSlot);
            const auto g = inMap.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(inMap, n, g, "spatial_filter.in_map");
            const uint8_t *maskPtr = nullptr;
            if (!maskSlot.empty()) {
                const auto &vm = getMask(ctx, maskSlot);
                checkMaskSize(vm, n, g, "spatial_filter.valid_mask");
                maskPtr = vm.data.data();
            }
            const bool lowDirect = lowOut != inSlot && lowOut != highOut;
            const bool highDirect = highOut != inSlot && highOut != lowOut;
            SlotBufferWriter lowWriter(ctx.maps, lowOut, g, n, lowDirect);
            SlotBufferWriter highWriter(ctx.maps, highOut, g, n, highDirect);
            char err[1024]{};
            checkStatus(IR_SpatialLowHighPassF64(inMap.data.data(), maskPtr, g.height, g.width, n,
                filterKind, kw, kh, lowWriter.data(), highWriter.data(),
                err, sizeof err), err);
            IRMapSummary lowSum{}, highSum{};
            checkStatus(IR_MapSummaryF64(lowWriter.data(), maskPtr, g.height, g.width, n,
                0, 0, 0, 0, 0.0, 100.0, &lowSum, err, sizeof err), err);
            checkStatus(IR_MapSummaryF64(highWriter.data(), maskPtr, g.height, g.width, n,
                0, 0, 0, 0, 0.0, 100.0, &highSum, err, sizeof err), err);
            lowWriter.commit();
            highWriter.commit();
            recordScalarOutputs(ctx, name, outs, {lowSum.pop_std, highSum.pop_std});
        }
    }};
}

PreparedMetric prepMapBinaryOp(const std::string &name, const Json &inputs, const Json &params,
                                 const Json &outputs, std::map<std::string, SlotKind> &slots,
                                 const std::string &path)
{
    allowFields(inputs, {"a_map", "b_map"}, path + ".inputs");
    allowFields(params, {"op", "param"}, path + ".params");
    allowFields(outputs, {"out_map"}, path + ".outputs");
    const auto aSlot = requireInputSlot(inputs, "a_map", SlotKind::double_map, slots, path);
    const auto bSlot = requireInputSlot(inputs, "b_map", SlotKind::double_map, slots, path);
    const auto opText = stringField(params, "op", path + ".params");
    int32_t op{};
    if (opText == "sub") op = IR_MAP_OP_SUB;
    else if (opText == "add") op = IR_MAP_OP_ADD;
    else if (opText == "mul") op = IR_MAP_OP_MUL;
    else if (opText == "safe_div") op = IR_MAP_OP_SAFE_DIV;
    else if (opText == "variance_subtract") op = IR_MAP_OP_VARIANCE_SUBTRACT;
    else throw Error(IR_BAD_ARGUMENT, path + ".params.op is unsupported: " + opText);
    const double paramVal = optionalNumber(params, "param", 1.0, path + ".params");
    const auto outSlot = bindOutputSlot(outputs, "out_map", SlotKind::double_map, true, slots, path);

    std::vector<MetricOutput> outs{{".mean", "DN", "map_binary_op", 1}};
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &a = getMap(ctx, aSlot);
            const auto &b = getMap(ctx, bSlot);
            const auto g = a.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(a, n, g, "map_binary_op.a_map");
            checkMapSize(b, n, g, "map_binary_op.b_map");
            SlotBufferWriter outWriter(ctx.maps, outSlot, g, n, outSlot != aSlot && outSlot != bSlot);
            char err[1024]{};
            checkStatus(IR_MapBinaryOpF64(a.data.data(), b.data.data(), g.height, g.width, n,
                op, paramVal, outWriter.data(), err, sizeof err), err);
            IRMapSummary ms{};
            checkStatus(IR_MapSummaryF64(outWriter.data(), nullptr, g.height, g.width, n,
                0, 0, 0, 0, 0.0, 100.0, &ms, err, sizeof err), err);
            outWriter.commit();
            recordScalarOutputs(ctx, name, outs, {ms.mean});
        }
    }};
}

PreparedMetric prepThresholdMask(const std::string &name, const Json &inputs, const Json &params,
                                   const Json &outputs, std::map<std::string, SlotKind> &slots,
                                   const std::string &path)
{
    allowFields(inputs, {"map", "base_valid_mask"}, path + ".inputs");
    allowFields(params, {"mode", "low", "high"}, path + ".params");
    allowFields(outputs, {"out_mask"}, path + ".outputs");
    const auto mapSlot = requireInputSlot(inputs, "map", SlotKind::double_map, slots, path);
    const auto baseSlot = optionalInputSlot(inputs, "base_valid_mask", SlotKind::byte_mask, slots, path);
    const auto modeText = stringField(params, "mode", path + ".params");
    int32_t mode{};
    if (modeText == "absolute") mode = IR_THRESH_ABSOLUTE;
    else if (modeText == "mean_multiplier") mode = IR_THRESH_MEAN_MULTIPLIER;
    else if (modeText == "mean_sigma") mode = IR_THRESH_MEAN_SIGMA;
    else if (modeText == "upper_only") mode = IR_THRESH_UPPER_ONLY;
    else if (modeText == "lower_only") mode = IR_THRESH_LOWER_ONLY;
    else throw Error(IR_BAD_ARGUMENT, path + ".params.mode is unsupported: " + modeText);
    const double lowParam = optionalNumber(params, "low", 0.0, path + ".params");
    const double highParam = optionalNumber(params, "high", 0.0, path + ".params");
    const auto outSlot = bindOutputSlot(outputs, "out_mask", SlotKind::byte_mask, true, slots, path);

    std::vector<MetricOutput> outs{
        {".pass_count", "count", "threshold_mask", 1},
        {".fail_count", "count", "threshold_mask", 1}
    };
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &m = getMap(ctx, mapSlot);
            const auto g = m.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(m, n, g, "threshold_mask.map");
            const uint8_t *basePtr = nullptr;
            if (!baseSlot.empty()) {
                const auto &bm = getMask(ctx, baseSlot);
                checkMaskSize(bm, n, g, "threshold_mask.base_valid_mask");
                basePtr = bm.data.data();
            }
            SlotBufferWriter passWriter(ctx.masks, outSlot, g, n, outSlot != baseSlot);
            int32_t passCnt = 0, failCnt = 0;
            char err[1024]{};
            checkStatus(IR_ThresholdMaskF64(m.data.data(), basePtr, g.height, g.width, n,
                mode, lowParam, highParam, passWriter.data(), &passCnt, &failCnt, err, sizeof err), err);
            passWriter.commit();
            recordScalarOutputs(ctx, name, outs, {double(passCnt), double(failCnt)});
        }
    }};
}

PreparedMetric prepMaskCombine(const std::string &name, const Json &inputs, const Json &params,
                                 const Json &outputs, std::map<std::string, SlotKind> &slots,
                                 const std::string &path)
{
    allowFields(inputs, {"mask_a", "mask_b"}, path + ".inputs");
    allowFields(params, {"op"}, path + ".params");
    allowFields(outputs, {"out_mask"}, path + ".outputs");
    const auto aSlot = requireInputSlot(inputs, "mask_a", SlotKind::byte_mask, slots, path);
    const auto opText = stringField(params, "op", path + ".params");
    int32_t op{};
    if (opText == "and") op = IR_MASK_AND;
    else if (opText == "or") op = IR_MASK_OR;
    else if (opText == "and_not") op = IR_MASK_AND_NOT;
    else if (opText == "not") op = IR_MASK_NOT;
    else throw Error(IR_BAD_ARGUMENT, path + ".params.op is unsupported: " + opText);
    const auto bSlot = (op == IR_MASK_NOT)
        ? optionalInputSlot(inputs, "mask_b", SlotKind::byte_mask, slots, path)
        : requireInputSlot(inputs, "mask_b", SlotKind::byte_mask, slots, path);
    const auto outSlot = bindOutputSlot(outputs, "out_mask", SlotKind::byte_mask, true, slots, path);

    std::vector<MetricOutput> outs{{".nonzero_count", "count", "mask_combine", 1}};
    return {1, outs, {
        {},
        [=](AnalysisRunContext &runCtx) {
            ExecutionContext &ctx = runCtx.ctx;
            const auto &ma = getMask(ctx, aSlot);
            const auto g = ma.geometry;
            const auto n = pixels(g.height, g.width);
            checkMaskSize(ma, n, g, "mask_combine.mask_a");
            const uint8_t *bPtr = nullptr;
            if (!bSlot.empty()) {
                const auto &mb = getMask(ctx, bSlot);
                checkMaskSize(mb, n, g, "mask_combine.mask_b");
                bPtr = mb.data.data();
            }
            SlotBufferWriter outWriter(ctx.masks, outSlot, g, n, outSlot != aSlot && outSlot != bSlot);
            int32_t nonzeroCnt = 0;
            char err[1024]{};
            checkStatus(IR_MaskCombine(ma.data.data(), bPtr, g.height, g.width, n, op,
                outWriter.data(), &nonzeroCnt, err, sizeof err), err);
            outWriter.commit();
            recordScalarOutputs(ctx, name, outs, {double(nonzeroCnt)});
        }
    }};
}

constexpr MetricSpec kBasicMethods[]{
    {"temporal_noise", "frames", prepTemporalNoise},
    {"roi_mean", "frames", prepRoiMean},
    {"frame_stats", "frames", prepFrameStats},
    {"map_summary", "map", prepMapSummary},
    {"two_point_nuc", nullptr, prepTwoPointNuc},
    {"spatial_filter", "in_map", prepSpatialFilter},
    {"map_binary_op", nullptr, prepMapBinaryOp},
    {"threshold_mask", "map", prepThresholdMask},
    {"mask_combine", "mask_a", prepMaskCombine},
};

} // namespace

std::span<const MetricSpec> basicMetrics() noexcept
{
    return kBasicMethods;
}

} // namespace ir::procedures::recipe
