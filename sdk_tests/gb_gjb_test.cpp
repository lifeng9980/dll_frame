#include "IR_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
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

IRCalcParameters defaultCalcParams()
{
    IRCalcParameters p{};
    p.t0_c = 20.0;
    p.t1_c = 30.0;
    p.integration_time_us = 1000;
    p.invalid_integration_us = 0;
    p.pixel_pitch_um = 15.0;
    p.emissivity = 1.0;
    p.system_gain = 1.0;
    p.adc_swing_v = 2.0;
    p.digital_full = 65535;
    p.f_number = 1.0;
    p.vmax_per_count = 2.0;
    return p;
}

void testNonOverlappingCategoriesMatchTotal()
{
    const int32_t rows = 512, cols = 640;
    const int64_t n = int64_t(rows) * cols;
    std::vector<double> response(size_t(n), 100.0);
    std::vector<double> noise(size_t(n), 100.0);
    std::vector<double> netd(size_t(n), 1.0);
    for (int col = 0; col < 130; ++col)
        response[size_t(col)] = 10.0;
    for (int col = 130; col < 130 + 787; ++col)
        noise[size_t(col)] = 1000.0;

    const IRGbCriteria criteria{1.5, 0.5, 1.5, 2.0, 0.0, 0.0, 3, 0};
    IRGbBadPixelSummary summary{};
    char err[256]{};
    const int32_t status = IR_GbProcessBadPixels(
        response.data(), noise.data(), netd.data(),
        rows, cols, n, &criteria, &summary, nullptr, err, sizeof err);
    CHECK(status == IR_OK);
    CHECK(summary.response_bad_count == 130);
    CHECK(summary.noise_bad_count == 787);
    CHECK(summary.netd_bad_count == 0);
    CHECK(summary.total_bad_count == 917);
    CHECK(summary.response_bad_count + summary.noise_bad_count + summary.netd_bad_count ==
          summary.total_bad_count);
}

void testOverlappingCategoriesCountIndependently()
{
    const int32_t rows = 64, cols = 64;
    const int64_t n = int64_t(rows) * cols;
    std::vector<double> response(size_t(n), 100.0);
    std::vector<double> noise(size_t(n), 100.0);
    std::vector<double> netd(size_t(n), 1.0);

    response[0] = 10.0;
    response[1] = 10.0;
    noise[1] = 1000.0;
    response[2] = 10000.0;

    std::vector<uint8_t> validMask(size_t(n), 9);
    std::vector<uint8_t> totalBadMask(size_t(n), 9);
    std::vector<uint8_t> respBadMask(size_t(n), 9);
    std::vector<uint8_t> noiseBadMask(size_t(n), 9);
    std::vector<uint8_t> netdBadMask(size_t(n), 9);
    IRGbBadPixelMasks masks{
        validMask.data(), totalBadMask.data(), respBadMask.data(),
        noiseBadMask.data(), netdBadMask.data(), nullptr, nullptr, n};

    const IRGbCriteria criteria{1.5, 0.5, 1.5, 2.0, 0.0, 0.0, 3, 0};
    IRGbBadPixelSummary summary{};
    char err[256]{};
    const int32_t status = IR_GbProcessBadPixels(
        response.data(), noise.data(), netd.data(),
        rows, cols, n, &criteria, &summary, &masks, err, sizeof err);
    CHECK(status == IR_OK);
    CHECK(summary.response_bad_count == 3);
    CHECK(summary.noise_bad_count == 1);
    CHECK(summary.netd_bad_count == 0);
    CHECK(summary.total_bad_count == 3);
    CHECK(summary.response_bad_count + summary.noise_bad_count + summary.netd_bad_count == 4);
    CHECK(summary.response_upper_bad_count == 1);
    CHECK(summary.response_lower_bad_count == 2);

    CHECK(validMask[0] == 0 && validMask[1] == 0 && validMask[2] == 0 && validMask[3] == 1);
    CHECK(totalBadMask[0] == 1 && totalBadMask[1] == 1 && totalBadMask[2] == 1 && totalBadMask[3] == 0);
    CHECK(respBadMask[0] == 1 && respBadMask[1] == 1 && respBadMask[2] == 1);
    CHECK(noiseBadMask[0] == 0 && noiseBadMask[1] == 1 && noiseBadMask[2] == 0);
    for (int64_t i = 0; i < n; ++i)
    {
        CHECK((validMask[size_t(i)] == 0) || (validMask[size_t(i)] == 1));
        CHECK((totalBadMask[size_t(i)] == 0) || (totalBadMask[size_t(i)] == 1));
    }
}

void testNonFiniteResponseIsBadButNotSplit()
{
    const int32_t rows = 64, cols = 64;
    const int64_t n = int64_t(rows) * cols;
    std::vector<double> response(size_t(n), 100.0);
    std::vector<double> noise(size_t(n), 100.0);
    std::vector<double> netd(size_t(n), 1.0);

    response[0] = std::numeric_limits<double>::quiet_NaN();
    response[1] = 10.0;
    response[2] = 10000.0;

    const IRGbCriteria criteria{1.5, 0.5, 1.5, 2.0, 0.0, 0.0, 3, 0};
    IRGbBadPixelSummary summary{};
    char err[256]{};
    const int32_t status = IR_GbProcessBadPixels(
        response.data(), noise.data(), netd.data(),
        rows, cols, n, &criteria, &summary, nullptr, err, sizeof err);
    CHECK(status == IR_OK);
    CHECK(summary.response_bad_count == 3);
    CHECK(summary.total_bad_count == 3);
    CHECK(summary.response_upper_bad_count == 1);
    CHECK(summary.response_lower_bad_count == 1);
    CHECK(summary.response_upper_bad_count + summary.response_lower_bad_count <
          summary.response_bad_count);
}

void testNetdCountIsNotLost()
{
    const int32_t rows = 512, cols = 640;
    const int64_t n = int64_t(rows) * cols;
    std::vector<double> response(size_t(n), 100.0);
    std::vector<double> noise(size_t(n), 100.0);
    std::vector<double> netd(size_t(n), 1.0);
    for (int col = 0; col < 11; ++col)
        netd[size_t(col)] = 10.0;

    const IRGbCriteria criteria{1.5, 0.5, 1.5, 2.0, 0.0, 0.0, 3, 0};
    IRGbBadPixelSummary summary{};
    char err[256]{};
    const int32_t status = IR_GbProcessBadPixels(
        response.data(), noise.data(), netd.data(),
        rows, cols, n, &criteria, &summary, nullptr, err, sizeof err);
    CHECK(status == IR_OK);
    CHECK(summary.response_bad_count == 0);
    CHECK(summary.noise_bad_count == 0);
    CHECK(summary.netd_bad_count == 11);
    CHECK(summary.total_bad_count == 11);
}

void testLargeArrayGbAndGjbProcessing()
{
    const int32_t rows = 1024, cols = 1280;
    const int64_t n = int64_t(rows) * cols;
    {
        std::vector<double> response(size_t(n), 100.0);
        std::vector<double> noise(size_t(n), 5.0);
        std::vector<double> netd(size_t(n), 1.0);
        response[size_t(512) * cols + 640] = 1000.0;
        const IRGbCriteria gbCriteria{1.5, 0.5, 1.5, 2.0, 0.0, 0.0, 1, 0};
        IRGbBadPixelSummary gb{};
        std::vector<uint8_t> validMask(size_t(n), 0);
        IRGbBadPixelMasks masks{};
        masks.valid_mask = validMask.data();
        masks.pixel_capacity = n;
        char err[256]{};
        CHECK(IR_GbProcessBadPixels(response.data(), noise.data(), netd.data(),
                                    rows, cols, n, &gbCriteria, &gb, &masks, err, sizeof err) == IR_OK);
        CHECK(gb.response_bad_count == 1);
        CHECK(gb.cluster_counts[IR_GB_REGION_A][IR_GB_CLUSTER_IV] == 1);
        CHECK(gb.chip_grade == IR_CHIP_GRADE_A);
        CHECK(validMask[size_t(512) * cols + 640] == 0);
    }

    {
        std::vector<double> v1(size_t(n), 200.0);
        std::vector<double> v2(size_t(n), 100.0);
        std::vector<double> vn(size_t(n), 5.0);
        std::vector<double> vm(size_t(n), 150.0);
        v1[size_t(512) * cols + 640] = 1100.0;
        const IRCalcParameters params = defaultCalcParams();
        const IRGjbCriteria gjbCriteria{1.0, 1.0, 0.5, 0.5, 1, 0};
        IRGjbSummary gjb{};
        std::vector<uint8_t> validMask(size_t(n), 0);
        IRGjbBadPixelMasks masks{};
        masks.valid_mask = validMask.data();
        masks.pixel_capacity = n;
        char err[256]{};
        CHECK(IR_GjbEvaluateMaps(v1.data(), v2.data(), vn.data(), vm.data(),
                                 rows, cols, n, &params, &gjbCriteria, &gjb, &masks, err, sizeof err) == IR_OK);
        CHECK(gjb.bad_pixels.total_bad_count == 1);
        CHECK(gjb.bad_pixels.total_bad_rate_pct > 0.0);
        CHECK(std::isfinite(gjb.after_avg_responsivity));
        CHECK(gjb.bad_pixels.center_clusters.count_1_to_6 == 1);
        CHECK(validMask[size_t(512) * cols + 640] == 0);
    }
}

void testGbEvaluateMapsClosedFormAndZeroNoiseParity()
{
    IRCalcParameters params{};
    params.t0_c = 20.0;
    params.t1_c = 30.0;
    params.integration_time_us = 1000;
    params.invalid_integration_us = 0;
    params.pixel_pitch_um = 10.0;
    params.emissivity = 1.0;
    params.system_gain = 1.0;
    params.adc_swing_v = 2.0;
    params.digital_full = 10000;
    params.f_number = 1.0;
    params.vmax_per_count = 2.0;

    const IRGbCriteria criteria{1.5, 0.5, 1.5, 2.0, 0.0, 0.0, 3, 0};

    // Clearer single set with one short bad pixel (>45000), one open bad pixel (<20000), and response=200
    const double v1Case[4] = {46000.0, 19200.0, 30200.0, 30200.0};
    const double v2Case[4] = {45800.0, 19000.0, 30000.0, 30000.0};
    const double vnCase[4] = {4.0, 4.0, 4.0, 4.0};

    uint8_t shortMask[4]{};
    uint8_t openMask[4]{};
    IRGbBadPixelMasks masks{};
    masks.short_bad_mask = shortMask;
    masks.open_bad_mask = openMask;
    masks.pixel_capacity = 4;

    IRGbSummary summary{};
    char err[256]{};
    CHECK(IR_GbEvaluateMaps(v1Case, v2Case, vnCase, 2, 2, 4,
                            &params, &criteria, &summary, &masks, err, sizeof err) == IR_OK);

    const double expectedVoltsPerCount = 2.0 / 10000.0;
    const double expectedIrrad =
        (1e-3 * 1e-3) * 1.0 * 5.673e-12 * (std::pow(303.0, 4.0) - std::pow(293.0, 4.0)) / 5.0;
    const double expectedRespVoltage = 200.0 * expectedVoltsPerCount;
    const double expectedResponsivity = expectedRespVoltage / expectedIrrad;
    const double expectedNoiseV = 4.0 * expectedVoltsPerCount;
    const double expectedDet =
        (expectedResponsivity / expectedNoiseV) * std::sqrt(1e-6 / (2.0 * 1e-3));

    CHECK_NEAR(summary.volts_per_count, expectedVoltsPerCount, 1e-18);
    CHECK_NEAR(summary.before_avg_pixel_response_count, 200.0, 1e-12);
    CHECK_NEAR(summary.before_avg_pixel_response_voltage, expectedRespVoltage, 1e-12);
    CHECK_NEAR(summary.before_avg_snr, 50.0, 1e-12);
    CHECK_NEAR(summary.irradiance_power_diff, expectedIrrad, 1e-22);
    CHECK_NEAR(summary.before_avg_blackbody_response, 200.0 / (1000.0 * expectedIrrad), 1e-4);
    CHECK_NEAR(summary.before_avg_noise_voltage_v, expectedNoiseV, 1e-15);
    CHECK_NEAR(summary.before_avg_netd_mk, 200.0, 1e-12);
    CHECK_NEAR(summary.before_avg_responsivity, expectedResponsivity, 1e-3);
    CHECK_NEAR(summary.before_avg_detectivity, expectedDet, 1e-3);
    CHECK_NEAR(summary.before_sitf, 20.0, 1e-12);
    CHECK(summary.short_bad_pixel_count == 1);
    CHECK(summary.open_bad_pixel_count == 1);
    CHECK(shortMask[0] == 1 && shortMask[1] == 0 && shortMask[2] == 0 && shortMask[3] == 0);
    CHECK(openMask[0] == 0 && openMask[1] == 1 && openMask[2] == 0 && openMask[3] == 0);
    CHECK(summary.bad_pixels.total_bad_count == 0);
    CHECK(summary.bad_pixels.chip_grade == IR_CHIP_GRADE_A);
    CHECK_NEAR(summary.after_response_mv_per_ms_k, 4.0, 1e-12);
    CHECK_NEAR(summary.after_avg_noise_voltage_mv, 0.8, 1e-12);
    CHECK_NEAR(summary.after_avg_nep_w, expectedNoiseV / expectedResponsivity, 1e-22);
    CHECK_NEAR(summary.after_response_nonuniformity_pct, 0.0, 1e-12);

    // 2) Zero-noise OpenCV safeDivide parity check (vn=0 -> snr=0, detectivity=0, netd=0, not Inf/NaN)
    const double vnZero[4] = {0.0, 4.0, 4.0, 4.0};
    IRGbSummary zeroSummary{};
    CHECK(IR_GbEvaluateMaps(v1Case, v2Case, vnZero, 2, 2, 4,
                            &params, &criteria, &zeroSummary, nullptr, err, sizeof err) == IR_OK);
    CHECK(std::isfinite(zeroSummary.before_avg_snr));
    CHECK(std::isfinite(zeroSummary.before_avg_detectivity));
    CHECK(std::isfinite(zeroSummary.before_avg_netd_mk));
    CHECK_NEAR(zeroSummary.before_avg_snr, 150.0 / 4.0, 1e-12);
    CHECK_NEAR(zeroSummary.before_avg_netd_mk, (0.0 + 200.0 + 200.0 + 200.0) / 4.0, 1e-12);
}

void testGbClusterGrades()
{
    const int32_t rows = 20, cols = 20;
    const int64_t n = int64_t(rows) * cols;
    const IRGbCriteria criteria{1.5, 0.5, 1.5, 2.0, 0.0, 0.0, 1, 0};
    char err[256]{};

    // Grade D triggered by O-class cluster (5 pixels in a row inside region A)
    {
        std::vector<double> resp(size_t(n), 100.0);
        std::vector<double> vn(size_t(n), 5.0);
        std::vector<double> netd(size_t(n), 1.0);
        for (int c = 8; c < 13; ++c)
            resp[size_t(10) * cols + c] = 500.0;
        IRGbBadPixelSummary summary{};
        CHECK(IR_GbProcessBadPixels(resp.data(), vn.data(), netd.data(),
                                    rows, cols, n, &criteria, &summary, nullptr, err, sizeof err) == IR_OK);
        CHECK(summary.cluster_counts[IR_GB_REGION_A][IR_GB_CLUSTER_O] == 1);
        CHECK(summary.chip_grade == IR_CHIP_GRADE_D);
    }

    // Grade B triggered by II cluster in region A (size 3 in center 4x5 box)
    {
        std::vector<double> resp(size_t(n), 100.0);
        std::vector<double> vn(size_t(n), 5.0);
        std::vector<double> netd(size_t(n), 1.0);
        for (int c = 8; c < 11; ++c)
            resp[size_t(10) * cols + c] = 500.0;
        IRGbBadPixelSummary summary{};
        CHECK(IR_GbProcessBadPixels(resp.data(), vn.data(), netd.data(),
                                    rows, cols, n, &criteria, &summary, nullptr, err, sizeof err) == IR_OK);
        CHECK(summary.cluster_counts[IR_GB_REGION_A][IR_GB_CLUSTER_II] == 1);
        CHECK(summary.chip_grade == IR_CHIP_GRADE_B);
    }

    // Grade C triggered by I cluster (size 4 in region C corner)
    {
        std::vector<double> resp(size_t(n), 100.0);
        std::vector<double> vn(size_t(n), 5.0);
        std::vector<double> netd(size_t(n), 1.0);
        for (int c = 0; c < 4; ++c)
            resp[size_t(c)] = 500.0;
        IRGbBadPixelSummary summary{};
        CHECK(IR_GbProcessBadPixels(resp.data(), vn.data(), netd.data(),
                                    rows, cols, n, &criteria, &summary, nullptr, err, sizeof err) == IR_OK);
        CHECK(summary.cluster_counts[IR_GB_REGION_C][IR_GB_CLUSTER_I] == 1);
        CHECK(summary.chip_grade == IR_CHIP_GRADE_C);
    }
}

void testGjbClustersBlocksAndRfpn()
{
    // 8x8 array: centerRect is x=2,y=2,w=4,h=4 (rows 2..5, cols 2..5).
    // Place a 4x4 block of high response pixels covering rows 2..5, cols 2..5 ->
    // 1) In centerBadMask (4x4): greedy 3x3 removal clears rows 0..2, cols 0..2 (center_3x3_block_count == 1),
    //    leaving 7 bad pixels on bottom/right of center.
    // 2) Full 4x4 block in blockFilteredBadMask no longer has all 16 bits set (filtered_4x4_block_count == 0).
    // Now also test an edge 4x4 block outside center, or use two separate tests!
    const int32_t rows = 8, cols = 8;
    const int64_t n = int64_t(rows) * cols;
    const IRCalcParameters params = defaultCalcParams();
    const IRGjbCriteria criteria{0.2, 2.0, 0.5, 0.5, 1, 0};

    // Case 1: Center 3x3 removal inside 8x8 (center rows 2..5, cols 2..5)
    {
        std::vector<double> v1(size_t(n), 200.0);
        std::vector<double> v2(size_t(n), 100.0);
        std::vector<double> vn(size_t(n), 5.0);
        std::vector<double> vm(size_t(n), 150.0);
        // Make one valid pixel vm slightly off to verify positive RFPN
        vm[0] = 152.0;

        for (int r = 2; r <= 4; ++r)
            for (int c = 2; c <= 4; ++c)
                v1[size_t(r) * cols + c] = 1200.0; // triggers k1 bad pixel

        // Also add one 7-pixel cluster in edge (row 0, cols 0..6)
        // Wait: keep [0] valid for vm[0]=152 test, so put 7 pixels at row 7, cols 0..6!
        for (int c = 0; c < 7; ++c)
            v1[size_t(7) * cols + c] = 1200.0;

        std::vector<uint8_t> k1Mask(size_t(n), 0);
        std::vector<uint8_t> blockFilteredMask(size_t(n), 0);
        IRGjbBadPixelMasks masks{};
        masks.k1_bad_mask = k1Mask.data();
        masks.block_filtered_bad_mask = blockFilteredMask.data();
        masks.pixel_capacity = n;

        IRGjbSummary gjb{};
        char err[256]{};
        CHECK(IR_GjbEvaluateMaps(v1.data(), v2.data(), vn.data(), vm.data(),
                                 rows, cols, n, &params, &criteria, &gjb, &masks, err, sizeof err) == IR_OK);
        CHECK(gjb.bad_pixels.total_bad_count == 16);
        CHECK(gjb.bad_pixels.center_3x3_block_count == 1);
        CHECK(gjb.bad_pixels.filtered_4x4_block_count == 0);
        CHECK(gjb.bad_pixels.center_clusters.count_7_to_12 == 1);
        CHECK(gjb.bad_pixels.edge_clusters.count_7_to_12 == 1);
        CHECK(gjb.bad_pixels.total_clusters.count_7_to_12 == 2);
        CHECK(k1Mask[size_t(7) * cols] == 1);
        CHECK(blockFilteredMask[size_t(2) * cols + 2] == 0);
        CHECK(blockFilteredMask[size_t(7) * cols] == 1);

        // Compute exact expected RFPN on remaining 48 valid pixels:
        // All 48 valid pixels have v1=200, v2=100 -> same t1, t2 -> k = (avgT1 - avgT2)/(t1 - t2), b = avgT1 - k*t1.
        // Only valid pixel [0] has vm=152 (vs 150 on the other 47 valid pixels).
        // Let's verify exact RFPN analytically!
        const double vpc = params.adc_swing_v / params.digital_full / params.system_gain;
        const double meanV1 = (48.0 * 200.0 + 16.0 * 1200.0) / 64.0;
        const double avgSigT1 = meanV1 * vpc;
        const double avgSigT2 = 100.0 * vpc;
        const double kValid = (avgSigT1 - avgSigT2) / ((200.0 - 100.0) * vpc);
        // vnuc difference between vm=152 and vm=150 is kValid * 2.0 * vpc; std across 48 pixels with 1 outlier is sqrt(47)/48 * diff
        const double delta = kValid * 2.0 * vpc;
        const double expectedRfpn = std::sqrt(47.0) / 48.0 * delta;
        CHECK_NEAR(gjb.after_rfpn_v, expectedRfpn, 1e-12);
        const double expectedMaxDr =
            20.0 * std::log10((params.vmax_per_count * vpc) / (gjb.after_avg_noise_voltage_mv / 1000.0));
        CHECK_NEAR(gjb.after_max_dynamic_range_db, expectedMaxDr, 1e-12);
        CHECK_NEAR(gjb.after_spatial_noise,
                   gjb.after_avg_noise_voltage_mv * gjb.after_response_nonuniformity_pct, 1e-12);
    }

    // Case 2: Non-center 4x4 block stays intact after center 3x3 pass and is counted in filtered_4x4_block_count
    {
        const int32_t bigRows = 12, bigCols = 12;
        const int64_t bigN = int64_t(bigRows) * bigCols;
        std::vector<double> v1(size_t(bigN), 200.0);
        std::vector<double> v2(size_t(bigN), 100.0);
        std::vector<double> vn(size_t(bigN), 5.0);
        std::vector<double> vm(size_t(bigN), 150.0);
        // Put 4x4 bad block at top-left (rows 0..3, cols 0..3), partially outside center (rows 3..8, cols 3..8)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                v1[size_t(r) * bigCols + c] = 1200.0;
        IRGjbSummary gjb{};
        char err[256]{};
        CHECK(IR_GjbEvaluateMaps(v1.data(), v2.data(), vn.data(), vm.data(),
                                 bigRows, bigCols, bigN, &params, &criteria, &gjb, nullptr, err, sizeof err) == IR_OK);
        CHECK(gjb.bad_pixels.total_bad_count == 16);
        CHECK(gjb.bad_pixels.center_3x3_block_count == 0);
        CHECK(gjb.bad_pixels.filtered_4x4_block_count == 1);
    }
}

void testRaw16PopulationStdIntegrationAndValidation()
{
    // Verify end-to-end: RAW16 frames -> IR_FrameStatistics(sample_std=0) -> IR_GbEvaluateMaps / IR_GjbEvaluateMaps
    // Frame 0: all 200; Frame 1: all 208 -> mean=204, population std (N) = 4, sample std (N-1) = 4*sqrt(2)
    const int32_t rows = 4, cols = 4;
    const int64_t n = int64_t(rows) * cols;
    std::vector<uint16_t> t1Frames(static_cast<size_t>(n * 2), uint16_t{0});
    std::fill_n(t1Frames.begin(), static_cast<size_t>(n), uint16_t{20000});
    std::fill_n(t1Frames.begin() + n, static_cast<size_t>(n), uint16_t{20008});
    std::vector<double> v1Mean(static_cast<size_t>(n), 0.0);
    std::vector<double> vnPop(static_cast<size_t>(n), 0.0);
    std::vector<double> vnSample(static_cast<size_t>(n), 0.0);
    std::vector<double> tmpMean(static_cast<size_t>(n), 0.0);
    double summaryPop[2]{}, summarySample[2]{};
    char err[256]{};
    const IRArrayView input{t1Frames.data(),
        static_cast<uint64_t>(t1Frames.size()) * sizeof(uint16_t), IR_TYPE_U16,
        cols, rows, 2};

    CHECK(IR_FrameStatistics(&input, 0, v1Mean.data(), vnPop.data(), n,
                             summaryPop, 2, err, sizeof err) == IR_OK);
    CHECK(IR_FrameStatistics(&input, 1, tmpMean.data(), vnSample.data(), n,
                             summarySample, 2, err, sizeof err) == IR_OK);
    CHECK_NEAR(summaryPop[1], 4.0, 1e-12);
    CHECK_NEAR(summarySample[1], 4.0 * std::sqrt(2.0), 1e-12);

    std::vector<double> v2Mean(size_t(n), 20004.0 - 200.0); // resp = 200 DN
    std::vector<double> vmMean(size_t(n), 20004.0 - 100.0);
    const IRCalcParameters params = defaultCalcParams();
    const IRGbCriteria gbCrit{1.5, 0.5, 1.5, 2.0, 0.0, 0.0, 2, 0};
    const IRGjbCriteria gjbCrit{1.0, 1.0, 0.5, 0.5, 2, 0};

    IRGbSummary gb{};
    IRGjbSummary gjb{};
    CHECK(IR_GbEvaluateMaps(v1Mean.data(), v2Mean.data(), vnPop.data(),
                            rows, cols, n, &params, &gbCrit, &gb, nullptr, err, sizeof err) == IR_OK);
    CHECK(IR_GjbEvaluateMaps(v1Mean.data(), v2Mean.data(), vnPop.data(), vmMean.data(),
                             rows, cols, n, &params, &gjbCrit, &gjb, nullptr, err, sizeof err) == IR_OK);
    CHECK_NEAR(gb.before_avg_snr, 200.0 / 4.0, 1e-12);
    CHECK_NEAR(gjb.before_avg_snr, 200.0 / 4.0, 1e-12);

    // Negative / error handling tests
    const double one[1] = {1.0};
    CHECK(IR_GbProcessBadPixels(nullptr, one, one, 1, 1, 1, &gbCrit,
                                &gb.bad_pixels, nullptr, err, sizeof err) == IR_NULL_POINTER);
    CHECK(IR_GbProcessBadPixels(one, one, one, 2, 2, 3, &gbCrit,
                                &gb.bad_pixels, nullptr, err, sizeof err) == IR_BUFFER_TOO_SMALL);
    IRGbBadPixelMasks smallMasks{};
    uint8_t smallBuf[2]{};
    smallMasks.valid_mask = smallBuf;
    smallMasks.pixel_capacity = 2;
    CHECK(IR_GbProcessBadPixels(v1Mean.data(), vnPop.data(), v2Mean.data(), rows, cols, n,
                                &gbCrit, &gb.bad_pixels, &smallMasks, err, sizeof err) == IR_BUFFER_TOO_SMALL);

    IRGjbCriteria badGjbCrit = gjbCrit;
    badGjbCrit.k3 = 0.0;
    CHECK(IR_GjbEvaluateMaps(v1Mean.data(), v2Mean.data(), vnPop.data(), vmMean.data(),
                             rows, cols, n, &params, &badGjbCrit, &gjb, nullptr, err, sizeof err) == IR_BAD_ARGUMENT);

    IRCalcParameters badParams = params;
    badParams.t1_c = badParams.t0_c;
    CHECK(IR_GbEvaluateMaps(v1Mean.data(), v2Mean.data(), vnPop.data(),
                            rows, cols, n, &badParams, &gbCrit, &gb, nullptr, err, sizeof err) == IR_BAD_ARGUMENT);

    // err_bytes < 0 check
    CHECK(IR_GbEvaluateMaps(v1Mean.data(), v2Mean.data(), vnPop.data(),
                            rows, cols, n, &params, &gbCrit, &gb, nullptr, nullptr, -1) == IR_BAD_ARGUMENT);

    // Custom 14-bit short/open bad pixel thresholds (v1 > 14000, v2 < 2000)
    const IRGbCriteria custom14BitCrit{1.5, 0.5, 1.5, 2.0, 14000.0, 2000.0, 2, 0};
    const double v1_14_adj[4] = {15000.0, 1700.0, 8200.0, 8200.0};
    const double v2_14_adj[4] = {14800.0, 1500.0, 8000.0, 8000.0};
    const double vn_14[4] = {4.0, 4.0, 4.0, 4.0};
    IRGbSummary customGb{};
    CHECK(IR_GbEvaluateMaps(v1_14_adj, v2_14_adj, vn_14, 2, 2, 4,
                            &params, &custom14BitCrit, &customGb, nullptr, err, sizeof err) == IR_OK);
    CHECK(customGb.short_bad_pixel_count == 1);
    CHECK(customGb.open_bad_pixel_count == 1);
}

} // namespace

int main()
{
    testNonOverlappingCategoriesMatchTotal();
    testOverlappingCategoriesCountIndependently();
    testNonFiniteResponseIsBadButNotSplit();
    testNetdCountIsNotLost();
    testLargeArrayGbAndGjbProcessing();
    testGbEvaluateMapsClosedFormAndZeroNoiseParity();
    testGbClusterGrades();
    testGjbClustersBlocksAndRfpn();
    testRaw16PopulationStdIntegrationAndValidation();

    if (gFailures == 0)
        std::printf("gb_gjb_test: ALL PASSED\n");
    return gFailures == 0 ? 0 : 1;
}
