#include "IR_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <vector>

namespace
{

int gFailures = 0;

void checkTrue(bool cond, const char *expr, int line)
{
    if (!cond)
    {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
        ++gFailures;
    }
}

void checkNear(double actual, double expected, double tol, const char *label, int line)
{
    const double diff = std::abs(actual - expected);
    if (!std::isfinite(actual) || diff > tol)
    {
        std::fprintf(stderr, "FAIL line %d (%s): actual=%.17g expected=%.17g diff=%.17g tol=%.17g\n",
                     line, label, actual, expected, diff, tol);
        ++gFailures;
    }
}

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)
#define CHECK_NEAR(actual, expected, tol) checkNear((actual), (expected), (tol), #actual, __LINE__)

void testBuildCalibDn()
{
    const int32_t rows = 2, cols = 3;
    const int64_t n = 6;
    const double meanHigh[6] = {1100.0, 1100.0, 1100.0, 1100.0, 1300.0, 1999.0};
    const double meanLow[6] = {1000.0, 1000.0, 1000.0, 1000.0, 1000.0, 1000.0};
    const double rmsMap[6] = {2.0, 2.0, 2.0, 2.0, 2.0, 8.0}; // rms mean = 3.0; pixel 5 blinded first
    const double bMap[6] = {10.0, 20.0, 30.0, 40.0, 999.0, 999.0};

    const IRNoise3DCalibParams params{0.5, 1.5, 0.5, 1.5, 1e-6};
    uint8_t respBlind[6]{}, rmsBlind[6]{}, totalBlind[6]{}, validMask[6]{};
    double kMap[6]{}, stats[IR_NOISE3D_STATS_COUNT]{};
    IRNoise3DCalibOutputs outputs{
        respBlind, rmsBlind, totalBlind, validMask, kMap, stats, n, IR_NOISE3D_STATS_COUNT, 0};

    char err[256]{};
    CHECK(IR_Noise3DBuildCalibDn(meanHigh, meanLow, rmsMap, bMap, rows, cols, n,
                                 &params, &outputs, err, sizeof err) == IR_OK);

    // Pixel 5 was RMS blind so responseMean only averages [0..4] = (400 + 300) / 5 = 140
    CHECK(rmsBlind[5] == 1 && rmsBlind[4] == 0);
    CHECK(respBlind[4] == 1 && respBlind[5] == 0);
    CHECK(totalBlind[4] == 1 && totalBlind[5] == 1);
    for (int i = 0; i < 4; ++i)
    {
        CHECK(totalBlind[i] == 0 && validMask[i] == 1);
        CHECK_NEAR(kMap[i], 140.0 / (100.0 + 1e-6), 1e-12);
    }
    CHECK(kMap[4] == 0.0 && kMap[5] == 0.0);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_RMS_MEAN], 3.0, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_RMS_LOW], 1.5, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_RMS_HIGH], 4.5, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_RMS_BLIND_COUNT], 1.0, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_RESPONSE_MEAN], 140.0, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_RESPONSE_LOW], 70.0, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_RESPONSE_HIGH], 210.0, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_RESPONSE_BLIND_COUNT], 1.0, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_TOTAL_BLIND_COUNT], 2.0, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_VALID_COUNT], 4.0, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_B_AVG], 25.0, 1e-12);
    CHECK_NEAR(stats[IR_NOISE3D_STAT_K_MEAN_VALID], 140.0 / (100.0 + 1e-6), 1e-12);
}

void testComputeFramesOrgHandDerived()
{
    const int32_t frameCount = 2, rows = 2, cols = 2;
    const int64_t n = 4, totalSamples = 8;
    const uint16_t frames[8] = {
        10, 20, 30, 40,
        14, 22, 38, 42
    };
    const uint8_t blindMask[4] = {0, 0, 0, 0};
    const int32_t histInterval = 4;

    double scalars[IR_NOISE3D_SCALAR_COUNT]{};
    double hist[IR_NOISE3D_HIST_GROUP_COUNT * 4]{};
    double histMeta[IR_NOISE3D_HIST_GROUP_COUNT * 3]{};
    double mapIJ[4]{}, mapTI[4]{}, mapTJ[4]{}, meta[IR_NOISE3D_META_COUNT]{};

    const IRNoise3DComputeParams params{IR_NOISE3D_MODE_ORG, 4, histInterval, 0, 0.0, -999.0};
    IRNoise3DComputeOutputs outputs{};
    outputs.scalars = scalars;
    outputs.hist = hist;
    outputs.hist_meta = histMeta;
    outputs.map_ij = mapIJ;
    outputs.map_ti = mapTI;
    outputs.map_tj = mapTJ;
    outputs.meta = meta;
    outputs.hist_capacity = IR_NOISE3D_HIST_GROUP_COUNT * histInterval;
    outputs.map_ij_capacity = n;
    outputs.map_ti_capacity = frameCount * rows;
    outputs.map_tj_capacity = frameCount * cols;
    outputs.scalars_capacity = IR_NOISE3D_SCALAR_COUNT;
    outputs.hist_meta_capacity = IR_NOISE3D_HIST_GROUP_COUNT * 3;
    outputs.meta_capacity = IR_NOISE3D_META_COUNT;

    char err[256]{};
    CHECK(IR_Noise3DComputeFramesDn(frames, totalSamples, frameCount, rows, cols,
                                    blindMask, n, nullptr, nullptr, 0,
                                    &params, &outputs, err, sizeof err) == IR_OK);

    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_T], std::sqrt(8.0), 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_I], std::sqrt(220.5), 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_J], std::sqrt(32.0), 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_IJ], std::sqrt(1.0 / 3.0), 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_TI], std::sqrt(1.0 / 3.0), 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_TJ], std::sqrt(4.0 / 3.0), 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_TIJ], std::sqrt(2.0 / 7.0), 1e-12);
    const double expectedTotal = std::sqrt(
        8.0 + 220.5 + 32.0 + (1.0 / 3.0) + (1.0 / 3.0) + (4.0 / 3.0) + (2.0 / 7.0));
    const double expectedSys = std::sqrt((1.0 / 3.0) + (2.0 / 7.0));
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_TOTAL], expectedTotal, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_SYS], expectedSys, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_GLOBAL_MEAN], 27.0, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_VALID_PIXEL_COUNT], 4.0, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_VALID_SAMPLE_COUNT], 8.0, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_FRAME_COUNT], 2.0, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_ROWS], 2.0, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_COLS], 2.0, 1e-12);

    CHECK_NEAR(mapIJ[0], -0.5, 1e-12);
    CHECK_NEAR(mapIJ[1], 0.5, 1e-12);
    CHECK_NEAR(mapIJ[2], 0.5, 1e-12);
    CHECK_NEAR(mapIJ[3], -0.5, 1e-12);

    CHECK_NEAR(mapTI[0], 0.5, 1e-12);
    CHECK_NEAR(mapTI[1], -0.5, 1e-12);
    CHECK_NEAR(mapTI[2], -0.5, 1e-12);
    CHECK_NEAR(mapTI[3], 0.5, 1e-12);

    CHECK_NEAR(mapTJ[0], -1.0, 1e-12);
    CHECK_NEAR(mapTJ[1], 1.0, 1e-12);
    CHECK_NEAR(mapTJ[2], 1.0, 1e-12);
    CHECK_NEAR(mapTJ[3], -1.0, 1e-12);

    // Check histogram conservation
    auto sumGroup = [&](int group) {
        return std::accumulate(hist + group * histInterval, hist + (group + 1) * histInterval, 0.0);
    };
    CHECK_NEAR(sumGroup(IR_NOISE3D_HIST_T), double(frameCount), 1e-12);
    CHECK_NEAR(sumGroup(IR_NOISE3D_HIST_I), double(rows), 1e-12);
    CHECK_NEAR(sumGroup(IR_NOISE3D_HIST_J), double(cols), 1e-12);
    CHECK_NEAR(sumGroup(IR_NOISE3D_HIST_TIJ), double(totalSamples), 1e-12);
    CHECK_NEAR(meta[IR_NOISE3D_META_EFFECTIVE_THREAD_COUNT], 1.0, 1e-12);
}

void testComputeFramesNucAndBlindPixels()
{
    const int32_t frameCount = 3, rows = 2, cols = 3;
    const int64_t n = 6, totalSamples = 18;
    // Pixel (1,2) (index 5) is blind; org has 9999 there to verify exclusion
    const uint16_t orgFrames[18] = {
        15, 20, 25, 17, 22, 9999,
        16, 21, 26, 18, 23, 9999,
        14, 19, 24, 16, 21, 9999
    };
    const uint8_t blindMask[6] = {0, 0, 0, 0, 0, 1};
    const double kMap[6] = {2.0, 2.0, 2.0, 2.0, 2.0, 0.0};
    const double bMap[6] = {10.0, 10.0, 10.0, 10.0, 10.0, 99.0};
    const double bAvg = 100.0;
    const double invalidFill = -1234.5;
    const int32_t histInterval = 5;

    std::vector<uint16_t> nucFrames(size_t(totalSamples), 0);
    double scalars[IR_NOISE3D_SCALAR_COUNT]{};
    double hist[IR_NOISE3D_HIST_GROUP_COUNT * 5]{};
    double histMeta[IR_NOISE3D_HIST_GROUP_COUNT * 3]{};
    double mapIJ[6]{}, mapTI[9]{}, mapTJ[9]{};

    const IRNoise3DComputeParams params{IR_NOISE3D_MODE_NUC, 1, histInterval, 0, bAvg, invalidFill};
    IRNoise3DComputeOutputs outputs{};
    outputs.nuc_frames = nucFrames.data();
    outputs.scalars = scalars;
    outputs.hist = hist;
    outputs.hist_meta = histMeta;
    outputs.map_ij = mapIJ;
    outputs.map_ti = mapTI;
    outputs.map_tj = mapTJ;
    outputs.nuc_frames_capacity = totalSamples;
    outputs.hist_capacity = IR_NOISE3D_HIST_GROUP_COUNT * histInterval;
    outputs.map_ij_capacity = n;
    outputs.map_ti_capacity = frameCount * rows;
    outputs.map_tj_capacity = frameCount * cols;
    outputs.scalars_capacity = IR_NOISE3D_SCALAR_COUNT;
    outputs.hist_meta_capacity = IR_NOISE3D_HIST_GROUP_COUNT * 3;

    char err[256]{};
    CHECK(IR_Noise3DComputeFramesDn(orgFrames, totalSamples, frameCount, rows, cols,
                                    blindMask, n, kMap, bMap, n,
                                    &params, &outputs, err, sizeof err) == IR_OK);

    // Blind pixel must receive bAvg in nuc_frames and invalidFill in map_ij
    CHECK(nucFrames[5] == 100 && nucFrames[11] == 100 && nucFrames[17] == 100);
    // Valid pixel (0,0) in frame 0: 2*(15-10)+100 = 110
    CHECK(nucFrames[0] == 110);
    CHECK_NEAR(mapIJ[5], invalidFill, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_VALID_PIXEL_COUNT], 5.0, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_VALID_SAMPLE_COUNT], 15.0, 1e-12);
    // Notice all residuals across t are identically shifted per frame (+2 on t=1, -2 on t=2) while row/col profiles match across frames:
    // So DELTA_IJ, DELTA_TI, DELTA_TJ, DELTA_TIJ should be near 0!
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_T], 2.0, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_TI], 0.0, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_TJ], 0.0, 1e-12);
    CHECK_NEAR(scalars[IR_NOISE3D_SCALAR_DELTA_TIJ], 0.0, 1e-12);
}

void testErrorPaths()
{
    const int32_t rows = 2, cols = 2, frameCount = 2;
    const int64_t n = 4, totalSamples = 8;
    const uint16_t frames[8] = {10, 11, 12, 13, 10, 11, 12, 13};
    const double meanH[4] = {200, 200, 200, 200};
    const double meanL[4] = {100, 100, 100, 100};
    const double rms[4] = {2, 2, 2, 2};
    const double bMap[4] = {50, 50, 50, 50};

    const IRNoise3DCalibParams cParams{0.5, 1.5, 0.5, 1.5, 1e-6};
    uint8_t rB[4]{}, nB[4]{}, tB[4]{}, vM[4]{};
    double kM[4]{}, stats[IR_NOISE3D_STATS_COUNT]{};
    IRNoise3DCalibOutputs cOut{rB, nB, tB, vM, kM, stats, n, IR_NOISE3D_STATS_COUNT, 0};
    char err[256]{};

    CHECK(IR_Noise3DBuildCalibDn(nullptr, meanL, rms, bMap, rows, cols, n,
                                 &cParams, &cOut, err, sizeof err) == IR_NULL_POINTER);
    IRNoise3DCalibOutputs smallOut = cOut;
    smallOut.stats_capacity = 10;
    CHECK(IR_Noise3DBuildCalibDn(meanH, meanL, rms, bMap, rows, cols, n,
                                 &cParams, &smallOut, err, sizeof err) == IR_BUFFER_TOO_SMALL);
    double badMeanH[4] = {200, 200, std::numeric_limits<double>::quiet_NaN(), 200};
    CHECK(IR_Noise3DBuildCalibDn(badMeanH, meanL, rms, bMap, rows, cols, n,
                                 &cParams, &cOut, err, sizeof err) == IR_BAD_ARGUMENT);

    // Compute error paths: empty row & < 2 valid pixels
    double scalars[IR_NOISE3D_SCALAR_COUNT]{};
    double hist[IR_NOISE3D_HIST_GROUP_COUNT * 2]{};
    double histMeta[IR_NOISE3D_HIST_GROUP_COUNT * 3]{};
    double mapIJ[4]{}, mapTI[4]{}, mapTJ[4]{};
    const IRNoise3DComputeParams pOrg{IR_NOISE3D_MODE_ORG, 1, 2, 0, 0.0, 0.0};
    IRNoise3DComputeOutputs outOrg{};
    outOrg.scalars = scalars;
    outOrg.hist = hist;
    outOrg.hist_meta = histMeta;
    outOrg.map_ij = mapIJ;
    outOrg.map_ti = mapTI;
    outOrg.map_tj = mapTJ;
    outOrg.hist_capacity = IR_NOISE3D_HIST_GROUP_COUNT * 2;
    outOrg.map_ij_capacity = 4;
    outOrg.map_ti_capacity = 4;
    outOrg.map_tj_capacity = 4;
    outOrg.scalars_capacity = IR_NOISE3D_SCALAR_COUNT;
    outOrg.hist_meta_capacity = IR_NOISE3D_HIST_GROUP_COUNT * 3;

    const uint8_t emptyRowMask[4] = {1, 1, 0, 0};
    CHECK(IR_Noise3DComputeFramesDn(frames, totalSamples, frameCount, rows, cols,
                                    emptyRowMask, n, nullptr, nullptr, 0,
                                    &pOrg, &outOrg, err, sizeof err) == IR_COMPUTATION_FAILED);

    const uint8_t oneValidMask[4] = {0, 1, 1, 1};
    CHECK(IR_Noise3DComputeFramesDn(frames, totalSamples, frameCount, rows, cols,
                                    oneValidMask, n, nullptr, nullptr, 0,
                                    &pOrg, &outOrg, err, sizeof err) == IR_COMPUTATION_FAILED);

    const IRNoise3DComputeParams pNuc{IR_NOISE3D_MODE_NUC, 1, 2, 0, 0.0, 0.0};
    const uint8_t okMask[4] = {0, 0, 0, 0};
    CHECK(IR_Noise3DComputeFramesDn(frames, totalSamples, frameCount, rows, cols,
                                    okMask, n, nullptr, bMap, n,
                                    &pNuc, &outOrg, err, sizeof err) == IR_NULL_POINTER);
}

} // namespace

int main()
{
    testBuildCalibDn();
    testComputeFramesOrgHandDerived();
    testComputeFramesNucAndBlindPixels();
    testErrorPaths();

    if (gFailures == 0)
        std::printf("noise3d_test: ALL PASSED\n");
    return gFailures == 0 ? 0 : 1;
}
