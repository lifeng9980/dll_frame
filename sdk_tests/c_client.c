#include "IR_station.h"
#include "IR_analysis.h"
#include "IR_device.h"
#include "IR_recipe.h"
#include "IR_plugin_abi.h"
#include "IR_power_driver_abi.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Compiler-verified ABI gate: size, alignment and every field offset of the 41 public
   structs declared in include/IR_*.h. ``--dump-abi-layout`` prints the full layout;
   ``--dump-abi-sizes`` prints the flat {name: sizeof} map produced by the same walk. */
#include <stddef.h>

/* offsetof(struct { char pad; T value; }, value) == alignof(T) on every supported ABI. */
#define IR_ALIGN_PROBE(T) struct IRAlign_##T { char pad; T value; };
IR_ALIGN_PROBE(IRGeometry)
IR_ALIGN_PROBE(IRFrameStamp)
IR_ALIGN_PROBE(IRArrayView)
IR_ALIGN_PROBE(IRMutableArrayView)
IR_ALIGN_PROBE(IRFrameSourceInfo)
IR_ALIGN_PROBE(IRFrameBatchView)
IR_ALIGN_PROBE(IRCaptureStats)
IR_ALIGN_PROBE(IRBlackbodyReading)
IR_ALIGN_PROBE(IRPowerReading)
IR_ALIGN_PROBE(IRPowerCurrentStableSpec)
IR_ALIGN_PROBE(IROvenReading)
IR_ALIGN_PROBE(IRDmmReading)
IR_ALIGN_PROBE(IRDetectorCurrentReading)
IR_ALIGN_PROBE(IRCoolerReading)
IR_ALIGN_PROBE(IRRecipeResult)
IR_ALIGN_PROBE(IRRecipeStepEvent)
IR_ALIGN_PROBE(IRRecipeRunOptions)
IR_ALIGN_PROBE(IRSlotView)
IR_ALIGN_PROBE(IRPrepareCallbacks)
IR_ALIGN_PROBE(IRHostCallbacks)
IR_ALIGN_PROBE(IRMetricPlugin)
IR_ALIGN_PROBE(IRPowerDriverDescriptor)
IR_ALIGN_PROBE(IRCalcParameters)
IR_ALIGN_PROBE(IRGbCriteria)
IR_ALIGN_PROBE(IRGbBadPixelSummary)
IR_ALIGN_PROBE(IRGbBadPixelMasks)
IR_ALIGN_PROBE(IRGbSummary)
IR_ALIGN_PROBE(IRGjbCriteria)
IR_ALIGN_PROBE(IRGjbClusterStats)
IR_ALIGN_PROBE(IRGjbBadPixelSummary)
IR_ALIGN_PROBE(IRGjbBadPixelMasks)
IR_ALIGN_PROBE(IRGjbSummary)
IR_ALIGN_PROBE(IRNoise3DCalibParams)
IR_ALIGN_PROBE(IRNoise3DCalibOutputs)
IR_ALIGN_PROBE(IRNoise3DComputeParams)
IR_ALIGN_PROBE(IRNoise3DComputeOutputs)
IR_ALIGN_PROBE(IRMapSummary)
IR_ALIGN_PROBE(IRGjbDriftContext)
IR_ALIGN_PROBE(IRGjbBurstMetrics)
IR_ALIGN_PROBE(IRFlickerSummary)
IR_ALIGN_PROBE(IRFft2dSummary)
#undef IR_ALIGN_PROBE

#define IR_ALIGNOF(T) offsetof(struct IRAlign_##T, value)

static int irAbiLayout = 1;
static int irAbiFirstStruct = 1;
static int irAbiFirstField = 1;

static void irAbiBeginStruct(const char *name, size_t size, size_t alignment)
{
    if (!irAbiLayout) {
        printf("%s  \"%s\": %zu", irAbiFirstStruct ? "" : ",\n", name, size);
        irAbiFirstStruct = 0;
        return;
    }
    printf("%s  \"%s\": {\n    \"size\": %zu,\n    \"alignment\": %zu,\n    \"fields\": {",
        irAbiFirstStruct ? "" : ",\n", name, size, alignment);
    irAbiFirstStruct = 0;
    irAbiFirstField = 1;
}

static void irAbiField(const char *name, size_t offset)
{
    if (!irAbiLayout)
        return;
    printf("%s\"%s\": %zu", irAbiFirstField ? "" : ", ", name, offset);
    irAbiFirstField = 0;
}

static void irAbiEndStruct(void)
{
    if (irAbiLayout)
        printf("}\n  }");
}

#define IR_FIELD(T, field) irAbiField(#field, offsetof(T, field))

static void irAbiDump(int layout)
{
    irAbiLayout = layout;
    irAbiFirstStruct = 1;
    irAbiFirstField = 1;
    printf("{\n");
    irAbiBeginStruct("IRGeometry", sizeof(IRGeometry), IR_ALIGNOF(IRGeometry));
    IR_FIELD(IRGeometry, width);
    IR_FIELD(IRGeometry, height);
    IR_FIELD(IRGeometry, valid_bits);
    irAbiEndStruct();
    irAbiBeginStruct("IRFrameStamp", sizeof(IRFrameStamp), IR_ALIGNOF(IRFrameStamp));
    IR_FIELD(IRFrameStamp, sequence);
    IR_FIELD(IRFrameStamp, driver_sequence);
    IR_FIELD(IRFrameStamp, timestamp_ns);
    IR_FIELD(IRFrameStamp, lost_frames);
    IR_FIELD(IRFrameStamp, fields);
    IR_FIELD(IRFrameStamp, time_origin);
    IR_FIELD(IRFrameStamp, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRArrayView", sizeof(IRArrayView), IR_ALIGNOF(IRArrayView));
    IR_FIELD(IRArrayView, data);
    IR_FIELD(IRArrayView, byte_size);
    IR_FIELD(IRArrayView, data_type);
    IR_FIELD(IRArrayView, width);
    IR_FIELD(IRArrayView, height);
    IR_FIELD(IRArrayView, frames);
    irAbiEndStruct();
    irAbiBeginStruct("IRMutableArrayView", sizeof(IRMutableArrayView), IR_ALIGNOF(IRMutableArrayView));
    IR_FIELD(IRMutableArrayView, data);
    IR_FIELD(IRMutableArrayView, byte_size);
    IR_FIELD(IRMutableArrayView, data_type);
    IR_FIELD(IRMutableArrayView, width);
    IR_FIELD(IRMutableArrayView, height);
    IR_FIELD(IRMutableArrayView, frames);
    irAbiEndStruct();
    irAbiBeginStruct("IRFrameSourceInfo", sizeof(IRFrameSourceInfo), IR_ALIGNOF(IRFrameSourceInfo));
    IR_FIELD(IRFrameSourceInfo, geometry);
    IR_FIELD(IRFrameSourceInfo, data_type);
    irAbiEndStruct();
    irAbiBeginStruct("IRFrameBatchView", sizeof(IRFrameBatchView), IR_ALIGNOF(IRFrameBatchView));
    IR_FIELD(IRFrameBatchView, pixels);
    IR_FIELD(IRFrameBatchView, valid_bits);
    IR_FIELD(IRFrameBatchView, stamps);
    IR_FIELD(IRFrameBatchView, stamp_capacity);
    irAbiEndStruct();
    irAbiBeginStruct("IRCaptureStats", sizeof(IRCaptureStats), IR_ALIGNOF(IRCaptureStats));
    IR_FIELD(IRCaptureStats, frames_received);
    IR_FIELD(IRCaptureStats, frames_delivered);
    IR_FIELD(IRCaptureStats, frames_dropped);
    IR_FIELD(IRCaptureStats, preview_frames);
    IR_FIELD(IRCaptureStats, queue_capacity);
    IR_FIELD(IRCaptureStats, queue_depth);
    IR_FIELD(IRCaptureStats, width);
    IR_FIELD(IRCaptureStats, height);
    IR_FIELD(IRCaptureStats, running);
    IR_FIELD(IRCaptureStats, reserved);
    IR_FIELD(IRCaptureStats, estimated_fps);
    irAbiEndStruct();
    irAbiBeginStruct("IRBlackbodyReading", sizeof(IRBlackbodyReading), IR_ALIGNOF(IRBlackbodyReading));
    IR_FIELD(IRBlackbodyReading, actual_c);
    IR_FIELD(IRBlackbodyReading, setpoint_c);
    IR_FIELD(IRBlackbodyReading, stable);
    IR_FIELD(IRBlackbodyReading, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRPowerReading", sizeof(IRPowerReading), IR_ALIGNOF(IRPowerReading));
    IR_FIELD(IRPowerReading, setpoint_v);
    IR_FIELD(IRPowerReading, current_limit_a);
    IR_FIELD(IRPowerReading, measured_v);
    IR_FIELD(IRPowerReading, measured_a);
    IR_FIELD(IRPowerReading, output_enabled);
    IR_FIELD(IRPowerReading, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRPowerCurrentStableSpec", sizeof(IRPowerCurrentStableSpec), IR_ALIGNOF(IRPowerCurrentStableSpec));
    IR_FIELD(IRPowerCurrentStableSpec, expected_voltage_v);
    IR_FIELD(IRPowerCurrentStableSpec, voltage_tolerance_v);
    IR_FIELD(IRPowerCurrentStableSpec, max_current_a);
    IR_FIELD(IRPowerCurrentStableSpec, startup_threshold_a);
    IR_FIELD(IRPowerCurrentStableSpec, stable_threshold_a);
    IR_FIELD(IRPowerCurrentStableSpec, max_variation_a);
    IR_FIELD(IRPowerCurrentStableSpec, startup_required_samples);
    IR_FIELD(IRPowerCurrentStableSpec, startup_timeout_ms);
    IR_FIELD(IRPowerCurrentStableSpec, stable_required_samples);
    IR_FIELD(IRPowerCurrentStableSpec, sample_interval_ms);
    IR_FIELD(IRPowerCurrentStableSpec, timeout_ms);
    IR_FIELD(IRPowerCurrentStableSpec, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IROvenReading", sizeof(IROvenReading), IR_ALIGNOF(IROvenReading));
    IR_FIELD(IROvenReading, actual_c);
    IR_FIELD(IROvenReading, setpoint_c);
    IR_FIELD(IROvenReading, mode);
    IR_FIELD(IROvenReading, alarm_active);
    irAbiEndStruct();
    irAbiBeginStruct("IRDmmReading", sizeof(IRDmmReading), IR_ALIGNOF(IRDmmReading));
    IR_FIELD(IRDmmReading, value);
    IR_FIELD(IRDmmReading, mode);
    IR_FIELD(IRDmmReading, valid);
    irAbiEndStruct();
    irAbiBeginStruct("IRDetectorCurrentReading", sizeof(IRDetectorCurrentReading), IR_ALIGNOF(IRDetectorCurrentReading));
    IR_FIELD(IRDetectorCurrentReading, raw_code);
    IR_FIELD(IRDetectorCurrentReading, reserved);
    IR_FIELD(IRDetectorCurrentReading, sense_voltage_v);
    IR_FIELD(IRDetectorCurrentReading, current_ma);
    irAbiEndStruct();
    irAbiBeginStruct("IRCoolerReading", sizeof(IRCoolerReading), IR_ALIGNOF(IRCoolerReading));
    IR_FIELD(IRCoolerReading, raw_value);
    IR_FIELD(IRCoolerReading, valid);
    IR_FIELD(IRCoolerReading, voltage_mv);
    irAbiEndStruct();
    irAbiBeginStruct("IRRecipeResult", sizeof(IRRecipeResult), IR_ALIGNOF(IRRecipeResult));
    IR_FIELD(IRRecipeResult, completed_frames);
    IR_FIELD(IRRecipeResult, failed_stage);
    IR_FIELD(IRRecipeResult, failed_step_index);
    IR_FIELD(IRRecipeResult, cleanup_status);
    irAbiEndStruct();
    irAbiBeginStruct("IRRecipeStepEvent", sizeof(IRRecipeStepEvent), IR_ALIGNOF(IRRecipeStepEvent));
    IR_FIELD(IRRecipeStepEvent, struct_size);
    IR_FIELD(IRRecipeStepEvent, event_kind);
    IR_FIELD(IRRecipeStepEvent, step_index);
    IR_FIELD(IRRecipeStepEvent, stage);
    IR_FIELD(IRRecipeStepEvent, status);
    IR_FIELD(IRRecipeStepEvent, reserved);
    IR_FIELD(IRRecipeStepEvent, duration_ms);
    IR_FIELD(IRRecipeStepEvent, step_id_utf8);
    IR_FIELD(IRRecipeStepEvent, step_type_utf8);
    IR_FIELD(IRRecipeStepEvent, detail_utf8);
    irAbiEndStruct();
    irAbiBeginStruct("IRRecipeRunOptions", sizeof(IRRecipeRunOptions), IR_ALIGNOF(IRRecipeRunOptions));
    IR_FIELD(IRRecipeRunOptions, struct_size);
    IR_FIELD(IRRecipeRunOptions, flags);
    IR_FIELD(IRRecipeRunOptions, on_step_event);
    IR_FIELD(IRRecipeRunOptions, user_data);
    irAbiEndStruct();
    irAbiBeginStruct("IRSlotView", sizeof(IRSlotView), IR_ALIGNOF(IRSlotView));
    IR_FIELD(IRSlotView, slot_kind);
    IR_FIELD(IRSlotView, array);
    IR_FIELD(IRSlotView, valid_bits);
    IR_FIELD(IRSlotView, stamps);
    IR_FIELD(IRSlotView, opaque_data);
    IR_FIELD(IRSlotView, opaque_byte_size);
    irAbiEndStruct();
    irAbiBeginStruct("IRPrepareCallbacks", sizeof(IRPrepareCallbacks), IR_ALIGNOF(IRPrepareCallbacks));
    IR_FIELD(IRPrepareCallbacks, prepare_ctx);
    IR_FIELD(IRPrepareCallbacks, require_input_slot);
    IR_FIELD(IRPrepareCallbacks, bind_output_slot);
    IR_FIELD(IRPrepareCallbacks, declare_scalar_output);
    irAbiEndStruct();
    irAbiBeginStruct("IRHostCallbacks", sizeof(IRHostCallbacks), IR_ALIGNOF(IRHostCallbacks));
    IR_FIELD(IRHostCallbacks, host_ctx);
    IR_FIELD(IRHostCallbacks, has_slot);
    IR_FIELD(IRHostCallbacks, get_slot);
    IR_FIELD(IRHostCallbacks, alloc_double_map);
    IR_FIELD(IRHostCallbacks, alloc_byte_mask);
    IR_FIELD(IRHostCallbacks, emit_scalar);
    IR_FIELD(IRHostCallbacks, is_cancelled);
    IR_FIELD(IRHostCallbacks, log_message);
    irAbiEndStruct();
    irAbiBeginStruct("IRMetricPlugin", sizeof(IRMetricPlugin), IR_ALIGNOF(IRMetricPlugin));
    IR_FIELD(IRMetricPlugin, abi_version);
    IR_FIELD(IRMetricPlugin, plugin_id);
    IR_FIELD(IRMetricPlugin, method_name);
    IR_FIELD(IRMetricPlugin, default_input_key);
    IR_FIELD(IRMetricPlugin, prepare);
    IR_FIELD(IRMetricPlugin, execute);
    irAbiEndStruct();
    irAbiBeginStruct("IRPowerDriverDescriptor", sizeof(IRPowerDriverDescriptor), IR_ALIGNOF(IRPowerDriverDescriptor));
    IR_FIELD(IRPowerDriverDescriptor, abi_version);
    IR_FIELD(IRPowerDriverDescriptor, struct_size);
    IR_FIELD(IRPowerDriverDescriptor, driver_id);
    IR_FIELD(IRPowerDriverDescriptor, capabilities);
    IR_FIELD(IRPowerDriverDescriptor, min_channel);
    IR_FIELD(IRPowerDriverDescriptor, max_channel);
    IR_FIELD(IRPowerDriverDescriptor, validate_config);
    IR_FIELD(IRPowerDriverDescriptor, open);
    IR_FIELD(IRPowerDriverDescriptor, read);
    IR_FIELD(IRPowerDriverDescriptor, set_output);
    IR_FIELD(IRPowerDriverDescriptor, set_channel);
    IR_FIELD(IRPowerDriverDescriptor, close);
    irAbiEndStruct();
    irAbiBeginStruct("IRCalcParameters", sizeof(IRCalcParameters), IR_ALIGNOF(IRCalcParameters));
    IR_FIELD(IRCalcParameters, t0_c);
    IR_FIELD(IRCalcParameters, t1_c);
    IR_FIELD(IRCalcParameters, pixel_pitch_um);
    IR_FIELD(IRCalcParameters, emissivity);
    IR_FIELD(IRCalcParameters, system_gain);
    IR_FIELD(IRCalcParameters, adc_swing_v);
    IR_FIELD(IRCalcParameters, f_number);
    IR_FIELD(IRCalcParameters, vmax_per_count);
    IR_FIELD(IRCalcParameters, integration_time_us);
    IR_FIELD(IRCalcParameters, invalid_integration_us);
    IR_FIELD(IRCalcParameters, digital_full);
    IR_FIELD(IRCalcParameters, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRGbCriteria", sizeof(IRGbCriteria), IR_ALIGNOF(IRGbCriteria));
    IR_FIELD(IRGbCriteria, response_upper);
    IR_FIELD(IRGbCriteria, response_lower);
    IR_FIELD(IRGbCriteria, noise_upper);
    IR_FIELD(IRGbCriteria, netd_upper);
    IR_FIELD(IRGbCriteria, short_bad_threshold);
    IR_FIELD(IRGbCriteria, open_bad_threshold);
    IR_FIELD(IRGbCriteria, cycle_upper);
    IR_FIELD(IRGbCriteria, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRGbBadPixelSummary", sizeof(IRGbBadPixelSummary), IR_ALIGNOF(IRGbBadPixelSummary));
    IR_FIELD(IRGbBadPixelSummary, total_bad_rate_pct);
    IR_FIELD(IRGbBadPixelSummary, response_bad_count);
    IR_FIELD(IRGbBadPixelSummary, response_upper_bad_count);
    IR_FIELD(IRGbBadPixelSummary, response_lower_bad_count);
    IR_FIELD(IRGbBadPixelSummary, noise_bad_count);
    IR_FIELD(IRGbBadPixelSummary, netd_bad_count);
    IR_FIELD(IRGbBadPixelSummary, total_bad_count);
    IR_FIELD(IRGbBadPixelSummary, iteration_count);
    IR_FIELD(IRGbBadPixelSummary, chip_grade);
    IR_FIELD(IRGbBadPixelSummary, cluster_counts);
    IR_FIELD(IRGbBadPixelSummary, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRGbBadPixelMasks", sizeof(IRGbBadPixelMasks), IR_ALIGNOF(IRGbBadPixelMasks));
    IR_FIELD(IRGbBadPixelMasks, valid_mask);
    IR_FIELD(IRGbBadPixelMasks, total_bad_mask);
    IR_FIELD(IRGbBadPixelMasks, response_bad_mask);
    IR_FIELD(IRGbBadPixelMasks, noise_bad_mask);
    IR_FIELD(IRGbBadPixelMasks, netd_bad_mask);
    IR_FIELD(IRGbBadPixelMasks, short_bad_mask);
    IR_FIELD(IRGbBadPixelMasks, open_bad_mask);
    IR_FIELD(IRGbBadPixelMasks, pixel_capacity);
    irAbiEndStruct();
    irAbiBeginStruct("IRGbSummary", sizeof(IRGbSummary), IR_ALIGNOF(IRGbSummary));
    IR_FIELD(IRGbSummary, before_avg_pixel_response_count);
    IR_FIELD(IRGbSummary, before_avg_pixel_response_voltage);
    IR_FIELD(IRGbSummary, volts_per_count);
    IR_FIELD(IRGbSummary, before_avg_snr);
    IR_FIELD(IRGbSummary, irradiance_power_diff);
    IR_FIELD(IRGbSummary, before_avg_blackbody_response);
    IR_FIELD(IRGbSummary, before_avg_noise_voltage_v);
    IR_FIELD(IRGbSummary, before_avg_netd_mk);
    IR_FIELD(IRGbSummary, before_avg_detectivity);
    IR_FIELD(IRGbSummary, before_avg_responsivity);
    IR_FIELD(IRGbSummary, before_sitf);
    IR_FIELD(IRGbSummary, after_response_mv_per_ms_k);
    IR_FIELD(IRGbSummary, after_avg_response_voltage_v);
    IR_FIELD(IRGbSummary, after_avg_responsivity);
    IR_FIELD(IRGbSummary, after_avg_nep_w);
    IR_FIELD(IRGbSummary, after_response_nonuniformity_pct);
    IR_FIELD(IRGbSummary, after_avg_noise_voltage_mv);
    IR_FIELD(IRGbSummary, after_avg_netd_mk);
    IR_FIELD(IRGbSummary, after_avg_detectivity);
    IR_FIELD(IRGbSummary, short_bad_pixel_count);
    IR_FIELD(IRGbSummary, open_bad_pixel_count);
    IR_FIELD(IRGbSummary, bad_pixels);
    irAbiEndStruct();
    irAbiBeginStruct("IRGjbCriteria", sizeof(IRGjbCriteria), IR_ALIGNOF(IRGjbCriteria));
    IR_FIELD(IRGjbCriteria, k1);
    IR_FIELD(IRGjbCriteria, k2);
    IR_FIELD(IRGjbCriteria, k3);
    IR_FIELD(IRGjbCriteria, k4);
    IR_FIELD(IRGjbCriteria, cycle_upper);
    IR_FIELD(IRGjbCriteria, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRGjbClusterStats", sizeof(IRGjbClusterStats), IR_ALIGNOF(IRGjbClusterStats));
    IR_FIELD(IRGjbClusterStats, count_1_to_6);
    IR_FIELD(IRGjbClusterStats, count_7_to_12);
    IR_FIELD(IRGjbClusterStats, count_13_to_19);
    IR_FIELD(IRGjbClusterStats, count_20_to_35);
    IR_FIELD(IRGjbClusterStats, count_gt_35);
    irAbiEndStruct();
    irAbiBeginStruct("IRGjbBadPixelSummary", sizeof(IRGjbBadPixelSummary), IR_ALIGNOF(IRGjbBadPixelSummary));
    IR_FIELD(IRGjbBadPixelSummary, total_bad_rate_pct);
    IR_FIELD(IRGjbBadPixelSummary, total_bad_count);
    IR_FIELD(IRGjbBadPixelSummary, iteration_count);
    IR_FIELD(IRGjbBadPixelSummary, center_3x3_block_count);
    IR_FIELD(IRGjbBadPixelSummary, filtered_4x4_block_count);
    IR_FIELD(IRGjbBadPixelSummary, total_clusters);
    IR_FIELD(IRGjbBadPixelSummary, center_clusters);
    IR_FIELD(IRGjbBadPixelSummary, edge_clusters);
    IR_FIELD(IRGjbBadPixelSummary, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRGjbBadPixelMasks", sizeof(IRGjbBadPixelMasks), IR_ALIGNOF(IRGjbBadPixelMasks));
    IR_FIELD(IRGjbBadPixelMasks, valid_mask);
    IR_FIELD(IRGjbBadPixelMasks, total_bad_mask);
    IR_FIELD(IRGjbBadPixelMasks, k1_bad_mask);
    IR_FIELD(IRGjbBadPixelMasks, k2_bad_mask);
    IR_FIELD(IRGjbBadPixelMasks, k3_bad_mask);
    IR_FIELD(IRGjbBadPixelMasks, k4_bad_mask);
    IR_FIELD(IRGjbBadPixelMasks, block_filtered_bad_mask);
    IR_FIELD(IRGjbBadPixelMasks, pixel_capacity);
    irAbiEndStruct();
    irAbiBeginStruct("IRGjbSummary", sizeof(IRGjbSummary), IR_ALIGNOF(IRGjbSummary));
    IR_FIELD(IRGjbSummary, before_avg_pixel_response_count);
    IR_FIELD(IRGjbSummary, before_avg_pixel_response_voltage);
    IR_FIELD(IRGjbSummary, volts_per_count);
    IR_FIELD(IRGjbSummary, before_avg_snr);
    IR_FIELD(IRGjbSummary, fov);
    IR_FIELD(IRGjbSummary, irradiance_power_diff);
    IR_FIELD(IRGjbSummary, before_avg_noise_voltage_v);
    IR_FIELD(IRGjbSummary, before_avg_netd_mk);
    IR_FIELD(IRGjbSummary, before_avg_detectivity);
    IR_FIELD(IRGjbSummary, before_avg_responsivity);
    IR_FIELD(IRGjbSummary, after_avg_response_voltage_v);
    IR_FIELD(IRGjbSummary, after_avg_responsivity);
    IR_FIELD(IRGjbSummary, after_avg_nep_w);
    IR_FIELD(IRGjbSummary, after_response_nonuniformity_pct);
    IR_FIELD(IRGjbSummary, after_avg_noise_voltage_mv);
    IR_FIELD(IRGjbSummary, after_avg_netd_mk);
    IR_FIELD(IRGjbSummary, after_avg_detectivity);
    IR_FIELD(IRGjbSummary, after_spatial_noise);
    IR_FIELD(IRGjbSummary, after_max_dynamic_range_db);
    IR_FIELD(IRGjbSummary, after_rfpn_v);
    IR_FIELD(IRGjbSummary, bad_pixels);
    irAbiEndStruct();
    irAbiBeginStruct("IRNoise3DCalibParams", sizeof(IRNoise3DCalibParams), IR_ALIGNOF(IRNoise3DCalibParams));
    IR_FIELD(IRNoise3DCalibParams, response_lower_mul);
    IR_FIELD(IRNoise3DCalibParams, response_upper_mul);
    IR_FIELD(IRNoise3DCalibParams, rms_lower_mul);
    IR_FIELD(IRNoise3DCalibParams, rms_upper_mul);
    IR_FIELD(IRNoise3DCalibParams, k_epsilon);
    irAbiEndStruct();
    irAbiBeginStruct("IRNoise3DCalibOutputs", sizeof(IRNoise3DCalibOutputs), IR_ALIGNOF(IRNoise3DCalibOutputs));
    IR_FIELD(IRNoise3DCalibOutputs, response_blind_mask);
    IR_FIELD(IRNoise3DCalibOutputs, rms_blind_mask);
    IR_FIELD(IRNoise3DCalibOutputs, total_blind_mask);
    IR_FIELD(IRNoise3DCalibOutputs, valid_mask);
    IR_FIELD(IRNoise3DCalibOutputs, k_map);
    IR_FIELD(IRNoise3DCalibOutputs, stats);
    IR_FIELD(IRNoise3DCalibOutputs, pixel_capacity);
    IR_FIELD(IRNoise3DCalibOutputs, stats_capacity);
    IR_FIELD(IRNoise3DCalibOutputs, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRNoise3DComputeParams", sizeof(IRNoise3DComputeParams), IR_ALIGNOF(IRNoise3DComputeParams));
    IR_FIELD(IRNoise3DComputeParams, compute_mode);
    IR_FIELD(IRNoise3DComputeParams, thread_count);
    IR_FIELD(IRNoise3DComputeParams, hist_interval);
    IR_FIELD(IRNoise3DComputeParams, reserved);
    IR_FIELD(IRNoise3DComputeParams, b_avg);
    IR_FIELD(IRNoise3DComputeParams, invalid_fill);
    irAbiEndStruct();
    irAbiBeginStruct("IRNoise3DComputeOutputs", sizeof(IRNoise3DComputeOutputs), IR_ALIGNOF(IRNoise3DComputeOutputs));
    IR_FIELD(IRNoise3DComputeOutputs, scalars);
    IR_FIELD(IRNoise3DComputeOutputs, hist);
    IR_FIELD(IRNoise3DComputeOutputs, hist_meta);
    IR_FIELD(IRNoise3DComputeOutputs, map_ij);
    IR_FIELD(IRNoise3DComputeOutputs, map_ti);
    IR_FIELD(IRNoise3DComputeOutputs, map_tj);
    IR_FIELD(IRNoise3DComputeOutputs, meta);
    IR_FIELD(IRNoise3DComputeOutputs, hist_capacity);
    IR_FIELD(IRNoise3DComputeOutputs, map_ij_capacity);
    IR_FIELD(IRNoise3DComputeOutputs, map_ti_capacity);
    IR_FIELD(IRNoise3DComputeOutputs, map_tj_capacity);
    IR_FIELD(IRNoise3DComputeOutputs, scalars_capacity);
    IR_FIELD(IRNoise3DComputeOutputs, hist_meta_capacity);
    IR_FIELD(IRNoise3DComputeOutputs, meta_capacity);
    IR_FIELD(IRNoise3DComputeOutputs, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRMapSummary", sizeof(IRMapSummary), IR_ALIGNOF(IRMapSummary));
    IR_FIELD(IRMapSummary, mean);
    IR_FIELD(IRMapSummary, pop_std);
    IR_FIELD(IRMapSummary, sample_std);
    IR_FIELD(IRMapSummary, min_val);
    IR_FIELD(IRMapSummary, max_val);
    IR_FIELD(IRMapSummary, span);
    IR_FIELD(IRMapSummary, percentile_low);
    IR_FIELD(IRMapSummary, percentile_high);
    IR_FIELD(IRMapSummary, percentile_span);
    IR_FIELD(IRMapSummary, valid_count);
    irAbiEndStruct();
    irAbiBeginStruct("IRGjbDriftContext", sizeof(IRGjbDriftContext), IR_ALIGNOF(IRGjbDriftContext));
    IR_FIELD(IRGjbDriftContext, volts_per_count);
    IR_FIELD(IRGjbDriftContext, delta_t_k);
    IR_FIELD(IRGjbDriftContext, ref_response_count);
    IR_FIELD(IRGjbDriftContext, ref_responsivity);
    IR_FIELD(IRGjbDriftContext, vmax_per_count);
    IR_FIELD(IRGjbDriftContext, noise_subtract_factor);
    IR_FIELD(IRGjbDriftContext, burst_frame_count);
    IR_FIELD(IRGjbDriftContext, noise_subtract_mode);
    IR_FIELD(IRGjbDriftContext, lrnu_kernel_size);
    IR_FIELD(IRGjbDriftContext, reserved);
    irAbiEndStruct();
    irAbiBeginStruct("IRGjbBurstMetrics", sizeof(IRGjbBurstMetrics), IR_ALIGNOF(IRGjbBurstMetrics));
    IR_FIELD(IRGjbBurstMetrics, mean_dn);
    IR_FIELD(IRGjbBurstMetrics, mean_v);
    IR_FIELD(IRGjbBurstMetrics, span_dn);
    IR_FIELD(IRGjbBurstMetrics, spatial_std_dn);
    IR_FIELD(IRGjbBurstMetrics, spatial_std_v);
    IR_FIELD(IRGjbBurstMetrics, spatial_std_denoised_dn);
    IR_FIELD(IRGjbBurstMetrics, spatial_std_denoised_v);
    IR_FIELD(IRGjbBurstMetrics, nonuniformity_pct);
    IR_FIELD(IRGjbBurstMetrics, avg_temporal_noise_dn);
    IR_FIELD(IRGjbBurstMetrics, avg_temporal_noise_mv);
    IR_FIELD(IRGjbBurstMetrics, rms_temporal_noise_dn);
    IR_FIELD(IRGjbBurstMetrics, estimated_netd_mk);
    IR_FIELD(IRGjbBurstMetrics, estimated_nep_w);
    IR_FIELD(IRGjbBurstMetrics, spatial_noise);
    IR_FIELD(IRGjbBurstMetrics, max_dynamic_range_db);
    IR_FIELD(IRGjbBurstMetrics, lrnu_std_dn);
    IR_FIELD(IRGjbBurstMetrics, lrnu_span_dn);
    IR_FIELD(IRGjbBurstMetrics, lrnu_pct);
    IR_FIELD(IRGjbBurstMetrics, hrnu_std_dn);
    IR_FIELD(IRGjbBurstMetrics, hrnu_pct);
    IR_FIELD(IRGjbBurstMetrics, valid_pixel_count);
    irAbiEndStruct();
    irAbiBeginStruct("IRFlickerSummary", sizeof(IRFlickerSummary), IR_ALIGNOF(IRFlickerSummary));
    IR_FIELD(IRFlickerSummary, std_dev_multiplier);
    IR_FIELD(IRFlickerSummary, flicker_ratio_pct);
    IR_FIELD(IRFlickerSummary, mean_frame_mean_dn);
    IR_FIELD(IRFlickerSummary, mean_frame_sigma_dn);
    IR_FIELD(IRFlickerSummary, exceed_count_threshold);
    IR_FIELD(IRFlickerSummary, frame_count);
    IR_FIELD(IRFlickerSummary, valid_pixel_count);
    IR_FIELD(IRFlickerSummary, flicker_count);
    irAbiEndStruct();
    irAbiBeginStruct("IRFft2dSummary", sizeof(IRFft2dSummary), IR_ALIGNOF(IRFft2dSummary));
    IR_FIELD(IRFft2dSummary, dc_magnitude);
    IR_FIELD(IRFft2dSummary, peak_ac_magnitude);
    IR_FIELD(IRFft2dSummary, total_ac_power);
    IR_FIELD(IRFft2dSummary, low_freq_power_ratio);
    IR_FIELD(IRFft2dSummary, high_freq_power_ratio);
    IR_FIELD(IRFft2dSummary, peak_ac_row);
    IR_FIELD(IRFft2dSummary, peak_ac_col);
    IR_FIELD(IRFft2dSummary, rows);
    IR_FIELD(IRFft2dSummary, cols);
    IR_FIELD(IRFft2dSummary, valid_pixel_count);
    IR_FIELD(IRFft2dSummary, reserved);
    irAbiEndStruct();
    printf("\n}\n");
}

#undef IR_FIELD

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--dump-abi-layout") == 0) {
        irAbiDump(1);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--dump-abi-sizes") == 0) {
        irAbiDump(0);
        return 0;
    }
    const uint16_t frames[] = {20, 22};
    double mean, noise, summary[2];
    char error[512];
    const IRArrayView input = {frames, sizeof frames, IR_TYPE_U16, 1, 1, 2};
    int status = IR_FrameStatistics(&input, 1, &mean, &noise,
        1, summary, 2, error, sizeof error);
    if (status || mean != 21 || fabs(noise - sqrt(2.0)) > 1e-12) {
        fprintf(stderr, "Analysis: %s\n", error);
        return 1;
    }
    IRFrameSourceInfo source_info;
    status = IR_OpenFrames(0, "test", 0, &source_info, error, sizeof error);
    if (status != IR_UNSUPPORTED || IR_CloseFrames(error, sizeof error)) {
        fprintf(stderr, "Station: %s\n", error);
        return 1;
    }
    status = IR_OpenFrames(IR_NI_IMAQ, "img0", 0, &source_info, error, sizeof error);
    if (status != IR_UNSUPPORTED) {
        fprintf(stderr, "Reserved NI-IMAQ backend: %s\n", error);
        return 1;
    }
    IRHandle device = 1;
    status = IR_OpenDevice(0, "{}", &device, error, sizeof error);
    if (status != IR_UNSUPPORTED || device != 0) {
        fprintf(stderr, "Device: %s\n", error);
        return 1;
    }
    if (sizeof(IRCaptureStats) != 64) {
        fprintf(stderr, "Unexpected IRCaptureStats ABI size: %zu\n", sizeof(IRCaptureStats));
        return 1;
    }
    if (strcmp(IR_PLUGIN_QUERY_SYMBOL, "ir_plugin_query") != 0 ||
        IR_POWER_DRIVER_ABI_VERSION != 1u ||
        strcmp(IR_POWER_DRIVER_QUERY_SYMBOL, "ir_power_driver_query") != 0 ||
        sizeof(IRRecipeStepEvent) != 56 ||
        sizeof(IRRecipeRunOptions) != 24 ||
        sizeof(IRSlotView) != 72 ||
        sizeof(IRPrepareCallbacks) != 32 ||
        sizeof(IRHostCallbacks) != 64 ||
        sizeof(IRMetricPlugin) != 48 ||
        IR_PLUGIN_ABI_VERSION != 2u) {
        fprintf(stderr, "Unexpected recipe/plugin ABI size or symbol\n");
        return 1;
    }
    {
        IRRecipeRunOptions bad_opts = {0, 0, NULL, NULL};
        IRRecipeResult rr = {0};
        status = IR_RunRecipeJson("{}", "{}", "out", &bad_opts, &rr, error, sizeof error);
        if (status != IR_BAD_ARGUMENT || rr.failed_stage != IR_RECIPE_STAGE_VALIDATE) {
            fprintf(stderr, "RunRecipeJson bad_opts: status=%d stage=%d err=%s\n",
                status, rr.failed_stage, error);
            return 1;
        }
    }
    IRCaptureStats stats = {0};
    uint16_t dummy_px = 0;
    status = IR_SetFrameQueueCapacity(64, error, sizeof error);
    if (status != IR_OK) {
        fprintf(stderr, "SetFrameQueueCapacity: %s\n", error);
        return 1;
    }
    status = IR_GetCaptureStats(&stats, error, sizeof error);
    if (status != IR_OK || stats.queue_capacity != 64) {
        fprintf(stderr, "GetCaptureStats: status=%d cap=%d err=%s\n", status, stats.queue_capacity, error);
        return 1;
    }
    IRFrameBatchView latest = {{&dummy_px, sizeof dummy_px, IR_TYPE_U16, 0, 0, 1}, 0, NULL, 0};
    status = IR_TryReadLatestFrame(&latest, error, sizeof error);
    if (status != IR_OK || latest.pixels.frames != 0) {
        fprintf(stderr, "TryReadLatestFrame: status=%d frames=%d err=%s\n",
            status, latest.pixels.frames, error);
        return 1;
    }
    const char *analysis_version = IR_AnalysisVersion();
    const char *station_version = IR_StationVersion();
    if (!analysis_version || !station_version || strcmp(analysis_version, station_version) != 0) {
        fprintf(stderr, "Version mismatch: analysis=%s station=%s\n",
            analysis_version ? analysis_version : "(null)",
            station_version ? station_version : "(null)");
        return 1;
    }
    puts(station_version);
    return argc > 1 && strcmp(argv[1], station_version) != 0;
}
