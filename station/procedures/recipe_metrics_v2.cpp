#include "recipe_config.h"
#include "IR_analysis.h"
#include "api_common.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <iterator>
#include <vector>

namespace ir::procedures::recipe {
using namespace ir::api;

namespace {

const char *slotKindName(SlotKind kind) noexcept
{
    switch (kind) {
    case SlotKind::batch: return "batch";
    case SlotKind::double_map: return "double_map";
    case SlotKind::byte_mask: return "byte_mask";
    case SlotKind::drift_context: return "drift_context";
    }
    return "unknown";
}

std::string requireInputSlot(const Json &inputs, const char *key, SlotKind expected,
                             const std::map<std::string, SlotKind> &knownSlots,
                             const std::string &path)
{
    const auto slot = stringField(inputs, key, path + ".inputs");
    const auto found = knownSlots.find(slot);
    require(found != knownSlots.end(), IR_BAD_ARGUMENT,
            path + ".inputs." + key + " references undefined slot: " + slot);
    require(found->second == expected, IR_BAD_ARGUMENT,
            path + ".inputs." + key + " expects slot type " + slotKindName(expected) +
                ", got " + slotKindName(found->second));
    return slot;
}

std::string optionalInputSlot(const Json &inputs, const char *key, SlotKind expected,
                              const std::map<std::string, SlotKind> &knownSlots,
                              const std::string &path)
{
    if (!inputs.contains(key))
        return {};
    return requireInputSlot(inputs, key, expected, knownSlots, path);
}

std::string bindOutputSlot(const Json &outputs, const char *key, SlotKind kind, bool required,
                           std::map<std::string, SlotKind> &knownSlots, const std::string &path)
{
    if (!outputs.contains(key)) {
        require(!required, IR_BAD_ARGUMENT, path + ".outputs." + key + " is required");
        return {};
    }
    const auto slot = stringField(outputs, key, path + ".outputs");
    const auto [it, inserted] = knownSlots.emplace(slot, kind);
    if (!inserted) {
        require(it->second == kind, IR_BAD_ARGUMENT,
                path + ".outputs." + key + " cannot overwrite slot '" + slot + "' of type " +
                    slotKindName(it->second) + " with " + slotKindName(kind));
    }
    return slot;
}

const FrameBatch &getBatch(const ExecutionContext &ctx, const std::string &slot)
{
    const auto it = ctx.batches.find(slot);
    require(it != ctx.batches.end(), IR_BAD_ARGUMENT, "Missing runtime batch slot: " + slot);
    return it->second;
}

const DoubleMap &getMap(const ExecutionContext &ctx, const std::string &slot)
{
    const auto it = ctx.maps.find(slot);
    require(it != ctx.maps.end(), IR_BAD_ARGUMENT, "Missing runtime map slot: " + slot);
    return it->second;
}

const ByteMask &getMask(const ExecutionContext &ctx, const std::string &slot)
{
    const auto it = ctx.masks.find(slot);
    require(it != ctx.masks.end(), IR_BAD_ARGUMENT, "Missing runtime mask slot: " + slot);
    return it->second;
}

const IRGjbDriftContext &getDriftContext(const ExecutionContext &ctx, const std::string &slot)
{
    const auto it = ctx.driftContexts.find(slot);
    require(it != ctx.driftContexts.end(), IR_BAD_ARGUMENT, "Missing runtime drift_ctx slot: " + slot);
    return it->second;
}

void ensureSameGeometry(IRGeometry a, IRGeometry b, const char *label)
{
    require(a.width == b.width && a.height == b.height, IR_BAD_ARGUMENT,
            std::string("Geometry mismatch in ") + label);
}

void recordScalarOutputs(ExecutionContext &ctx, const std::string &name,
                         const std::vector<MetricOutput> &outputs,
                         std::initializer_list<double> values)
{
    require(values.size() == outputs.size(), IR_COMPUTATION_FAILED,
            "Internal metric output count mismatch");
    size_t i = 0;
    for (double val : values) {
        const auto &out = outputs[i++];
        const std::string key = name + out.suffix;
        ctx.scalarValues[key] = val;
        ctx.metrics[key] = {
            {"value", val}, {"unit", out.unit}, {"method", out.method}, {"version", out.version}
        };
    }
}

void checkMapSize(const DoubleMap &m, int64_t expectedPixels, IRGeometry g, const char *label)
{
    ensureSameGeometry(m.geometry, g, label);
    require(int64_t(m.data.size()) == expectedPixels, IR_BAD_ARGUMENT,
            std::string("Unexpected pixel buffer size in ") + label);
}

void checkMaskSize(const ByteMask &m, int64_t expectedPixels, IRGeometry g, const char *label)
{
    ensureSameGeometry(m.geometry, g, label);
    require(int64_t(m.data.size()) == expectedPixels, IR_BAD_ARGUMENT,
            std::string("Unexpected mask buffer size in ") + label);
}

IRCalcParameters parseCalcParams(const Json &params, const std::string &path)
{
    IRCalcParameters p{};
    p.t0_c = numberField(params, "t0_c", path);
    p.t1_c = numberField(params, "t1_c", path);
    p.pixel_pitch_um = optionalNumber(params, "pixel_pitch_um", 15.0, path);
    p.emissivity = optionalNumber(params, "emissivity", 1.0, path);
    p.system_gain = optionalNumber(params, "system_gain", 1.0, path);
    p.adc_swing_v = optionalNumber(params, "adc_swing_v", 2.0, path);
    p.f_number = optionalNumber(params, "f_number", 1.0, path);
    p.vmax_per_count = optionalNumber(params, "vmax_per_count", 2.0, path);
    p.integration_time_us = optionalInteger(params, "integration_time_us", 1000, 1, INT32_MAX, path);
    p.invalid_integration_us = optionalInteger(params, "invalid_integration_us", 0, 0, INT32_MAX, path);
    p.digital_full = optionalInteger(params, "digital_full", 65535, 1, INT32_MAX, path);
    p.reserved = 0;
    return p;
}

int32_t parseNoiseSubtractMode(const std::string &text, const std::string &path)
{
    if (text == "none") return IR_NOISE_SUBTRACT_NONE;
    if (text == "quadrature") return IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE;
    throw Error(IR_BAD_ARGUMENT, path + ".noise_subtract_mode must be 'none' or 'quadrature'");
}

int32_t parseWindowKind(const std::string &text, const std::string &path)
{
    if (text == "rectangular") return IR_WINDOW_RECTANGULAR;
    if (text == "hann") return IR_WINDOW_HANN;
    if (text == "hamming") return IR_WINDOW_HAMMING;
    throw Error(IR_BAD_ARGUMENT, path + ".window_kind must be rectangular, hann, or hamming");
}

struct PreparedMetricV2 {
    int32_t version{1};
    std::vector<MetricOutput> outputs;
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

// ======================== Method Preparers ========================

PreparedMetricV2 prepTemporalNoise(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
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

PreparedMetricV2 prepRoiMean(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
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

PreparedMetricV2 prepFrameStats(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
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
            std::vector<double> meanMap(static_cast<size_t>(n));
            std::vector<double> stdMap(static_cast<size_t>(n));
            double summary[2]{};
            char err[1024]{};
            const int32_t frameCount = int32_t(b.stamps.size());
            if (!kPtr && !darkPtr && !maskPtr) {
                checkStatus(IR_FrameStatisticsU16(b.pixels.data(), int64_t(b.pixels.size()),
                    frameCount, g.height, g.width, sampleStd, meanMap.data(), stdMap.data(),
                    n, summary, 2, err, sizeof err), err);
            } else {
                checkStatus(IR_FrameStatisticsNucU16(b.pixels.data(), int64_t(b.pixels.size()),
                    frameCount, g.height, g.width, sampleStd, kPtr, bPtr, darkPtr, maskPtr,
                    meanMap.data(), stdMap.data(), n, summary, 2, err, sizeof err), err);
            }
            if (!meanOut.empty()) ctx.maps[meanOut] = {g, std::move(meanMap)};
            if (!stdOut.empty()) ctx.maps[stdOut] = {g, std::move(stdMap)};
            recordScalarOutputs(ctx, name, outs, {summary[0], summary[1]});
        }
    }};
}

PreparedMetricV2 prepMapSummary(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
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

PreparedMetricV2 prepTwoPointNuc(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
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
            std::vector<double> kMap(static_cast<size_t>(n)), bMap(static_cast<size_t>(n));
            double summary[4]{};
            char err[1024]{};
            checkStatus(IR_BuildTwoPointNucF64(v1.data.data(), v2.data.data(), maskPtr, refPtr,
                g.height, g.width, n, kMap.data(), bMap.data(), summary, 4, err, sizeof err), err);
            ctx.maps[kOut] = {g, std::move(kMap)};
            ctx.maps[bOut] = {g, std::move(bMap)};
            recordScalarOutputs(ctx, name, outs, {summary[0], summary[1], summary[2], summary[3]});
        }
    }};
}

PreparedMetricV2 prepSpatialFilter(const std::string &name, const Json &inputs, const Json &params,
                                   const Json &outputs, std::map<std::string, SlotKind> &slots,
                                   const std::string &path)
{
    allowFields(inputs, {"in_map", "valid_mask"}, path + ".inputs");
    allowFields(params, {"filter_kind", "kernel_w", "kernel_h"}, path + ".params");
    allowFields(outputs, {"low_map", "high_map"}, path + ".outputs");
    const auto inSlot = requireInputSlot(inputs, "in_map", SlotKind::double_map, slots, path);
    const auto maskSlot = optionalInputSlot(inputs, "valid_mask", SlotKind::byte_mask, slots, path);
    const auto kind = optionalString(params, "filter_kind", "box_mean", path + ".params");
    require(kind == "box_mean", IR_BAD_ARGUMENT, path + ".params.filter_kind must be box_mean");
    const int kw = optionalInteger(params, "kernel_w", 15, 1, 1025, path + ".params");
    const int kh = optionalInteger(params, "kernel_h", 15, 1, 1025, path + ".params");
    require((kw % 2) == 1 && (kh % 2) == 1, IR_BAD_ARGUMENT,
            path + ".params: kernel_w and kernel_h must be odd >= 1");
    const auto lowOut = bindOutputSlot(outputs, "low_map", SlotKind::double_map, false, slots, path);
    const auto highOut = bindOutputSlot(outputs, "high_map", SlotKind::double_map, false, slots, path);
    require(!lowOut.empty() || !highOut.empty(), IR_BAD_ARGUMENT,
            path + ".outputs: provide low_map and/or high_map");

    std::vector<MetricOutput> outs{
        {".low_std_dn", "DN", "spatial_box_mean_low", 1},
        {".high_std_dn", "DN", "spatial_box_mean_high", 1}
    };
    return {1, outs, {
        {},
        [=](ExecutionContext &ctx) {
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
            std::vector<double> lowMap(static_cast<size_t>(n)), highMap(static_cast<size_t>(n));
            char err[1024]{};
            checkStatus(IR_SpatialLowHighPassF64(inMap.data.data(), maskPtr, g.height, g.width, n,
                IR_SPATIAL_FILTER_BOX_MEAN, kw, kh, lowMap.data(), highMap.data(),
                err, sizeof err), err);
            IRMapSummary lowSum{}, highSum{};
            checkStatus(IR_MapSummaryF64(lowMap.data(), maskPtr, g.height, g.width, n,
                0, 0, 0, 0, 0.0, 100.0, &lowSum, err, sizeof err), err);
            checkStatus(IR_MapSummaryF64(highMap.data(), maskPtr, g.height, g.width, n,
                0, 0, 0, 0, 0.0, 100.0, &highSum, err, sizeof err), err);
            if (!lowOut.empty()) ctx.maps[lowOut] = {g, std::move(lowMap)};
            if (!highOut.empty()) ctx.maps[highOut] = {g, std::move(highMap)};
            recordScalarOutputs(ctx, name, outs, {lowSum.pop_std, highSum.pop_std});
        }
    }};
}

PreparedMetricV2 prepMapBinaryOp(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
            const auto &a = getMap(ctx, aSlot);
            const auto &b = getMap(ctx, bSlot);
            const auto g = a.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(a, n, g, "map_binary_op.a_map");
            checkMapSize(b, n, g, "map_binary_op.b_map");
            std::vector<double> outMap(static_cast<size_t>(n));
            char err[1024]{};
            checkStatus(IR_MapBinaryOpF64(a.data.data(), b.data.data(), g.height, g.width, n,
                op, paramVal, outMap.data(), err, sizeof err), err);
            IRMapSummary ms{};
            checkStatus(IR_MapSummaryF64(outMap.data(), nullptr, g.height, g.width, n,
                0, 0, 0, 0, 0.0, 100.0, &ms, err, sizeof err), err);
            ctx.maps[outSlot] = {g, std::move(outMap)};
            recordScalarOutputs(ctx, name, outs, {ms.mean});
        }
    }};
}

PreparedMetricV2 prepThresholdMask(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
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
            std::vector<uint8_t> passMask(static_cast<size_t>(n));
            int32_t passCnt = 0, failCnt = 0;
            char err[1024]{};
            checkStatus(IR_ThresholdMaskF64(m.data.data(), basePtr, g.height, g.width, n,
                mode, lowParam, highParam, passMask.data(), &passCnt, &failCnt, err, sizeof err), err);
            ctx.masks[outSlot] = {g, std::move(passMask)};
            recordScalarOutputs(ctx, name, outs, {double(passCnt), double(failCnt)});
        }
    }};
}

PreparedMetricV2 prepMaskCombine(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
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
            std::vector<uint8_t> outMask(static_cast<size_t>(n));
            int32_t nonzeroCnt = 0;
            char err[1024]{};
            checkStatus(IR_MaskCombine(ma.data.data(), bPtr, g.height, g.width, n, op,
                outMask.data(), &nonzeroCnt, err, sizeof err), err);
            ctx.masks[outSlot] = {g, std::move(outMask)};
            recordScalarOutputs(ctx, name, outs, {double(nonzeroCnt)});
        }
    }};
}

PreparedMetricV2 prepGbEvaluate(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
            const auto &v1 = getMap(ctx, v1Slot);
            const auto &v2 = getMap(ctx, v2Slot);
            const auto &vn = getMap(ctx, vnSlot);
            const auto g = v1.geometry;
            const auto n = pixels(g.height, g.width);
            checkMapSize(v1, n, g, "gb_evaluate.v1_map");
            checkMapSize(v2, n, g, "gb_evaluate.v2_map");
            checkMapSize(vn, n, g, "gb_evaluate.vn_map");
            std::vector<uint8_t> validMask, badMask;
            IRGbBadPixelMasks masks{};
            masks.pixel_capacity = n;
            if (!validOut.empty()) {
                validMask.assign(static_cast<size_t>(n), 0);
                masks.valid_mask = validMask.data();
            }
            if (!badOut.empty()) {
                badMask.assign(static_cast<size_t>(n), 0);
                masks.total_bad_mask = badMask.data();
            }
            IRGbSummary gb{};
            char err[1024]{};
            checkStatus(IR_GbEvaluateMaps(v1.data.data(), v2.data.data(), vn.data.data(),
                g.height, g.width, n, &cp, &crit, &gb,
                (!validOut.empty() || !badOut.empty()) ? &masks : nullptr, err, sizeof err), err);
            if (!validOut.empty()) ctx.masks[validOut] = {g, std::move(validMask)};
            if (!badOut.empty()) ctx.masks[badOut] = {g, std::move(badMask)};
            recordScalarOutputs(ctx, name, outs, {
                gb.before_avg_snr, gb.after_avg_netd_mk, gb.after_response_nonuniformity_pct,
                double(gb.bad_pixels.total_bad_count), gb.bad_pixels.total_bad_rate_pct,
                double(gb.bad_pixels.chip_grade)
            });
        }
    }};
}

PreparedMetricV2 prepGjbEvaluate(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
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

            std::vector<uint8_t> validMask, badMask;
            IRGjbBadPixelMasks masks{};
            masks.pixel_capacity = n;
            if (!validOut.empty()) {
                validMask.assign(static_cast<size_t>(n), 0);
                masks.valid_mask = validMask.data();
            }
            if (!badOut.empty()) {
                badMask.assign(static_cast<size_t>(n), 0);
                masks.total_bad_mask = badMask.data();
            }
            IRGjbSummary gjb{};
            char err[1024]{};
            checkStatus(IR_GjbEvaluateMaps(v1.data.data(), v2.data.data(), vn.data.data(), vm.data.data(),
                g.height, g.width, n, &cp, &crit, &gjb,
                (!validOut.empty() || !badOut.empty()) ? &masks : nullptr, err, sizeof err), err);
            if (!validOut.empty()) ctx.masks[validOut] = {g, std::move(validMask)};
            if (!badOut.empty()) ctx.masks[badOut] = {g, std::move(badMask)};
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

PreparedMetricV2 prepGjbBurst(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
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

PreparedMetricV2 prepNoise3dCompute(const std::string &name, const Json &inputs, const Json &params,
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
        [=](ExecutionContext &ctx) {
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
            checkStatus(IR_Noise3DComputeFramesDn(b.pixels.data(), int64_t(b.pixels.size()),
                frameCount, g.height, g.width, blind.data.data(), n, kPtr, bPtr, kPtr ? n : 0,
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
        [=](ExecutionContext &ctx) {
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

constexpr MetricSpecV2 kMethodsV2[]{
    {"temporal_noise", "frames", prepTemporalNoise},
    {"roi_mean", "frames", prepRoiMean},
    {"frame_stats", "frames", prepFrameStats},
    {"map_summary", "map", prepMapSummary},
    {"two_point_nuc", nullptr, prepTwoPointNuc},
    {"spatial_filter", "in_map", prepSpatialFilter},
    {"map_binary_op", nullptr, prepMapBinaryOp},
    {"threshold_mask", "map", prepThresholdMask},
    {"mask_combine", "mask_a", prepMaskCombine},
    {"gb_evaluate", nullptr, prepGbEvaluate},
    {"gjb_evaluate", nullptr, prepGjbEvaluate},
    {"gjb_burst", nullptr, prepGjbBurst},
    {"noise3d_compute", "frames", prepNoise3dCompute},
    {"channel_spectrum", "frames", prepChannelSpectrum},
};

} // namespace

AnalysisStepV2 prepareAnalysisV2(StepRef ref, const Json &fields, const std::string &path,
                                 std::map<std::string, SlotKind> &knownSlots)
{
    allowFields(fields, {"id", "type", "method", "version", "name", "input", "inputs", "params", "outputs"}, path);
    const auto method = stringField(fields, "method", path);
    const auto found = std::find_if(std::begin(kMethodsV2), std::end(kMethodsV2),
        [&](const MetricSpecV2 &spec) { return method == spec.name; });
    require(found != std::end(kMethodsV2), IR_BAD_ARGUMENT, path + ".method is unsupported: " + method);

    const std::string stepName = fields.contains("name")
        ? stringField(fields, "name", path)
        : ref.id;
    const int32_t reqVersion = optionalInteger(fields, "version", 1, 1, 1000, path);

    Json normalizedInputs = Json::object();
    if (fields.contains("inputs"))
        normalizedInputs = objectField(fields, "inputs", path);
    if (fields.contains("input")) {
        const auto shorthand = stringField(fields, "input", path);
        require(found->defaultInputKey != nullptr, IR_BAD_ARGUMENT,
                path + ".input shorthand is unsupported for method '" + method + "'; use inputs object");
        require(!normalizedInputs.contains(found->defaultInputKey), IR_BAD_ARGUMENT,
                path + ": cannot provide both input shorthand and inputs." + found->defaultInputKey);
        normalizedInputs[found->defaultInputKey] = shorthand;
    }

    const Json params = fields.contains("params") ? objectField(fields, "params", path) : Json::object();
    const Json outputs = fields.contains("outputs") ? objectField(fields, "outputs", path) : Json::object();

    auto prepared = found->prepare(stepName, normalizedInputs, params, outputs, knownSlots, path);
    require(reqVersion == prepared.version, IR_BAD_ARGUMENT,
            path + ".version=" + std::to_string(reqVersion) + " does not match method version " +
                std::to_string(prepared.version));

    return {
        std::move(ref),
        stepName,
        method,
        prepared.version,
        std::move(prepared.outputs),
        std::move(prepared.ops)
    };
}

} // namespace ir::procedures::recipe
