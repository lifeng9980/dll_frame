#include "IR_analysis.h"
#include "matrix_view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace ir::api;
using namespace ir::analysis;

namespace
{

static_assert(sizeof(IRMapSummary) == 80, "Unexpected IRMapSummary size");
static_assert(sizeof(IRGjbDriftContext) == 64, "Unexpected IRGjbDriftContext size");
static_assert(sizeof(IRGjbBurstMetrics) == 168, "Unexpected IRGjbBurstMetrics size");

double percentileSorted(const std::vector<double> &sorted, double pct)
{
    if (sorted.size() == 1)
        return sorted[0];
    const double pos = (pct / 100.0) * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = static_cast<size_t>(std::ceil(pos));
    const double frac = pos - static_cast<double>(lo);
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

void spatialBoxLowHighPassInternal(const double *inMap,
                                   const uint8_t *validMask,
                                   int32_t rows,
                                   int32_t cols,
                                   int32_t kernelW,
                                   int32_t kernelH,
                                   double *outLowMap,
                                   double *outHighMap)
{
    require(kernelW >= 1 && kernelH >= 1 && (kernelW % 2 == 1) && (kernelH % 2 == 1),
            IR_BAD_ARGUMENT, "Spatial filter kernel dimensions must be odd integers >= 1");

    const int64_t total = int64_t(rows) * cols;
    double sumValid = 0.0;
    int64_t validCount = 0;
    for (int64_t i = 0; i < total; ++i)
    {
        if (validMask && validMask[i] == 0)
            continue;
        const double v = inMap[i];
        if (!std::isfinite(v))
            continue;
        sumValid += v;
        ++validCount;
    }
    require(validCount > 0, IR_COMPUTATION_FAILED, "No valid finite pixels for spatial box filter");
    const double baseMean = sumValid / static_cast<double>(validCount);

    const size_t stride = static_cast<size_t>(cols) + 1;
    std::vector<double> sumTable(static_cast<size_t>(rows + 1) * stride, 0.0);
    std::vector<int64_t> cntTable(static_cast<size_t>(rows + 1) * stride, 0);

    for (int32_t r = 0; r < rows; ++r)
    {
        double rowSum = 0.0;
        int64_t rowCnt = 0;
        const int64_t inBase = int64_t(r) * cols;
        const size_t curBase = static_cast<size_t>(r + 1) * stride;
        const size_t prevBase = static_cast<size_t>(r) * stride;
        for (int32_t c = 0; c < cols; ++c)
        {
            const int64_t idx = inBase + c;
            const double val = inMap[idx];
            const bool isOk = (!validMask || validMask[idx] != 0) && std::isfinite(val);
            if (isOk)
            {
                rowSum += (val - baseMean);
                ++rowCnt;
            }
            const size_t col1 = static_cast<size_t>(c + 1);
            sumTable[curBase + col1] = sumTable[prevBase + col1] + rowSum;
            cntTable[curBase + col1] = cntTable[prevBase + col1] + rowCnt;
        }
    }

    const int32_t ry = kernelH / 2;
    const int32_t rx = kernelW / 2;
    for (int32_t r = 0; r < rows; ++r)
    {
        const int32_t r0 = std::max(0, r - ry);
        const int32_t r1 = std::min(rows - 1, r + ry) + 1;
        const size_t topBase = static_cast<size_t>(r0) * stride;
        const size_t botBase = static_cast<size_t>(r1) * stride;
        const int64_t outBase = int64_t(r) * cols;

        for (int32_t c = 0; c < cols; ++c)
        {
            const int32_t c0 = std::max(0, c - rx);
            const int32_t c1 = std::min(cols - 1, c + rx) + 1;
            const int64_t cnt = cntTable[botBase + c1] - cntTable[topBase + c1] -
                                cntTable[botBase + c0] + cntTable[topBase + c0];
            const double sum = sumTable[botBase + c1] - sumTable[topBase + c1] -
                               sumTable[botBase + c0] + sumTable[topBase + c0];
            const double low = (cnt > 0) ? (baseMean + sum / static_cast<double>(cnt)) : 0.0;
            const int64_t idx = outBase + c;
            if (outLowMap)
                outLowMap[idx] = low;
            if (outHighMap)
            {
                const double v = inMap[idx];
                const bool isOk = (!validMask || validMask[idx] != 0) && std::isfinite(v) && cnt > 0;
                outHighMap[idx] = isOk ? (v - low) : 0.0;
            }
        }
    }
}

std::vector<double> buildGaussianKernel1D(int32_t k)
{
    if (k <= 1)
        return {1.0};
    const int32_t radius = k / 2;
    const double sigma = 0.3 * (static_cast<double>(k - 1) * 0.5 - 1.0) + 0.8;
    const double invTwoSigmaSq = 1.0 / (2.0 * sigma * sigma);
    std::vector<double> weights(static_cast<size_t>(k), 0.0);
    double sum = 0.0;
    for (int32_t d = -radius; d <= radius; ++d)
    {
        const double w = std::exp(-static_cast<double>(d) * static_cast<double>(d) * invTwoSigmaSq);
        weights[static_cast<size_t>(d + radius)] = w;
        sum += w;
    }
    for (double &w : weights)
        w /= sum;
    return weights;
}

void spatialGaussianLowHighPassInternal(const double *inMap,
                                        const uint8_t *validMask,
                                        int32_t rows,
                                        int32_t cols,
                                        int32_t kernelW,
                                        int32_t kernelH,
                                        double *outLowMap,
                                        double *outHighMap)
{
    require(kernelW >= 1 && kernelH >= 1 && (kernelW % 2 == 1) && (kernelH % 2 == 1),
            IR_BAD_ARGUMENT, "Spatial filter kernel dimensions must be odd integers >= 1");

    const int64_t total = int64_t(rows) * cols;
    double sumValid = 0.0;
    int64_t validCount = 0;
    for (int64_t i = 0; i < total; ++i)
    {
        if (validMask && validMask[i] == 0)
            continue;
        const double v = inMap[i];
        if (!std::isfinite(v))
            continue;
        sumValid += v;
        ++validCount;
    }
    require(validCount > 0, IR_COMPUTATION_FAILED, "No valid finite pixels for spatial Gaussian filter");
    const double baseMean = sumValid / static_cast<double>(validCount);

    const auto wx = buildGaussianKernel1D(kernelW);
    const auto wy = buildGaussianKernel1D(kernelH);
    const int32_t rx = kernelW / 2;
    const int32_t ry = kernelH / 2;

    std::vector<double> hNum(static_cast<size_t>(total), 0.0);
    std::vector<double> hDen(static_cast<size_t>(total), 0.0);

    for (int32_t r = 0; r < rows; ++r)
    {
        const int64_t rowBase = int64_t(r) * cols;
        for (int32_t c = 0; c < cols; ++c)
        {
            const int32_t cMin = std::max(0, c - rx);
            const int32_t cMax = std::min(cols - 1, c + rx);
            double accNum = 0.0;
            double accDen = 0.0;
            for (int32_t nc = cMin; nc <= cMax; ++nc)
            {
                const int64_t idx = rowBase + nc;
                if (validMask && validMask[idx] == 0)
                    continue;
                const double val = inMap[idx];
                if (!std::isfinite(val))
                    continue;
                const double w = wx[static_cast<size_t>(nc - c + rx)];
                accNum += w * (val - baseMean);
                accDen += w;
            }
            const size_t outIdx = static_cast<size_t>(rowBase + c);
            hNum[outIdx] = accNum;
            hDen[outIdx] = accDen;
        }
    }

    for (int32_t r = 0; r < rows; ++r)
    {
        const int32_t rMin = std::max(0, r - ry);
        const int32_t rMax = std::min(rows - 1, r + ry);
        const int64_t outBase = int64_t(r) * cols;
        for (int32_t c = 0; c < cols; ++c)
        {
            double accNum = 0.0;
            double accDen = 0.0;
            for (int32_t nr = rMin; nr <= rMax; ++nr)
            {
                const size_t srcIdx = static_cast<size_t>(int64_t(nr) * cols + c);
                const double w = wy[static_cast<size_t>(nr - r + ry)];
                accNum += w * hNum[srcIdx];
                accDen += w * hDen[srcIdx];
            }
            const double low = (accDen > 0.0) ? (baseMean + accNum / accDen) : 0.0;
            const int64_t idx = outBase + c;
            if (outLowMap)
                outLowMap[idx] = low;
            if (outHighMap)
            {
                const double v = inMap[idx];
                const bool isOk = (!validMask || validMask[idx] != 0) && std::isfinite(v) && accDen > 0.0;
                outHighMap[idx] = isOk ? (v - low) : 0.0;
            }
        }
    }
}

struct MaskedStats
{
    double mean = 0.0;
    double popStd = 0.0;
    double sampleStd = 0.0;
    double minVal = 0.0;
    double maxVal = 0.0;
    int64_t count = 0;
};

MaskedStats computeMaskedStats(const double *map, const uint8_t *validMask, int64_t total)
{
    MaskedStats st;
    double sum = 0.0;
    st.minVal = std::numeric_limits<double>::infinity();
    st.maxVal = -std::numeric_limits<double>::infinity();

    for (int64_t i = 0; i < total; ++i)
    {
        if (validMask && validMask[i] == 0)
            continue;
        const double v = map[i];
        if (!std::isfinite(v))
            continue;
        sum += v;
        st.minVal = std::min(st.minVal, v);
        st.maxVal = std::max(st.maxVal, v);
        ++st.count;
    }
    require(st.count > 0, IR_COMPUTATION_FAILED, "No valid finite pixels in map");
    st.mean = sum / static_cast<double>(st.count);

    double m2 = 0.0;
    for (int64_t i = 0; i < total; ++i)
    {
        if (validMask && validMask[i] == 0)
            continue;
        const double v = map[i];
        if (!std::isfinite(v))
            continue;
        const double d = v - st.mean;
        m2 += d * d;
    }
    st.popStd = std::sqrt(m2 / static_cast<double>(st.count));
    st.sampleStd = (st.count >= 2) ? std::sqrt(m2 / static_cast<double>(st.count - 1)) : 0.0;
    return st;
}

} // namespace

extern "C" IR_ANALYSIS_API int32_t IR_BuildTwoPointNucF64(
    const double *v1_map,
    const double *v2_map,
    const uint8_t *valid_mask,
    const double *single_point_ref_map,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    double *out_k_map,
    double *out_b_map,
    double *out_summary,
    int32_t summary_count,
    char *err,
    int32_t err_bytes)
{
    return guarded(err, err_bytes, [&] {
        const int64_t n = pixels(rows, cols);
        needed(v1_map, pixel_capacity, n);
        needed(v2_map, pixel_capacity, n);
        if (valid_mask)
            needed(valid_mask, pixel_capacity, n);
        if (single_point_ref_map)
            needed(single_point_ref_map, pixel_capacity, n);
        needed(out_k_map, pixel_capacity, n);
        needed(out_b_map, pixel_capacity, n);
        if (out_summary)
            needed(out_summary, summary_count, 4);

        double sum1 = 0.0;
        double sum2 = 0.0;
        double sumRef = 0.0;
        int64_t validCount = 0;
        for (int64_t i = 0; i < n; ++i)
        {
            if (valid_mask && valid_mask[i] == 0)
                continue;
            const double a = v1_map[i];
            const double b = v2_map[i];
            if (!std::isfinite(a) || !std::isfinite(b))
                continue;
            if (single_point_ref_map && !std::isfinite(single_point_ref_map[i]))
                continue;
            sum1 += a;
            sum2 += b;
            if (single_point_ref_map)
                sumRef += single_point_ref_map[i];
            ++validCount;
        }
        require(validCount > 0, IR_COMPUTATION_FAILED, "No valid finite pixels for two-point NUC");

        const double mean1 = sum1 / static_cast<double>(validCount);
        const double mean2 = sum2 / static_cast<double>(validCount);
        const double meanRef = single_point_ref_map ? (sumRef / static_cast<double>(validCount)) : mean1;
        const double diffMean = mean1 - mean2;

        for (int64_t i = 0; i < n; ++i)
        {
            const bool isOk = (!valid_mask || valid_mask[i] != 0) &&
                              std::isfinite(v1_map[i]) && std::isfinite(v2_map[i]) &&
                              (!single_point_ref_map || std::isfinite(single_point_ref_map[i]));
            if (!isOk)
            {
                out_k_map[i] = 0.0;
                out_b_map[i] = 0.0;
                continue;
            }
            const double k = safeDivide(diffMean, v1_map[i] - v2_map[i]);
            const double b = single_point_ref_map
                                 ? (meanRef - k * single_point_ref_map[i])
                                 : (mean1 - k * v1_map[i]);
            out_k_map[i] = k;
            out_b_map[i] = b;
        }

        if (out_summary)
        {
            out_summary[0] = mean1;
            out_summary[1] = mean2;
            out_summary[2] = meanRef;
            out_summary[3] = static_cast<double>(validCount);
        }
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_ApplyAffineMapF64(
    const double *in_map,
    const double *k_map,
    const double *b_map,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    double *out_map,
    char *err,
    int32_t err_bytes)
{
    return guarded(err, err_bytes, [&] {
        const int64_t n = pixels(rows, cols);
        needed(in_map, pixel_capacity, n);
        needed(k_map, pixel_capacity, n);
        needed(b_map, pixel_capacity, n);
        needed(out_map, pixel_capacity, n);
        for (int64_t i = 0; i < n; ++i)
            out_map[i] = k_map[i] * in_map[i] + b_map[i];
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_FrameStatisticsNucU16(
    const uint16_t *frames,
    int64_t frame_elements,
    int32_t frame_count,
    int32_t rows,
    int32_t cols,
    int32_t sample_std,
    const double *k_map,
    const double *b_map,
    const double *dark_map,
    const uint8_t *valid_mask,
    double *mean_map,
    double *std_map,
    int64_t pixel_capacity,
    double *summary,
    int32_t summary_count,
    char *err,
    int32_t err_bytes)
{
    return guarded(err, err_bytes, [&] {
        const int64_t n = pixels(rows, cols);
        needed(frames, frame_elements, stackSize(n, frame_count));
        require((sample_std == 0 || sample_std == 1) && frame_count > sample_std,
                IR_BAD_ARGUMENT, "Invalid standard deviation convention or frame count");
        require((k_map == nullptr && b_map == nullptr) || (k_map != nullptr && b_map != nullptr),
                IR_BAD_ARGUMENT, "k_map and b_map must both be null or both non-null");
        if (k_map)
        {
            needed(k_map, pixel_capacity, n);
            needed(b_map, pixel_capacity, n);
        }
        if (dark_map)
            needed(dark_map, pixel_capacity, n);
        if (valid_mask)
            needed(valid_mask, pixel_capacity, n);
        needed(mean_map, pixel_capacity, n);
        needed(std_map, pixel_capacity, n);
        needed(summary, summary_count, 2);

        const bool useNuc = (k_map != nullptr);
        std::vector<double> m(static_cast<size_t>(n), 0.0);
        std::vector<double> m2(static_cast<size_t>(n), 0.0);

        for (int32_t f = 0; f < frame_count; ++f)
        {
            const int64_t base = int64_t(f) * n;
            const double invCount = 1.0 / static_cast<double>(f + 1);
            for (int64_t p = 0; p < n; ++p)
            {
                double val = static_cast<double>(frames[base + p]);
                if (useNuc)
                    val = k_map[p] * val + b_map[p];
                if (dark_map)
                    val -= dark_map[p];
                const size_t idx = static_cast<size_t>(p);
                const double delta = val - m[idx];
                m[idx] += delta * invCount;
                m2[idx] += delta * (val - m[idx]);
            }
        }

        const double denom = static_cast<double>(frame_count - sample_std);
        double sumMean = 0.0;
        double sumStd = 0.0;
        int64_t validCount = 0;
        for (int64_t p = 0; p < n; ++p)
        {
            const size_t idx = static_cast<size_t>(p);
            const double meanVal = m[idx];
            const double stdVal = std::sqrt(std::max(0.0, m2[idx] / denom));
            mean_map[p] = meanVal;
            std_map[p] = stdVal;
            if ((!valid_mask || valid_mask[p] != 0) && std::isfinite(meanVal) && std::isfinite(stdVal))
            {
                sumMean += meanVal;
                sumStd += stdVal;
                ++validCount;
            }
        }
        require(validCount > 0, IR_COMPUTATION_FAILED, "No valid finite pixels for burst statistics");
        summary[0] = sumMean / static_cast<double>(validCount);
        summary[1] = sumStd / static_cast<double>(validCount);
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_MapSummaryF64(
    const double *map,
    const uint8_t *valid_mask,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t roi_x,
    int32_t roi_y,
    int32_t roi_w,
    int32_t roi_h,
    double p_low,
    double p_high,
    IRMapSummary *summary,
    char *err,
    int32_t err_bytes)
{
    if (summary)
        *summary = IRMapSummary{};
    return guarded(err, err_bytes, [&] {
        const int64_t n = pixels(rows, cols);
        needed(map, pixel_capacity, n);
        if (valid_mask)
            needed(valid_mask, pixel_capacity, n);
        require(summary != nullptr, IR_NULL_POINTER, "summary must not be null");
        require(std::isfinite(p_low) && std::isfinite(p_high) &&
                    p_low >= 0.0 && p_low <= p_high && p_high <= 100.0,
                IR_BAD_ARGUMENT, "Percentiles must satisfy 0 <= p_low <= p_high <= 100");

        if (roi_w == 0 && roi_h == 0)
        {
            roi_x = 0;
            roi_y = 0;
            roi_w = cols;
            roi_h = rows;
        }
        else
        {
            require(roi_x >= 0 && roi_y >= 0 && roi_w > 0 && roi_h > 0 &&
                        roi_x <= cols - roi_w && roi_y <= rows - roi_h,
                    IR_BAD_ARGUMENT, "ROI is outside the image geometry");
        }

        std::vector<double> values;
        values.reserve(static_cast<size_t>(int64_t(roi_w) * roi_h));
        double sum = 0.0;
        for (int32_t r = roi_y; r < roi_y + roi_h; ++r)
        {
            const int64_t rowBase = int64_t(r) * cols;
            for (int32_t c = roi_x; c < roi_x + roi_w; ++c)
            {
                const int64_t idx = rowBase + c;
                if (valid_mask && valid_mask[idx] == 0)
                    continue;
                const double v = map[idx];
                if (!std::isfinite(v))
                    continue;
                values.push_back(v);
                sum += v;
            }
        }
        require(!values.empty(), IR_COMPUTATION_FAILED, "No valid finite pixels in ROI");

        const int64_t count = static_cast<int64_t>(values.size());
        const double mean = sum / static_cast<double>(count);
        double m2 = 0.0;
        for (double v : values)
        {
            const double d = v - mean;
            m2 += d * d;
        }
        std::sort(values.begin(), values.end());

        IRMapSummary out{};
        out.mean = mean;
        out.pop_std = std::sqrt(m2 / static_cast<double>(count));
        out.sample_std = (count >= 2) ? std::sqrt(m2 / static_cast<double>(count - 1)) : 0.0;
        out.min_val = values.front();
        out.max_val = values.back();
        out.span = out.max_val - out.min_val;
        out.percentile_low = percentileSorted(values, p_low);
        out.percentile_high = percentileSorted(values, p_high);
        out.percentile_span = out.percentile_high - out.percentile_low;
        out.valid_count = count;
        *summary = out;
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_SpatialLowHighPassF64(
    const double *in_map,
    const uint8_t *valid_mask,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t filter_kind,
    int32_t kernel_w,
    int32_t kernel_h,
    double *out_low_map,
    double *out_high_map,
    char *err,
    int32_t err_bytes)
{
    return guarded(err, err_bytes, [&] {
        const int64_t n = pixels(rows, cols);
        needed(in_map, pixel_capacity, n);
        if (valid_mask)
            needed(valid_mask, pixel_capacity, n);
        require(filter_kind == IR_SPATIAL_FILTER_BOX_MEAN || filter_kind == IR_SPATIAL_FILTER_GAUSSIAN,
                IR_BAD_ARGUMENT, "Unsupported spatial filter kind");
        require(out_low_map != nullptr || out_high_map != nullptr, IR_NULL_POINTER,
                "At least one of out_low_map or out_high_map must be non-null");
        if (out_low_map)
            needed(out_low_map, pixel_capacity, n);
        if (out_high_map)
            needed(out_high_map, pixel_capacity, n);

        if (filter_kind == IR_SPATIAL_FILTER_GAUSSIAN)
        {
            spatialGaussianLowHighPassInternal(in_map, valid_mask, rows, cols,
                                               kernel_w, kernel_h, out_low_map, out_high_map);
        }
        else
        {
            spatialBoxLowHighPassInternal(in_map, valid_mask, rows, cols,
                                          kernel_w, kernel_h, out_low_map, out_high_map);
        }
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_MapBinaryOpF64(
    const double *a_map,
    const double *b_map,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t op,
    double param,
    double *out_map,
    char *err,
    int32_t err_bytes)
{
    return guarded(err, err_bytes, [&] {
        const int64_t n = pixels(rows, cols);
        needed(a_map, pixel_capacity, n);
        needed(b_map, pixel_capacity, n);
        needed(out_map, pixel_capacity, n);
        require(std::isfinite(param), IR_BAD_ARGUMENT, "op param must be finite");
        require(op >= IR_MAP_OP_SUB && op <= IR_MAP_OP_VARIANCE_SUBTRACT,
                IR_BAD_ARGUMENT, "Unsupported map binary op");
        if (op == IR_MAP_OP_VARIANCE_SUBTRACT)
            require(param >= 0.0, IR_BAD_ARGUMENT, "variance subtraction param must be >= 0");

        for (int64_t i = 0; i < n; ++i)
        {
            const double a = a_map[i];
            const double b = b_map[i];
            switch (op)
            {
            case IR_MAP_OP_SUB:
                out_map[i] = a - param * b;
                break;
            case IR_MAP_OP_ADD:
                out_map[i] = a + param * b;
                break;
            case IR_MAP_OP_MUL:
                out_map[i] = param * a * b;
                break;
            case IR_MAP_OP_SAFE_DIV:
                out_map[i] = safeDivide(a, b) * param;
                break;
            case IR_MAP_OP_VARIANCE_SUBTRACT:
            {
                const double diff = a * a - param * b * b;
                out_map[i] = std::sqrt(std::max(0.0, diff));
                break;
            }
            default:
                break;
            }
        }
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_ThresholdMaskF64(
    const double *map,
    const uint8_t *base_valid_mask,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t mode,
    double low_param,
    double high_param,
    uint8_t *out_pass_mask,
    int32_t *out_pass_count,
    int32_t *out_fail_count,
    char *err,
    int32_t err_bytes)
{
    if (out_pass_count)
        *out_pass_count = 0;
    if (out_fail_count)
        *out_fail_count = 0;
    return guarded(err, err_bytes, [&] {
        const int64_t n = pixels(rows, cols);
        needed(map, pixel_capacity, n);
        if (base_valid_mask)
            needed(base_valid_mask, pixel_capacity, n);
        needed(out_pass_mask, pixel_capacity, n);
        require(mode >= IR_THRESH_ABSOLUTE && mode <= IR_THRESH_LOWER_ONLY,
                IR_BAD_ARGUMENT, "Unsupported threshold mode");
        require(std::isfinite(low_param) && std::isfinite(high_param),
                IR_BAD_ARGUMENT, "Threshold parameters must be finite");

        double lower = low_param;
        double upper = high_param;
        if (mode == IR_THRESH_ABSOLUTE)
        {
            require(lower <= upper, IR_BAD_ARGUMENT, "Absolute threshold requires low_param <= high_param");
        }
        else if (mode == IR_THRESH_MEAN_MULTIPLIER)
        {
            const MaskedStats st = computeMaskedStats(map, base_valid_mask, n);
            lower = low_param * st.mean;
            upper = high_param * st.mean;
        }
        else if (mode == IR_THRESH_MEAN_SIGMA)
        {
            const MaskedStats st = computeMaskedStats(map, base_valid_mask, n);
            lower = st.mean - low_param * st.popStd;
            upper = st.mean + high_param * st.popStd;
        }

        int32_t passCount = 0;
        int32_t failCount = 0;
        for (int64_t i = 0; i < n; ++i)
        {
            const double v = map[i];
            bool pass = (!base_valid_mask || base_valid_mask[i] != 0) && std::isfinite(v);
            if (pass)
            {
                if (mode == IR_THRESH_UPPER_ONLY)
                    pass = (v <= upper);
                else if (mode == IR_THRESH_LOWER_ONLY)
                    pass = (v >= lower);
                else
                    pass = (v >= lower && v <= upper);
            }
            out_pass_mask[i] = pass ? 1 : 0;
            if (pass)
                ++passCount;
            else
                ++failCount;
        }

        if (out_pass_count)
            *out_pass_count = passCount;
        if (out_fail_count)
            *out_fail_count = failCount;
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_MaskCombine(
    const uint8_t *mask_a,
    const uint8_t *mask_b,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t op,
    uint8_t *out_mask,
    int32_t *out_nonzero_count,
    char *err,
    int32_t err_bytes)
{
    if (out_nonzero_count)
        *out_nonzero_count = 0;
    return guarded(err, err_bytes, [&] {
        const int64_t n = pixels(rows, cols);
        needed(mask_a, pixel_capacity, n);
        require(op >= IR_MASK_AND && op <= IR_MASK_NOT, IR_BAD_ARGUMENT, "Unsupported mask op");
        if (op != IR_MASK_NOT)
            needed(mask_b, pixel_capacity, n);
        needed(out_mask, pixel_capacity, n);

        int32_t nonzeroCount = 0;
        for (int64_t i = 0; i < n; ++i)
        {
            const bool a = (mask_a[i] != 0);
            const bool b = (op != IR_MASK_NOT) && (mask_b[i] != 0);
            bool res = false;
            switch (op)
            {
            case IR_MASK_AND:
                res = a && b;
                break;
            case IR_MASK_OR:
                res = a || b;
                break;
            case IR_MASK_AND_NOT:
                res = a && !b;
                break;
            case IR_MASK_NOT:
                res = !a;
                break;
            default:
                break;
            }
            out_mask[i] = res ? 1 : 0;
            if (res)
                ++nonzeroCount;
        }
        if (out_nonzero_count)
            *out_nonzero_count = nonzeroCount;
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_GjbEvaluateBurstMetrics(
    const double *mean_map,
    const double *vn_map,
    const uint8_t *valid_mask,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    const IRGjbDriftContext *ctx,
    IRGjbBurstMetrics *metrics,
    char *err,
    int32_t err_bytes)
{
    if (metrics)
        *metrics = IRGjbBurstMetrics{};
    return guarded(err, err_bytes, [&] {
        const int64_t n = pixels(rows, cols);
        needed(mean_map, pixel_capacity, n);
        needed(vn_map, pixel_capacity, n);
        if (valid_mask)
            needed(valid_mask, pixel_capacity, n);
        require(ctx != nullptr && metrics != nullptr, IR_NULL_POINTER,
                "ctx and metrics must not be null");
        require(std::isfinite(ctx->volts_per_count) && std::isfinite(ctx->delta_t_k) &&
                    std::isfinite(ctx->ref_response_count) && std::isfinite(ctx->ref_responsivity) &&
                    std::isfinite(ctx->vmax_per_count) && std::isfinite(ctx->noise_subtract_factor),
                IR_BAD_ARGUMENT, "Drift context values must be finite");
        require(ctx->burst_frame_count >= 1, IR_BAD_ARGUMENT, "burst_frame_count must be >= 1");
        require(ctx->noise_subtract_mode == IR_NOISE_SUBTRACT_NONE ||
                    ctx->noise_subtract_mode == IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE,
                IR_BAD_ARGUMENT, "Unsupported noise_subtract_mode");
        require(ctx->noise_subtract_factor >= 0.0, IR_BAD_ARGUMENT,
                "noise_subtract_factor must be >= 0");
        require(ctx->lrnu_kernel_size <= 1 || (ctx->lrnu_kernel_size % 2 == 1),
                IR_BAD_ARGUMENT, "lrnu_kernel_size must be <= 1 or an odd integer >= 3");

        double sumMean = 0.0;
        double sumVn = 0.0;
        double sumVnSq = 0.0;
        double minMean = std::numeric_limits<double>::infinity();
        double maxMean = -std::numeric_limits<double>::infinity();
        int64_t validCount = 0;

        for (int64_t i = 0; i < n; ++i)
        {
            if (valid_mask && valid_mask[i] == 0)
                continue;
            const double m = mean_map[i];
            const double vn = vn_map[i];
            if (!std::isfinite(m) || !std::isfinite(vn))
                continue;
            sumMean += m;
            sumVn += vn;
            sumVnSq += vn * vn;
            minMean = std::min(minMean, m);
            maxMean = std::max(maxMean, m);
            ++validCount;
        }
        require(validCount > 0, IR_COMPUTATION_FAILED, "No valid finite pixels in burst maps");

        const double meanDn = sumMean / static_cast<double>(validCount);
        const double avgVnDn = sumVn / static_cast<double>(validCount);
        const double meanVnSq = sumVnSq / static_cast<double>(validCount);
        const double rmsVnDn = std::sqrt(std::max(0.0, meanVnSq));

        double spatialM2 = 0.0;
        for (int64_t i = 0; i < n; ++i)
        {
            if (valid_mask && valid_mask[i] == 0)
                continue;
            const double m = mean_map[i];
            const double vn = vn_map[i];
            if (!std::isfinite(m) || !std::isfinite(vn))
                continue;
            const double d = m - meanDn;
            spatialM2 += d * d;
        }
        const double spatialStdDn = std::sqrt(spatialM2 / static_cast<double>(validCount));

        double spatialStdDenoisedDn = spatialStdDn;
        if (ctx->noise_subtract_mode == IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE)
        {
            const double temporalVarTerm =
                ctx->noise_subtract_factor * meanVnSq / static_cast<double>(ctx->burst_frame_count);
            spatialStdDenoisedDn = std::sqrt(std::max(0.0, spatialStdDn * spatialStdDn - temporalVarTerm));
        }

        IRGjbBurstMetrics out{};
        out.mean_dn = meanDn;
        out.mean_v = meanDn * ctx->volts_per_count;
        out.span_dn = maxMean - minMean;
        out.spatial_std_dn = spatialStdDn;
        out.spatial_std_v = spatialStdDn * ctx->volts_per_count;
        out.spatial_std_denoised_dn = spatialStdDenoisedDn;
        out.spatial_std_denoised_v = spatialStdDenoisedDn * ctx->volts_per_count;
        out.nonuniformity_pct =
            (meanDn != 0.0) ? (100.0 * spatialStdDn / std::abs(meanDn)) : 0.0;
        out.avg_temporal_noise_dn = avgVnDn;
        out.avg_temporal_noise_mv = avgVnDn * ctx->volts_per_count * 1000.0;
        out.rms_temporal_noise_dn = rmsVnDn;
        out.estimated_netd_mk = (ctx->ref_response_count > 0.0 && ctx->delta_t_k > 0.0)
                                    ? (1000.0 * ctx->delta_t_k * avgVnDn / ctx->ref_response_count)
                                    : 0.0;
        const double avgNoiseV = avgVnDn * ctx->volts_per_count;
        out.estimated_nep_w =
            (ctx->ref_responsivity > 0.0) ? (avgNoiseV / ctx->ref_responsivity) : 0.0;
        out.spatial_noise = out.avg_temporal_noise_mv * out.nonuniformity_pct;
        out.max_dynamic_range_db =
            (ctx->vmax_per_count > 0.0 && avgNoiseV > 0.0)
                ? 20.0 * std::log10((ctx->vmax_per_count * ctx->volts_per_count) / avgNoiseV)
                : 0.0;
        out.valid_pixel_count = validCount;

        if (ctx->lrnu_kernel_size >= 3)
        {
            std::vector<uint8_t> combinedMask(static_cast<size_t>(n), 0);
            for (int64_t i = 0; i < n; ++i)
            {
                const bool isOk = (!valid_mask || valid_mask[i] != 0) &&
                                  std::isfinite(mean_map[i]) && std::isfinite(vn_map[i]);
                combinedMask[static_cast<size_t>(i)] = isOk ? 1 : 0;
            }
            std::vector<double> lowMap(static_cast<size_t>(n), 0.0);
            std::vector<double> highMap(static_cast<size_t>(n), 0.0);
            spatialBoxLowHighPassInternal(mean_map, combinedMask.data(), rows, cols,
                                          ctx->lrnu_kernel_size, ctx->lrnu_kernel_size,
                                          lowMap.data(), highMap.data());
            const MaskedStats lowSt = computeMaskedStats(lowMap.data(), combinedMask.data(), n);
            const MaskedStats highSt = computeMaskedStats(highMap.data(), combinedMask.data(), n);
            out.lrnu_std_dn = lowSt.popStd;
            out.lrnu_span_dn = lowSt.maxVal - lowSt.minVal;
            out.lrnu_pct = (meanDn != 0.0) ? (100.0 * lowSt.popStd / std::abs(meanDn)) : 0.0;
            out.hrnu_std_dn = highSt.popStd;
            out.hrnu_pct = (meanDn != 0.0) ? (100.0 * highSt.popStd / std::abs(meanDn)) : 0.0;
        }

        *metrics = out;
    });
}
