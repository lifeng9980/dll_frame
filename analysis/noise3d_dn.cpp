#include "IR_analysis.h"
#include "api_common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

using namespace ir::api;

namespace
{

static_assert(sizeof(IRNoise3DCalibParams) == 40, "Unexpected IRNoise3DCalibParams size");
static_assert(sizeof(IRNoise3DComputeParams) == 32, "Unexpected IRNoise3DComputeParams size");
#if defined(_WIN64) || defined(__x86_64__) || defined(__LP64__)
static_assert(sizeof(IRNoise3DCalibOutputs) == 64, "Unexpected IRNoise3DCalibOutputs x64 size");
static_assert(sizeof(IRNoise3DComputeOutputs) == 104, "Unexpected IRNoise3DComputeOutputs x64 size");
#endif

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxFramePixels = 10ull * 1000ull * 1000ull;
constexpr size_t kMaxFrameSamples = 1000ull * 1000ull * 1000ull;

struct ValidPixelDn
{
    size_t index;
    int32_t row;
    int32_t col;
};

double elapsedSeconds(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double>(end - begin).count();
}

bool isFiniteArray(const double *values, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (!std::isfinite(values[i]))
            return false;
    }
    return true;
}

bool checkedMulSize(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > (std::numeric_limits<size_t>::max() / a))
        return false;
    *out = a * b;
    return true;
}

bool checkedImageSizes(int32_t frameCount, int32_t rows, int32_t cols,
                       size_t *framePixels, size_t *frameRowCount,
                       size_t *frameColCount, size_t *sampleCount)
{
    size_t nPixels = 0;
    if (!checkedMulSize(static_cast<size_t>(rows), static_cast<size_t>(cols), &nPixels))
        return false;
    if (nPixels > kMaxFramePixels)
        return false;
    size_t tiCount = 0;
    if (!checkedMulSize(static_cast<size_t>(frameCount), static_cast<size_t>(rows), &tiCount))
        return false;
    size_t tjCount = 0;
    if (!checkedMulSize(static_cast<size_t>(frameCount), static_cast<size_t>(cols), &tjCount))
        return false;
    size_t samples = 0;
    if (!checkedMulSize(static_cast<size_t>(frameCount), nPixels, &samples))
        return false;
    if (samples > kMaxFrameSamples)
        return false;
    *framePixels = nPixels;
    *frameRowCount = tiCount;
    *frameColCount = tjCount;
    *sampleCount = samples;
    return true;
}

double sampleStdVector(const std::vector<double> &values)
{
    if (values.size() < 2)
        return 0.0;
    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    const double mean = sum / static_cast<double>(values.size());
    double ss = 0.0;
    for (double value : values)
    {
        const double d = value - mean;
        ss += d * d;
    }
    return std::sqrt(ss / static_cast<double>(values.size() - 1));
}

double sampleStdSum(double sum, double sumSquares, size_t count)
{
    if (count < 2)
        return 0.0;
    double numerator = sumSquares - (sum * sum / static_cast<double>(count));
    if (numerator < 0.0 && numerator > -1.0e-8)
        numerator = 0.0;
    return std::sqrt(numerator / static_cast<double>(count - 1));
}

void addHistogramValue(double value, int32_t histInterval, double minValue, double maxValue,
                       double width, double *hist)
{
    int32_t bin = 0;
    if (maxValue > minValue)
    {
        bin = static_cast<int32_t>((value - minValue) / width);
        if (bin >= histInterval)
            bin = histInterval - 1;
        if (bin < 0)
            bin = 0;
    }
    hist[bin] += 1.0;
}

void buildHistogram(const std::vector<double> &values, int32_t histInterval,
                    double *hist, double *meta)
{
    if (values.empty())
    {
        meta[0] = 0.0;
        meta[1] = 0.0;
        meta[2] = 1.0;
        return;
    }

    const auto limits = std::minmax_element(values.begin(), values.end());
    const double minValue = *limits.first;
    const double maxValue = *limits.second;
    const double width = (maxValue > minValue)
                             ? ((maxValue - minValue) / static_cast<double>(histInterval))
                             : 1.0;

    meta[0] = minValue;
    meta[1] = maxValue;
    meta[2] = width;

    for (double value : values)
    {
        addHistogramValue(value, histInterval, minValue, maxValue, width, hist);
    }
}

template <typename T>
double frameValue(const T *orgFrames, size_t base, size_t pixelIndex,
                  int32_t computeMode, const double *kMap, const double *bMap,
                  double bAvg)
{
    const double org = static_cast<double>(orgFrames[base + pixelIndex]);
    if (computeMode == IR_NOISE3D_MODE_ORG)
        return org;
    return kMap[pixelIndex] * (org - bMap[pixelIndex]) + bAvg;
}

void collectValidPixels(const uint8_t *totalBlindMask, int32_t rows, int32_t cols,
                        std::vector<ValidPixelDn> *validPixels,
                        std::vector<size_t> *rowCounts,
                        std::vector<size_t> *colCounts)
{
    const size_t framePixels = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    validPixels->clear();
    validPixels->reserve(framePixels);
    rowCounts->assign(static_cast<size_t>(rows), 0);
    colCounts->assign(static_cast<size_t>(cols), 0);

    for (int32_t y = 0; y < rows; ++y)
    {
        for (int32_t x = 0; x < cols; ++x)
        {
            const size_t p = static_cast<size_t>(y) * static_cast<size_t>(cols) + static_cast<size_t>(x);
            if (totalBlindMask[p] == 0)
            {
                validPixels->push_back(ValidPixelDn{p, y, x});
                ++(*rowCounts)[static_cast<size_t>(y)];
                ++(*colCounts)[static_cast<size_t>(x)];
            }
        }
    }

    require(validPixels->size() >= 2, IR_COMPUTATION_FAILED, "At least two valid pixels are required");
    for (int32_t y = 0; y < rows; ++y)
    {
        require((*rowCounts)[static_cast<size_t>(y)] > 0, IR_COMPUTATION_FAILED,
                "Each row must contain at least one valid pixel");
    }
    for (int32_t x = 0; x < cols; ++x)
    {
        require((*colCounts)[static_cast<size_t>(x)] > 0, IR_COMPUTATION_FAILED,
                "Each column must contain at least one valid pixel");
    }
}

} // namespace

extern "C" IR_ANALYSIS_API int32_t IR_Noise3DBuildCalibDn(
    const double *mean_high_map,
    const double *mean_low_map,
    const double *rms_map,
    const double *b_map,
    int32_t rows,
    int32_t cols,
    int64_t input_pixel_capacity,
    const IRNoise3DCalibParams *params,
    const IRNoise3DCalibOutputs *outputs,
    char *err,
    int32_t err_bytes)
noexcept{
    return guarded(err, err_bytes, [&] {
        require(params != nullptr && outputs != nullptr, IR_NULL_POINTER,
                "Noise3D calibration params and outputs must not be null");
        const int64_t pixelCountI64 = pixels(rows, cols);
        const size_t pixelCount = static_cast<size_t>(pixelCountI64);
        require(pixelCount <= kMaxFramePixels, IR_BAD_ARGUMENT,
                "rows * cols exceeds supported Noise3D image size");

        needed(mean_high_map, input_pixel_capacity, pixelCountI64);
        needed(mean_low_map, input_pixel_capacity, pixelCountI64);
        needed(rms_map, input_pixel_capacity, pixelCountI64);
        needed(b_map, input_pixel_capacity, pixelCountI64);

        needed(outputs->response_blind_mask, outputs->pixel_capacity, pixelCountI64);
        needed(outputs->rms_blind_mask, outputs->pixel_capacity, pixelCountI64);
        needed(outputs->total_blind_mask, outputs->pixel_capacity, pixelCountI64);
        needed(outputs->valid_mask, outputs->pixel_capacity, pixelCountI64);
        needed(outputs->k_map, outputs->pixel_capacity, pixelCountI64);
        needed(outputs->stats, outputs->stats_capacity, IR_NOISE3D_STATS_COUNT);

        require(std::isfinite(params->response_lower_mul) &&
                    std::isfinite(params->response_upper_mul) &&
                    std::isfinite(params->rms_lower_mul) &&
                    std::isfinite(params->rms_upper_mul) &&
                    std::isfinite(params->k_epsilon),
                IR_BAD_ARGUMENT, "Noise3D calibration threshold arguments must be finite");

        require(isFiniteArray(mean_high_map, pixelCount) &&
                    isFiniteArray(mean_low_map, pixelCount) &&
                    isFiniteArray(rms_map, pixelCount) &&
                    isFiniteArray(b_map, pixelCount),
                IR_BAD_ARGUMENT, "Noise3D calibration input maps must be finite");

        std::fill(outputs->stats, outputs->stats + IR_NOISE3D_STATS_COUNT, 0.0);

        const double rmsMean = std::accumulate(rms_map, rms_map + pixelCount, 0.0) /
                               static_cast<double>(pixelCount);
        const double rmsLow = params->rms_lower_mul * rmsMean;
        const double rmsHigh = params->rms_upper_mul * rmsMean;

        size_t rmsBlindCount = 0;
        for (size_t p = 0; p < pixelCount; ++p)
        {
            const bool blind = (rms_map[p] < rmsLow) || (rms_map[p] > rmsHigh);
            outputs->rms_blind_mask[p] = blind ? 1 : 0;
            outputs->response_blind_mask[p] = 0;
            outputs->total_blind_mask[p] = outputs->rms_blind_mask[p];
            outputs->valid_mask[p] = blind ? 0 : 1;
            if (blind)
                ++rmsBlindCount;
        }

        double responseSum = 0.0;
        size_t responseCount = 0;
        for (size_t p = 0; p < pixelCount; ++p)
        {
            if (outputs->valid_mask[p] != 0)
            {
                responseSum += mean_high_map[p] - mean_low_map[p];
                ++responseCount;
            }
        }
        require(responseCount >= 2, IR_COMPUTATION_FAILED,
                "Not enough pixels remaining after Noise3D RMS filter");

        const double responseMean = responseSum / static_cast<double>(responseCount);
        require(responseMean > 0.0 && std::isfinite(responseMean), IR_BAD_ARGUMENT,
                "responseMean must be positive; check mean_high_map/mean_low_map order");

        const double responseLow = params->response_lower_mul * responseMean;
        const double responseHigh = params->response_upper_mul * responseMean;

        size_t responseBlindCount = 0;
        for (size_t p = 0; p < pixelCount; ++p)
        {
            if (outputs->valid_mask[p] != 0)
            {
                const double response = mean_high_map[p] - mean_low_map[p];
                const bool blind = (response < responseLow) || (response > responseHigh);
                if (blind)
                {
                    outputs->response_blind_mask[p] = 1;
                    outputs->total_blind_mask[p] = 1;
                    outputs->valid_mask[p] = 0;
                    ++responseBlindCount;
                }
            }
        }

        size_t totalBlindCount = 0;
        size_t validCount = 0;
        double bSum = 0.0;
        double kSum = 0.0;
        for (size_t p = 0; p < pixelCount; ++p)
        {
            if (outputs->total_blind_mask[p] != 0)
            {
                outputs->k_map[p] = 0.0;
                ++totalBlindCount;
            }
            else
            {
                const double response = mean_high_map[p] - mean_low_map[p];
                outputs->k_map[p] = responseMean / (response + params->k_epsilon);
                bSum += b_map[p];
                kSum += outputs->k_map[p];
                ++validCount;
            }
        }
        require(validCount >= 2, IR_COMPUTATION_FAILED,
                "Not enough valid pixels remaining after Noise3D calibration filters");

        outputs->stats[IR_NOISE3D_STAT_RESPONSE_MEAN] = responseMean;
        outputs->stats[IR_NOISE3D_STAT_RESPONSE_LOW] = responseLow;
        outputs->stats[IR_NOISE3D_STAT_RESPONSE_HIGH] = responseHigh;
        outputs->stats[IR_NOISE3D_STAT_RESPONSE_BLIND_COUNT] = static_cast<double>(responseBlindCount);
        outputs->stats[IR_NOISE3D_STAT_RESPONSE_BLIND_RATIO] =
            static_cast<double>(responseBlindCount) / static_cast<double>(pixelCount);
        outputs->stats[IR_NOISE3D_STAT_RMS_MEAN] = rmsMean;
        outputs->stats[IR_NOISE3D_STAT_RMS_LOW] = rmsLow;
        outputs->stats[IR_NOISE3D_STAT_RMS_HIGH] = rmsHigh;
        outputs->stats[IR_NOISE3D_STAT_RMS_BLIND_COUNT] = static_cast<double>(rmsBlindCount);
        outputs->stats[IR_NOISE3D_STAT_RMS_BLIND_RATIO] =
            static_cast<double>(rmsBlindCount) / static_cast<double>(pixelCount);
        outputs->stats[IR_NOISE3D_STAT_TOTAL_BLIND_COUNT] = static_cast<double>(totalBlindCount);
        outputs->stats[IR_NOISE3D_STAT_TOTAL_BLIND_RATIO] =
            static_cast<double>(totalBlindCount) / static_cast<double>(pixelCount);
        outputs->stats[IR_NOISE3D_STAT_VALID_COUNT] = static_cast<double>(validCount);
        outputs->stats[IR_NOISE3D_STAT_VALID_RATIO] =
            static_cast<double>(validCount) / static_cast<double>(pixelCount);
        outputs->stats[IR_NOISE3D_STAT_B_AVG] = bSum / static_cast<double>(validCount);
        outputs->stats[IR_NOISE3D_STAT_K_MEAN_VALID] = kSum / static_cast<double>(validCount);
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_Noise3DComputeFramesDn(
    const IRArrayView *org_frames,
    const uint8_t *total_blind_mask,
    int64_t mask_capacity,
    const double *k_map,
    const double *b_map,
    int64_t calib_map_capacity,
    const IRNoise3DComputeParams *params,
    const IRNoise3DComputeOutputs *outputs,
    char *err,
    int32_t err_bytes) noexcept
{
    return guarded(err, err_bytes, [&] {
        require(org_frames != nullptr && params != nullptr && outputs != nullptr, IR_NULL_POINTER,
                "Noise3D compute params and outputs must not be null");
        needed(org_frames->data, 1, 1);
        const int32_t frame_count = org_frames->frames;
        const int32_t rows = org_frames->height;
        const int32_t cols = org_frames->width;
        require(frame_count >= 2 && rows > 0 && cols > 0, IR_BAD_ARGUMENT,
                "frame_count must be >= 2 and rows/cols must be positive");
        size_t elementBytes = 0;
        switch (org_frames->data_type)
        {
        case IR_TYPE_U16: elementBytes = sizeof(uint16_t); break;
        case IR_TYPE_U32: elementBytes = sizeof(uint32_t); break;
        default: throw Error(IR_UNSUPPORTED, "Noise3D supports U16 and U32 input");
        }
        require(params->compute_mode == IR_NOISE3D_MODE_ORG ||
                    params->compute_mode == IR_NOISE3D_MODE_NUC,
                IR_BAD_ARGUMENT, "Noise3D compute_mode is invalid");
        require(params->hist_interval > 0, IR_BAD_ARGUMENT,
                "Noise3D hist_interval must be positive");
        require(std::isfinite(params->invalid_fill), IR_BAD_ARGUMENT,
                "Noise3D invalid_fill must be finite");

        size_t framePixels = 0;
        size_t frameRowCount = 0;
        size_t frameColCount = 0;
        size_t fullSampleCount = 0;
        require(checkedImageSizes(frame_count, rows, cols, &framePixels, &frameRowCount,
                                  &frameColCount, &fullSampleCount),
                IR_BAD_ARGUMENT, "Noise3D image dimensions exceed supported limits");

        require(fullSampleCount <= UINT64_MAX / elementBytes &&
                    org_frames->byte_size >= static_cast<uint64_t>(fullSampleCount) * elementBytes,
                IR_BUFFER_TOO_SMALL, "Noise3D input array byte_size is too small");
        require(reinterpret_cast<uintptr_t>(org_frames->data) % elementBytes == 0,
                IR_BAD_ARGUMENT, "Noise3D input array is not aligned for its data type");
        needed(total_blind_mask, mask_capacity, static_cast<int64_t>(framePixels));

        if (params->compute_mode == IR_NOISE3D_MODE_NUC)
        {
            needed(k_map, calib_map_capacity, static_cast<int64_t>(framePixels));
            needed(b_map, calib_map_capacity, static_cast<int64_t>(framePixels));
            require(std::isfinite(params->b_avg), IR_BAD_ARGUMENT,
                    "Noise3D NUC b_avg must be finite");
            require(isFiniteArray(k_map, framePixels) && isFiniteArray(b_map, framePixels),
                    IR_BAD_ARGUMENT, "Noise3D NUC k_map and b_map must be finite");
        }

        const int64_t histCount =
            static_cast<int64_t>(IR_NOISE3D_HIST_GROUP_COUNT) * static_cast<int64_t>(params->hist_interval);
        needed(outputs->scalars, outputs->scalars_capacity, IR_NOISE3D_SCALAR_COUNT);
        needed(outputs->hist, outputs->hist_capacity, histCount);
        needed(outputs->hist_meta, outputs->hist_meta_capacity, IR_NOISE3D_HIST_GROUP_COUNT * 3);
        needed(outputs->map_ij, outputs->map_ij_capacity, static_cast<int64_t>(framePixels));
        needed(outputs->map_ti, outputs->map_ti_capacity, static_cast<int64_t>(frameRowCount));
        needed(outputs->map_tj, outputs->map_tj_capacity, static_cast<int64_t>(frameColCount));

        if (outputs->meta != nullptr)
        {
            needed(outputs->meta, outputs->meta_capacity, IR_NOISE3D_META_COUNT);
        }

        const auto computeTyped = [&](const auto *typedOrgFrames) {
            const auto totalBegin = Clock::now();
            auto stageBegin = totalBegin;

            std::vector<ValidPixelDn> validPixels;
            std::vector<size_t> rowCounts;
            std::vector<size_t> colCounts;
            collectValidPixels(total_blind_mask, rows, cols, &validPixels, &rowCounts, &colCounts);

            const double maskSeconds = elapsedSeconds(stageBegin, Clock::now());
            stageBegin = Clock::now();

            const size_t validPixelCount = validPixels.size();
            const size_t validSampleCount = validPixelCount * static_cast<size_t>(frame_count);
            std::vector<double> frameRawSums(static_cast<size_t>(frame_count), 0.0);
            std::vector<double> pixelRawSums(framePixels, 0.0);
            double globalSum = 0.0;

            for (int32_t t = 0; t < frame_count; ++t)
            {
                const size_t base = static_cast<size_t>(t) * framePixels;
                double frameSum = 0.0;
                for (const ValidPixelDn &pixel : validPixels)
                {
                    const double value = frameValue(typedOrgFrames, base, pixel.index, params->compute_mode,
                                                    k_map, b_map, params->b_avg);
                    frameSum += value;
                    pixelRawSums[pixel.index] += value;
                }
                frameRawSums[static_cast<size_t>(t)] = frameSum;
                globalSum += frameSum;
            }

            const double globalMean = globalSum / static_cast<double>(validSampleCount);
            const double firstPassSeconds = elapsedSeconds(stageBegin, Clock::now());
            stageBegin = Clock::now();

            std::vector<double> frameMean(static_cast<size_t>(frame_count), 0.0);
            std::vector<double> pixelMean(framePixels, 0.0);
            std::vector<double> rowMean(static_cast<size_t>(rows), 0.0);
            std::vector<double> colMean(static_cast<size_t>(cols), 0.0);
            std::vector<double> mapTI(frameRowCount, 0.0);
            std::vector<double> mapTJ(frameColCount, 0.0);

            for (int32_t t = 0; t < frame_count; ++t)
            {
                frameMean[static_cast<size_t>(t)] =
                    frameRawSums[static_cast<size_t>(t)] / static_cast<double>(validPixelCount) - globalMean;
            }
            for (const ValidPixelDn &pixel : validPixels)
            {
                const double mean = pixelRawSums[pixel.index] / static_cast<double>(frame_count) - globalMean;
                pixelMean[pixel.index] = mean;
                rowMean[static_cast<size_t>(pixel.row)] += mean;
                colMean[static_cast<size_t>(pixel.col)] += mean;
            }
            for (int32_t y = 0; y < rows; ++y)
                rowMean[static_cast<size_t>(y)] /= static_cast<double>(rowCounts[static_cast<size_t>(y)]);
            for (int32_t x = 0; x < cols; ++x)
                colMean[static_cast<size_t>(x)] /= static_cast<double>(colCounts[static_cast<size_t>(x)]);

            for (size_t p = 0; p < framePixels; ++p)
                outputs->map_ij[p] = params->invalid_fill;
            for (const ValidPixelDn &pixel : validPixels)
            {
                outputs->map_ij[pixel.index] = pixelMean[pixel.index] -
                                               rowMean[static_cast<size_t>(pixel.row)] -
                                               colMean[static_cast<size_t>(pixel.col)];
            }

            const double meanBuildSeconds = elapsedSeconds(stageBegin, Clock::now());
            stageBegin = Clock::now();

            for (int32_t t = 0; t < frame_count; ++t)
            {
                const size_t base = static_cast<size_t>(t) * framePixels;
                const size_t tiBase = static_cast<size_t>(t) * static_cast<size_t>(rows);
                const size_t tjBase = static_cast<size_t>(t) * static_cast<size_t>(cols);
                for (const ValidPixelDn &pixel : validPixels)
                {
                    const double value = frameValue(typedOrgFrames, base, pixel.index, params->compute_mode,
                                                    k_map, b_map, params->b_avg) - globalMean;
                    mapTI[tiBase + static_cast<size_t>(pixel.row)] += value;
                    mapTJ[tjBase + static_cast<size_t>(pixel.col)] += value;
                }
                for (int32_t y = 0; y < rows; ++y)
                {
                    const size_t idx = tiBase + static_cast<size_t>(y);
                    mapTI[idx] = mapTI[idx] / static_cast<double>(rowCounts[static_cast<size_t>(y)]) -
                                 frameMean[static_cast<size_t>(t)] - rowMean[static_cast<size_t>(y)];
                    outputs->map_ti[idx] = mapTI[idx];
                }
                for (int32_t x = 0; x < cols; ++x)
                {
                    const size_t idx = tjBase + static_cast<size_t>(x);
                    mapTJ[idx] = mapTJ[idx] / static_cast<double>(colCounts[static_cast<size_t>(x)]) -
                                 frameMean[static_cast<size_t>(t)] - colMean[static_cast<size_t>(x)];
                    outputs->map_tj[idx] = mapTJ[idx];
                }
            }

            const double tiTjSeconds = elapsedSeconds(stageBegin, Clock::now());
            stageBegin = Clock::now();

            std::fill(outputs->scalars, outputs->scalars + IR_NOISE3D_SCALAR_COUNT, 0.0);
            std::fill(outputs->hist, outputs->hist + histCount, 0.0);
            std::fill(outputs->hist_meta, outputs->hist_meta + IR_NOISE3D_HIST_GROUP_COUNT * 3, 0.0);

            outputs->scalars[IR_NOISE3D_SCALAR_DELTA_T] = sampleStdVector(frameMean);
            outputs->scalars[IR_NOISE3D_SCALAR_DELTA_I] = sampleStdVector(rowMean);
            outputs->scalars[IR_NOISE3D_SCALAR_DELTA_J] = sampleStdVector(colMean);

            double ijSum = 0.0;
            double ijSumSquares = 0.0;
            for (const ValidPixelDn &pixel : validPixels)
            {
                const double value = outputs->map_ij[pixel.index];
                ijSum += value;
                ijSumSquares += value * value;
            }
            outputs->scalars[IR_NOISE3D_SCALAR_DELTA_IJ] =
                sampleStdSum(ijSum, ijSumSquares, validPixelCount);

            const double tiSum = std::accumulate(mapTI.begin(), mapTI.end(), 0.0);
            double tiSumSquares = 0.0;
            for (double value : mapTI)
                tiSumSquares += value * value;
            outputs->scalars[IR_NOISE3D_SCALAR_DELTA_TI] =
                sampleStdSum(tiSum, tiSumSquares, mapTI.size());

            const double tjSum = std::accumulate(mapTJ.begin(), mapTJ.end(), 0.0);
            double tjSumSquares = 0.0;
            for (double value : mapTJ)
                tjSumSquares += value * value;
            outputs->scalars[IR_NOISE3D_SCALAR_DELTA_TJ] =
                sampleStdSum(tjSum, tjSumSquares, mapTJ.size());

            double tijSum = 0.0;
            double tijSumSquares = 0.0;
            double tijMin = std::numeric_limits<double>::infinity();
            double tijMax = -std::numeric_limits<double>::infinity();
            for (int32_t t = 0; t < frame_count; ++t)
            {
                const size_t base = static_cast<size_t>(t) * framePixels;
                const size_t tiBase = static_cast<size_t>(t) * static_cast<size_t>(rows);
                const size_t tjBase = static_cast<size_t>(t) * static_cast<size_t>(cols);
                for (const ValidPixelDn &pixel : validPixels)
                {
                    const double centered = frameValue(typedOrgFrames, base, pixel.index, params->compute_mode,
                                                       k_map, b_map, params->b_avg) - globalMean;
                    const double value =
                        centered - frameMean[static_cast<size_t>(t)] -
                        rowMean[static_cast<size_t>(pixel.row)] -
                        colMean[static_cast<size_t>(pixel.col)] -
                        outputs->map_ij[pixel.index] -
                        mapTI[tiBase + static_cast<size_t>(pixel.row)] -
                        mapTJ[tjBase + static_cast<size_t>(pixel.col)];
                    tijSum += value;
                    tijSumSquares += value * value;
                    tijMin = std::min(tijMin, value);
                    tijMax = std::max(tijMax, value);
                }
            }
            outputs->scalars[IR_NOISE3D_SCALAR_DELTA_TIJ] =
                sampleStdSum(tijSum, tijSumSquares, validSampleCount);

            double totalSquares = 0.0;
            for (int32_t i = IR_NOISE3D_SCALAR_DELTA_T; i <= IR_NOISE3D_SCALAR_DELTA_TIJ; ++i)
                totalSquares += outputs->scalars[i] * outputs->scalars[i];
            outputs->scalars[IR_NOISE3D_SCALAR_DELTA_TOTAL] = std::sqrt(totalSquares);
            outputs->scalars[IR_NOISE3D_SCALAR_DELTA_SYS] = std::sqrt(
                outputs->scalars[IR_NOISE3D_SCALAR_DELTA_IJ] * outputs->scalars[IR_NOISE3D_SCALAR_DELTA_IJ] +
                outputs->scalars[IR_NOISE3D_SCALAR_DELTA_TIJ] * outputs->scalars[IR_NOISE3D_SCALAR_DELTA_TIJ]);
            outputs->scalars[IR_NOISE3D_SCALAR_GLOBAL_MEAN] = globalMean;
            outputs->scalars[IR_NOISE3D_SCALAR_VALID_PIXEL_COUNT] = static_cast<double>(validPixelCount);
            outputs->scalars[IR_NOISE3D_SCALAR_VALID_SAMPLE_COUNT] = static_cast<double>(validSampleCount);
            outputs->scalars[IR_NOISE3D_SCALAR_FRAME_COUNT] = static_cast<double>(frame_count);
            outputs->scalars[IR_NOISE3D_SCALAR_ROWS] = static_cast<double>(rows);
            outputs->scalars[IR_NOISE3D_SCALAR_COLS] = static_cast<double>(cols);

            const size_t histBins = static_cast<size_t>(params->hist_interval);
            buildHistogram(frameMean, params->hist_interval,
                           outputs->hist + static_cast<size_t>(IR_NOISE3D_HIST_T) * histBins,
                           outputs->hist_meta + static_cast<size_t>(IR_NOISE3D_HIST_T) * 3);
            buildHistogram(rowMean, params->hist_interval,
                           outputs->hist + static_cast<size_t>(IR_NOISE3D_HIST_I) * histBins,
                           outputs->hist_meta + static_cast<size_t>(IR_NOISE3D_HIST_I) * 3);
            buildHistogram(colMean, params->hist_interval,
                           outputs->hist + static_cast<size_t>(IR_NOISE3D_HIST_J) * histBins,
                           outputs->hist_meta + static_cast<size_t>(IR_NOISE3D_HIST_J) * 3);

            double *tijHist = outputs->hist + static_cast<size_t>(IR_NOISE3D_HIST_TIJ) * histBins;
            double *tijMeta = outputs->hist_meta + static_cast<size_t>(IR_NOISE3D_HIST_TIJ) * 3;
            const double tijWidth = (tijMax > tijMin)
                                        ? ((tijMax - tijMin) / static_cast<double>(params->hist_interval))
                                        : 1.0;
            tijMeta[0] = tijMin;
            tijMeta[1] = tijMax;
            tijMeta[2] = tijWidth;
            for (int32_t t = 0; t < frame_count; ++t)
            {
                const size_t base = static_cast<size_t>(t) * framePixels;
                const size_t tiBase = static_cast<size_t>(t) * static_cast<size_t>(rows);
                const size_t tjBase = static_cast<size_t>(t) * static_cast<size_t>(cols);
                for (const ValidPixelDn &pixel : validPixels)
                {
                    const double centered = frameValue(typedOrgFrames, base, pixel.index, params->compute_mode,
                                                       k_map, b_map, params->b_avg) - globalMean;
                    const double value =
                        centered - frameMean[static_cast<size_t>(t)] -
                        rowMean[static_cast<size_t>(pixel.row)] -
                        colMean[static_cast<size_t>(pixel.col)] -
                        outputs->map_ij[pixel.index] -
                        mapTI[tiBase + static_cast<size_t>(pixel.row)] -
                        mapTJ[tjBase + static_cast<size_t>(pixel.col)];
                    addHistogramValue(value, params->hist_interval, tijMin, tijMax, tijWidth, tijHist);
                }
            }

            require(isFiniteArray(outputs->scalars, IR_NOISE3D_SCALAR_COUNT) &&
                        isFiniteArray(outputs->hist, static_cast<size_t>(histCount)) &&
                        isFiniteArray(outputs->hist_meta, static_cast<size_t>(IR_NOISE3D_HIST_GROUP_COUNT) * 3) &&
                        isFiniteArray(outputs->map_ij, framePixels) &&
                        isFiniteArray(outputs->map_ti, frameRowCount) &&
                        isFiniteArray(outputs->map_tj, frameColCount),
                    IR_COMPUTATION_FAILED, "Noise3D produced non-finite scalar, map, or histogram output");

            const double stdHistSeconds = elapsedSeconds(stageBegin, Clock::now());
            const double totalSeconds = elapsedSeconds(totalBegin, Clock::now());
            if (outputs->meta != nullptr)
            {
                std::fill(outputs->meta, outputs->meta + IR_NOISE3D_META_COUNT, 0.0);
                (void)params->thread_count;
                outputs->meta[IR_NOISE3D_META_EFFECTIVE_THREAD_COUNT] = 1.0;
                outputs->meta[IR_NOISE3D_META_MASK_BUILD_SECONDS] = maskSeconds;
                outputs->meta[IR_NOISE3D_META_FIRST_PASS_SECONDS] = firstPassSeconds;
                outputs->meta[IR_NOISE3D_META_MEAN_BUILD_SECONDS] = meanBuildSeconds;
                outputs->meta[IR_NOISE3D_META_TI_TJ_SECONDS] = tiTjSeconds;
                outputs->meta[IR_NOISE3D_META_STD_HIST_SECONDS] = stdHistSeconds;
                outputs->meta[IR_NOISE3D_META_TOTAL_SECONDS] = totalSeconds;
            }
        };

        if (org_frames->data_type == IR_TYPE_U16)
            computeTyped(static_cast<const uint16_t *>(org_frames->data));
        else
            computeTyped(static_cast<const uint32_t *>(org_frames->data));
    });
}
