#include "IR_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>
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

void testNucBuilderAffineAndFrameStats()
{
    const int32_t rows = 2, cols = 2;
    const int64_t n = 4;
    // Pixel 3 is masked invalid
    const double v1[4] = {30200.0, 30100.0, 30300.0, 9999.0};
    const double v2[4] = {30000.0, 30000.0, 30000.0, 9999.0};
    const double ref[4] = {30100.0, 30050.0, 30150.0, 9999.0};
    const uint8_t validMask[4] = {1, 1, 1, 0};

    double kMap[4]{}, bMap[4]{}, nucSummary[4]{};
    char err[256]{};
    CHECK(IR_BuildTwoPointNucF64(v1, v2, validMask, ref, rows, cols, n,
                                  kMap, bMap, nucSummary, 4, err, sizeof err) == IR_OK);
    CHECK_NEAR(nucSummary[0], 30200.0, 1e-12);
    CHECK_NEAR(nucSummary[1], 30000.0, 1e-12);
    CHECK_NEAR(nucSummary[2], 30100.0, 1e-12);
    CHECK_NEAR(nucSummary[3], 3.0, 1e-12);

    CHECK_NEAR(kMap[0], 1.0, 1e-12);
    CHECK_NEAR(kMap[1], 2.0, 1e-12);
    CHECK_NEAR(kMap[2], 2.0 / 3.0, 1e-12);
    CHECK_NEAR(kMap[3], 0.0, 1e-12);
    CHECK_NEAR(bMap[3], 0.0, 1e-12);

    double aff[4]{};
    CHECK(IR_ApplyAffineMapF64(ref, kMap, bMap, rows, cols, n, aff, err, sizeof err) == IR_OK);
    CHECK_NEAR(aff[0], 30100.0, 1e-12);
    CHECK_NEAR(aff[1], 30100.0, 1e-12);
    CHECK_NEAR(aff[2], 30100.0, 1e-12);
    CHECK_NEAR(aff[3], 0.0, 1e-12);

    // 2 frames centered at ref with +/-4 DN post-NUC swing (raw swing = 4 / k)
    const uint16_t frames[8] = {
        30096, 30048, 30144, 1234,
        30104, 30052, 30156, 5678
    };
    const double dark[4] = {100.0, 100.0, 100.0, 999.0};
    double meanMap[4]{}, stdMap[4]{}, summary[2]{};
    CHECK(IR_FrameStatisticsNucU16(frames, 8, 2, rows, cols, 0,
                                  kMap, bMap, dark, validMask,
                                  meanMap, stdMap, n, summary, 2, err, sizeof err) == IR_OK);
    CHECK_NEAR(meanMap[0], 30000.0, 1e-12);
    CHECK_NEAR(meanMap[1], 30000.0, 1e-12);
    CHECK_NEAR(meanMap[2], 30000.0, 1e-12);
    CHECK_NEAR(stdMap[0], 4.0, 1e-12);
    CHECK_NEAR(stdMap[1], 4.0, 1e-12);
    CHECK_NEAR(stdMap[2], 4.0, 1e-12);
    CHECK_NEAR(summary[0], 30000.0, 1e-12);
    CHECK_NEAR(summary[1], 4.0, 1e-12);
}

void testMapSummaryAndBinaryAndThresholds()
{
    const int32_t rows = 4, cols = 4;
    const int64_t n = 16;
    std::vector<double> map(static_cast<size_t>(n));
    for (int i = 0; i < 16; ++i)
        map[size_t(i)] = static_cast<double>(i + 1); // 1..16

    char err[256]{};
    IRMapSummary fullSum{};
    CHECK(IR_MapSummaryF64(map.data(), nullptr, rows, cols, n,
                            0, 0, 0, 0, 0.0, 100.0, &fullSum, err, sizeof err) == IR_OK);
    CHECK_NEAR(fullSum.mean, 8.5, 1e-12);
    CHECK_NEAR(fullSum.min_val, 1.0, 1e-12);
    CHECK_NEAR(fullSum.max_val, 16.0, 1e-12);
    CHECK_NEAR(fullSum.span, 15.0, 1e-12);
    CHECK_NEAR(fullSum.percentile_span, 15.0, 1e-12);
    CHECK(fullSum.valid_count == 16);

    // ROI (x=1, y=1, w=2, h=2) -> {6, 7, 10, 11}, mean = 8.5, span = 5, p25=6.75, p75=10.25 -> pspan=3.5
    IRMapSummary roiSum{};
    CHECK(IR_MapSummaryF64(map.data(), nullptr, rows, cols, n,
                            1, 1, 2, 2, 25.0, 75.0, &roiSum, err, sizeof err) == IR_OK);
    CHECK(roiSum.valid_count == 4);
    CHECK_NEAR(roiSum.mean, 8.5, 1e-12);
    CHECK_NEAR(roiSum.span, 5.0, 1e-12);
    CHECK_NEAR(roiSum.percentile_low, 6.75, 1e-12);
    CHECK_NEAR(roiSum.percentile_high, 10.25, 1e-12);
    CHECK_NEAR(roiSum.percentile_span, 3.5, 1e-12);

    // Binary ops including variance subtraction and safeDivide
    const double a[4] = {5.0, 2.0, 10.0, 6.0};
    const double b[4] = {3.0, 4.0, 0.0, 2.0};
    double out[4]{};
    CHECK(IR_MapBinaryOpF64(a, b, 2, 2, 4, IR_MAP_OP_VARIANCE_SUBTRACT, 1.0, out, err, sizeof err) == IR_OK);
    CHECK_NEAR(out[0], 4.0, 1e-12);
    CHECK_NEAR(out[1], 0.0, 1e-12);
    CHECK(IR_MapBinaryOpF64(a, b, 2, 2, 4, IR_MAP_OP_SAFE_DIV, 1.0, out, err, sizeof err) == IR_OK);
    CHECK_NEAR(out[2], 0.0, 1e-12);
    CHECK_NEAR(out[3], 3.0, 1e-12);

    // Dynamic thresholds & boolean combination
    uint8_t baseMask[16]{};
    std::fill_n(baseMask, 16, uint8_t{1});
    baseMask[0] = 0; // exclude 1.0
    uint8_t passAbs[16]{}, passMult[16]{}, passSigma[16]{}, combined[16]{};
    int32_t pCnt = 0, fCnt = 0, combCnt = 0;
    CHECK(IR_ThresholdMaskF64(map.data(), baseMask, rows, cols, n,
                              IR_THRESH_ABSOLUTE, 5.0, 12.0, passAbs, &pCnt, &fCnt, err, sizeof err) == IR_OK);
    CHECK(pCnt == 8 && fCnt == 8);
    CHECK(IR_ThresholdMaskF64(map.data(), baseMask, rows, cols, n,
                              IR_THRESH_MEAN_MULTIPLIER, 0.5, 1.5, passMult, &pCnt, &fCnt, err, sizeof err) == IR_OK);
    CHECK(pCnt == 9 && fCnt == 7); // mean of 2..16 is 9.0 -> [4.5, 13.5] -> 5..13 (9 pixels)
    CHECK(IR_ThresholdMaskF64(map.data(), baseMask, rows, cols, n,
                              IR_THRESH_MEAN_SIGMA, 1.0, 1.0, passSigma, &pCnt, &fCnt, err, sizeof err) == IR_OK);
    CHECK(pCnt > 0);
    CHECK(IR_MaskCombine(passMult, passAbs, rows, cols, n, IR_MASK_AND_NOT,
                         combined, &combCnt, err, sizeof err) == IR_OK);
    CHECK(combCnt == 1 && combined[12] == 1); // pixel value 13 is at index 12
    CHECK(IR_MaskCombine(baseMask, nullptr, rows, cols, n, IR_MASK_NOT,
                         combined, &combCnt, err, sizeof err) == IR_OK);
    CHECK(combCnt == 1 && combined[0] == 1);
}

void testSpatialLowHighPass15x15WithDcAndBlindMask()
{
    const int32_t rows = 32, cols = 32;
    const int64_t n = int64_t(rows) * cols;
    // Constant 50000.0 + blind pixel at (15,15) holding NaN -> must NOT contaminate neighbors
    std::vector<double> map(size_t(n), 50000.0);
    std::vector<uint8_t> mask(size_t(n), 1);
    map[size_t(15) * cols + 15] = std::numeric_limits<double>::quiet_NaN();
    mask[size_t(15) * cols + 15] = 0;

    std::vector<double> low(size_t(n), 0.0), high(size_t(n), 0.0);
    char err[256]{};
    CHECK(IR_SpatialLowHighPassF64(map.data(), mask.data(), rows, cols, n,
                                   IR_SPATIAL_FILTER_BOX_MEAN, 15, 15,
                                   low.data(), high.data(), err, sizeof err) == IR_OK);
    for (int64_t i = 0; i < n; ++i)
    {
        CHECK_NEAR(low[size_t(i)], 50000.0, 1e-12);
        CHECK_NEAR(high[size_t(i)], 0.0, 1e-12);
    }

    // Small 5x5 exact border clamping check with 3x3 kernel and masked pixel (0,1)
    double small[25];
    uint8_t smallMask[25];
    for (int i = 0; i < 25; ++i)
    {
        small[i] = static_cast<double>(i + 1);
        smallMask[i] = 1;
    }
    small[1] = 9999.0;
    smallMask[1] = 0;
    double smallLow[25]{}, smallHigh[25]{};
    CHECK(IR_SpatialLowHighPassF64(small, smallMask, 5, 5, 25,
                                   IR_SPATIAL_FILTER_BOX_MEAN, 3, 3,
                                   smallLow, smallHigh, err, sizeof err) == IR_OK);
    // Corner (0,0) neighborhood is rows 0..1, cols 0..1 -> indices {0, 1(masked), 5, 6} -> mean(1, 6, 7) = 14/3
    CHECK_NEAR(smallLow[0], 14.0 / 3.0, 1e-12);
    CHECK_NEAR(smallHigh[0], 1.0 - 14.0 / 3.0, 1e-12);
    CHECK_NEAR(smallHigh[1], 0.0, 1e-12);
}

void testSpatialGaussianWithDcAndBlindMask()
{
    const int32_t rows = 15, cols = 15;
    const int64_t n = int64_t(rows) * cols;
    std::vector<double> dcMap(size_t(n), 50000.0);
    std::vector<uint8_t> mask(size_t(n), 1);
    dcMap[size_t(7) * cols + 7] = std::numeric_limits<double>::quiet_NaN();
    mask[size_t(7) * cols + 7] = 0;

    std::vector<double> low(size_t(n), 0.0), high(size_t(n), 0.0);
    char err[256]{};
    CHECK(IR_SpatialLowHighPassF64(dcMap.data(), mask.data(), rows, cols, n,
                                   IR_SPATIAL_FILTER_GAUSSIAN, 5, 5,
                                   low.data(), high.data(), err, sizeof err) == IR_OK);
    for (int64_t i = 0; i < n; ++i)
    {
        CHECK_NEAR(low[size_t(i)], 50000.0, 1e-12);
        CHECK_NEAR(high[size_t(i)], 0.0, 1e-12);
    }

    // Cross-check 5x5 Gaussian against exact 2D masked normalized Gaussian formula with gradient + blind NaN
    std::vector<double> grad(25);
    std::vector<uint8_t> gMask(25, 1);
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 5; ++c)
            grad[size_t(r) * 5 + c] = 30000.0 + 10.0 * r + 3.0 * c * c;
    grad[1 * 5 + 2] = std::numeric_limits<double>::quiet_NaN();
    gMask[1 * 5 + 2] = 0;
    grad[2 * 5 + 2] = 99999.0;
    gMask[2 * 5 + 2] = 0;

    double gLow[25]{}, gHigh[25]{};
    const int32_t k = 5;
    CHECK(IR_SpatialLowHighPassF64(grad.data(), gMask.data(), 5, 5, 25,
                                   IR_SPATIAL_FILTER_GAUSSIAN, k, k,
                                   gLow, gHigh, err, sizeof err) == IR_OK);
    const double sigma = 0.3 * (double(k - 1) * 0.5 - 1.0) + 0.8;
    double w1[5]{}, wSum = 0.0;
    for (int d = -2; d <= 2; ++d)
    {
        w1[d + 2] = std::exp(-double(d * d) / (2.0 * sigma * sigma));
        wSum += w1[d + 2];
    }
    for (double &w : w1) w /= wSum;
    for (int r = 0; r < 5; ++r)
    {
        for (int c = 0; c < 5; ++c)
        {
            double num = 0.0, den = 0.0;
            for (int nr = std::max(0, r - 2); nr <= std::min(4, r + 2); ++nr)
            {
                for (int nc = std::max(0, c - 2); nc <= std::min(4, c + 2); ++nc)
                {
                    const int idx = nr * 5 + nc;
                    if (!gMask[idx] || !std::isfinite(grad[idx])) continue;
                    const double w2d = w1[nr - r + 2] * w1[nc - c + 2];
                    num += w2d * grad[idx];
                    den += w2d;
                }
            }
            const double expectedLow = (den > 0.0) ? (num / den) : 0.0;
            const int cur = r * 5 + c;
            const double expectedHigh = (gMask[cur] && std::isfinite(grad[cur]) && den > 0.0)
                ? (grad[cur] - expectedLow) : 0.0;
            CHECK_NEAR(gLow[cur], expectedLow, 1e-10);
            CHECK_NEAR(gHigh[cur], expectedHigh, 1e-10);
        }
    }
}

void testFftAndChannelSpectrum()
{
    constexpr double kPi = std::numbers::pi;
    char err[256]{};

    // 1. Power-of-2 N=64, Fs=64 Hz, 5 Hz sine amplitude 3.5 + DC 12000
    {
        const int32_t N = 64;
        std::vector<double> sig(N);
        for (int i = 0; i < N; ++i)
            sig[size_t(i)] = 12000.0 + 3.5 * std::sin(2.0 * kPi * 5.0 * i / 64.0);
        double freqs[33]{}, amps[33]{}, psd[33]{};
        CHECK(IR_ComputeRfft1D(sig.data(), N, 64.0, IR_WINDOW_RECTANGULAR, 1,
                               freqs, amps, psd, 33, err, sizeof err) == IR_OK);
        CHECK_NEAR(freqs[5], 5.0, 1e-12);
        CHECK_NEAR(amps[0], 0.0, 1e-11);
        CHECK_NEAR(amps[5], 3.5, 1e-11);
        CHECK_NEAR(amps[6], 0.0, 1e-11);
        const double totalPsdPower = std::accumulate(psd, psd + 33, 0.0); // df = 1.0 Hz
        CHECK_NEAR(totalPsdPower, 0.5 * 3.5 * 3.5, 1e-10);
    }

    // 2. Non-power-of-2 Bluestein N=100, Fs=100 Hz, 10 Hz sine + Hann window check
    {
        const int32_t N = 100;
        std::vector<double> sig(N);
        for (int i = 0; i < N; ++i)
            sig[size_t(i)] = 500.0 + 4.25 * std::cos(2.0 * kPi * 10.0 * i / 100.0);
        double freqs[51]{}, amps[51]{};
        CHECK(IR_ComputeRfft1D(sig.data(), N, 100.0, IR_WINDOW_RECTANGULAR, 1,
                               freqs, amps, nullptr, 51, err, sizeof err) == IR_OK);
        CHECK_NEAR(freqs[10], 10.0, 1e-12);
        CHECK_NEAR(amps[10], 4.25, 1e-10);
        CHECK_NEAR(amps[0], 0.0, 1e-10);

        CHECK(IR_ComputeRfft1D(sig.data(), N, 100.0, IR_WINDOW_HANN, 1,
                               freqs, amps, nullptr, 51, err, sizeof err) == IR_OK);
        // Symmetric Hann (denom N-1) has an exact O(1/N^2) finite-length leakage offset (~8.21e-5 at N=100);
        // verify Bluestein matches exact DFT with symmetric Hann weights to 1e-12.
        double sigMean = 0.0;
        for (double v : sig) sigMean += v;
        sigMean /= N;
        double refRe = 0.0, refIm = 0.0, wSum = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (N - 1)));
            const double xw = (sig[size_t(i)] - sigMean) * w;
            const double angle = 2.0 * kPi * 10.0 * i / N;
            refRe += xw * std::cos(angle);
            refIm -= xw * std::sin(angle);
            wSum += w;
        }
        const double expectedHannAmp = 2.0 * std::hypot(refRe, refIm) / wSum;
        CHECK_NEAR(amps[10], expectedHannAmp, 1e-12);
        CHECK_NEAR(amps[10], 4.25, 1e-3);
    }

    // 3. Multi-channel U16 spectrum: 4 column channels, 8x8 frames, 100 frames @ 100 Hz, 25 Hz cosine on ch=2
    {
        const int32_t T = 100, rows = 8, cols = 8, chCount = 4;
        const int64_t totalSamples = int64_t(T) * rows * cols;
        std::vector<uint16_t> frames(size_t(totalSamples), 1000);
        for (int t = 0; t < T; ++t)
        {
            const int delta = static_cast<int>(std::lround(10.0 * std::cos(2.0 * kPi * 25.0 * t / 100.0)));
            for (int r = 0; r < rows; ++r)
                for (int c = 2; c < cols; c += chCount)
                    frames[size_t(t) * 64 + size_t(r) * cols + c] = static_cast<uint16_t>(1000 + delta);
        }
        const int32_t bins = T / 2 + 1;
        double freqs[51]{};
        std::vector<double> series(size_t(chCount) * T);
        std::vector<double> amps(size_t(chCount) * bins);
        double chMean[4]{}, chStd[4]{};
        CHECK(IR_ChannelSpectrumU16(frames.data(), totalSamples, T, rows, cols,
                                    IR_SPECTRUM_AXIS_COLUMNS, chCount, 100.0, IR_WINDOW_RECTANGULAR,
                                    freqs, bins, series.data(), int64_t(series.size()),
                                    amps.data(), int64_t(amps.size()),
                                    chMean, chStd, chCount, err, sizeof err) == IR_OK);
        CHECK_NEAR(chMean[0], 1000.0, 1e-12);
        CHECK_NEAR(chStd[0], 0.0, 1e-12);
        CHECK_NEAR(chMean[2], 1000.0, 1e-12);
        CHECK_NEAR(chStd[2], std::sqrt(50.0), 1e-12); // amplitude 10 cosine RMS = 10 / sqrt(2)
        CHECK_NEAR(amps[size_t(2) * bins + 25], 10.0, 1e-10);
        CHECK_NEAR(amps[size_t(0) * bins + 25], 0.0, 1e-12);
    }
}

void testGjbBurstMetricsAndErrors()
{
    const int32_t rows = 32, cols = 32;
    const int64_t n = int64_t(rows) * cols;
    std::vector<double> meanMap(static_cast<size_t>(n));
    std::vector<double> vnMap(size_t(n), 5.0);
    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            const double lf = 20.0 * (static_cast<double>(r) - 15.5) / 15.5;
            const double hf = (((r + c) % 2) == 0) ? 2.0 : -2.0;
            meanMap[size_t(r) * cols + c] = 30000.0 + lf + hf;
        }
    }

    const IRGjbDriftContext ctx{
        2.0 / 65535.0, 10.0, 200.0, 1.0e5, 2.0,
        1.0, 100, IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE, 15, 0
    };
    IRGjbBurstMetrics m{};
    char err[256]{};
    CHECK(IR_GjbEvaluateBurstMetrics(meanMap.data(), vnMap.data(), nullptr,
                                     rows, cols, n, &ctx, &m, err, sizeof err) == IR_OK);
    CHECK(m.valid_pixel_count == n);
    CHECK_NEAR(m.avg_temporal_noise_dn, 5.0, 1e-12);
    CHECK_NEAR(m.rms_temporal_noise_dn, 5.0, 1e-12);
    CHECK_NEAR(m.estimated_netd_mk, 250.0, 1e-12);
    CHECK(m.spatial_std_dn > 0.0);
    const double expectedDenoised =
        std::sqrt(std::max(0.0, m.spatial_std_dn * m.spatial_std_dn - 25.0 / 100.0));
    CHECK_NEAR(m.spatial_std_denoised_dn, expectedDenoised, 1e-12);
    CHECK(m.lrnu_std_dn > 0.0 && m.lrnu_span_dn > 0.0 && m.hrnu_std_dn > 0.0);

    // Negative / boundary checks
    double dummy[4]{};
    CHECK(IR_SpatialLowHighPassF64(meanMap.data(), nullptr, 4, 4, 16,
                                   IR_SPATIAL_FILTER_BOX_MEAN, 14, 15,
                                   dummy, nullptr, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_ComputeRfft1D(dummy, 2, 10.0, IR_WINDOW_HANN, 1,
                           nullptr, dummy, nullptr, 2, err, sizeof err) == IR_BAD_ARGUMENT);
    IRGjbDriftContext badCtx = ctx;
    badCtx.lrnu_kernel_size = 14;
    CHECK(IR_GjbEvaluateBurstMetrics(meanMap.data(), vnMap.data(), nullptr,
                                     rows, cols, n, &badCtx, &m, err, sizeof err) == IR_BAD_ARGUMENT);
}

} // namespace

int main()
{
    testNucBuilderAffineAndFrameStats();
    testMapSummaryAndBinaryAndThresholds();
    testSpatialLowHighPass15x15WithDcAndBlindMask();
    testSpatialGaussianWithDcAndBlindMask();
    testFftAndChannelSpectrum();
    testGjbBurstMetricsAndErrors();

    if (gFailures == 0)
        std::printf("composable_ops_test: ALL PASSED\n");
    return gFailures == 0 ? 0 : 1;
}
