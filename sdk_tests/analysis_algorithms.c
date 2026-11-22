#include "IR_analysis.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int check_abi_sizes(void)
{
    if (sizeof(IRCalcParameters) != 80 ||
        sizeof(IRGbCriteria) != 56 ||
        sizeof(IRGbBadPixelSummary) != 104 ||
        sizeof(IRGbSummary) != 264 ||
        sizeof(IRGjbCriteria) != 40 ||
        sizeof(IRGjbClusterStats) != 20 ||
        sizeof(IRGjbBadPixelSummary) != 88 ||
        sizeof(IRGjbSummary) != 248 ||
        sizeof(IRNoise3DCalibParams) != 40 ||
        sizeof(IRNoise3DComputeParams) != 32 ||
        sizeof(IRMapSummary) != 80 ||
        sizeof(IRGjbDriftContext) != 64 ||
        sizeof(IRGjbBurstMetrics) != 168 ||
        sizeof(IRFlickerSummary) != 48) {
        fprintf(stderr, "Unexpected fixed-width struct size in IR_analysis.h\n");
        return 1;
    }
    if (sizeof(void *) == 8) {
        if (sizeof(IRNoise3DCalibOutputs) != 64 ||
            sizeof(IRNoise3DComputeOutputs) != 120 ||
            sizeof(IRGbBadPixelMasks) != 64 ||
            sizeof(IRGjbBadPixelMasks) != 64) {
            fprintf(stderr, "Unexpected x64 pointer struct size in IR_analysis.h\n");
            return 1;
        }
    }
    return 0;
}

static char grade_char(int32_t grade)
{
    switch (grade) {
    case IR_CHIP_GRADE_A: return 'A';
    case IR_CHIP_GRADE_B: return 'B';
    case IR_CHIP_GRADE_C: return 'C';
    case IR_CHIP_GRADE_D: return 'D';
    default: return '?';
    }
}

int main(void)
{
    if (check_abi_sizes() != 0)
        return 1;

    const char *version = IR_AnalysisVersion();
    if (!version || version[0] == '\0') {
        fprintf(stderr, "IR_AnalysisVersion returned empty string\n");
        return 1;
    }

    char err[512] = {0};

    /* 1. Compute T1 mean and population noise map vn (sample_std = 0, denominator N) from 2x2x2 RAW16 */
    const uint16_t t1_frames[8] = {
        30196, 30196, 30196, 30196,
        30204, 30204, 30204, 30204
    };
    double v1_map[4] = {0};
    double vn_map[4] = {0};
    double stats_summary[2] = {0};
    int32_t status = IR_FrameStatisticsU16(
        t1_frames, 8, 2, 2, 2, 0,
        v1_map, vn_map, 4, stats_summary, 2, err, (int32_t)sizeof err);
    if (status != IR_OK || fabs(vn_map[0] - 4.0) > 1e-12) {
        fprintf(stderr, "IR_FrameStatisticsU16 failed: status=%d err=%s\n", status, err);
        return 1;
    }

    const double v2_map[4] = {30000.0, 30000.0, 30000.0, 30000.0};
    const double vm_map[4] = {30100.0, 30100.0, 30100.0, 30100.0};
    const IRCalcParameters params = {
        20.0, 30.0, 15.0, 1.0, 1.0, 2.0, 1.0, 2.0,
        1000, 0, 65535, 0
    };
    const IRGbCriteria gb_criteria = {1.5, 0.5, 1.5, 2.0, 0.0, 0.0, 3, 0};
    const IRGjbCriteria gjb_criteria = {1.0, 1.0, 0.5, 0.5, 3, 0};

    uint8_t valid_mask[4] = {0};
    IRGbBadPixelMasks gb_masks = {0};
    gb_masks.valid_mask = valid_mask;
    gb_masks.pixel_capacity = 4;

    IRGbSummary gb = {0};
    status = IR_GbEvaluateMaps(v1_map, v2_map, vn_map, 2, 2, 4,
                               &params, &gb_criteria, &gb, &gb_masks,
                               err, (int32_t)sizeof err);
    if (status != IR_OK || gb.bad_pixels.chip_grade != IR_CHIP_GRADE_A ||
        fabs(gb.before_avg_snr - 50.0) > 1e-12) {
        fprintf(stderr, "IR_GbEvaluateMaps failed: status=%d err=%s\n", status, err);
        return 1;
    }

    IRGjbSummary gjb = {0};
    status = IR_GjbEvaluateMaps(v1_map, v2_map, vn_map, vm_map, 2, 2, 4,
                                &params, &gjb_criteria, &gjb, NULL,
                                err, (int32_t)sizeof err);
    if (status != IR_OK || gjb.bad_pixels.total_bad_count != 0 ||
        fabs(gjb.before_avg_snr - 50.0) > 1e-12) {
        fprintf(stderr, "IR_GjbEvaluateMaps failed: status=%d err=%s\n", status, err);
        return 1;
    }

    /* 2. Noise3D calibration build + ORG frame evaluation */
    const double mean_h[4] = {1200.0, 1200.0, 1200.0, 1200.0};
    const double mean_l[4] = {1000.0, 1000.0, 1000.0, 1000.0};
    const double rms_in[4] = {4.0, 4.0, 4.0, 4.0};
    const double b_in[4] = {1100.0, 1100.0, 1100.0, 1100.0};
    const IRNoise3DCalibParams calib_params = {0.5, 1.5, 0.5, 1.5, 1e-6};
    uint8_t resp_blind[4] = {0}, rms_blind[4] = {0}, total_blind[4] = {0}, calib_valid[4] = {0};
    double k_map[4] = {0}, calib_stats[IR_NOISE3D_STATS_COUNT] = {0};
    const IRNoise3DCalibOutputs calib_out = {
        resp_blind, rms_blind, total_blind, calib_valid,
        k_map, calib_stats, 4, IR_NOISE3D_STATS_COUNT, 0
    };
    status = IR_Noise3DBuildCalibDn(mean_h, mean_l, rms_in, b_in, 2, 2, 4,
                                    &calib_params, &calib_out, err, (int32_t)sizeof err);
    if (status != IR_OK || fabs(calib_stats[IR_NOISE3D_STAT_VALID_COUNT] - 4.0) > 1e-12) {
        fprintf(stderr, "IR_Noise3DBuildCalibDn failed: status=%d err=%s\n", status, err);
        return 1;
    }

    const uint16_t n3d_frames[8] = {10, 20, 30, 40, 14, 22, 38, 42};
    const IRNoise3DComputeParams compute_params = {IR_NOISE3D_MODE_ORG, 1, 4, 0, 0.0, -999.0};
    double scalars[IR_NOISE3D_SCALAR_COUNT] = {0};
    double hist[IR_NOISE3D_HIST_GROUP_COUNT * 4] = {0};
    double hist_meta[IR_NOISE3D_HIST_GROUP_COUNT * 3] = {0};
    double map_ij[4] = {0}, map_ti[4] = {0}, map_tj[4] = {0};
    IRNoise3DComputeOutputs compute_out = {0};
    compute_out.scalars = scalars;
    compute_out.hist = hist;
    compute_out.hist_meta = hist_meta;
    compute_out.map_ij = map_ij;
    compute_out.map_ti = map_ti;
    compute_out.map_tj = map_tj;
    compute_out.hist_capacity = IR_NOISE3D_HIST_GROUP_COUNT * 4;
    compute_out.map_ij_capacity = 4;
    compute_out.map_ti_capacity = 4;
    compute_out.map_tj_capacity = 4;
    compute_out.scalars_capacity = IR_NOISE3D_SCALAR_COUNT;
    compute_out.hist_meta_capacity = IR_NOISE3D_HIST_GROUP_COUNT * 3;

    status = IR_Noise3DComputeFramesDn(n3d_frames, 8, 2, 2, 2,
                                       total_blind, 4, NULL, NULL, 0,
                                       &compute_params, &compute_out,
                                       err, (int32_t)sizeof err);
    if (status != IR_OK || !isfinite(scalars[IR_NOISE3D_SCALAR_DELTA_TOTAL])) {
        fprintf(stderr, "IR_Noise3DComputeFramesDn failed: status=%d err=%s\n", status, err);
        return 1;
    }

    /* 3. Stage A composable primitives: NUC + 15x15 LRNU/HRNU + masks + FFT + GJB burst */
    double v1_15[225] = {0}, v2_15[225] = {0};
    uint16_t burst_frames[450] = {0};
    for (int r = 0; r < 15; ++r) {
        for (int c = 0; c < 15; ++c) {
            const int idx = r * 15 + c;
            v1_15[idx] = 30200.0;
            v2_15[idx] = 30000.0;
            burst_frames[idx] = (uint16_t)(30100 + r * 2 - 1);
            burst_frames[225 + idx] = (uint16_t)(30100 + r * 2 + 1);
        }
    }

    double k15[225] = {0}, b15[225] = {0}, nuc_sum[4] = {0}, affine15[225] = {0};
    status = IR_BuildTwoPointNucF64(v1_15, v2_15, NULL, NULL, 15, 15, 225,
                                    k15, b15, nuc_sum, 4, err, (int32_t)sizeof err);
    if (status != IR_OK || fabs(k15[0] - 1.0) > 1e-12 || fabs(nuc_sum[3] - 225.0) > 1e-12) {
        fprintf(stderr, "IR_BuildTwoPointNucF64 failed: status=%d err=%s\n", status, err);
        return 1;
    }

    status = IR_ApplyAffineMapF64(v1_15, k15, b15, 15, 15, 225, affine15, err, (int32_t)sizeof err);
    if (status != IR_OK || fabs(affine15[0] - 30200.0) > 1e-12) {
        fprintf(stderr, "IR_ApplyAffineMapF64 failed: status=%d err=%s\n", status, err);
        return 1;
    }

    double mean15[225] = {0}, vn15[225] = {0}, nuc_stat_sum[2] = {0};
    status = IR_FrameStatisticsNucU16(burst_frames, 450, 2, 15, 15, 0,
                                     k15, b15, NULL, NULL, mean15, vn15, 225,
                                     nuc_stat_sum, 2, err, (int32_t)sizeof err);
    if (status != IR_OK || fabs(mean15[0] - 30100.0) > 1e-12 || fabs(vn15[0] - 1.0) > 1e-12) {
        fprintf(stderr, "IR_FrameStatisticsNucU16 failed: status=%d err=%s\n", status, err);
        return 1;
    }

    IRMapSummary map_summary = {0};
    status = IR_MapSummaryF64(mean15, NULL, 15, 15, 225, 0, 0, 0, 0, 5.0, 95.0,
                             &map_summary, err, (int32_t)sizeof err);
    if (status != IR_OK || map_summary.valid_count != 225 || fabs(map_summary.span - 28.0) > 1e-12) {
        fprintf(stderr, "IR_MapSummaryF64 failed: status=%d err=%s\n", status, err);
        return 1;
    }

    double low15[225] = {0}, high15[225] = {0};
    status = IR_SpatialLowHighPassF64(mean15, NULL, 15, 15, 225,
                                      IR_SPATIAL_FILTER_BOX_MEAN, 15, 15,
                                      low15, high15, err, (int32_t)sizeof err);
    const int center_idx = 7 * 15 + 7;
    if (status != IR_OK || fabs(low15[center_idx] - 30114.0) > 1e-9 ||
        fabs(high15[center_idx]) > 1e-9 || fabs(high15[0] + 7.0) > 1e-9) {
        fprintf(stderr, "IR_SpatialLowHighPassF64 failed: status=%d err=%s\n", status, err);
        return 1;
    }
    const int lrnu_ok = 1;

    uint8_t mask_pass[225] = {0}, mask_inv[225] = {0};
    int32_t pass_cnt = 0, fail_cnt = 0, inv_cnt = 0;
    status = IR_ThresholdMaskF64(vn15, NULL, 15, 15, 225, IR_THRESH_UPPER_ONLY,
                                  0.0, 2.0, mask_pass, &pass_cnt, &fail_cnt,
                                  err, (int32_t)sizeof err);
    if (status != IR_OK || pass_cnt != 225 || fail_cnt != 0) {
        fprintf(stderr, "IR_ThresholdMaskF64 failed: status=%d err=%s\n", status, err);
        return 1;
    }
    status = IR_MaskCombine(mask_pass, NULL, 15, 15, 225, IR_MASK_NOT,
                            mask_inv, &inv_cnt, err, (int32_t)sizeof err);
    if (status != IR_OK || inv_cnt != 0) {
        fprintf(stderr, "IR_MaskCombine failed: status=%d err=%s\n", status, err);
        return 1;
    }

    double var_sub[225] = {0};
    status = IR_MapBinaryOpF64(mean15, vn15, 15, 15, 225,
                               IR_MAP_OP_VARIANCE_SUBTRACT, 1.0, var_sub,
                               err, (int32_t)sizeof err);
    if (status != IR_OK || !isfinite(var_sub[0])) {
        fprintf(stderr, "IR_MapBinaryOpF64 failed: status=%d err=%s\n", status, err);
        return 1;
    }

    double sig[8] = {0}, freqs[5] = {0}, amp[5] = {0}, psd[5] = {0};
    const double two_pi = 6.28318530717958647692;
    for (int i = 0; i < 8; ++i)
        sig[i] = 2.0 + 3.0 * cos(two_pi * (double)i / 8.0);
    status = IR_ComputeRfft1D(sig, 8, 80.0, IR_WINDOW_RECTANGULAR, 1,
                             freqs, amp, psd, 5, err, (int32_t)sizeof err);
    if (status != IR_OK || fabs(freqs[1] - 10.0) > 1e-12 || fabs(amp[1] - 3.0) > 1e-9) {
        fprintf(stderr, "IR_ComputeRfft1D failed: status=%d err=%s\n", status, err);
        return 1;
    }

    uint16_t ch_frames[8 * 2 * 2] = {0};
    for (int t = 0; t < 8; ++t) {
        const uint16_t c0 = (uint16_t)(1000.0 + 100.0 * cos(two_pi * (double)t / 8.0));
        const uint16_t c1 = 2000;
        ch_frames[t * 4 + 0] = c0;
        ch_frames[t * 4 + 1] = c1;
        ch_frames[t * 4 + 2] = c0;
        ch_frames[t * 4 + 3] = c1;
    }
    double ch_freqs[5] = {0}, ch_series[16] = {0}, ch_amps[10] = {0}, ch_means[2] = {0}, ch_stds[2] = {0};
    status = IR_ChannelSpectrumU16(ch_frames, 32, 8, 2, 2, IR_SPECTRUM_AXIS_COLUMNS, 2,
                                  80.0, IR_WINDOW_RECTANGULAR,
                                  ch_freqs, 5, ch_series, 16, ch_amps, 10,
                                  ch_means, ch_stds, 2, err, (int32_t)sizeof err);
    if (status != IR_OK || fabs(ch_amps[1] - 100.0) > 1.5 || fabs(ch_amps[5 + 1]) > 1e-9) {
        fprintf(stderr, "IR_ChannelSpectrumU16 failed: status=%d err=%s\n", status, err);
        return 1;
    }
    const int fft_ok = 1;

    const IRGjbDriftContext ctx = {1.0, 15.0, 200.0, 1.0, 1.0, 1.0, 2, IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE, 15, 0};
    IRGjbBurstMetrics burst = {0};
    status = IR_GjbEvaluateBurstMetrics(mean15, vn15, mask_pass, 15, 15, 225,
                                        &ctx, &burst, err, (int32_t)sizeof err);
    if (status != IR_OK || burst.valid_pixel_count != 225 || !isfinite(burst.lrnu_pct) ||
        !isfinite(burst.estimated_netd_mk)) {
        fprintf(stderr, "IR_GjbEvaluateBurstMetrics failed: status=%d err=%s\n", status, err);
        return 1;
    }

    double g_low[225] = {0}, g_high[225] = {0};
    status = IR_SpatialLowHighPassF64(mean15, NULL, 15, 15, 225,
                                      IR_SPATIAL_FILTER_GAUSSIAN, 5, 5,
                                      g_low, g_high, err, (int32_t)sizeof err);
    if (status != IR_OK || !isfinite(g_low[center_idx]) ||
        fabs(mean15[center_idx] - (g_low[center_idx] + g_high[center_idx])) > 1e-9) {
        fprintf(stderr, "IR_SpatialLowHighPassF64(GAUSSIAN) failed: status=%d err=%s\n", status, err);
        return 1;
    }
    const int gaussian_ok = 1;

    uint16_t flick_frames[16] = {
        1000, 1000, 1000, 1020,
        1000, 1000, 1000, 1020,
        1000, 1000, 1000, 1020,
        1000, 1000, 1000, 1000
    };
    IRFlickerSummary flick = {0};
    uint8_t flick_mask[4] = {0};
    status = IR_FlickerDetectU16(flick_frames, 16, 4, 1, 4, NULL, NULL, NULL,
                                 1.5, 2, &flick, NULL, flick_mask, 4,
                                 NULL, NULL, 0, err, (int32_t)sizeof err);
    if (status != IR_OK || flick.flicker_count != 1 || flick_mask[3] != 1) {
        fprintf(stderr, "IR_FlickerDetectU16 failed: status=%d err=%s\n", status, err);
        return 1;
    }
    const int flicker_ok = 1;

    printf("analysis_version=%s\n", version);
    printf("gb_grade=%c\n", grade_char(gb.bad_pixels.chip_grade));
    printf("gjb_bad_count=%d\n", gjb.bad_pixels.total_bad_count);
    printf("noise3d_total=%.12g\n", scalars[IR_NOISE3D_SCALAR_DELTA_TOTAL]);
    printf("lrnu_ok=%d\n", lrnu_ok);
    printf("fft_ok=%d\n", fft_ok);
    printf("gaussian_ok=%d\n", gaussian_ok);
    printf("flicker_ok=%d\n", flicker_ok);
    return 0;
}
