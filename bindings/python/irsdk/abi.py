"""Single-source ctypes mirror of the IR SDK pure C ABI.

This module is the only place that describes the ABI: enumerator values, public structs,
callbacks, function signatures, ``sizeof`` invariants and the ``(char *err, int32_t err_bytes)``
error-buffer convention.  It is a pure FFI declaration layer:

* standard library only (``ctypes`` / ``os`` / ``pathlib``), no numpy / Qt / json5;
* no hardware I/O, no threading, no business logic;
* every struct layout is asserted against the Windows x64 / LP64 sizes at import time on
  64-bit platforms, so a drift from ``include/IR_*.h`` fails loudly instead of corrupting
  memory.

Use :func:`load_sdk` to get both libraries with the versions reported by the DLLs themselves,
or :func:`load_station` / :func:`load_analysis` for a single library.
"""
from __future__ import annotations

import ctypes as c
import os
from pathlib import Path

# Binding/ABI gate identity: the binding refuses to run on a 32-bit interpreter and only
# accepts the SDK ``major.minor`` it was written against.
BINDING_VERSION = "1.8.0"
EXPECTED_SDK_MAJOR_MINOR = "1.8"

D = c.c_double
I = c.c_int32
I64 = c.c_int64
U8 = c.c_uint8
U16 = c.c_uint16
U32 = c.c_uint32
U64 = c.c_uint64
HANDLE = c.c_uint64
P = c.POINTER

# --------------------------------------------------------------------------- IR_status.h
IR_OK = 0
IR_NULL_POINTER = 1
IR_BAD_ARGUMENT = 2
IR_BUFFER_TOO_SMALL = 3
IR_COMPUTATION_FAILED = 4
IR_INVALID_HANDLE = 5
IR_IO_ERROR = 6
IR_TIMEOUT = 7
IR_CANCELLED = 8
IR_BUSY = 9
IR_UNSUPPORTED = 10
IR_END_OF_STREAM = 11
IR_FRAME_GAP = 12

STATUS_NAMES = {
    IR_OK: "IR_OK",
    IR_NULL_POINTER: "IR_NULL_POINTER",
    IR_BAD_ARGUMENT: "IR_BAD_ARGUMENT",
    IR_BUFFER_TOO_SMALL: "IR_BUFFER_TOO_SMALL",
    IR_COMPUTATION_FAILED: "IR_COMPUTATION_FAILED",
    IR_INVALID_HANDLE: "IR_INVALID_HANDLE",
    IR_IO_ERROR: "IR_IO_ERROR",
    IR_TIMEOUT: "IR_TIMEOUT",
    IR_CANCELLED: "IR_CANCELLED",
    IR_BUSY: "IR_BUSY",
    IR_UNSUPPORTED: "IR_UNSUPPORTED",
    IR_END_OF_STREAM: "IR_END_OF_STREAM",
    IR_FRAME_GAP: "IR_FRAME_GAP",
}

# --------------------------------------------------------------------------- IR_types.h
# IRFrameFields
IR_HAS_SEQUENCE = 1
IR_HAS_DRIVER_SEQUENCE = 2
IR_HAS_LOSS = 4
IR_SEQUENCE_IS_CALLBACK = 8
IR_PIXELS_INVERTED = 16

# IRTimeOrigin
IR_TIME_UNKNOWN = 0
IR_TIME_SOURCE = 1
IR_TIME_HOST = 2
IR_TIME_RECORDED_MONOTONIC = 3
IR_TIME_RECORDED_RELATIVE = 4

# IRDataType
IR_TYPE_U8 = 1
IR_TYPE_U16 = 2
IR_TYPE_U32 = 3
IR_TYPE_F64 = 4

# --------------------------------------------------------------------------- IR_station.h
# IRBackend
IR_DIRECTSHOW = 1
IR_RAW_REPLAY = 2
IR_V4L2 = 3
IR_NI_IMAQ = 4

# --------------------------------------------------------------------------- IR_device.h
# IRDeviceKind
IR_PCN7LT = 2
IR_DCN1000 = 3
IR_IT6332A = 4
IR_OVEN_GWS_STEN = 5
IR_DMM6500 = 6
IR_DETECTOR_SERIAL = 7
IR_COOLER_MODBUS = 8
IR_UDP6730 = 9
IR_EXTERNAL_POWER = 10

# IROvenMode
IR_OVEN_MODE_UNKNOWN = 0
IR_OVEN_MODE_STANDBY = 1
IR_OVEN_MODE_CONSTANT = 2
IR_OVEN_MODE_PROGRAM = 3
IR_OVEN_MODE_OTHER = 4

# IRDmmMode
IR_DMM_MODE_ANY = 0
IR_DMM_MODE_DCV = 1
IR_DMM_MODE_DCI = 2
IR_DMM_MODE_RES = 3
IR_DMM_MODE_DIODE = 4

DRIVER_KINDS = {
    "pcn7-lt": IR_PCN7LT,
    "dcn1000": IR_DCN1000,
    "it6332a": IR_IT6332A,
    "oven-gws-sten": IR_OVEN_GWS_STEN,
    "dmm6500": IR_DMM6500,
    "detector-serial": IR_DETECTOR_SERIAL,
    "cooler-modbus": IR_COOLER_MODBUS,
    "udp6730": IR_UDP6730,
    "external-power": IR_EXTERNAL_POWER,
}

# --------------------------------------------------------------------------- IR_recipe.h
# IRRecipeStage
IR_RECIPE_STAGE_NONE = 0
IR_RECIPE_STAGE_VALIDATE = 1
IR_RECIPE_STAGE_RESERVE = 2
IR_RECIPE_STAGE_PREPARE = 3
IR_RECIPE_STAGE_INSTRUMENT = 4
IR_RECIPE_STAGE_CAPTURE = 5
IR_RECIPE_STAGE_ANALYZE = 6
IR_RECIPE_STAGE_ARCHIVE = 7
IR_RECIPE_STAGE_CLEANUP = 8

# IRRecipeStepEventKind
IR_STEP_EVENT_STARTED = 1
IR_STEP_EVENT_COMPLETED = 2
IR_STEP_EVENT_FAILED = 3

# --------------------------------------------------------------------------- IR_plugin_abi.h
IR_PLUGIN_ABI_VERSION = 2
IR_PLUGIN_QUERY_SYMBOL = "ir_plugin_query"

# IRPluginSlotKind
IR_PLUGIN_SLOT_BATCH = 1
IR_PLUGIN_SLOT_DOUBLE_MAP = 2
IR_PLUGIN_SLOT_BYTE_MASK = 3
IR_PLUGIN_SLOT_DRIFT_CONTEXT = 4

# IRPluginLogLevel
IR_PLUGIN_LOG_DEBUG = 0
IR_PLUGIN_LOG_INFO = 1
IR_PLUGIN_LOG_WARN = 2
IR_PLUGIN_LOG_ERROR = 3

# --------------------------------------------------------------------------- IR_power_driver_abi.h
IR_POWER_DRIVER_ABI_VERSION = 1
IR_POWER_DRIVER_QUERY_SYMBOL = "ir_power_driver_query"

# IRPowerCapabilities
IR_POWER_CAP_READ = 1
IR_POWER_CAP_OUTPUT = 2
IR_POWER_CAP_SET = 4

# --------------------------------------------------------------------------- IR_analysis.h
# IRNoise3DMode
IR_NOISE3D_MODE_ORG = 0
IR_NOISE3D_MODE_NUC = 1

# IRNoise3DScalarIndex
IR_NOISE3D_SCALAR_DELTA_T = 0
IR_NOISE3D_SCALAR_DELTA_I = 1
IR_NOISE3D_SCALAR_DELTA_J = 2
IR_NOISE3D_SCALAR_DELTA_IJ = 3
IR_NOISE3D_SCALAR_DELTA_TI = 4
IR_NOISE3D_SCALAR_DELTA_TJ = 5
IR_NOISE3D_SCALAR_DELTA_TIJ = 6
IR_NOISE3D_SCALAR_DELTA_TOTAL = 7
IR_NOISE3D_SCALAR_DELTA_SYS = 8
IR_NOISE3D_SCALAR_GLOBAL_MEAN = 9
IR_NOISE3D_SCALAR_VALID_PIXEL_COUNT = 10
IR_NOISE3D_SCALAR_VALID_SAMPLE_COUNT = 11
IR_NOISE3D_SCALAR_FRAME_COUNT = 12
IR_NOISE3D_SCALAR_ROWS = 13
IR_NOISE3D_SCALAR_COLS = 14
IR_NOISE3D_SCALAR_RESERVED = 15
IR_NOISE3D_SCALAR_COUNT = 16

# IRNoise3DStatsIndex
IR_NOISE3D_STAT_RESPONSE_MEAN = 0
IR_NOISE3D_STAT_RESPONSE_LOW = 1
IR_NOISE3D_STAT_RESPONSE_HIGH = 2
IR_NOISE3D_STAT_RESPONSE_BLIND_COUNT = 3
IR_NOISE3D_STAT_RESPONSE_BLIND_RATIO = 4
IR_NOISE3D_STAT_RMS_MEAN = 5
IR_NOISE3D_STAT_RMS_LOW = 6
IR_NOISE3D_STAT_RMS_HIGH = 7
IR_NOISE3D_STAT_RMS_BLIND_COUNT = 8
IR_NOISE3D_STAT_RMS_BLIND_RATIO = 9
IR_NOISE3D_STAT_TOTAL_BLIND_COUNT = 10
IR_NOISE3D_STAT_TOTAL_BLIND_RATIO = 11
IR_NOISE3D_STAT_VALID_COUNT = 12
IR_NOISE3D_STAT_VALID_RATIO = 13
IR_NOISE3D_STAT_B_AVG = 14
IR_NOISE3D_STAT_K_MEAN_VALID = 15
IR_NOISE3D_STATS_COUNT = 32

# IRNoise3DHistGroup
IR_NOISE3D_HIST_T = 0
IR_NOISE3D_HIST_I = 1
IR_NOISE3D_HIST_J = 2
IR_NOISE3D_HIST_TIJ = 3
IR_NOISE3D_HIST_GROUP_COUNT = 4

# IRNoise3DMetaIndex
IR_NOISE3D_META_EFFECTIVE_THREAD_COUNT = 0
IR_NOISE3D_META_MASK_BUILD_SECONDS = 1
IR_NOISE3D_META_FIRST_PASS_SECONDS = 2
IR_NOISE3D_META_MEAN_BUILD_SECONDS = 3
IR_NOISE3D_META_TI_TJ_SECONDS = 4
IR_NOISE3D_META_STD_HIST_SECONDS = 5
IR_NOISE3D_META_TOTAL_SECONDS = 6
IR_NOISE3D_META_RESERVED = 7
IR_NOISE3D_META_COUNT = 32

# IRChipGrade
IR_CHIP_GRADE_UNKNOWN = 0
IR_CHIP_GRADE_A = 1
IR_CHIP_GRADE_B = 2
IR_CHIP_GRADE_C = 3
IR_CHIP_GRADE_D = 4

# IRGbRegion
IR_GB_REGION_A = 0
IR_GB_REGION_B = 1
IR_GB_REGION_C = 2
IR_GB_REGION_COUNT = 3

# IRGbClusterType
IR_GB_CLUSTER_O = 0
IR_GB_CLUSTER_I = 1
IR_GB_CLUSTER_II = 2
IR_GB_CLUSTER_III = 3
IR_GB_CLUSTER_IV = 4
IR_GB_CLUSTER_TYPE_COUNT = 5

# IRSpatialFilterKind
IR_SPATIAL_FILTER_BOX_MEAN = 0
IR_SPATIAL_FILTER_GAUSSIAN = 1

# IRMapBinaryOp
IR_MAP_OP_SUB = 0
IR_MAP_OP_ADD = 1
IR_MAP_OP_MUL = 2
IR_MAP_OP_SAFE_DIV = 3
IR_MAP_OP_VARIANCE_SUBTRACT = 4

# IRThresholdMode
IR_THRESH_ABSOLUTE = 0
IR_THRESH_MEAN_MULTIPLIER = 1
IR_THRESH_MEAN_SIGMA = 2
IR_THRESH_UPPER_ONLY = 3
IR_THRESH_LOWER_ONLY = 4

# IRMaskOp
IR_MASK_AND = 0
IR_MASK_OR = 1
IR_MASK_AND_NOT = 2
IR_MASK_NOT = 3

# IRWindowKind
IR_WINDOW_RECTANGULAR = 0
IR_WINDOW_HANN = 1
IR_WINDOW_HAMMING = 2

# IRSpectrumAxis
IR_SPECTRUM_AXIS_COLUMNS = 0
IR_SPECTRUM_AXIS_ROWS = 1

# IRNoiseSubtractMode
IR_NOISE_SUBTRACT_NONE = 0
IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE = 1


# --------------------------------------------------------------------------- IR_types.h structs
class IRGeometry(c.Structure):
    _fields_ = [("width", I), ("height", I), ("valid_bits", I)]


class IRFrameStamp(c.Structure):
    _fields_ = [("sequence", U64), ("driver_sequence", U64), ("timestamp_ns", c.c_int64),
                ("lost_frames", U32), ("fields", U32), ("time_origin", I), ("reserved", I)]


class IRArrayView(c.Structure):
    _fields_ = [("data", c.c_void_p), ("byte_size", U64), ("data_type", I),
                ("width", I), ("height", I), ("frames", I)]

    # ``data_type`` is the on-wire name; ``type`` keeps call sites readable.
    @property
    def type(self):
        return self.data_type

    @type.setter
    def type(self, value):
        self.data_type = int(value)


# ``IRMutableArrayView`` has exactly the same layout as ``IRArrayView`` (ctypes carries no
# const qualifier), so one class describes both the const input view and the mutable batch
# view; a second class would reject valid struct reuse.
IRMutableArrayView = IRArrayView


class IRFrameSourceInfo(c.Structure):
    _fields_ = [("geometry", IRGeometry), ("data_type", I)]

    @property
    def type(self):
        return self.data_type

    @type.setter
    def type(self, value):
        self.data_type = int(value)


class IRFrameBatchView(c.Structure):
    _fields_ = [("pixels", IRMutableArrayView), ("valid_bits", I),
                ("stamps", P(IRFrameStamp)), ("stamp_capacity", I)]

    @property
    def array(self):
        return self.pixels


# --------------------------------------------------------------------------- IR_station.h structs
class IRCaptureStats(c.Structure):
    _fields_ = [("frames_received", U64), ("frames_delivered", U64),
                ("frames_dropped", U64), ("preview_frames", U64),
                ("queue_capacity", I), ("queue_depth", I),
                ("width", I), ("height", I), ("running", I), ("reserved", I),
                ("estimated_fps", D)]


# --------------------------------------------------------------------------- IR_device.h structs
class IRBlackbodyReading(c.Structure):
    _fields_ = [("actual_c", D), ("setpoint_c", D), ("stable", I), ("reserved", I)]


class IRPowerReading(c.Structure):
    _fields_ = [("setpoint_v", D), ("current_limit_a", D), ("measured_v", D),
                ("measured_a", D), ("output_enabled", I), ("reserved", I)]


class IRPowerCurrentStableSpec(c.Structure):
    _fields_ = [("expected_voltage_v", D), ("voltage_tolerance_v", D),
                ("max_current_a", D), ("startup_threshold_a", D),
                ("stable_threshold_a", D), ("max_variation_a", D),
                ("startup_required_samples", I), ("startup_timeout_ms", I),
                ("stable_required_samples", I), ("sample_interval_ms", I),
                ("timeout_ms", I), ("reserved", I)]


class IROvenReading(c.Structure):
    _fields_ = [("actual_c", D), ("setpoint_c", D), ("mode", I), ("alarm_active", I)]


class IRDmmReading(c.Structure):
    _fields_ = [("value", D), ("mode", I), ("valid", I)]


class IRDetectorCurrentReading(c.Structure):
    _fields_ = [("raw_code", U32), ("reserved", U32),
                ("sense_voltage_v", D), ("current_ma", D)]


class IRCoolerReading(c.Structure):
    _fields_ = [("raw_value", U32), ("valid", I), ("voltage_mv", D)]


# --------------------------------------------------------------------------- IR_recipe.h structs
class IRRecipeResult(c.Structure):
    _fields_ = [("completed_frames", I), ("failed_stage", I),
                ("failed_step_index", I), ("cleanup_status", I)]


class IRRecipeStepEvent(c.Structure):
    _fields_ = [("struct_size", U32), ("event_kind", I), ("step_index", I),
                ("stage", I), ("status", I), ("reserved", I), ("duration_ms", D),
                ("step_id_utf8", c.c_char_p), ("step_type_utf8", c.c_char_p),
                ("detail_utf8", c.c_char_p)]


# IR_recipe.h: void (*IRRecipeStepCallback)(const IRRecipeStepEvent *, void *);
IRRecipeStepCallback = c.CFUNCTYPE(None, P(IRRecipeStepEvent), c.c_void_p)


class IRRecipeRunOptions(c.Structure):
    _fields_ = [("struct_size", U32), ("flags", U32),
                ("on_step_event", IRRecipeStepCallback), ("user_data", c.c_void_p)]


# --------------------------------------------------------------------------- IR_plugin_abi.h structs
class IRSlotView(c.Structure):
    _fields_ = [("slot_kind", I), ("array", IRArrayView), ("valid_bits", I),
                ("stamps", P(IRFrameStamp)), ("opaque_data", c.c_void_p),
                ("opaque_byte_size", U64)]


# Function-pointer fields use the matching CFUNCTYPE class so a Python callable can be
# assigned directly (a plain c_void_p field would reject CFunctionType instances).
IRPrepareSlotFn = c.CFUNCTYPE(I, c.c_void_p, c.c_char_p, I, I, P(I))
IRDeclareScalarFn = c.CFUNCTYPE(I, c.c_void_p, c.c_char_p, c.c_char_p)


class IRPrepareCallbacks(c.Structure):
    _fields_ = [("prepare_ctx", c.c_void_p),
                ("require_input_slot", IRPrepareSlotFn),
                ("bind_output_slot", IRPrepareSlotFn),
                ("declare_scalar_output", IRDeclareScalarFn)]


IRHostHasSlotFn = c.CFUNCTYPE(I, c.c_void_p, c.c_char_p)
IRHostGetSlotFn = c.CFUNCTYPE(I, c.c_void_p, c.c_char_p, P(IRSlotView))
IRHostAllocDoubleMapFn = c.CFUNCTYPE(I, c.c_void_p, c.c_char_p, I, I, P(P(D)))
IRHostAllocByteMaskFn = c.CFUNCTYPE(I, c.c_void_p, c.c_char_p, I, I, P(P(U8)))
IRHostEmitScalarFn = c.CFUNCTYPE(I, c.c_void_p, c.c_char_p, D)
IRHostIsCancelledFn = c.CFUNCTYPE(I, c.c_void_p)
IRHostLogFn = c.CFUNCTYPE(None, c.c_void_p, I, c.c_char_p)


class IRHostCallbacks(c.Structure):
    _fields_ = [("host_ctx", c.c_void_p),
                ("has_slot", IRHostHasSlotFn),
                ("get_slot", IRHostGetSlotFn),
                ("alloc_double_map", IRHostAllocDoubleMapFn),
                ("alloc_byte_mask", IRHostAllocByteMaskFn),
                ("emit_scalar", IRHostEmitScalarFn),
                ("is_cancelled", IRHostIsCancelledFn),
                ("log_message", IRHostLogFn)]


# Writable ``char *err`` / ``char *identity_utf8`` buffers are POINTER(c_char): a c_char_p
# parameter would copy the Python bytes object and never expose the SDK-written text.
IRPluginPrepareFn = c.CFUNCTYPE(I, P(IRPrepareCallbacks), c.c_char_p, P(c.c_char), I)
IRPluginExecuteFn = c.CFUNCTYPE(I, P(IRHostCallbacks), IRGeometry, c.c_char_p, P(c.c_char), I)


class IRMetricPlugin(c.Structure):
    _fields_ = [("abi_version", U32), ("plugin_id", c.c_char_p),
                ("method_name", c.c_char_p), ("default_input_key", c.c_char_p),
                ("prepare", IRPluginPrepareFn), ("execute", IRPluginExecuteFn)]


IRPluginQueryFn = c.CFUNCTYPE(I, P(P(IRMetricPlugin)), P(U32))


# --------------------------------------------------------------------------- IR_power_driver_abi.h
IRPowerDriverValidateConfigFn = c.CFUNCTYPE(I, c.c_char_p, P(c.c_char), I)
IRPowerDriverOpenFn = c.CFUNCTYPE(I, c.c_char_p, P(c.c_void_p), P(c.c_char), I,
                                  P(c.c_char), I)
IRPowerDriverReadFn = c.CFUNCTYPE(I, c.c_void_p, I, P(IRPowerReading), P(c.c_char), I)
IRPowerDriverSetOutputFn = c.CFUNCTYPE(I, c.c_void_p, I, I, P(IRPowerReading),
                                       P(c.c_char), I)
IRPowerDriverSetChannelFn = c.CFUNCTYPE(I, c.c_void_p, I, D, D, P(IRPowerReading),
                                        P(c.c_char), I)
IRPowerDriverCloseFn = c.CFUNCTYPE(I, c.c_void_p, P(c.c_char), I)


class IRPowerDriverDescriptor(c.Structure):
    _fields_ = [("abi_version", U32), ("struct_size", U32),
                ("driver_id", c.c_char_p), ("capabilities", U32),
                ("min_channel", I), ("max_channel", I),
                ("validate_config", IRPowerDriverValidateConfigFn),
                ("open", IRPowerDriverOpenFn),
                ("read", IRPowerDriverReadFn),
                ("set_output", IRPowerDriverSetOutputFn),
                ("set_channel", IRPowerDriverSetChannelFn),
                ("close", IRPowerDriverCloseFn)]


IRPowerDriverQueryFn = c.CFUNCTYPE(I, P(P(IRPowerDriverDescriptor)), P(c.c_char), I)


# --------------------------------------------------------------------------- IR_analysis.h structs
class IRCalcParameters(c.Structure):
    _fields_ = [("t0_c", D), ("t1_c", D), ("pixel_pitch_um", D), ("emissivity", D),
                ("system_gain", D), ("adc_swing_v", D), ("f_number", D),
                ("vmax_per_count", D), ("integration_time_us", I),
                ("invalid_integration_us", I), ("digital_full", I), ("reserved", I)]


class IRGbCriteria(c.Structure):
    _fields_ = [("response_upper", D), ("response_lower", D), ("noise_upper", D),
                ("netd_upper", D), ("short_bad_threshold", D), ("open_bad_threshold", D),
                ("cycle_upper", I), ("reserved", I)]


class IRGbBadPixelSummary(c.Structure):
    _fields_ = [("total_bad_rate_pct", D), ("response_bad_count", I),
                ("response_upper_bad_count", I), ("response_lower_bad_count", I),
                ("noise_bad_count", I), ("netd_bad_count", I), ("total_bad_count", I),
                ("iteration_count", I), ("chip_grade", I),
                ("cluster_counts", (I * IR_GB_CLUSTER_TYPE_COUNT) * IR_GB_REGION_COUNT),
                ("reserved", I)]


class IRGbBadPixelMasks(c.Structure):
    _fields_ = [("valid_mask", P(U8)), ("total_bad_mask", P(U8)),
                ("response_bad_mask", P(U8)), ("noise_bad_mask", P(U8)),
                ("netd_bad_mask", P(U8)), ("short_bad_mask", P(U8)),
                ("open_bad_mask", P(U8)), ("pixel_capacity", I64)]


class IRGbSummary(c.Structure):
    _fields_ = [("before_avg_pixel_response_count", D),
                ("before_avg_pixel_response_voltage", D), ("volts_per_count", D),
                ("before_avg_snr", D), ("irradiance_power_diff", D),
                ("before_avg_blackbody_response", D), ("before_avg_noise_voltage_v", D),
                ("before_avg_netd_mk", D), ("before_avg_detectivity", D),
                ("before_avg_responsivity", D), ("before_sitf", D),
                ("after_response_mv_per_ms_k", D), ("after_avg_response_voltage_v", D),
                ("after_avg_responsivity", D), ("after_avg_nep_w", D),
                ("after_response_nonuniformity_pct", D), ("after_avg_noise_voltage_mv", D),
                ("after_avg_netd_mk", D), ("after_avg_detectivity", D),
                ("short_bad_pixel_count", I), ("open_bad_pixel_count", I),
                ("bad_pixels", IRGbBadPixelSummary)]


class IRGjbCriteria(c.Structure):
    _fields_ = [("k1", D), ("k2", D), ("k3", D), ("k4", D),
                ("cycle_upper", I), ("reserved", I)]


class IRGjbClusterStats(c.Structure):
    _fields_ = [("count_1_to_6", I), ("count_7_to_12", I), ("count_13_to_19", I),
                ("count_20_to_35", I), ("count_gt_35", I)]


class IRGjbBadPixelSummary(c.Structure):
    _fields_ = [("total_bad_rate_pct", D), ("total_bad_count", I), ("iteration_count", I),
                ("center_3x3_block_count", I), ("filtered_4x4_block_count", I),
                ("total_clusters", IRGjbClusterStats), ("center_clusters", IRGjbClusterStats),
                ("edge_clusters", IRGjbClusterStats), ("reserved", I)]


class IRGjbBadPixelMasks(c.Structure):
    _fields_ = [("valid_mask", P(U8)), ("total_bad_mask", P(U8)), ("k1_bad_mask", P(U8)),
                ("k2_bad_mask", P(U8)), ("k3_bad_mask", P(U8)), ("k4_bad_mask", P(U8)),
                ("block_filtered_bad_mask", P(U8)), ("pixel_capacity", I64)]


class IRGjbSummary(c.Structure):
    _fields_ = [("before_avg_pixel_response_count", D),
                ("before_avg_pixel_response_voltage", D), ("volts_per_count", D),
                ("before_avg_snr", D), ("fov", D), ("irradiance_power_diff", D),
                ("before_avg_noise_voltage_v", D), ("before_avg_netd_mk", D),
                ("before_avg_detectivity", D), ("before_avg_responsivity", D),
                ("after_avg_response_voltage_v", D), ("after_avg_responsivity", D),
                ("after_avg_nep_w", D), ("after_response_nonuniformity_pct", D),
                ("after_avg_noise_voltage_mv", D), ("after_avg_netd_mk", D),
                ("after_avg_detectivity", D), ("after_spatial_noise", D),
                ("after_max_dynamic_range_db", D), ("after_rfpn_v", D),
                ("bad_pixels", IRGjbBadPixelSummary)]


class IRNoise3DCalibParams(c.Structure):
    _fields_ = [("response_lower_mul", D), ("response_upper_mul", D),
                ("rms_lower_mul", D), ("rms_upper_mul", D), ("k_epsilon", D)]


class IRNoise3DCalibOutputs(c.Structure):
    _fields_ = [("response_blind_mask", P(U8)), ("rms_blind_mask", P(U8)),
                ("total_blind_mask", P(U8)), ("valid_mask", P(U8)),
                ("k_map", P(D)), ("stats", P(D)),
                ("pixel_capacity", I64), ("stats_capacity", I), ("reserved", I)]


class IRNoise3DComputeParams(c.Structure):
    _fields_ = [("compute_mode", I), ("thread_count", I), ("hist_interval", I),
                ("reserved", I), ("b_avg", D), ("invalid_fill", D)]


class IRNoise3DComputeOutputs(c.Structure):
    _fields_ = [("scalars", P(D)), ("hist", P(D)), ("hist_meta", P(D)),
                ("map_ij", P(D)), ("map_ti", P(D)), ("map_tj", P(D)), ("meta", P(D)),
                ("hist_capacity", I64), ("map_ij_capacity", I64),
                ("map_ti_capacity", I64), ("map_tj_capacity", I64),
                ("scalars_capacity", I), ("hist_meta_capacity", I),
                ("meta_capacity", I), ("reserved", I)]


class IRMapSummary(c.Structure):
    _fields_ = [("mean", D), ("pop_std", D), ("sample_std", D), ("min_val", D),
                ("max_val", D), ("span", D), ("percentile_low", D),
                ("percentile_high", D), ("percentile_span", D), ("valid_count", I64)]


class IRGjbDriftContext(c.Structure):
    _fields_ = [("volts_per_count", D), ("delta_t_k", D), ("ref_response_count", D),
                ("ref_responsivity", D), ("vmax_per_count", D),
                ("noise_subtract_factor", D), ("burst_frame_count", I),
                ("noise_subtract_mode", I), ("lrnu_kernel_size", I), ("reserved", I)]


class IRGjbBurstMetrics(c.Structure):
    _fields_ = [("mean_dn", D), ("mean_v", D), ("span_dn", D), ("spatial_std_dn", D),
                ("spatial_std_v", D), ("spatial_std_denoised_dn", D),
                ("spatial_std_denoised_v", D), ("nonuniformity_pct", D),
                ("avg_temporal_noise_dn", D), ("avg_temporal_noise_mv", D),
                ("rms_temporal_noise_dn", D), ("estimated_netd_mk", D),
                ("estimated_nep_w", D), ("spatial_noise", D),
                ("max_dynamic_range_db", D), ("lrnu_std_dn", D), ("lrnu_span_dn", D),
                ("lrnu_pct", D), ("hrnu_std_dn", D), ("hrnu_pct", D),
                ("valid_pixel_count", I64)]


class IRFlickerSummary(c.Structure):
    _fields_ = [("std_dev_multiplier", D), ("flicker_ratio_pct", D),
                ("mean_frame_mean_dn", D), ("mean_frame_sigma_dn", D),
                ("exceed_count_threshold", I), ("frame_count", I),
                ("valid_pixel_count", I), ("flicker_count", I)]


class IRFft2dSummary(c.Structure):
    _fields_ = [("dc_magnitude", D), ("peak_ac_magnitude", D), ("total_ac_power", D),
                ("low_freq_power_ratio", D), ("high_freq_power_ratio", D),
                ("peak_ac_row", I), ("peak_ac_col", I), ("rows", I), ("cols", I),
                ("valid_pixel_count", I), ("reserved", I)]



# --------------------------------------------------------------------------- ABI invariants
# Windows x64 / LP64 sizes derived field-by-field from include/IR_*.h (all 41 public structs).
EXPECTED_SIZES = {
    "IRGeometry": 12,
    "IRFrameStamp": 40,
    "IRArrayView": 32,
    "IRMutableArrayView": 32,
    "IRFrameSourceInfo": 16,
    "IRFrameBatchView": 56,
    "IRCaptureStats": 64,
    "IRBlackbodyReading": 24,
    "IRPowerReading": 40,
    "IRPowerCurrentStableSpec": 72,
    "IROvenReading": 24,
    "IRDmmReading": 16,
    "IRDetectorCurrentReading": 24,
    "IRCoolerReading": 16,
    "IRRecipeResult": 16,
    "IRRecipeStepEvent": 56,
    "IRRecipeRunOptions": 24,
    "IRSlotView": 72,
    "IRPrepareCallbacks": 32,
    "IRHostCallbacks": 64,
    "IRMetricPlugin": 48,
    "IRPowerDriverDescriptor": 80,
    "IRCalcParameters": 80,
    "IRGbCriteria": 56,
    "IRGbBadPixelSummary": 104,
    "IRGbBadPixelMasks": 64,
    "IRGbSummary": 264,
    "IRGjbCriteria": 40,
    "IRGjbClusterStats": 20,
    "IRGjbBadPixelSummary": 88,
    "IRGjbBadPixelMasks": 64,
    "IRGjbSummary": 248,
    "IRNoise3DCalibParams": 40,
    "IRNoise3DCalibOutputs": 64,
    "IRNoise3DComputeParams": 32,
    "IRNoise3DComputeOutputs": 104,
    "IRMapSummary": 80,
    "IRGjbDriftContext": 64,
    "IRGjbBurstMetrics": 168,
    "IRFlickerSummary": 48,
    "IRFft2dSummary": 64,
}


def verify_struct_sizes():
    """Return ``{name: sizeof}`` after asserting every struct matches the C ABI."""
    mismatch = []
    sizes = {}
    for name, expected in EXPECTED_SIZES.items():
        actual = c.sizeof(globals()[name])
        sizes[name] = actual
        if actual != expected:
            mismatch.append(f"{name}: ctypes.sizeof={actual} expected {expected}")
    if mismatch:
        raise RuntimeError("IR SDK ctypes structs do not match the C ABI: " + "; ".join(mismatch))
    return sizes


# Every layout below is derived for 64-bit pointers (Windows x64 / LP64); a 32-bit
# interpreter would silently missize every struct, so fail loudly instead.
if c.sizeof(c.c_void_p) != 8:
    raise RuntimeError(
        f"irsdk requires 64-bit Python (pointer size={c.sizeof(c.c_void_p)} bytes)")

STRUCT_SIZES = verify_struct_sizes()


def array_view(data, data_type, width, height, frames=1, byte_size=None):
    """Build an :class:`IRArrayView` over caller-owned memory.

    ``data`` may be a ctypes array, a ``ctypes.c_void_p`` or a pointer/address; the view only
    borrows the buffer, exactly like the C ABI. ``byte_size`` is inferred only for ctypes
    arrays; raw pointers/addresses must pass it explicitly (a wrong size would let the SDK
    write out of bounds).
    """
    if isinstance(data, c.Array):
        if byte_size is None:
            byte_size = c.sizeof(data)
        pointer = c.cast(data, c.c_void_p)
    else:
        if byte_size is None:
            raise ValueError("byte_size is required for raw pointers")
        pointer = data if isinstance(data, c.c_void_p) else c.cast(data, c.c_void_p)
    return IRArrayView(pointer, byte_size, data_type, width, height, frames)


# Public surface: every ctypes struct, enumerator, callback and helper defined above.
# ``loader`` adds the signatures and the library-loading helpers on top of this module.
__all__ = [name for name in globals()
           if not name.startswith("_") and name not in ("c", "os", "Path", "annotations")]
