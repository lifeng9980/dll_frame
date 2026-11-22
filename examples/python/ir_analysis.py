#!/usr/bin/env python3
"""Call IR_analysis Noise3D and GB/GJB C ABI functions via Python standard library ctypes."""

import argparse
import ctypes as c
import math
import os
from pathlib import Path
import sys


class IRCalcParameters(c.Structure):
    _fields_ = [
        ("t0_c", c.c_double),
        ("t1_c", c.c_double),
        ("pixel_pitch_um", c.c_double),
        ("emissivity", c.c_double),
        ("system_gain", c.c_double),
        ("adc_swing_v", c.c_double),
        ("f_number", c.c_double),
        ("vmax_per_count", c.c_double),
        ("integration_time_us", c.c_int32),
        ("invalid_integration_us", c.c_int32),
        ("digital_full", c.c_int32),
        ("reserved", c.c_int32),
    ]


class IRGbCriteria(c.Structure):
    _fields_ = [
        ("response_upper", c.c_double),
        ("response_lower", c.c_double),
        ("noise_upper", c.c_double),
        ("netd_upper", c.c_double),
        ("short_bad_threshold", c.c_double),
        ("open_bad_threshold", c.c_double),
        ("cycle_upper", c.c_int32),
        ("reserved", c.c_int32),
    ]


class IRGbBadPixelSummary(c.Structure):
    _fields_ = [
        ("total_bad_rate_pct", c.c_double),
        ("response_bad_count", c.c_int32),
        ("response_upper_bad_count", c.c_int32),
        ("response_lower_bad_count", c.c_int32),
        ("noise_bad_count", c.c_int32),
        ("netd_bad_count", c.c_int32),
        ("total_bad_count", c.c_int32),
        ("iteration_count", c.c_int32),
        ("chip_grade", c.c_int32),
        ("cluster_counts", (c.c_int32 * 5) * 3),
        ("reserved", c.c_int32),
    ]


class IRGbBadPixelMasks(c.Structure):
    _fields_ = [
        ("valid_mask", c.POINTER(c.c_uint8)),
        ("total_bad_mask", c.POINTER(c.c_uint8)),
        ("response_bad_mask", c.POINTER(c.c_uint8)),
        ("noise_bad_mask", c.POINTER(c.c_uint8)),
        ("netd_bad_mask", c.POINTER(c.c_uint8)),
        ("short_bad_mask", c.POINTER(c.c_uint8)),
        ("open_bad_mask", c.POINTER(c.c_uint8)),
        ("pixel_capacity", c.c_int64),
    ]


class IRGbSummary(c.Structure):
    _fields_ = [
        ("before_avg_pixel_response_count", c.c_double),
        ("before_avg_pixel_response_voltage", c.c_double),
        ("volts_per_count", c.c_double),
        ("before_avg_snr", c.c_double),
        ("irradiance_power_diff", c.c_double),
        ("before_avg_blackbody_response", c.c_double),
        ("before_avg_noise_voltage_v", c.c_double),
        ("before_avg_netd_mk", c.c_double),
        ("before_avg_detectivity", c.c_double),
        ("before_avg_responsivity", c.c_double),
        ("before_sitf", c.c_double),
        ("after_response_mv_per_ms_k", c.c_double),
        ("after_avg_response_voltage_v", c.c_double),
        ("after_avg_responsivity", c.c_double),
        ("after_avg_nep_w", c.c_double),
        ("after_response_nonuniformity_pct", c.c_double),
        ("after_avg_noise_voltage_mv", c.c_double),
        ("after_avg_netd_mk", c.c_double),
        ("after_avg_detectivity", c.c_double),
        ("short_bad_pixel_count", c.c_int32),
        ("open_bad_pixel_count", c.c_int32),
        ("bad_pixels", IRGbBadPixelSummary),
    ]


class IRGjbCriteria(c.Structure):
    _fields_ = [
        ("k1", c.c_double),
        ("k2", c.c_double),
        ("k3", c.c_double),
        ("k4", c.c_double),
        ("cycle_upper", c.c_int32),
        ("reserved", c.c_int32),
    ]


class IRGjbClusterStats(c.Structure):
    _fields_ = [
        ("count_1_to_6", c.c_int32),
        ("count_7_to_12", c.c_int32),
        ("count_13_to_19", c.c_int32),
        ("count_20_to_35", c.c_int32),
        ("count_gt_35", c.c_int32),
    ]


class IRGjbBadPixelSummary(c.Structure):
    _fields_ = [
        ("total_bad_rate_pct", c.c_double),
        ("total_bad_count", c.c_int32),
        ("iteration_count", c.c_int32),
        ("center_3x3_block_count", c.c_int32),
        ("filtered_4x4_block_count", c.c_int32),
        ("total_clusters", IRGjbClusterStats),
        ("center_clusters", IRGjbClusterStats),
        ("edge_clusters", IRGjbClusterStats),
        ("reserved", c.c_int32),
    ]


class IRGjbBadPixelMasks(c.Structure):
    _fields_ = [
        ("valid_mask", c.POINTER(c.c_uint8)),
        ("total_bad_mask", c.POINTER(c.c_uint8)),
        ("k1_bad_mask", c.POINTER(c.c_uint8)),
        ("k2_bad_mask", c.POINTER(c.c_uint8)),
        ("k3_bad_mask", c.POINTER(c.c_uint8)),
        ("k4_bad_mask", c.POINTER(c.c_uint8)),
        ("block_filtered_bad_mask", c.POINTER(c.c_uint8)),
        ("pixel_capacity", c.c_int64),
    ]


class IRGjbSummary(c.Structure):
    _fields_ = [
        ("before_avg_pixel_response_count", c.c_double),
        ("before_avg_pixel_response_voltage", c.c_double),
        ("volts_per_count", c.c_double),
        ("before_avg_snr", c.c_double),
        ("fov", c.c_double),
        ("irradiance_power_diff", c.c_double),
        ("before_avg_noise_voltage_v", c.c_double),
        ("before_avg_netd_mk", c.c_double),
        ("before_avg_detectivity", c.c_double),
        ("before_avg_responsivity", c.c_double),
        ("after_avg_response_voltage_v", c.c_double),
        ("after_avg_responsivity", c.c_double),
        ("after_avg_nep_w", c.c_double),
        ("after_response_nonuniformity_pct", c.c_double),
        ("after_avg_noise_voltage_mv", c.c_double),
        ("after_avg_netd_mk", c.c_double),
        ("after_avg_detectivity", c.c_double),
        ("after_spatial_noise", c.c_double),
        ("after_max_dynamic_range_db", c.c_double),
        ("after_rfpn_v", c.c_double),
        ("bad_pixels", IRGjbBadPixelSummary),
    ]


class IRNoise3DCalibParams(c.Structure):
    _fields_ = [
        ("response_lower_mul", c.c_double),
        ("response_upper_mul", c.c_double),
        ("rms_lower_mul", c.c_double),
        ("rms_upper_mul", c.c_double),
        ("k_epsilon", c.c_double),
    ]


class IRNoise3DCalibOutputs(c.Structure):
    _fields_ = [
        ("response_blind_mask", c.POINTER(c.c_uint8)),
        ("rms_blind_mask", c.POINTER(c.c_uint8)),
        ("total_blind_mask", c.POINTER(c.c_uint8)),
        ("valid_mask", c.POINTER(c.c_uint8)),
        ("k_map", c.POINTER(c.c_double)),
        ("stats", c.POINTER(c.c_double)),
        ("pixel_capacity", c.c_int64),
        ("stats_capacity", c.c_int32),
        ("reserved", c.c_int32),
    ]


class IRNoise3DComputeParams(c.Structure):
    _fields_ = [
        ("compute_mode", c.c_int32),
        ("thread_count", c.c_int32),
        ("hist_interval", c.c_int32),
        ("reserved", c.c_int32),
        ("b_avg", c.c_double),
        ("invalid_fill", c.c_double),
    ]


class IRNoise3DComputeOutputs(c.Structure):
    _fields_ = [
        ("nuc_frames", c.POINTER(c.c_uint16)),
        ("scalars", c.POINTER(c.c_double)),
        ("hist", c.POINTER(c.c_double)),
        ("hist_meta", c.POINTER(c.c_double)),
        ("map_ij", c.POINTER(c.c_double)),
        ("map_ti", c.POINTER(c.c_double)),
        ("map_tj", c.POINTER(c.c_double)),
        ("meta", c.POINTER(c.c_double)),
        ("nuc_frames_capacity", c.c_int64),
        ("hist_capacity", c.c_int64),
        ("map_ij_capacity", c.c_int64),
        ("map_ti_capacity", c.c_int64),
        ("map_tj_capacity", c.c_int64),
        ("scalars_capacity", c.c_int32),
        ("hist_meta_capacity", c.c_int32),
        ("meta_capacity", c.c_int32),
        ("reserved", c.c_int32),
    ]


IR_SPATIAL_FILTER_BOX_MEAN = 0
IR_SPATIAL_FILTER_GAUSSIAN = 1

IR_MAP_OP_SUB = 0
IR_MAP_OP_ADD = 1
IR_MAP_OP_MUL = 2
IR_MAP_OP_SAFE_DIV = 3
IR_MAP_OP_VARIANCE_SUBTRACT = 4

IR_THRESH_ABSOLUTE = 0
IR_THRESH_MEAN_MULTIPLIER = 1
IR_THRESH_MEAN_SIGMA = 2
IR_THRESH_UPPER_ONLY = 3
IR_THRESH_LOWER_ONLY = 4

IR_MASK_AND = 0
IR_MASK_OR = 1
IR_MASK_AND_NOT = 2
IR_MASK_NOT = 3

IR_WINDOW_RECTANGULAR = 0
IR_WINDOW_HANN = 1
IR_WINDOW_HAMMING = 2

IR_SPECTRUM_AXIS_COLUMNS = 0
IR_SPECTRUM_AXIS_ROWS = 1

IR_NOISE_SUBTRACT_NONE = 0
IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE = 1


class IRMapSummary(c.Structure):
    _fields_ = [
        ("mean", c.c_double),
        ("pop_std", c.c_double),
        ("sample_std", c.c_double),
        ("min_val", c.c_double),
        ("max_val", c.c_double),
        ("span", c.c_double),
        ("percentile_low", c.c_double),
        ("percentile_high", c.c_double),
        ("percentile_span", c.c_double),
        ("valid_count", c.c_int64),
    ]


class IRGjbDriftContext(c.Structure):
    _fields_ = [
        ("volts_per_count", c.c_double),
        ("delta_t_k", c.c_double),
        ("ref_response_count", c.c_double),
        ("ref_responsivity", c.c_double),
        ("vmax_per_count", c.c_double),
        ("noise_subtract_factor", c.c_double),
        ("burst_frame_count", c.c_int32),
        ("noise_subtract_mode", c.c_int32),
        ("lrnu_kernel_size", c.c_int32),
        ("reserved", c.c_int32),
    ]


class IRGjbBurstMetrics(c.Structure):
    _fields_ = [
        ("mean_dn", c.c_double),
        ("mean_v", c.c_double),
        ("span_dn", c.c_double),
        ("spatial_std_dn", c.c_double),
        ("spatial_std_v", c.c_double),
        ("spatial_std_denoised_dn", c.c_double),
        ("spatial_std_denoised_v", c.c_double),
        ("nonuniformity_pct", c.c_double),
        ("avg_temporal_noise_dn", c.c_double),
        ("avg_temporal_noise_mv", c.c_double),
        ("rms_temporal_noise_dn", c.c_double),
        ("estimated_netd_mk", c.c_double),
        ("estimated_nep_w", c.c_double),
        ("spatial_noise", c.c_double),
        ("max_dynamic_range_db", c.c_double),
        ("lrnu_std_dn", c.c_double),
        ("lrnu_span_dn", c.c_double),
        ("lrnu_pct", c.c_double),
        ("hrnu_std_dn", c.c_double),
        ("hrnu_pct", c.c_double),
        ("valid_pixel_count", c.c_int64),
    ]


class IRFlickerSummary(c.Structure):
    _fields_ = [
        ("std_dev_multiplier", c.c_double),
        ("flicker_ratio_pct", c.c_double),
        ("mean_frame_mean_dn", c.c_double),
        ("mean_frame_sigma_dn", c.c_double),
        ("exceed_count_threshold", c.c_int32),
        ("frame_count", c.c_int32),
        ("valid_pixel_count", c.c_int32),
        ("flicker_count", c.c_int32),
    ]


class IRFft2dSummary(c.Structure):
    _fields_ = [
        ("dc_magnitude", c.c_double),
        ("peak_ac_magnitude", c.c_double),
        ("total_ac_power", c.c_double),
        ("low_freq_power_ratio", c.c_double),
        ("high_freq_power_ratio", c.c_double),
        ("peak_ac_row", c.c_int32),
        ("peak_ac_col", c.c_int32),
        ("rows", c.c_int32),
        ("cols", c.c_int32),
        ("valid_pixel_count", c.c_int32),
        ("reserved", c.c_int32),
    ]


def sdk_library_directory() -> Path:
    configured = os.environ.get("IR_SDK_LIB_DIR")
    if configured:
        return Path(configured).resolve()
    package_root = Path(__file__).resolve().parents[2]
    return package_root / ("windows-x64/bin" if os.name == "nt" else "linux-x86_64/lib")


def load_analysis(directory: Path):
    if os.name == "nt":
        dll_path = directory / "IR_analysis.dll"
        dll_directory = os.add_dll_directory(str(directory))
        try:
            return c.CDLL(str(dll_path)), dll_directory
        except Exception:
            dll_directory.close()
            raise
    return c.CDLL(str(directory / "libIR_analysis.so")), None


def verify_abi_sizes() -> None:
    expected = [
        (IRCalcParameters, 80),
        (IRGbCriteria, 56),
        (IRGbBadPixelSummary, 104),
        (IRGbSummary, 264),
        (IRGjbCriteria, 40),
        (IRGjbClusterStats, 20),
        (IRGjbBadPixelSummary, 88),
        (IRGjbSummary, 248),
        (IRNoise3DCalibParams, 40),
        (IRNoise3DComputeParams, 32),
        (IRMapSummary, 80),
        (IRGjbDriftContext, 64),
        (IRGjbBurstMetrics, 168),
        (IRFlickerSummary, 48),
        (IRFft2dSummary, 64),
    ]
    if c.sizeof(c.c_void_p) == 8:
        expected.extend([
            (IRGbBadPixelMasks, 64),
            (IRGjbBadPixelMasks, 64),
            (IRNoise3DCalibOutputs, 64),
            (IRNoise3DComputeOutputs, 120),
        ])
    for cls, size in expected:
        if c.sizeof(cls) != size:
            raise RuntimeError(f"ABI size mismatch for {cls.__name__}: {c.sizeof(cls)} != {size}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library-dir", type=Path, default=sdk_library_directory())
    args = parser.parse_args()

    verify_abi_sizes()
    analysis, dll_directory = load_analysis(args.library_dir.resolve())
    try:
        err = c.create_string_buffer(1024)
        err_args = [c.c_char_p, c.c_int32]

        analysis.IR_AnalysisVersion.argtypes = []
        analysis.IR_AnalysisVersion.restype = c.c_char_p

        bindings = [
            ("IR_FrameStatisticsU16", [
                c.POINTER(c.c_uint16), c.c_int64, c.c_int32, c.c_int32, c.c_int32, c.c_int32,
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int64,
                c.POINTER(c.c_double), c.c_int32, *err_args,
            ]),
            ("IR_ROIMeanU16", [
                c.POINTER(c.c_uint16), c.c_int64, c.c_int32, c.c_int32, c.c_int32,
                c.c_int32, c.c_int32, c.c_int32, c.c_int32, c.POINTER(c.c_double), *err_args,
            ]),
            ("IR_GbProcessBadPixels", [
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double),
                c.c_int32, c.c_int32, c.c_int64,
                c.POINTER(IRGbCriteria), c.POINTER(IRGbBadPixelSummary), c.POINTER(IRGbBadPixelMasks), *err_args,
            ]),
            ("IR_GbEvaluateMaps", [
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double),
                c.c_int32, c.c_int32, c.c_int64,
                c.POINTER(IRCalcParameters), c.POINTER(IRGbCriteria),
                c.POINTER(IRGbSummary), c.POINTER(IRGbBadPixelMasks), *err_args,
            ]),
            ("IR_GjbProcessBadPixels", [
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double),
                c.POINTER(c.c_double), c.POINTER(c.c_double),
                c.c_int32, c.c_int32, c.c_int64,
                c.POINTER(IRGjbCriteria), c.POINTER(IRGjbBadPixelSummary), c.POINTER(IRGjbBadPixelMasks), *err_args,
            ]),
            ("IR_GjbEvaluateMaps", [
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double),
                c.c_int32, c.c_int32, c.c_int64,
                c.POINTER(IRCalcParameters), c.POINTER(IRGjbCriteria),
                c.POINTER(IRGjbSummary), c.POINTER(IRGjbBadPixelMasks), *err_args,
            ]),
            ("IR_Noise3DBuildCalibDn", [
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double),
                c.c_int32, c.c_int32, c.c_int64,
                c.POINTER(IRNoise3DCalibParams), c.POINTER(IRNoise3DCalibOutputs), *err_args,
            ]),
            ("IR_Noise3DComputeFramesDn", [
                c.POINTER(c.c_uint16), c.c_int64, c.c_int32, c.c_int32, c.c_int32,
                c.POINTER(c.c_uint8), c.c_int64, c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int64,
                c.POINTER(IRNoise3DComputeParams), c.POINTER(IRNoise3DComputeOutputs), *err_args,
            ]),
            ("IR_BuildTwoPointNucF64", [
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_uint8), c.POINTER(c.c_double),
                c.c_int32, c.c_int32, c.c_int64,
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int32, *err_args,
            ]),
            ("IR_ApplyAffineMapF64", [
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double),
                c.c_int32, c.c_int32, c.c_int64, c.POINTER(c.c_double), *err_args,
            ]),
            ("IR_FrameStatisticsNucU16", [
                c.POINTER(c.c_uint16), c.c_int64, c.c_int32, c.c_int32, c.c_int32, c.c_int32,
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_uint8),
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int64,
                c.POINTER(c.c_double), c.c_int32, *err_args,
            ]),
            ("IR_MapSummaryF64", [
                c.POINTER(c.c_double), c.POINTER(c.c_uint8), c.c_int32, c.c_int32, c.c_int64,
                c.c_int32, c.c_int32, c.c_int32, c.c_int32, c.c_double, c.c_double,
                c.POINTER(IRMapSummary), *err_args,
            ]),
            ("IR_SpatialLowHighPassF64", [
                c.POINTER(c.c_double), c.POINTER(c.c_uint8), c.c_int32, c.c_int32, c.c_int64,
                c.c_int32, c.c_int32, c.c_int32, c.POINTER(c.c_double), c.POINTER(c.c_double), *err_args,
            ]),
            ("IR_MapBinaryOpF64", [
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int32, c.c_int32, c.c_int64,
                c.c_int32, c.c_double, c.POINTER(c.c_double), *err_args,
            ]),
            ("IR_ThresholdMaskF64", [
                c.POINTER(c.c_double), c.POINTER(c.c_uint8), c.c_int32, c.c_int32, c.c_int64,
                c.c_int32, c.c_double, c.c_double,
                c.POINTER(c.c_uint8), c.POINTER(c.c_int32), c.POINTER(c.c_int32), *err_args,
            ]),
            ("IR_MaskCombine", [
                c.POINTER(c.c_uint8), c.POINTER(c.c_uint8), c.c_int32, c.c_int32, c.c_int64,
                c.c_int32, c.POINTER(c.c_uint8), c.POINTER(c.c_int32), *err_args,
            ]),
            ("IR_ComputeRfft1D", [
                c.POINTER(c.c_double), c.c_int32, c.c_double, c.c_int32, c.c_int32,
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int32, *err_args,
            ]),
            ("IR_ChannelSpectrumU16", [
                c.POINTER(c.c_uint16), c.c_int64, c.c_int32, c.c_int32, c.c_int32,
                c.c_int32, c.c_int32, c.c_double, c.c_int32,
                c.POINTER(c.c_double), c.c_int32, c.POINTER(c.c_double), c.c_int64,
                c.POINTER(c.c_double), c.c_int64, c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int32,
                *err_args,
            ]),
            ("IR_GjbEvaluateBurstMetrics", [
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_uint8),
                c.c_int32, c.c_int32, c.c_int64,
                c.POINTER(IRGjbDriftContext), c.POINTER(IRGjbBurstMetrics), *err_args,
            ]),
            ("IR_FlickerDetectU16", [
                c.POINTER(c.c_uint16), c.c_int64, c.c_int32, c.c_int32, c.c_int32,
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_uint8),
                c.c_double, c.c_int32, c.POINTER(IRFlickerSummary),
                c.POINTER(c.c_int32), c.POINTER(c.c_uint8), c.c_int64,
                c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int32,
                *err_args,
            ]),
            ("IR_Fft2dMagnitudeF64", [
                c.POINTER(c.c_double), c.POINTER(c.c_uint8),
                c.c_int32, c.c_int32, c.c_int64,
                c.c_int32, c.c_int32, c.c_int32,
                c.POINTER(IRFft2dSummary), c.POINTER(c.c_double), c.POINTER(c.c_double),
                *err_args,
            ]),
        ]
        for fn_name, argtypes in bindings:
            fn = getattr(analysis, fn_name)
            fn.argtypes = argtypes
            fn.restype = c.c_int32

        version = analysis.IR_AnalysisVersion().decode("utf-8")
        t1_frames = (c.c_uint16 * 8)(30196, 30196, 30196, 30196, 30204, 30204, 30204, 30204)
        v1_map = (c.c_double * 4)()
        vn_map = (c.c_double * 4)()
        stat_summary = (c.c_double * 2)()
        st = analysis.IR_FrameStatisticsU16(
            t1_frames, 8, 2, 2, 2, 0, v1_map, vn_map, 4, stat_summary, 2, err, len(err)
        )
        if st != 0 or abs(vn_map[0] - 4.0) > 1e-12:
            raise RuntimeError(f"IR_FrameStatisticsU16 failed: st={st} {err.value.decode()}")

        v2_map = (c.c_double * 4)(30000.0, 30000.0, 30000.0, 30000.0)
        vm_map = (c.c_double * 4)(30100.0, 30100.0, 30100.0, 30100.0)
        params = IRCalcParameters(20.0, 30.0, 15.0, 1.0, 1.0, 2.0, 1.0, 2.0, 1000, 0, 65535, 0)
        gb_crit = IRGbCriteria(1.5, 0.5, 1.5, 2.0, 0.0, 0.0, 3, 0)
        gjb_crit = IRGjbCriteria(1.0, 1.0, 0.5, 0.5, 3, 0)

        gb = IRGbSummary()
        st = analysis.IR_GbEvaluateMaps(
            v1_map, v2_map, vn_map, 2, 2, 4,
            c.byref(params), c.byref(gb_crit), c.byref(gb), None, err, len(err)
        )
        if st != 0 or gb.bad_pixels.chip_grade != 1 or abs(gb.before_avg_snr - 50.0) > 1e-12:
            raise RuntimeError(f"IR_GbEvaluateMaps failed: st={st} {err.value.decode()}")

        gjb = IRGjbSummary()
        st = analysis.IR_GjbEvaluateMaps(
            v1_map, v2_map, vn_map, vm_map, 2, 2, 4,
            c.byref(params), c.byref(gjb_crit), c.byref(gjb), None, err, len(err)
        )
        if st != 0 or gjb.bad_pixels.total_bad_count != 0 or abs(gjb.before_avg_snr - 50.0) > 1e-12:
            raise RuntimeError(f"IR_GjbEvaluateMaps failed: st={st} {err.value.decode()}")

        mean_h = (c.c_double * 4)(1200.0, 1200.0, 1200.0, 1200.0)
        mean_l = (c.c_double * 4)(1000.0, 1000.0, 1000.0, 1000.0)
        rms_in = (c.c_double * 4)(4.0, 4.0, 4.0, 4.0)
        b_in = (c.c_double * 4)(1100.0, 1100.0, 1100.0, 1100.0)
        calib_params = IRNoise3DCalibParams(0.5, 1.5, 0.5, 1.5, 1e-6)
        resp_b = (c.c_uint8 * 4)()
        rms_b = (c.c_uint8 * 4)()
        total_b = (c.c_uint8 * 4)()
        valid_m = (c.c_uint8 * 4)()
        k_map = (c.c_double * 4)()
        stats = (c.c_double * 32)()
        calib_out = IRNoise3DCalibOutputs(resp_b, rms_b, total_b, valid_m, k_map, stats, 4, 32, 0)
        st = analysis.IR_Noise3DBuildCalibDn(
            mean_h, mean_l, rms_in, b_in, 2, 2, 4,
            c.byref(calib_params), c.byref(calib_out), err, len(err)
        )
        if st != 0:
            raise RuntimeError(f"IR_Noise3DBuildCalibDn failed: st={st} {err.value.decode()}")

        n3d_frames = (c.c_uint16 * 8)(10, 20, 30, 40, 14, 22, 38, 42)
        c_params = IRNoise3DComputeParams(0, 1, 4, 0, 0.0, -999.0)
        scalars = (c.c_double * 16)()
        hist = (c.c_double * 16)()
        hist_meta = (c.c_double * 12)()
        map_ij = (c.c_double * 4)()
        map_ti = (c.c_double * 4)()
        map_tj = (c.c_double * 4)()
        c_out = IRNoise3DComputeOutputs(
            None, scalars, hist, hist_meta, map_ij, map_ti, map_tj, None,
            0, 16, 4, 4, 4, 16, 12, 0, 0
        )
        st = analysis.IR_Noise3DComputeFramesDn(
            n3d_frames, 8, 2, 2, 2, total_b, 4, None, None, 0,
            c.byref(c_params), c.byref(c_out), err, len(err)
        )
        if st != 0 or not math.isfinite(scalars[7]):
            raise RuntimeError(f"IR_Noise3DComputeFramesDn failed: st={st} {err.value.decode()}")
        noise3d_status = st

        # Stage A composable operators: NUC + 15x15 LRNU/HRNU + FFT + GJB burst metrics
        v1_15 = (c.c_double * 225)(*([30200.0] * 225))
        v2_15 = (c.c_double * 225)(*([30000.0] * 225))
        burst_frames = (c.c_uint16 * 450)()
        for r in range(15):
            for col in range(15):
                idx = r * 15 + col
                burst_frames[idx] = 30100 + r * 2 - 1
                burst_frames[225 + idx] = 30100 + r * 2 + 1

        k15 = (c.c_double * 225)()
        b15 = (c.c_double * 225)()
        nuc_sum = (c.c_double * 4)()
        st = analysis.IR_BuildTwoPointNucF64(
            v1_15, v2_15, None, None, 15, 15, 225, k15, b15, nuc_sum, 4, err, len(err)
        )
        if st != 0 or abs(k15[0] - 1.0) > 1e-12:
            raise RuntimeError(f"IR_BuildTwoPointNucF64 failed: st={st} {err.value.decode()}")

        mean15 = (c.c_double * 225)()
        vn15 = (c.c_double * 225)()
        nuc_stat_sum = (c.c_double * 2)()
        st = analysis.IR_FrameStatisticsNucU16(
            burst_frames, 450, 2, 15, 15, 0, k15, b15, None, None,
            mean15, vn15, 225, nuc_stat_sum, 2, err, len(err)
        )
        if st != 0 or abs(mean15[0] - 30100.0) > 1e-12 or abs(vn15[0] - 1.0) > 1e-12:
            raise RuntimeError(f"IR_FrameStatisticsNucU16 failed: st={st} {err.value.decode()}")

        low15 = (c.c_double * 225)()
        high15 = (c.c_double * 225)()
        st = analysis.IR_SpatialLowHighPassF64(
            mean15, None, 15, 15, 225, IR_SPATIAL_FILTER_BOX_MEAN, 15, 15,
            low15, high15, err, len(err)
        )
        center_idx = 7 * 15 + 7
        if st != 0 or abs(low15[center_idx] - 30114.0) > 1e-9 or abs(high15[center_idx]) > 1e-9:
            raise RuntimeError(f"IR_SpatialLowHighPassF64 failed: st={st} {err.value.decode()}")

        map_sum = IRMapSummary()
        st = analysis.IR_MapSummaryF64(
            mean15, None, 15, 15, 225, 0, 0, 0, 0, 5.0, 95.0, c.byref(map_sum), err, len(err)
        )
        if st != 0 or map_sum.valid_count != 225 or abs(map_sum.span - 28.0) > 1e-12:
            raise RuntimeError(f"IR_MapSummaryF64 failed: st={st} {err.value.decode()}")

        mask_pass = (c.c_uint8 * 225)()
        pass_cnt = c.c_int32()
        fail_cnt = c.c_int32()
        st = analysis.IR_ThresholdMaskF64(
            vn15, None, 15, 15, 225, IR_THRESH_UPPER_ONLY, 0.0, 2.0,
            mask_pass, c.byref(pass_cnt), c.byref(fail_cnt), err, len(err)
        )
        if st != 0 or pass_cnt.value != 225:
            raise RuntimeError(f"IR_ThresholdMaskF64 failed: st={st} {err.value.decode()}")

        sig = (c.c_double * 8)(*[2.0 + 3.0 * math.cos(2.0 * math.pi * i / 8.0) for i in range(8)])
        freqs = (c.c_double * 5)()
        amp = (c.c_double * 5)()
        psd = (c.c_double * 5)()
        st = analysis.IR_ComputeRfft1D(
            sig, 8, 80.0, IR_WINDOW_RECTANGULAR, 1, freqs, amp, psd, 5, err, len(err)
        )
        if st != 0 or abs(freqs[1] - 10.0) > 1e-12 or abs(amp[1] - 3.0) > 1e-9:
            raise RuntimeError(f"IR_ComputeRfft1D failed: st={st} {err.value.decode()}")

        ctx = IRGjbDriftContext(
            1.0, 15.0, 200.0, 1.0, 1.0, 1.0, 2, IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE, 15, 0
        )
        burst = IRGjbBurstMetrics()
        st = analysis.IR_GjbEvaluateBurstMetrics(
            mean15, vn15, mask_pass, 15, 15, 225, c.byref(ctx), c.byref(burst), err, len(err)
        )
        if st != 0 or burst.valid_pixel_count != 225 or not math.isfinite(burst.lrnu_pct):
            raise RuntimeError(f"IR_GjbEvaluateBurstMetrics failed: st={st} {err.value.decode()}")

        g_low15 = (c.c_double * 225)()
        g_high15 = (c.c_double * 225)()
        st = analysis.IR_SpatialLowHighPassF64(
            mean15, None, 15, 15, 225, IR_SPATIAL_FILTER_GAUSSIAN, 5, 5,
            g_low15, g_high15, err, len(err)
        )
        if st != 0 or not math.isfinite(g_low15[center_idx]) or abs(mean15[center_idx] - (g_low15[center_idx] + g_high15[center_idx])) > 1e-9:
            raise RuntimeError(f"IR_SpatialLowHighPassF64(GAUSSIAN) failed: st={st} {err.value.decode()}")

        flick_frames = (c.c_uint16 * 16)(
            1000, 1000, 1000, 1020,
            1000, 1000, 1000, 1020,
            1000, 1000, 1000, 1020,
            1000, 1000, 1000, 1000,
        )
        flick_sum = IRFlickerSummary()
        flick_mask = (c.c_uint8 * 4)()
        st = analysis.IR_FlickerDetectU16(
            flick_frames, 16, 4, 1, 4, None, None, None,
            1.5, 2, c.byref(flick_sum), None, flick_mask, 4,
            None, None, 0, err, len(err)
        )
        if st != 0 or flick_sum.flicker_count != 1 or flick_mask[3] != 1:
            raise RuntimeError(f"IR_FlickerDetectU16 failed: st={st} {err.value.decode()}")

        fft2d_in = (c.c_double * 16)(
            *[10.0 + math.cos(2.0 * math.pi * col / 4.0) for _row in range(4) for col in range(4)]
        )
        fft2d_sum = IRFft2dSummary()
        fft2d_mag = (c.c_double * 16)()
        st = analysis.IR_Fft2dMagnitudeF64(
            fft2d_in, None, 4, 4, 16,
            0, 0, 1, c.byref(fft2d_sum), fft2d_mag, None, err, len(err)
        )
        if (
            st != 0
            or abs(fft2d_sum.dc_magnitude - 10.0) > 1e-9
            or abs(fft2d_sum.peak_ac_magnitude - 0.5) > 1e-9
            or abs(fft2d_sum.total_ac_power - 0.5) > 1e-9
            or abs(fft2d_mag[2 * 4 + 2] - 10.0) > 1e-9
        ):
            raise RuntimeError(f"IR_Fft2dMagnitudeF64 failed: st={st} {err.value.decode()}")

        grade = {1: "A", 2: "B", 3: "C", 4: "D"}.get(gb.bad_pixels.chip_grade, "?")
        print(f"analysis_version={version}")
        print(f"gb_grade={grade}")
        print(f"gjb_bad_count={gjb.bad_pixels.total_bad_count}")
        print(f"noise3d_status={noise3d_status}")
        print(f"noise3d_total={scalars[7]:.12g}")
        print("lrnu_ok=1")
        print("fft_ok=1")
        print("gaussian_ok=1")
        print("flicker_ok=1")
        print("fft2d_ok=1")
        return 0
    finally:
        if dll_directory is not None:
            dll_directory.close()


if __name__ == "__main__":
    raise SystemExit(main())
