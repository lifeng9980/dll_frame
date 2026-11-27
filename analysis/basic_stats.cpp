#include "IR_analysis.h"
#include "api_common.h"
#include "matrix_view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace ir::api;
using ir::analysis::dispatchRawArrayView;
using ir::analysis::validateRawArrayView;

namespace
{

static_assert(sizeof(IRFlickerSummary) == 48, "Unexpected IRFlickerSummary size");

template <typename T>
void frameStatisticsCore(const T *frames, int32_t frameCount, int64_t pixelCount,
                         int32_t sampleStd, double *means, double *stds,
                         double *summary)
{
    std::fill_n(means, static_cast<size_t>(pixelCount), 0.0);
    std::fill_n(stds, static_cast<size_t>(pixelCount), 0.0);
    for (int32_t frame = 0; frame < frameCount; ++frame)
    {
        const int64_t frameOffset = int64_t(frame) * pixelCount;
        for (int64_t pixel = 0; pixel < pixelCount; ++pixel)
        {
            const double value = static_cast<double>(frames[frameOffset + pixel]);
            const size_t index = static_cast<size_t>(pixel);
            const double delta = value - means[index];
            means[index] += delta / (frame + 1);
            stds[index] += delta * (value - means[index]);
        }
    }

    double average = 0.0;
    double averageStd = 0.0;
    for (int64_t pixel = 0; pixel < pixelCount; ++pixel)
    {
        const size_t index = static_cast<size_t>(pixel);
        stds[index] = std::sqrt(std::max(0.0, stds[index] / (frameCount - sampleStd)));
        average += means[index] / pixelCount;
        averageStd += stds[index] / pixelCount;
    }
    summary[0] = average;
    summary[1] = averageStd;
}

} // namespace

extern "C" const char *IR_AnalysisVersion(void)
noexcept{
    return IR_ANALYSIS_VERSION;
}

// Welford 更新逐像元均值和平方差，避免高直流背景下的减法消差。
extern "C" IR_ANALYSIS_API int32_t IR_FrameStatistics(
    const IRArrayView *input, int32_t sample, double *means, double *stds,
    int64_t capacity, double *summary, int32_t sn, char *err, int32_t en) noexcept
{
    return guarded(err, en, [&] {
        const auto n = validateRawArrayView(input);
        require((sample == 0 || sample == 1) && input->frames > sample, IR_BAD_ARGUMENT,
                "Invalid standard deviation convention or frame count");
        needed(means, capacity, n);
        needed(stds, capacity, n);
        needed(summary, sn, 2);
        require(means != stds, IR_BAD_ARGUMENT,
                "mean_map and std_map must use separate buffers");
        dispatchRawArrayView(*input, [&](const auto *frames) {
            frameStatisticsCore(frames, input->frames, n, sample, means, stds, summary);
        });
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_ROIMean(
    const IRArrayView *input, int32_t x, int32_t y, int32_t roiWidth, int32_t roiHeight,
    double *meanDn, char *err, int32_t errBytes) noexcept
{
    if (meanDn) *meanDn = 0.0;
    return guarded(err, errBytes, [&] {
        const auto pixelCount = validateRawArrayView(input);
        const int32_t frameCount = input->frames;
        const int32_t rows = input->height;
        const int32_t cols = input->width;
        needed(meanDn, 1, 1);
        require(x >= 0 && y >= 0 && roiWidth > 0 && roiHeight > 0 &&
                x <= cols - roiWidth && y <= rows - roiHeight,
                IR_BAD_ARGUMENT, "ROI is outside the image geometry");

        dispatchRawArrayView(*input, [&](const auto *frames) {
            long double sum = 0.0L;
            for (int32_t frame = 0; frame < frameCount; ++frame) {
                const auto frameOffset = int64_t(frame) * pixelCount;
                for (int32_t row = y; row < y + roiHeight; ++row) {
                    const auto rowOffset = frameOffset + int64_t(row) * cols + x;
                    for (int32_t col = 0; col < roiWidth; ++col)
                        sum += frames[rowOffset + col];
                }
            }
            const auto samples = int64_t(frameCount) * roiWidth * roiHeight;
            *meanDn = static_cast<double>(sum / samples);
        });
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_FlickerDetect(
    const IRArrayView *input,
    const double *k_map,
    const double *b_map,
    const uint8_t *valid_mask,
    double std_dev_multiplier,
    int32_t exceed_count_threshold,
    IRFlickerSummary *summary,
    int32_t *out_exceed_count_map,
    uint8_t *out_flicker_mask,
    int64_t pixel_capacity,
    double *out_frame_means,
    double *out_frame_sigmas,
    int32_t frame_stats_capacity,
    char *err,
    int32_t err_bytes) noexcept
{
    return guarded(err, err_bytes, [&] {
        const int64_t n = validateRawArrayView(input);
        const int32_t frame_count = input->frames;
        require(frame_count >= 1, IR_BAD_ARGUMENT, "frame_count must be >= 1");
        require((k_map == nullptr) == (b_map == nullptr), IR_BAD_ARGUMENT,
                "k_map and b_map must both be null or both non-null");
        require(std::isfinite(std_dev_multiplier) && std_dev_multiplier > 0.0,
                IR_BAD_ARGUMENT, "std_dev_multiplier must be finite and > 0");
        require(exceed_count_threshold >= 0 && exceed_count_threshold < frame_count,
                IR_BAD_ARGUMENT, "exceed_count_threshold must satisfy 0 <= threshold < frame_count");
        needed(summary, 1, 1);

        if (k_map)
        {
            needed(k_map, pixel_capacity, n);
            needed(b_map, pixel_capacity, n);
        }
        if (valid_mask)
            needed(valid_mask, pixel_capacity, n);
        if (out_exceed_count_map)
            needed(out_exceed_count_map, pixel_capacity, n);
        if (out_flicker_mask)
            needed(out_flicker_mask, pixel_capacity, n);
        if (out_frame_means)
            needed(out_frame_means, frame_stats_capacity, frame_count);
        if (out_frame_sigmas)
            needed(out_frame_sigmas, frame_stats_capacity, frame_count);

        dispatchRawArrayView(*input, [&](const auto *frames) {
            const bool useNuc = (k_map != nullptr);
            std::vector<int32_t> exceedCount(static_cast<size_t>(n), 0);
            std::vector<uint8_t> observed(static_cast<size_t>(n), 0);
            double sumFrameMean = 0.0;
            double sumFrameSigma = 0.0;

            for (int32_t f = 0; f < frame_count; ++f)
            {
                const int64_t base = int64_t(f) * n;
                double mean = 0.0;
                double m2 = 0.0;
                int64_t validInFrame = 0;

                for (int64_t p = 0; p < n; ++p)
                {
                    if (valid_mask && valid_mask[p] == 0)
                        continue;
                    const double raw = static_cast<double>(frames[base + p]);
                    if (raw == 0.0)
                        continue;
                    const double val = useNuc ? (raw * k_map[p] + b_map[p]) : raw;
                    if (!std::isfinite(val) || val == 0.0)
                        continue;

                    observed[static_cast<size_t>(p)] = 1;
                    ++validInFrame;
                    const double delta = val - mean;
                    mean += delta / static_cast<double>(validInFrame);
                    m2 += delta * (val - mean);
                }

                require(validInFrame > 0, IR_COMPUTATION_FAILED, "Frame has no valid non-zero pixels");
                const double sigma = std::sqrt(std::max(0.0, m2 / static_cast<double>(validInFrame)));
                sumFrameMean += mean;
                sumFrameSigma += sigma;
                if (out_frame_means)
                    out_frame_means[f] = mean;
                if (out_frame_sigmas)
                    out_frame_sigmas[f] = sigma;

                const double limit = std_dev_multiplier * sigma;
                for (int64_t p = 0; p < n; ++p)
                {
                    if (valid_mask && valid_mask[p] == 0)
                        continue;
                    const double raw = static_cast<double>(frames[base + p]);
                    if (raw == 0.0)
                        continue;
                    const double val = useNuc ? (raw * k_map[p] + b_map[p]) : raw;
                    if (!std::isfinite(val) || val == 0.0)
                        continue;
                    if (std::abs(val - mean) > limit)
                        ++exceedCount[static_cast<size_t>(p)];
                }
            }

            int32_t validPixelCount = 0;
            int32_t flickerCount = 0;
            for (int64_t p = 0; p < n; ++p)
            {
                const size_t idx = static_cast<size_t>(p);
                const bool isObserved = (observed[idx] != 0);
                const bool isFlicker = isObserved && (exceedCount[idx] > exceed_count_threshold);
                if (isObserved)
                    ++validPixelCount;
                if (isFlicker)
                    ++flickerCount;
                if (out_exceed_count_map)
                    out_exceed_count_map[p] = exceedCount[idx];
                if (out_flicker_mask)
                    out_flicker_mask[p] = isFlicker ? 1 : 0;
            }
            require(validPixelCount > 0, IR_COMPUTATION_FAILED, "No observed valid pixels across frames");

            IRFlickerSummary result{};
            result.std_dev_multiplier = std_dev_multiplier;
            result.flicker_ratio_pct =
                100.0 * static_cast<double>(flickerCount) / static_cast<double>(validPixelCount);
            result.mean_frame_mean_dn = sumFrameMean / static_cast<double>(frame_count);
            result.mean_frame_sigma_dn = sumFrameSigma / static_cast<double>(frame_count);
            result.exceed_count_threshold = exceed_count_threshold;
            result.frame_count = frame_count;
            result.valid_pixel_count = validPixelCount;
            result.flicker_count = flickerCount;
            *summary = result;
        });
    });
}
