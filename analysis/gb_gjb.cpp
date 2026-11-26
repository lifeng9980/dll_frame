#include "IR_analysis.h"
#include "matrix_view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

using namespace ir::api;
using namespace ir::analysis;

namespace
{

static_assert(sizeof(IRCalcParameters) == 80, "Unexpected IRCalcParameters size");
static_assert(sizeof(IRGbCriteria) == 56, "Unexpected IRGbCriteria size");
static_assert(sizeof(IRGbBadPixelSummary) == 104, "Unexpected IRGbBadPixelSummary size");
static_assert(sizeof(IRGbSummary) == 264, "Unexpected IRGbSummary size");
static_assert(sizeof(IRGjbCriteria) == 40, "Unexpected IRGjbCriteria size");
static_assert(sizeof(IRGjbClusterStats) == 20, "Unexpected IRGjbClusterStats size");
static_assert(sizeof(IRGjbBadPixelSummary) == 88, "Unexpected IRGjbBadPixelSummary size");
static_assert(sizeof(IRGjbSummary) == 248, "Unexpected IRGjbSummary size");
#if defined(_WIN64) || defined(__x86_64__) || defined(__LP64__)
static_assert(sizeof(IRGbBadPixelMasks) == 64, "Unexpected IRGbBadPixelMasks x64 size");
static_assert(sizeof(IRGjbBadPixelMasks) == 64, "Unexpected IRGjbBadPixelMasks x64 size");
#endif

constexpr double kStefanBoltzmannConstant = 5.673e-12;
constexpr double kShortBadPixelThreshold = 45000.0;
constexpr double kOpenBadPixelThreshold = 20000.0;
constexpr double kGjbNewBadStopRatio = 0.001;

struct Coord2D
{
    int32_t row;
    int32_t col;
};

struct CommonBeforeData
{
    Matrix2D<double> pixelResponseCount;
    Matrix2D<double> pixelResponseVoltage;
    Matrix2D<double> snr;
    Matrix2D<double> responsivity;
    Matrix2D<double> detectivity;
    Matrix2D<double> noiseVoltage;
    Matrix2D<double> netd;
    double avgPixelResponseCount = 0.0;
    double avgPixelResponseVoltage = 0.0;
    double voltsPerCount = 0.0;
    double avgSnr = 0.0;
    double fov = 0.0;
    double irradiancePowerDiff = 0.0;
    double avgNoiseVoltageV = 0.0;
    double avgNetd = 0.0;
    double avgDetectivity = 0.0;
    double avgResponsivity = 0.0;
};

struct GbBadPixelData
{
    IRGbBadPixelSummary summary{};
    Matrix2D<uint8_t> validMask;
    Matrix2D<uint8_t> totalBadMask;
    Matrix2D<uint8_t> responseBadMask;
    Matrix2D<uint8_t> noiseBadMask;
    Matrix2D<uint8_t> netdBadMask;
};

struct GjbBadPixelData
{
    IRGjbBadPixelSummary summary{};
    Matrix2D<uint8_t> validMask;
    Matrix2D<uint8_t> totalBadMask;
    Matrix2D<uint8_t> k1BadMask;
    Matrix2D<uint8_t> k2BadMask;
    Matrix2D<uint8_t> k3BadMask;
    Matrix2D<uint8_t> k4BadMask;
    Matrix2D<uint8_t> blockFilteredBadMask;
};

void validateCalcParameters(const IRCalcParameters &p)
{
    require(std::isfinite(p.t0_c) && std::isfinite(p.t1_c) &&
                std::isfinite(p.pixel_pitch_um) && std::isfinite(p.emissivity) &&
                std::isfinite(p.system_gain) && std::isfinite(p.adc_swing_v) &&
                std::isfinite(p.f_number) && std::isfinite(p.vmax_per_count),
            IR_BAD_ARGUMENT, "Calculation parameters contain non-finite floating-point values");
    require(p.digital_full != 0, IR_BAD_ARGUMENT, "digital_full must not be zero");
    require(p.system_gain != 0.0, IR_BAD_ARGUMENT, "system_gain must not be zero");
    require(std::abs(p.t1_c - p.t0_c) != 0.0, IR_BAD_ARGUMENT, "t1_c - t0_c must not be zero");
    const double actualIntegrationTimeUs =
        static_cast<double>(p.integration_time_us) - static_cast<double>(p.invalid_integration_us);
    require(actualIntegrationTimeUs > 0.0, IR_BAD_ARGUMENT,
            "Actual integration time (integration_time_us - invalid_integration_us) must be positive");
}

void validateGbCriteria(const IRGbCriteria &c)
{
    require(std::isfinite(c.response_upper) && std::isfinite(c.response_lower) &&
                std::isfinite(c.noise_upper) && std::isfinite(c.netd_upper) &&
                std::isfinite(c.short_bad_threshold) && std::isfinite(c.open_bad_threshold),
            IR_BAD_ARGUMENT, "GB criteria thresholds must be finite");
    require(c.cycle_upper > 0, IR_BAD_ARGUMENT, "GB cycle_upper must be positive");
}

void validateGjbCriteria(const IRGjbCriteria &c)
{
    require(std::isfinite(c.k1) && std::isfinite(c.k2) &&
                std::isfinite(c.k3) && std::isfinite(c.k4),
            IR_BAD_ARGUMENT, "GJB criteria thresholds must be finite");
    require(c.k3 > 0.0 && c.k4 > 0.0, IR_BAD_ARGUMENT, "GJB k3 and k4 must be positive");
    require(c.cycle_upper > 0, IR_BAD_ARGUMENT, "GJB cycle_upper must be positive");
}

void validateGbMaskCapacities(const IRGbBadPixelMasks *masks, int64_t pixelCount)
{
    if (!masks)
        return;
    const bool anyMask = masks->valid_mask || masks->total_bad_mask || masks->response_bad_mask ||
                         masks->noise_bad_mask || masks->netd_bad_mask || masks->short_bad_mask ||
                         masks->open_bad_mask;
    if (anyMask || masks->pixel_capacity < 0)
    {
        require(masks->pixel_capacity >= pixelCount, IR_BUFFER_TOO_SMALL,
                "GB mask output buffer capacity is too small");
    }
}

void validateGjbMaskCapacities(const IRGjbBadPixelMasks *masks, int64_t pixelCount)
{
    if (!masks)
        return;
    const bool anyMask = masks->valid_mask || masks->total_bad_mask || masks->k1_bad_mask ||
                         masks->k2_bad_mask || masks->k3_bad_mask || masks->k4_bad_mask ||
                         masks->block_filtered_bad_mask;
    if (anyMask || masks->pixel_capacity < 0)
    {
        require(masks->pixel_capacity >= pixelCount, IR_BUFFER_TOO_SMALL,
                "GJB mask output buffer capacity is too small");
    }
}

void copyMaskIfPresent(uint8_t *dst, const Matrix2D<uint8_t> &src)
{
    if (dst && !src.empty())
        std::copy(src.data.begin(), src.data.end(), dst);
}

void zeroMaskIfPresent(uint8_t *dst, int64_t pixelCount)
{
    if (dst)
        std::fill_n(dst, static_cast<size_t>(pixelCount), uint8_t{0});
}

void copyGbMasks(const IRGbBadPixelMasks *masks, const GbBadPixelData &bad,
                 const Matrix2D<uint8_t> *shortMask, const Matrix2D<uint8_t> *openMask,
                 int64_t pixelCount)
{
    if (!masks)
        return;
    copyMaskIfPresent(masks->valid_mask, bad.validMask);
    copyMaskIfPresent(masks->total_bad_mask, bad.totalBadMask);
    copyMaskIfPresent(masks->response_bad_mask, bad.responseBadMask);
    copyMaskIfPresent(masks->noise_bad_mask, bad.noiseBadMask);
    copyMaskIfPresent(masks->netd_bad_mask, bad.netdBadMask);
    if (shortMask)
        copyMaskIfPresent(masks->short_bad_mask, *shortMask);
    else
        zeroMaskIfPresent(masks->short_bad_mask, pixelCount);
    if (openMask)
        copyMaskIfPresent(masks->open_bad_mask, *openMask);
    else
        zeroMaskIfPresent(masks->open_bad_mask, pixelCount);
}

void copyGjbMasks(const IRGjbBadPixelMasks *masks, const GjbBadPixelData &bad)
{
    if (!masks)
        return;
    copyMaskIfPresent(masks->valid_mask, bad.validMask);
    copyMaskIfPresent(masks->total_bad_mask, bad.totalBadMask);
    copyMaskIfPresent(masks->k1_bad_mask, bad.k1BadMask);
    copyMaskIfPresent(masks->k2_bad_mask, bad.k2BadMask);
    copyMaskIfPresent(masks->k3_bad_mask, bad.k3BadMask);
    copyMaskIfPresent(masks->k4_bad_mask, bad.k4BadMask);
    copyMaskIfPresent(masks->block_filtered_bad_mask, bad.blockFilteredBadMask);
}

CommonBeforeData computeCommonBefore(ConstMatrixView<double> v1,
                                     ConstMatrixView<double> v2,
                                     ConstMatrixView<double> vn,
                                     const IRCalcParameters &p)
{
    CommonBeforeData res;
    const int32_t rows = v1.rows;
    const int32_t cols = v1.cols;
    const int64_t total = v1.size();

    res.pixelResponseCount = Matrix2D<double>(rows, cols);
    res.pixelResponseVoltage = Matrix2D<double>(rows, cols);
    res.snr = Matrix2D<double>(rows, cols);
    res.responsivity = Matrix2D<double>(rows, cols);
    res.detectivity = Matrix2D<double>(rows, cols);
    res.noiseVoltage = Matrix2D<double>(rows, cols);
    res.netd = Matrix2D<double>(rows, cols);

    res.voltsPerCount = p.adc_swing_v / static_cast<double>(p.digital_full) / p.system_gain;
    res.fov = 4.0 * p.f_number * p.f_number + 1.0;

    const double pixelPitchCm = p.pixel_pitch_um / 10000.0;
    const double pixelAreaCm2 = pixelPitchCm * pixelPitchCm;
    const double t0Kelvin = p.t0_c + 273.0;
    const double t1Kelvin = p.t1_c + 273.0;
    const double temperaturePowerDiff = std::pow(t1Kelvin, 4.0) - std::pow(t0Kelvin, 4.0);
    res.irradiancePowerDiff = pixelAreaCm2 * p.emissivity * kStefanBoltzmannConstant *
                              temperaturePowerDiff / res.fov;
    require(res.irradiancePowerDiff != 0.0 && std::isfinite(res.irradiancePowerDiff),
            IR_BAD_ARGUMENT, "irradiancePowerDiff must be finite and non-zero");

    const double actualIntegrationTimeSeconds =
        (static_cast<double>(p.integration_time_us) - static_cast<double>(p.invalid_integration_us)) * 1e-6;
    const double detectivityFactor = std::sqrt(pixelAreaCm2 / (2.0 * actualIntegrationTimeSeconds));
    const double kelvinDiff = t1Kelvin - t0Kelvin;

    for (int64_t i = 0; i < total; ++i)
    {
        const double respCount = std::abs(v1.ptr[i] - v2.ptr[i]);
        const double respVoltage = respCount * res.voltsPerCount;
        const double snrVal = safeDivide(respCount, vn.ptr[i]);
        const double resp = respVoltage / res.irradiancePowerDiff;
        const double noiseV = vn.ptr[i] * res.voltsPerCount;
        const double detVal = safeDivide(resp, noiseV) * detectivityFactor;
        const double netdVal = safeDivide(kelvinDiff, snrVal) * 1000.0;

        res.pixelResponseCount.data[size_t(i)] = respCount;
        res.pixelResponseVoltage.data[size_t(i)] = respVoltage;
        res.snr.data[size_t(i)] = snrVal;
        res.responsivity.data[size_t(i)] = resp;
        res.noiseVoltage.data[size_t(i)] = noiseV;
        res.detectivity.data[size_t(i)] = detVal;
        res.netd.data[size_t(i)] = netdVal;
    }

    res.avgPixelResponseCount = finiteMean(res.pixelResponseCount.view());
    res.avgPixelResponseVoltage = finiteMean(res.pixelResponseVoltage.view());
    res.avgSnr = finiteMean(res.snr.view());
    res.avgResponsivity = finiteMean(res.responsivity.view());
    res.avgNoiseVoltageV = finiteMean(res.noiseVoltage.view());
    res.avgDetectivity = finiteMean(res.detectivity.view());
    res.avgNetd = finiteMean(res.netd.view());
    return res;
}

int32_t determineChipGrade(const int32_t counts[IR_GB_REGION_COUNT][IR_GB_CLUSTER_TYPE_COUNT])
{
    const auto c = [&](IRGbRegion region, IRGbClusterType type) {
        return counts[static_cast<int>(region)][static_cast<int>(type)];
    };

    const int32_t oTotal =
        c(IR_GB_REGION_A, IR_GB_CLUSTER_O) +
        c(IR_GB_REGION_B, IR_GB_CLUSTER_O) +
        c(IR_GB_REGION_C, IR_GB_CLUSTER_O);
    const int32_t iTotal =
        c(IR_GB_REGION_A, IR_GB_CLUSTER_I) +
        c(IR_GB_REGION_B, IR_GB_CLUSTER_I) +
        c(IR_GB_REGION_C, IR_GB_CLUSTER_I);
    const int32_t iiAB =
        c(IR_GB_REGION_A, IR_GB_CLUSTER_II) +
        c(IR_GB_REGION_B, IR_GB_CLUSTER_II);
    const int32_t iiA = c(IR_GB_REGION_A, IR_GB_CLUSTER_II);
    const int32_t iiiAB =
        c(IR_GB_REGION_A, IR_GB_CLUSTER_III) +
        c(IR_GB_REGION_B, IR_GB_CLUSTER_III);

    if (oTotal > 0)
        return IR_CHIP_GRADE_D;
    if (iTotal == 0 && iiAB <= 1 && iiA == 0 && iiiAB <= 20)
        return IR_CHIP_GRADE_A;
    if (iTotal == 0 && iiAB <= 2 && iiA <= 1 && iiiAB <= 30)
        return IR_CHIP_GRADE_B;
    if (iTotal <= 1 && iiAB <= 4 && iiA <= 2 && iiiAB <= 40)
        return IR_CHIP_GRADE_C;
    return IR_CHIP_GRADE_D;
}

void classifyGbClusters(ConstMatrixView<uint8_t> badMask,
                        int32_t counts[IR_GB_REGION_COUNT][IR_GB_CLUSTER_TYPE_COUNT])
{
    for (int r = 0; r < IR_GB_REGION_COUNT; ++r)
        for (int t = 0; t < IR_GB_CLUSTER_TYPE_COUNT; ++t)
            counts[r][t] = 0;

    const int32_t rows = badMask.rows;
    const int32_t cols = badMask.cols;
    const Rect2D regionA{(cols - cols / 5) / 2, (rows - rows / 4) / 2, cols / 5, rows / 4};
    const Rect2D regionB{(cols - cols / 2) / 2, (rows - rows / 2) / 2, cols / 2, rows / 2};

    Matrix2D<uint8_t> visited(rows, cols, 0);
    constexpr int32_t kDirections[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    std::vector<Coord2D> pending;

    for (int32_t row = 0; row < rows; ++row)
    {
        for (int32_t col = 0; col < cols; ++col)
        {
            if (badMask.at(row, col) == 0 || visited.at(row, col) != 0)
                continue;

            pending.clear();
            pending.push_back(Coord2D{row, col});
            visited.at(row, col) = 1;

            int32_t minRow = row, maxRow = row;
            int32_t minCol = col, maxCol = col;
            bool containsA = false;
            bool containsB = false;

            while (!pending.empty())
            {
                const Coord2D pt = pending.back();
                pending.pop_back();
                minRow = std::min(minRow, pt.row);
                maxRow = std::max(maxRow, pt.row);
                minCol = std::min(minCol, pt.col);
                maxCol = std::max(maxCol, pt.col);

                containsA |= regionA.contains(pt.col, pt.row);
                containsB |= regionB.contains(pt.col, pt.row);

                for (const auto &dir : kDirections)
                {
                    const int32_t nextRow = pt.row + dir[0];
                    const int32_t nextCol = pt.col + dir[1];
                    if (nextRow < 0 || nextRow >= rows || nextCol < 0 || nextCol >= cols)
                        continue;
                    if (badMask.at(nextRow, nextCol) == 0 || visited.at(nextRow, nextCol) != 0)
                        continue;
                    visited.at(nextRow, nextCol) = 1;
                    pending.push_back(Coord2D{nextRow, nextCol});
                }
            }

            const int32_t longestSide = std::max(maxRow - minRow + 1, maxCol - minCol + 1);
            IRGbClusterType type = IR_GB_CLUSTER_IV;
            if (longestSide >= 5)
                type = IR_GB_CLUSTER_O;
            else if (longestSide == 4)
                type = IR_GB_CLUSTER_I;
            else if (longestSide == 3)
                type = IR_GB_CLUSTER_II;
            else if (longestSide == 2)
                type = IR_GB_CLUSTER_III;

            const IRGbRegion region = containsA ? IR_GB_REGION_A
                                                : (containsB ? IR_GB_REGION_B : IR_GB_REGION_C);
            ++counts[static_cast<int>(region)][static_cast<int>(type)];
        }
    }
}

GbBadPixelData processGbBadPixelsInternal(ConstMatrixView<double> response,
                                          ConstMatrixView<double> vn,
                                          ConstMatrixView<double> netd,
                                          const IRGbCriteria &criteria)
{
    GbBadPixelData out;
    const int32_t rows = response.rows;
    const int32_t cols = response.cols;
    const int64_t total = response.size();

    out.validMask = Matrix2D<uint8_t>(rows, cols, 1);
    out.responseBadMask = Matrix2D<uint8_t>(rows, cols, 0);
    out.noiseBadMask = Matrix2D<uint8_t>(rows, cols, 0);
    out.netdBadMask = Matrix2D<uint8_t>(rows, cols, 0);

    int32_t cumulativeResponseBad = 0;
    int32_t cumulativeNoiseBad = 0;
    int32_t cumulativeNetdBad = 0;

    for (int32_t iteration = 0; iteration < criteria.cycle_upper; ++iteration)
    {
        // 1. Response filtering (sequentially updates validMask, independent responseBadMask)
        const double avgResponse = maskedFiniteMean(response, out.validMask.view());
        const double responseLower = criteria.response_lower * avgResponse;
        const double responseUpper = criteria.response_upper * avgResponse;
        const Matrix2D<uint8_t> responsePass = createRangePassMask(response, responseLower, responseUpper);

        int32_t newResponseBadCount = 0;
        for (int64_t i = 0; i < total; ++i)
        {
            const uint8_t pass = responsePass.data[size_t(i)];
            if (out.validMask.data[size_t(i)] != 0 && pass == 0)
            {
                ++newResponseBadCount;
                out.validMask.data[size_t(i)] = 0;
            }
            if (pass == 0 && out.responseBadMask.data[size_t(i)] == 0)
            {
                out.responseBadMask.data[size_t(i)] = 1;
                const double value = response.ptr[i];
                if (std::isfinite(value))
                {
                    if (value > responseUpper)
                        ++out.summary.response_upper_bad_count;
                    else if (value < responseLower)
                        ++out.summary.response_lower_bad_count;
                }
            }
        }

        // 2. Noise filtering
        const double avgNoise = maskedFiniteMean(vn, out.validMask.view());
        const double noiseUpper = criteria.noise_upper * avgNoise;
        const Matrix2D<uint8_t> noisePass = createUpperPassMask(vn, noiseUpper);

        int32_t newNoiseBadCount = 0;
        for (int64_t i = 0; i < total; ++i)
        {
            const uint8_t pass = noisePass.data[size_t(i)];
            if (out.validMask.data[size_t(i)] != 0 && pass == 0)
            {
                ++newNoiseBadCount;
                out.validMask.data[size_t(i)] = 0;
            }
            if (pass == 0)
                out.noiseBadMask.data[size_t(i)] = 1;
        }

        // 3. NETD filtering
        const double avgNetd = maskedFiniteMean(netd, out.validMask.view());
        const double netdUpper = criteria.netd_upper * avgNetd;
        const Matrix2D<uint8_t> netdPass = createUpperPassMask(netd, netdUpper);

        int32_t newNetdBadCount = 0;
        for (int64_t i = 0; i < total; ++i)
        {
            const uint8_t pass = netdPass.data[size_t(i)];
            if (out.validMask.data[size_t(i)] != 0 && pass == 0)
            {
                ++newNetdBadCount;
                out.validMask.data[size_t(i)] = 0;
            }
            if (pass == 0)
                out.netdBadMask.data[size_t(i)] = 1;
        }

        out.summary.iteration_count = iteration + 1;

        if (newResponseBadCount < cumulativeResponseBad &&
            newNetdBadCount < cumulativeNetdBad &&
            newNoiseBadCount < cumulativeNoiseBad)
        {
            break;
        }

        cumulativeResponseBad += newResponseBadCount;
        cumulativeNetdBad += newNetdBadCount;
        cumulativeNoiseBad += newNoiseBadCount;
    }

    out.totalBadMask = Matrix2D<uint8_t>(rows, cols, 0);
    for (int64_t i = 0; i < total; ++i)
        out.totalBadMask.data[size_t(i)] = (out.validMask.data[size_t(i)] == 0) ? 1 : 0;

    classifyGbClusters(out.totalBadMask.view(), out.summary.cluster_counts);
    out.summary.chip_grade = determineChipGrade(out.summary.cluster_counts);
    out.summary.response_bad_count = countNonZero(out.responseBadMask.view());
    out.summary.noise_bad_count = countNonZero(out.noiseBadMask.view());
    out.summary.netd_bad_count = countNonZero(out.netdBadMask.view());
    out.summary.total_bad_count = countNonZero(out.totalBadMask.view());
    out.summary.total_bad_rate_pct =
        100.0 * static_cast<double>(out.summary.total_bad_count) / static_cast<double>(total);
    return out;
}

IRGjbClusterStats summarizeGjbClusters(ConstMatrixView<uint8_t> badMask)
{
    IRGjbClusterStats stats{};
    if (badMask.empty())
        return stats;

    const int32_t rows = badMask.rows;
    const int32_t cols = badMask.cols;
    Matrix2D<uint8_t> visited(rows, cols, 0);
    constexpr int32_t kRowOffset[4] = {-1, 1, 0, 0};
    constexpr int32_t kColOffset[4] = {0, 0, -1, 1};
    std::queue<Coord2D> q;

    for (int32_t row = 0; row < rows; ++row)
    {
        for (int32_t col = 0; col < cols; ++col)
        {
            if (badMask.at(row, col) == 0 || visited.at(row, col) != 0)
                continue;

            int32_t clusterSize = 0;
            visited.at(row, col) = 1;
            q.push(Coord2D{row, col});

            while (!q.empty())
            {
                const Coord2D cur = q.front();
                q.pop();
                ++clusterSize;

                for (int32_t dir = 0; dir < 4; ++dir)
                {
                    const int32_t nextRow = cur.row + kRowOffset[dir];
                    const int32_t nextCol = cur.col + kColOffset[dir];
                    if (nextRow < 0 || nextRow >= rows || nextCol < 0 || nextCol >= cols)
                        continue;
                    if (badMask.at(nextRow, nextCol) == 0 || visited.at(nextRow, nextCol) != 0)
                        continue;
                    visited.at(nextRow, nextCol) = 1;
                    q.push(Coord2D{nextRow, nextCol});
                }
            }

            if (clusterSize <= 6)
                ++stats.count_1_to_6;
            else if (clusterSize <= 12)
                ++stats.count_7_to_12;
            else if (clusterSize <= 19)
                ++stats.count_13_to_19;
            else if (clusterSize <= 35)
                ++stats.count_20_to_35;
            else
                ++stats.count_gt_35;
        }
    }
    return stats;
}

int32_t countAndRemoveFullBadBlocks(Matrix2D<uint8_t> &badMask, int32_t blockSize)
{
    if (badMask.empty() || badMask.rows < blockSize || badMask.cols < blockSize)
        return 0;

    int32_t blockCount = 0;
    for (int32_t row = 0; row <= badMask.rows - blockSize; ++row)
    {
        for (int32_t col = 0; col <= badMask.cols - blockSize; ++col)
        {
            bool fullBad = true;
            for (int32_t r = 0; r < blockSize && fullBad; ++r)
            {
                const uint8_t *rowPtr = badMask.rowPtr(row + r) + col;
                for (int32_t c = 0; c < blockSize; ++c)
                {
                    if (rowPtr[c] == 0)
                    {
                        fullBad = false;
                        break;
                    }
                }
            }
            if (fullBad)
            {
                for (int32_t r = 0; r < blockSize; ++r)
                {
                    uint8_t *rowPtr = badMask.rowPtr(row + r) + col;
                    for (int32_t c = 0; c < blockSize; ++c)
                        rowPtr[c] = 0;
                }
                ++blockCount;
            }
        }
    }
    return blockCount;
}

GjbBadPixelData processGjbBadPixelsInternal(ConstMatrixView<double> response,
                                            ConstMatrixView<double> signalT1,
                                            ConstMatrixView<double> signalT2,
                                            ConstMatrixView<double> noiseVoltage,
                                            ConstMatrixView<double> netd,
                                            const IRGjbCriteria &criteria)
{
    GjbBadPixelData out;
    const int32_t rows = response.rows;
    const int32_t cols = response.cols;
    const int64_t total = response.size();

    out.validMask = Matrix2D<uint8_t>(rows, cols, 1);
    out.k1BadMask = Matrix2D<uint8_t>(rows, cols, 0);
    out.k2BadMask = Matrix2D<uint8_t>(rows, cols, 0);
    out.k3BadMask = Matrix2D<uint8_t>(rows, cols, 0);
    out.k4BadMask = Matrix2D<uint8_t>(rows, cols, 0);

    for (int32_t iteration = 0; iteration < criteria.cycle_upper; ++iteration)
    {
        const double avgResponse = maskedFiniteMean(response, out.validMask.view());
        const double avgT1 = maskedFiniteMean(signalT1, out.validMask.view());
        const double avgT2 = maskedFiniteMean(signalT2, out.validMask.view());
        const double avgNoise = maskedFiniteMean(noiseVoltage, out.validMask.view());
        const double avgNetd = maskedFiniteMean(netd, out.validMask.view());

        const Matrix2D<uint8_t> responsePass =
            createUpperPassMask(response, avgResponse * (criteria.k1 + 1.0));
        const Matrix2D<uint8_t> t1Pass =
            createUpperPassMask(signalT1, avgT1 * (criteria.k2 + 1.0));
        const Matrix2D<uint8_t> t2Pass =
            createUpperPassMask(signalT2, avgT2 * (criteria.k2 + 1.0));
        const Matrix2D<uint8_t> netdPass =
            createUpperPassMask(netd, avgNetd / criteria.k3);
        const Matrix2D<uint8_t> noisePass =
            createUpperPassMask(noiseVoltage, avgNoise / criteria.k4);

        int32_t newBadCount = 0;
        int32_t totalBadCount = 0;
        for (int64_t i = 0; i < total; ++i)
        {
            const uint8_t wasValid = out.validMask.data[size_t(i)];
            const uint8_t pResp = responsePass.data[size_t(i)];
            const uint8_t pK2 = (t1Pass.data[size_t(i)] != 0 && t2Pass.data[size_t(i)] != 0) ? 1 : 0;
            const uint8_t pNetd = netdPass.data[size_t(i)];
            const uint8_t pNoise = noisePass.data[size_t(i)];

            if (wasValid != 0)
            {
                if (pResp == 0)
                    out.k1BadMask.data[size_t(i)] = 1;
                if (pK2 == 0)
                    out.k2BadMask.data[size_t(i)] = 1;
                if (pNetd == 0)
                    out.k3BadMask.data[size_t(i)] = 1;
                if (pNoise == 0)
                    out.k4BadMask.data[size_t(i)] = 1;
            }

            const uint8_t nowValid =
                (wasValid != 0 && pResp != 0 && pK2 != 0 && pNoise != 0 && pNetd != 0) ? 1 : 0;
            if (wasValid != 0 && nowValid == 0)
                ++newBadCount;
            if (nowValid == 0)
                ++totalBadCount;
            out.validMask.data[size_t(i)] = nowValid;
        }

        out.summary.total_bad_count = totalBadCount;
        out.summary.iteration_count = iteration + 1;

        if (newBadCount == 0)
            break;

        const double newBadRate = static_cast<double>(newBadCount) / static_cast<double>(totalBadCount);
        if (newBadRate < kGjbNewBadStopRatio)
            break;
    }

    out.totalBadMask = Matrix2D<uint8_t>(rows, cols, 0);
    for (int64_t i = 0; i < total; ++i)
        out.totalBadMask.data[size_t(i)] = (out.validMask.data[size_t(i)] == 0) ? 1 : 0;
    out.summary.total_bad_count = countNonZero(out.totalBadMask.view());
    out.summary.total_bad_rate_pct =
        100.0 * static_cast<double>(out.summary.total_bad_count) / static_cast<double>(total);

    const int32_t centerW = cols / 2;
    const int32_t centerH = rows / 2;
    const Rect2D centerRect{(cols - centerW) / 2, (rows - centerH) / 2, centerW, centerH};

    Matrix2D<uint8_t> centerBadMask;
    Matrix2D<uint8_t> edgeBadMask = out.totalBadMask;
    if (!centerRect.empty())
    {
        centerBadMask = Matrix2D<uint8_t>(centerRect.height, centerRect.width, 0);
        for (int32_t r = 0; r < centerRect.height; ++r)
        {
            const uint8_t *srcRow = out.totalBadMask.rowPtr(centerRect.y + r) + centerRect.x;
            uint8_t *centerRow = centerBadMask.rowPtr(r);
            uint8_t *edgeRow = edgeBadMask.rowPtr(centerRect.y + r) + centerRect.x;
            for (int32_t c = 0; c < centerRect.width; ++c)
            {
                centerRow[c] = srcRow[c];
                edgeRow[c] = 0;
            }
        }
    }

    out.summary.total_clusters = summarizeGjbClusters(out.totalBadMask.view());
    out.summary.center_clusters = summarizeGjbClusters(centerBadMask.view());
    out.summary.edge_clusters = summarizeGjbClusters(edgeBadMask.view());

    Matrix2D<uint8_t> filteredCenterMask = centerBadMask;
    out.summary.center_3x3_block_count = countAndRemoveFullBadBlocks(filteredCenterMask, 3);

    out.blockFilteredBadMask = out.totalBadMask;
    if (!centerRect.empty())
    {
        for (int32_t r = 0; r < centerRect.height; ++r)
        {
            const uint8_t *filteredRow = filteredCenterMask.rowPtr(r);
            uint8_t *dstRow = out.blockFilteredBadMask.rowPtr(centerRect.y + r) + centerRect.x;
            std::copy_n(filteredRow, static_cast<size_t>(centerRect.width), dstRow);
        }
    }
    out.summary.filtered_4x4_block_count = countAndRemoveFullBadBlocks(out.blockFilteredBadMask, 4);
    return out;
}

} // namespace

extern "C" IR_ANALYSIS_API int32_t IR_GbProcessBadPixels(
    const double *response_map,
    const double *noise_map,
    const double *netd_map,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    const IRGbCriteria *criteria,
    IRGbBadPixelSummary *summary,
    const IRGbBadPixelMasks *masks,
    char *err,
    int32_t err_bytes)
noexcept{
    if (summary)
        *summary = IRGbBadPixelSummary{};
    return guarded(err, err_bytes, [&] {
        const int64_t pixelCount = pixels(rows, cols);
        needed(response_map, pixel_capacity, pixelCount);
        needed(noise_map, pixel_capacity, pixelCount);
        needed(netd_map, pixel_capacity, pixelCount);
        require(criteria != nullptr && summary != nullptr, IR_NULL_POINTER,
                "criteria and summary must not be null");
        validateGbCriteria(*criteria);
        validateGbMaskCapacities(masks, pixelCount);

        const auto bad = processGbBadPixelsInternal(
            ConstMatrixView<double>{response_map, rows, cols},
            ConstMatrixView<double>{noise_map, rows, cols},
            ConstMatrixView<double>{netd_map, rows, cols},
            *criteria);
        *summary = bad.summary;
        copyGbMasks(masks, bad, nullptr, nullptr, pixelCount);
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_GbEvaluateMaps(
    const double *v1_map,
    const double *v2_map,
    const double *vn_map,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    const IRCalcParameters *params,
    const IRGbCriteria *criteria,
    IRGbSummary *summary,
    const IRGbBadPixelMasks *masks,
    char *err,
    int32_t err_bytes)
noexcept{
    if (summary)
        *summary = IRGbSummary{};
    return guarded(err, err_bytes, [&] {
        const int64_t pixelCount = pixels(rows, cols);
        needed(v1_map, pixel_capacity, pixelCount);
        needed(v2_map, pixel_capacity, pixelCount);
        needed(vn_map, pixel_capacity, pixelCount);
        require(params != nullptr && criteria != nullptr && summary != nullptr, IR_NULL_POINTER,
                "params, criteria, and summary must not be null");
        validateCalcParameters(*params);
        validateGbCriteria(*criteria);
        validateGbMaskCapacities(masks, pixelCount);

        const ConstMatrixView<double> v1{v1_map, rows, cols};
        const ConstMatrixView<double> v2{v2_map, rows, cols};
        const ConstMatrixView<double> vn{vn_map, rows, cols};
        const CommonBeforeData before = computeCommonBefore(v1, v2, vn, *params);

        const double deltaT = std::abs(params->t1_c - params->t0_c);
        const double meanV1 = arithmeticMean(v1);
        const double meanV2 = arithmeticMean(v2);
        const double sitf = std::abs(meanV1 - meanV2) / params->system_gain / deltaT;

        const double shortThreshold =
            (criteria->short_bad_threshold > 0.0) ? criteria->short_bad_threshold : kShortBadPixelThreshold;
        const double openThreshold =
            (criteria->open_bad_threshold > 0.0) ? criteria->open_bad_threshold : kOpenBadPixelThreshold;

        Matrix2D<double> blackbodyResponse(rows, cols);
        Matrix2D<uint8_t> shortMask(rows, cols, 0);
        Matrix2D<uint8_t> openMask(rows, cols, 0);
        for (int64_t i = 0; i < pixelCount; ++i)
        {
            blackbodyResponse.data[size_t(i)] =
                before.pixelResponseCount.data[size_t(i)] / (1000.0 * before.irradiancePowerDiff);
            if (v1_map[i] > shortThreshold)
                shortMask.data[size_t(i)] = 1;
            if (v2_map[i] < openThreshold)
                openMask.data[size_t(i)] = 1;
        }
        const double avgBlackbodyResponse = finiteMean(blackbodyResponse.view());
        const int32_t shortBadCount = countNonZero(shortMask.view());
        const int32_t openBadCount = countNonZero(openMask.view());

        const GbBadPixelData bad = processGbBadPixelsInternal(
            before.responsivity.view(), vn, before.netd.view(), *criteria);

        const auto validView = bad.validMask.view();
        const double afterAvgRespVoltage = maskedFiniteMean(before.pixelResponseVoltage.view(), validView);
        const double afterAvgResponsivity = maskedFiniteMean(before.responsivity.view(), validView);
        const double afterAvgNetd = maskedFiniteMean(before.netd.view(), validView);
        const double afterAvgNoiseVoltageV = maskedFiniteMean(before.noiseVoltage.view(), validView);
        const double actualIntegrationTimeMs =
            (static_cast<double>(params->integration_time_us) -
             static_cast<double>(params->invalid_integration_us)) * 1e-3;
        const double responsivityStdDev =
            maskedFiniteStdDev(before.responsivity.view(), validView, afterAvgResponsivity);

        IRGbSummary out{};
        out.before_avg_pixel_response_count = before.avgPixelResponseCount;
        out.before_avg_pixel_response_voltage = before.avgPixelResponseVoltage;
        out.volts_per_count = before.voltsPerCount;
        out.before_avg_snr = before.avgSnr;
        out.irradiance_power_diff = before.irradiancePowerDiff;
        out.before_avg_blackbody_response = avgBlackbodyResponse;
        out.before_avg_noise_voltage_v = before.avgNoiseVoltageV;
        out.before_avg_netd_mk = before.avgNetd;
        out.before_avg_detectivity = before.avgDetectivity;
        out.before_avg_responsivity = before.avgResponsivity;
        out.before_sitf = sitf;
        out.short_bad_pixel_count = shortBadCount;
        out.open_bad_pixel_count = openBadCount;

        out.after_avg_response_voltage_v = afterAvgRespVoltage;
        out.after_avg_responsivity = afterAvgResponsivity;
        out.after_avg_netd_mk = afterAvgNetd;
        out.after_avg_noise_voltage_mv = afterAvgNoiseVoltageV * 1000.0;
        out.after_response_mv_per_ms_k =
            1000.0 * afterAvgRespVoltage / actualIntegrationTimeMs / (params->t1_c - params->t0_c);
        out.after_avg_nep_w = afterAvgNoiseVoltageV / afterAvgResponsivity;
        out.after_response_nonuniformity_pct =
            100.0 * responsivityStdDev / std::abs(afterAvgResponsivity);
        out.after_avg_detectivity = maskedFiniteMean(before.detectivity.view(), validView);
        out.bad_pixels = bad.summary;

        *summary = out;
        copyGbMasks(masks, bad, &shortMask, &openMask, pixelCount);
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_GjbProcessBadPixels(
    const double *responsivity_map,
    const double *signal_t1_v_map,
    const double *signal_t2_v_map,
    const double *noise_v_map,
    const double *netd_mk_map,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    const IRGjbCriteria *criteria,
    IRGjbBadPixelSummary *summary,
    const IRGjbBadPixelMasks *masks,
    char *err,
    int32_t err_bytes)
noexcept{
    if (summary)
        *summary = IRGjbBadPixelSummary{};
    return guarded(err, err_bytes, [&] {
        const int64_t pixelCount = pixels(rows, cols);
        needed(responsivity_map, pixel_capacity, pixelCount);
        needed(signal_t1_v_map, pixel_capacity, pixelCount);
        needed(signal_t2_v_map, pixel_capacity, pixelCount);
        needed(noise_v_map, pixel_capacity, pixelCount);
        needed(netd_mk_map, pixel_capacity, pixelCount);
        require(criteria != nullptr && summary != nullptr, IR_NULL_POINTER,
                "criteria and summary must not be null");
        validateGjbCriteria(*criteria);
        validateGjbMaskCapacities(masks, pixelCount);

        const auto bad = processGjbBadPixelsInternal(
            ConstMatrixView<double>{responsivity_map, rows, cols},
            ConstMatrixView<double>{signal_t1_v_map, rows, cols},
            ConstMatrixView<double>{signal_t2_v_map, rows, cols},
            ConstMatrixView<double>{noise_v_map, rows, cols},
            ConstMatrixView<double>{netd_mk_map, rows, cols},
            *criteria);
        *summary = bad.summary;
        copyGjbMasks(masks, bad);
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_GjbEvaluateMaps(
    const double *v1_map,
    const double *v2_map,
    const double *vn_map,
    const double *vm_map,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    const IRCalcParameters *params,
    const IRGjbCriteria *criteria,
    IRGjbSummary *summary,
    const IRGjbBadPixelMasks *masks,
    char *err,
    int32_t err_bytes)
noexcept{
    if (summary)
        *summary = IRGjbSummary{};
    return guarded(err, err_bytes, [&] {
        const int64_t pixelCount = pixels(rows, cols);
        needed(v1_map, pixel_capacity, pixelCount);
        needed(v2_map, pixel_capacity, pixelCount);
        needed(vn_map, pixel_capacity, pixelCount);
        needed(vm_map, pixel_capacity, pixelCount);
        require(params != nullptr && criteria != nullptr && summary != nullptr, IR_NULL_POINTER,
                "params, criteria, and summary must not be null");
        validateCalcParameters(*params);
        validateGjbCriteria(*criteria);
        validateGjbMaskCapacities(masks, pixelCount);

        const ConstMatrixView<double> v1{v1_map, rows, cols};
        const ConstMatrixView<double> v2{v2_map, rows, cols};
        const ConstMatrixView<double> vn{vn_map, rows, cols};
        const CommonBeforeData before = computeCommonBefore(v1, v2, vn, *params);

        Matrix2D<double> signalT1V(rows, cols);
        Matrix2D<double> signalT2V(rows, cols);
        Matrix2D<double> signalVmV(rows, cols);
        for (int64_t i = 0; i < pixelCount; ++i)
        {
            signalT1V.data[size_t(i)] = v1_map[i] * before.voltsPerCount;
            signalT2V.data[size_t(i)] = v2_map[i] * before.voltsPerCount;
            signalVmV.data[size_t(i)] = vm_map[i] * before.voltsPerCount;
        }

        const GjbBadPixelData bad = processGjbBadPixelsInternal(
            before.responsivity.view(), signalT1V.view(), signalT2V.view(),
            before.noiseVoltage.view(), before.netd.view(), *criteria);

        const auto validView = bad.validMask.view();
        const double afterAvgRespVoltage = maskedFiniteMean(before.pixelResponseVoltage.view(), validView);
        const double afterAvgResponsivity = maskedFiniteMean(before.responsivity.view(), validView);
        const double afterAvgNetd = maskedFiniteMean(before.netd.view(), validView);
        const double afterAvgNoiseVoltageV = maskedFiniteMean(before.noiseVoltage.view(), validView);
        const double afterAvgNoiseVoltageMv = afterAvgNoiseVoltageV * 1000.0;
        const double responsivityStdDev =
            maskedFiniteStdDev(before.responsivity.view(), validView, afterAvgResponsivity);
        const double respNonuniformityPct =
            100.0 * responsivityStdDev / std::abs(afterAvgResponsivity);

        const double avgSignalVoltageT1 = finiteMean(signalT1V.view());
        const double avgSignalVoltageT2 = finiteMean(signalT2V.view());
        Matrix2D<double> vnuc(rows, cols);
        for (int64_t i = 0; i < pixelCount; ++i)
        {
            const double t1 = signalT1V.data[size_t(i)];
            const double t2 = signalT2V.data[size_t(i)];
            const double k = safeDivide(avgSignalVoltageT1 - avgSignalVoltageT2, t1 - t2);
            const double b = avgSignalVoltageT1 - k * t1;
            vnuc.data[size_t(i)] = k * signalVmV.data[size_t(i)] + b;
        }
        const double avgVnuc = maskedFiniteMean(vnuc.view(), validView);
        const double rfpn = maskedFiniteStdDev(vnuc.view(), validView, avgVnuc);

        IRGjbSummary out{};
        out.before_avg_pixel_response_count = before.avgPixelResponseCount;
        out.before_avg_pixel_response_voltage = before.avgPixelResponseVoltage;
        out.volts_per_count = before.voltsPerCount;
        out.before_avg_snr = before.avgSnr;
        out.fov = before.fov;
        out.irradiance_power_diff = before.irradiancePowerDiff;
        out.before_avg_noise_voltage_v = before.avgNoiseVoltageV;
        out.before_avg_netd_mk = before.avgNetd;
        out.before_avg_detectivity = before.avgDetectivity;
        out.before_avg_responsivity = before.avgResponsivity;

        out.after_avg_response_voltage_v = afterAvgRespVoltage;
        out.after_avg_responsivity = afterAvgResponsivity;
        out.after_avg_nep_w = afterAvgNoiseVoltageV / afterAvgResponsivity;
        out.after_response_nonuniformity_pct = respNonuniformityPct;
        out.after_avg_noise_voltage_mv = afterAvgNoiseVoltageMv;
        out.after_avg_netd_mk = afterAvgNetd;
        out.after_avg_detectivity = maskedFiniteMean(before.detectivity.view(), validView);
        out.after_spatial_noise = afterAvgNoiseVoltageMv * respNonuniformityPct;
        const double maxOutputVoltage = params->vmax_per_count * before.voltsPerCount;
        out.after_max_dynamic_range_db = 20.0 * std::log10(maxOutputVoltage / afterAvgNoiseVoltageV);
        out.after_rfpn_v = rfpn;
        out.bad_pixels = bad.summary;

        *summary = out;
        copyGjbMasks(masks, bad);
    });
}
