#include "IR_analysis.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int gFailures = 0;

void checkTrue(bool cond, const char *expr, int line)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
        ++gFailures;
    }
}

void checkNear(double actual, double expected, double tol, const char *label, int line)
{
    const double diff = std::abs(actual - expected);
    if (!std::isfinite(actual) || diff > tol) {
        std::fprintf(stderr, "FAIL line %d (%s): actual=%.17g expected=%.17g diff=%.17g\n",
                     line, label, actual, expected, diff);
        ++gFailures;
    }
}

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)
#define CHECK_NEAR(a, b, tol) checkNear((a), (b), (tol), #a, __LINE__)

void testOrgFlickerHandComputed()
{
    // 4 frames, 1x4: pixel 0..2 constant around 1000, pixel 3 flickers sharply in frames 0,1,2
    // Wait: with 4 pixels {1000, 1000, 1000, 1020}: mean=1005, deviations={-5,-5,-5,+15},
    // var = (25+25+25+225)/4 = 75, sigma = sqrt(75) ~ 8.66025.
    // 1.5 * sigma ~ 12.99 -> |1020 - 1005| = 15 > 12.99 (exceeds!), while |-5| < 12.99.
    const uint16_t frames[16] = {
        1000, 1000, 1000, 1020,
        1000, 1000, 1000, 1020,
        1000, 1000, 1000, 1020,
        1000, 1000, 1000, 1000
    };
    const IRArrayView view{frames, sizeof frames, IR_TYPE_U16, 4, 1, 4};
    IRFlickerSummary summary{};
    int32_t exceedMap[4]{};
    uint8_t flickerMask[4]{};
    double frameMeans[4]{};
    double frameSigmas[4]{};
    char err[256]{};

    CHECK(IR_FlickerDetect(&view, nullptr, nullptr, nullptr,
                           1.5, 2, &summary, exceedMap, flickerMask, 4,
                           frameMeans, frameSigmas, 4, err, sizeof err) == IR_OK);
    CHECK(summary.frame_count == 4);
    CHECK(summary.valid_pixel_count == 4);
    CHECK(summary.flicker_count == 1);
    CHECK_NEAR(summary.flicker_ratio_pct, 25.0, 1e-12);
    CHECK_NEAR(frameMeans[0], 1005.0, 1e-12);
    CHECK_NEAR(frameSigmas[0], std::sqrt(75.0), 1e-12);
    CHECK_NEAR(frameMeans[3], 1000.0, 1e-12);
    CHECK_NEAR(frameSigmas[3], 0.0, 1e-12);
    CHECK(exceedMap[0] == 0 && exceedMap[1] == 0 && exceedMap[2] == 0 && exceedMap[3] == 3);
    CHECK(flickerMask[0] == 0 && flickerMask[3] == 1);

    // Threshold 3 means strictly > 3 -> 0 flicker pixels
    CHECK(IR_FlickerDetect(&view, nullptr, nullptr, nullptr,
                           1.5, 3, &summary, nullptr, flickerMask, 4,
                           nullptr, nullptr, 0, err, sizeof err) == IR_OK);
    CHECK(summary.flicker_count == 0);
    CHECK_NEAR(summary.flicker_ratio_pct, 0.0, 1e-12);
    CHECK(flickerMask[3] == 0);

    uint32_t frames32[16];
    for (int i = 0; i < 16; ++i)
        frames32[i] = 70000u + frames[i] - 1000u;
    const IRArrayView view32{frames32, sizeof frames32, IR_TYPE_U32, 4, 1, 4};
    CHECK(IR_FlickerDetect(&view32, nullptr, nullptr, nullptr,
                           1.5, 2, &summary, exceedMap, flickerMask, 4,
                           frameMeans, frameSigmas, 4, err, sizeof err) == IR_OK);
    CHECK(summary.flicker_count == 1 && flickerMask[3] == 1);
    CHECK_NEAR(frameMeans[0], 70005.0, 1e-12);
    CHECK_NEAR(frameSigmas[0], std::sqrt(75.0), 1e-12);
}

void testNucAndMaskAndZeroRawSkipping()
{
    // 2x2 grid (4 pixels):
    // pixel 0: valid=0 (masked out), huge value should be ignored
    // pixel 1: raw=0 in all frames (zero skipped) -> never observed!
    // pixels 2,3: valid=1; in frame 0 k*raw+b gives {1000, 1000}, wait need >2 valid for 1 outlier:
    // Use 1x5:
    // idx 0: blind (valid_mask=0)
    // idx 1: valid_mask=1, k=2,b=0, raw=500 -> 1000
    // idx 2: valid_mask=1, k=1,b=100, raw=900 -> 1000
    // idx 3: valid_mask=1, k=1,b=0, raw=1000 -> 1000
    // idx 4: valid_mask=1, k=1,b=0, raw=1040 -> 1040 (frames 0,1), raw=0 (frame 2: observed only in f0,f1!)
    const uint16_t frames[15] = {
        9999, 500, 900, 1000, 1040,
        9999, 500, 900, 1000, 1040,
        9999, 500, 900, 1000,    0
    };
    const IRArrayView view{frames, sizeof frames, IR_TYPE_U16, 5, 1, 3};
    const double kMap[5] = {1.0, 2.0, 1.0, 1.0, 1.0};
    const double bMap[5] = {0.0, 0.0, 100.0, 0.0, 0.0};
    const uint8_t validMask[5] = {0, 1, 1, 1, 1};
    IRFlickerSummary summary{};
    int32_t exceedMap[5]{};
    uint8_t flickerMask[5]{};
    char err[256]{};

    CHECK(IR_FlickerDetect(&view, kMap, bMap, validMask,
                           1.5, 1, &summary, exceedMap, flickerMask, 5,
                           nullptr, nullptr, 0, err, sizeof err) == IR_OK);
    CHECK(summary.valid_pixel_count == 4);
    CHECK(summary.flicker_count == 1);
    CHECK_NEAR(summary.flicker_ratio_pct, 25.0, 1e-12);
    CHECK(exceedMap[0] == 0 && flickerMask[0] == 0);
    CHECK(exceedMap[4] == 2 && flickerMask[4] == 1);
}

void testErrorPaths()
{
    const uint16_t frames[4] = {10, 20, 30, 40};
    const IRArrayView view{frames, sizeof frames, IR_TYPE_U16, 2, 1, 2};
    const double kMap[2] = {1.0, 1.0};
    const double bMap[2] = {0.0, 0.0};
    const uint8_t zeroMask[2] = {0, 0};
    IRFlickerSummary summary{};
    char err[256]{};

    CHECK(IR_FlickerDetect(&view, nullptr, nullptr, nullptr,
                           1.5, 0, nullptr, nullptr, nullptr, 0,
                           nullptr, nullptr, 0, err, sizeof err) == IR_NULL_POINTER);
    CHECK(IR_FlickerDetect(&view, kMap, nullptr, nullptr,
                           1.5, 0, &summary, nullptr, nullptr, 2,
                           nullptr, nullptr, 0, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_FlickerDetect(&view, nullptr, nullptr, nullptr,
                           0.0, 0, &summary, nullptr, nullptr, 2,
                           nullptr, nullptr, 0, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_FlickerDetect(&view, nullptr, nullptr, nullptr,
                           1.5, 2, &summary, nullptr, nullptr, 2,
                           nullptr, nullptr, 0, err, sizeof err) == IR_BAD_ARGUMENT);
    uint8_t smallMask[1]{};
    CHECK(IR_FlickerDetect(&view, nullptr, nullptr, nullptr,
                           1.5, 0, &summary, nullptr, smallMask, 1,
                           nullptr, nullptr, 0, err, sizeof err) == IR_BUFFER_TOO_SMALL);
    double smallMeans[1]{};
    CHECK(IR_FlickerDetect(&view, nullptr, nullptr, nullptr,
                           1.5, 0, &summary, nullptr, nullptr, 2,
                           smallMeans, nullptr, 1, err, sizeof err) == IR_BUFFER_TOO_SMALL);
    CHECK(IR_FlickerDetect(&view, kMap, bMap, zeroMask,
                           1.5, 0, &summary, nullptr, nullptr, 2,
                           nullptr, nullptr, 0, err, sizeof err) == IR_COMPUTATION_FAILED);
}

} // namespace

int main()
{
    CHECK(sizeof(IRFlickerSummary) == 48);
    testOrgFlickerHandComputed();
    testNucAndMaskAndZeroRawSkipping();
    testErrorPaths();
    if (gFailures == 0)
        std::printf("flicker_test: ALL PASSED\n");
    return gFailures == 0 ? 0 : 1;
}
