#pragma once
#include "IR_status.h"

#if defined(_WIN32) && defined(IR_ANALYSIS_BUILD)
#define IR_ANALYSIS_API __declspec(dllexport)
#elif defined(_WIN32)
#define IR_ANALYSIS_API __declspec(dllimport)
#elif defined(IR_ANALYSIS_BUILD) && defined(__GNUC__)
#define IR_ANALYSIS_API __attribute__((visibility("default")))
#else
#define IR_ANALYSIS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* 版本字符串为 UTF-8，由库持有，库卸载前有效。 */
IR_ANALYSIS_API const char *IR_AnalysisVersion(void);

/* 输入帧在调用期间有效，按 [帧][行][列] 紧密排列，容量按元素计。
 * sample_std=1 计算样本标准差（帧数至少 2），0 计算总体标准差（帧数至少 1）。
 * mean_map、std_map 各需 rows*cols 个 double，summary 需 2 个 double。
 * 输出单位均为 DN，summary 依次是全图均值、各像元时域标准差的全图均值。
 * 输入和输出缓冲区由调用方提供且不重叠，失败时输出无效。
 * 此调用同步等待计算完成，可在应用的工作线程执行。错误文本的约定见 IR_status.h。
 */
IR_ANALYSIS_API int32_t IR_FrameStatisticsU16(const uint16_t *frames, int64_t frame_elements,
    int32_t frame_count, int32_t rows, int32_t cols, int32_t sample_std, double *mean_map,
    double *std_map, int64_t pixel_capacity, double *summary, int32_t summary_count,
    char *err, int32_t err_bytes);

/* 计算所有输入帧中矩形 ROI 的像元均值，输出单位 DN。
 * 输入为紧密排列的 [帧][行][列] uint16_t 数据；x/y 为左上角坐标，宽高必须大于 0 且位于图像内。
 * 可用于离线数据或采集后的同一帧组；调用同步完成，输入由调用方持有且调用期间不可修改。
 */
IR_ANALYSIS_API int32_t IR_ROIMeanU16(const uint16_t *frames, int64_t frame_elements,
    int32_t frame_count, int32_t rows, int32_t cols, int32_t x, int32_t y,
    int32_t roi_width, int32_t roi_height, double *mean_dn, char *err, int32_t err_bytes);

/* ======================== 三维噪声 (3D Noise, DN 两阶段) ======================== */

/* 3D 噪声计算模式：0=原始 ORG 帧；1=在库内应用两点 NUC (k * (org - b) + b_avg)。 */
typedef enum IRNoise3DMode
{
    IR_NOISE3D_MODE_ORG = 0,
    IR_NOISE3D_MODE_NUC = 1
} IRNoise3DMode;

/* IRNoise3DComputeOutputs.scalars 数组索引（至少需 IR_NOISE3D_SCALAR_COUNT=16 个 double），单位 DN 或计数。 */
typedef enum IRNoise3DScalarIndex
{
    IR_NOISE3D_SCALAR_DELTA_T = 0,
    IR_NOISE3D_SCALAR_DELTA_I = 1,
    IR_NOISE3D_SCALAR_DELTA_J = 2,
    IR_NOISE3D_SCALAR_DELTA_IJ = 3,
    IR_NOISE3D_SCALAR_DELTA_TI = 4,
    IR_NOISE3D_SCALAR_DELTA_TJ = 5,
    IR_NOISE3D_SCALAR_DELTA_TIJ = 6,
    IR_NOISE3D_SCALAR_DELTA_TOTAL = 7,
    IR_NOISE3D_SCALAR_DELTA_SYS = 8,
    IR_NOISE3D_SCALAR_GLOBAL_MEAN = 9,
    IR_NOISE3D_SCALAR_VALID_PIXEL_COUNT = 10,
    IR_NOISE3D_SCALAR_VALID_SAMPLE_COUNT = 11,
    IR_NOISE3D_SCALAR_FRAME_COUNT = 12,
    IR_NOISE3D_SCALAR_ROWS = 13,
    IR_NOISE3D_SCALAR_COLS = 14,
    IR_NOISE3D_SCALAR_RESERVED = 15,
    IR_NOISE3D_SCALAR_COUNT = 16
} IRNoise3DScalarIndex;

/* IRNoise3DCalibOutputs.stats 数组索引（至少需 IR_NOISE3D_STATS_COUNT=32 个 double）。 */
typedef enum IRNoise3DStatsIndex
{
    IR_NOISE3D_STAT_RESPONSE_MEAN = 0,
    IR_NOISE3D_STAT_RESPONSE_LOW = 1,
    IR_NOISE3D_STAT_RESPONSE_HIGH = 2,
    IR_NOISE3D_STAT_RESPONSE_BLIND_COUNT = 3,
    IR_NOISE3D_STAT_RESPONSE_BLIND_RATIO = 4,
    IR_NOISE3D_STAT_RMS_MEAN = 5,
    IR_NOISE3D_STAT_RMS_LOW = 6,
    IR_NOISE3D_STAT_RMS_HIGH = 7,
    IR_NOISE3D_STAT_RMS_BLIND_COUNT = 8,
    IR_NOISE3D_STAT_RMS_BLIND_RATIO = 9,
    IR_NOISE3D_STAT_TOTAL_BLIND_COUNT = 10,
    IR_NOISE3D_STAT_TOTAL_BLIND_RATIO = 11,
    IR_NOISE3D_STAT_VALID_COUNT = 12,
    IR_NOISE3D_STAT_VALID_RATIO = 13,
    IR_NOISE3D_STAT_B_AVG = 14,
    IR_NOISE3D_STAT_K_MEAN_VALID = 15,
    IR_NOISE3D_STATS_COUNT = 32
} IRNoise3DStatsIndex;

/* 直方图分组索引；hist 共 4 * hist_interval 个 double，hist_meta 共 4 * 3 个 double (min, max, width)。 */
typedef enum IRNoise3DHistGroup
{
    IR_NOISE3D_HIST_T = 0,
    IR_NOISE3D_HIST_I = 1,
    IR_NOISE3D_HIST_J = 2,
    IR_NOISE3D_HIST_TIJ = 3,
    IR_NOISE3D_HIST_GROUP_COUNT = 4
} IRNoise3DHistGroup;

/* IRNoise3DComputeOutputs.meta 计时元数据索引（若传非 NULL，至少需 IR_NOISE3D_META_COUNT=32 个 double）。 */
typedef enum IRNoise3DMetaIndex
{
    IR_NOISE3D_META_EFFECTIVE_THREAD_COUNT = 0,
    IR_NOISE3D_META_MASK_BUILD_SECONDS = 1,
    IR_NOISE3D_META_FIRST_PASS_SECONDS = 2,
    IR_NOISE3D_META_MEAN_BUILD_SECONDS = 3,
    IR_NOISE3D_META_TI_TJ_SECONDS = 4,
    IR_NOISE3D_META_STD_HIST_SECONDS = 5,
    IR_NOISE3D_META_TOTAL_SECONDS = 6,
    IR_NOISE3D_META_RESERVED = 7,
    IR_NOISE3D_META_COUNT = 32
} IRNoise3DMetaIndex;

typedef struct IRNoise3DCalibParams
{
    double response_lower_mul;
    double response_upper_mul;
    double rms_lower_mul;
    double rms_upper_mul;
    double k_epsilon;
} IRNoise3DCalibParams;

/* 掩码约定：response_blind_mask / rms_blind_mask / total_blind_mask 中 1=盲元、0=非盲元；
 * valid_mask 中 1=有效像元、0=盲元；所有缓冲区由调用方分配且互不重叠。
 */
typedef struct IRNoise3DCalibOutputs
{
    uint8_t *response_blind_mask;
    uint8_t *rms_blind_mask;
    uint8_t *total_blind_mask;
    uint8_t *valid_mask;
    double *k_map;
    double *stats;
    int64_t pixel_capacity;
    int32_t stats_capacity;
    int32_t reserved;
} IRNoise3DCalibOutputs;

typedef struct IRNoise3DComputeParams
{
    int32_t compute_mode;
    int32_t thread_count;
    int32_t hist_interval;
    int32_t reserved;
    double b_avg;
    double invalid_fill;
} IRNoise3DComputeParams;

/* nuc_frames 与 meta 可为 NULL；其余输出指针必须非 NULL 且容量充足。 */
typedef struct IRNoise3DComputeOutputs
{
    uint16_t *nuc_frames;
    double *scalars;
    double *hist;
    double *hist_meta;
    double *map_ij;
    double *map_ti;
    double *map_tj;
    double *meta;
    int64_t nuc_frames_capacity;
    int64_t hist_capacity;
    int64_t map_ij_capacity;
    int64_t map_ti_capacity;
    int64_t map_tj_capacity;
    int32_t scalars_capacity;
    int32_t hist_meta_capacity;
    int32_t meta_capacity;
    int32_t reserved;
} IRNoise3DComputeOutputs;

/* 3D 噪声标定建表（第一阶段）：
 * 输入 mean_high_map、mean_low_map、rms_map、b_map 均按 [行][列] 紧密排列，单位 DN，全部值须有限且 pixel <= 10^7。
 * 内部先按 rmsMap 筛选噪声盲元，再在非噪声盲元中计算 mean_high - mean_low 均值与响应盲元，最后生成 K 图。
 * 至少保留 2 个有效像元，否则返回 IR_COMPUTATION_FAILED。调用同步完成，输入输出不可重叠。
 */
IR_ANALYSIS_API int32_t IR_Noise3DBuildCalibDn(
    const double *mean_high_map,
    const double *mean_low_map,
    const double *rms_map,
    const double *b_map,
    int32_t rows,
    int32_t cols,
    int64_t input_pixel_capacity,
    const IRNoise3DCalibParams *params,
    const IRNoise3DCalibOutputs *outputs,
    char *err,
    int32_t err_bytes);

/* 3D 噪声帧组计算（第二阶段）：
 * org_frames 为紧密 [帧][行][列] uint16_t (frame_count >= 2，总样本 <= 10^9)；total_blind_mask 中 0=有效、非0=盲元。
 * 要求总有效像元 >= 2 且每行、每列至少包含 1 个有效像元。
 * ORG 模式下 k_map、b_map 可传 NULL 且 calib_map_capacity=0；NUC 模式下二者必填且有限。
 * 盲元在 map_ij 写入 invalid_fill，若提供 nuc_frames 则盲元位置写入 clamp(b_avg)。调用同步完成。
 */
IR_ANALYSIS_API int32_t IR_Noise3DComputeFramesDn(
    const uint16_t *org_frames,
    int64_t frame_elements,
    int32_t frame_count,
    int32_t rows,
    int32_t cols,
    const uint8_t *total_blind_mask,
    int64_t mask_capacity,
    const double *k_map,
    const double *b_map,
    int64_t calib_map_capacity,
    const IRNoise3DComputeParams *params,
    const IRNoise3DComputeOutputs *outputs,
    char *err,
    int32_t err_bytes);

/* ======================== 国标 (GB) 与国军标 (GJB) 评价 ======================== */

/* GB 芯片等级 */
typedef enum IRChipGrade
{
    IR_CHIP_GRADE_UNKNOWN = 0,
    IR_CHIP_GRADE_A = 1,
    IR_CHIP_GRADE_B = 2,
    IR_CHIP_GRADE_C = 3,
    IR_CHIP_GRADE_D = 4
} IRChipGrade;

/* GB 盲元簇划分区域（A 为中心 1/5×1/4，B 为次中心 1/2×1/2，C 为其余边界，优先级 A > B > C）。 */
typedef enum IRGbRegion
{
    IR_GB_REGION_A = 0,
    IR_GB_REGION_B = 1,
    IR_GB_REGION_C = 2,
    IR_GB_REGION_COUNT = 3
} IRGbRegion;

/* GB 盲元簇按外接矩形最长边分类：O(>=5), I(4), II(3), III(2), IV(1)。 */
typedef enum IRGbClusterType
{
    IR_GB_CLUSTER_O = 0,
    IR_GB_CLUSTER_I = 1,
    IR_GB_CLUSTER_II = 2,
    IR_GB_CLUSTER_III = 3,
    IR_GB_CLUSTER_IV = 4,
    IR_GB_CLUSTER_TYPE_COUNT = 5
} IRGbClusterType;

/* 辐射与系统转换参数：
 * 温度单位 °C（物理计算使用 +273 K 口径），像元间距 um，系统增益无量纲，ADC 摆幅 V，积分时间 us。
 */
typedef struct IRCalcParameters
{
    double t0_c;
    double t1_c;
    double pixel_pitch_um;
    double emissivity;
    double system_gain;
    double adc_swing_v;
    double f_number;
    double vmax_per_count;
    int32_t integration_time_us;
    int32_t invalid_integration_us;
    int32_t digital_full;
    int32_t reserved;
} IRCalcParameters;

typedef struct IRGbCriteria
{
    double response_upper;
    double response_lower;
    double noise_upper;
    double netd_upper;
    double short_bad_threshold; /* <= 0 时沿用默认 45000.0 (V1 > threshold) */
    double open_bad_threshold;  /* <= 0 时沿用默认 20000.0 (V2 < threshold) */
    int32_t cycle_upper;
    int32_t reserved;
} IRGbCriteria;

typedef struct IRGbBadPixelSummary
{
    double total_bad_rate_pct;
    int32_t response_bad_count;
    int32_t response_upper_bad_count;
    int32_t response_lower_bad_count;
    int32_t noise_bad_count;
    int32_t netd_bad_count;
    int32_t total_bad_count;
    int32_t iteration_count;
    int32_t chip_grade;
    int32_t cluster_counts[IR_GB_REGION_COUNT][IR_GB_CLUSTER_TYPE_COUNT];
    int32_t reserved;
} IRGbBadPixelSummary;

/* 可选输出掩码容器：任一指针为 NULL 时跳过写入；非 NULL 缓冲区须具备至少 pixel_capacity >= rows*cols。
 * 掩码值约定：valid_mask 中 1=有效、0=盲元；其余 *_bad_mask 中 1=盲元、0=非盲元。
 */
typedef struct IRGbBadPixelMasks
{
    uint8_t *valid_mask;
    uint8_t *total_bad_mask;
    uint8_t *response_bad_mask;
    uint8_t *noise_bad_mask;
    uint8_t *netd_bad_mask;
    uint8_t *short_bad_mask;
    uint8_t *open_bad_mask;
    int64_t pixel_capacity;
} IRGbBadPixelMasks;

typedef struct IRGbSummary
{
    double before_avg_pixel_response_count;
    double before_avg_pixel_response_voltage;
    double volts_per_count;
    double before_avg_snr;
    double irradiance_power_diff;
    double before_avg_blackbody_response;
    double before_avg_noise_voltage_v;
    double before_avg_netd_mk;
    double before_avg_detectivity;
    double before_avg_responsivity;
    double before_sitf;
    double after_response_mv_per_ms_k;
    double after_avg_response_voltage_v;
    double after_avg_responsivity;
    double after_avg_nep_w;
    double after_response_nonuniformity_pct;
    double after_avg_noise_voltage_mv;
    double after_avg_netd_mk;
    double after_avg_detectivity;
    int32_t short_bad_pixel_count;
    int32_t open_bad_pixel_count;
    IRGbBadPixelSummary bad_pixels;
} IRGbSummary;

typedef struct IRGjbCriteria
{
    double k1;
    double k2;
    double k3;
    double k4;
    int32_t cycle_upper;
    int32_t reserved;
} IRGjbCriteria;

typedef struct IRGjbClusterStats
{
    int32_t count_1_to_6;
    int32_t count_7_to_12;
    int32_t count_13_to_19;
    int32_t count_20_to_35;
    int32_t count_gt_35;
} IRGjbClusterStats;

typedef struct IRGjbBadPixelSummary
{
    double total_bad_rate_pct;
    int32_t total_bad_count;
    int32_t iteration_count;
    int32_t center_3x3_block_count;
    int32_t filtered_4x4_block_count;
    IRGjbClusterStats total_clusters;
    IRGjbClusterStats center_clusters;
    IRGjbClusterStats edge_clusters;
    int32_t reserved;
} IRGjbBadPixelSummary;

/* 可选输出掩码容器：valid_mask 为 1=有效、0=盲元；其余 *_bad_mask 为 1=盲元、0=非盲元。 */
typedef struct IRGjbBadPixelMasks
{
    uint8_t *valid_mask;
    uint8_t *total_bad_mask;
    uint8_t *k1_bad_mask;
    uint8_t *k2_bad_mask;
    uint8_t *k3_bad_mask;
    uint8_t *k4_bad_mask;
    uint8_t *block_filtered_bad_mask;
    int64_t pixel_capacity;
} IRGjbBadPixelMasks;

typedef struct IRGjbSummary
{
    double before_avg_pixel_response_count;
    double before_avg_pixel_response_voltage;
    double volts_per_count;
    double before_avg_snr;
    double fov;
    double irradiance_power_diff;
    double before_avg_noise_voltage_v;
    double before_avg_netd_mk;
    double before_avg_detectivity;
    double before_avg_responsivity;
    double after_avg_response_voltage_v;
    double after_avg_responsivity;
    double after_avg_nep_w;
    double after_response_nonuniformity_pct;
    double after_avg_noise_voltage_mv;
    double after_avg_netd_mk;
    double after_avg_detectivity;
    double after_spatial_noise;
    double after_max_dynamic_range_db;
    double after_rfpn_v;
    IRGjbBadPixelSummary bad_pixels;
} IRGjbSummary;

/* 直接对响应图、噪声图与 NETD 图执行 GB 迭代盲元筛选与簇等级统计。
 * 非有限像元视为盲元并计入对应分类及总盲元，但仅有限值计入超上限/超下限计数；masks 可为 NULL。
 */
IR_ANALYSIS_API int32_t IR_GbProcessBadPixels(
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
    int32_t err_bytes);

/* GB 全流程评价：输入冷/热黑体均值图 v1_map、v2_map 及噪声图 vn_map（DN，建议由 IR_FrameStatisticsU16 传入 sample_std=0 即分母 N）。
 * 计算去盲元前指标、短路/断路盲元、迭代盲元筛选、簇等级及去盲元后指标。masks 可为 NULL。调用同步完成。
 */
IR_ANALYSIS_API int32_t IR_GbEvaluateMaps(
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
    int32_t err_bytes);

/* 直接对响应率图、信号电压图 T1/T2、噪声电压图与 NETD 图执行 GJB 迭代盲元筛选与方块/簇统计。masks 可为 NULL。 */
IR_ANALYSIS_API int32_t IR_GjbProcessBadPixels(
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
    int32_t err_bytes);

/* GJB 全流程评价：输入 v1_map、v2_map、噪声图 vn_map（建议 sample_std=0 分母 N 口径）及两点校正验证图 vm_map（DN）。
 * 计算去盲元前指标、k1..k4 盲元迭代、中心/边缘簇、3x3 中心块与 4x4 全图块，以及去盲元后指标与 RFPN。masks 可为 NULL。
 */
IR_ANALYSIS_API int32_t IR_GjbEvaluateMaps(
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
    int32_t err_bytes);

/* ======================== 可组合基础算子与温漂轻量指标 ======================== */

typedef enum IRSpatialFilterKind
{
    IR_SPATIAL_FILTER_BOX_MEAN = 0,
    IR_SPATIAL_FILTER_GAUSSIAN = 1
} IRSpatialFilterKind;

typedef enum IRMapBinaryOp
{
    IR_MAP_OP_SUB = 0,
    IR_MAP_OP_ADD = 1,
    IR_MAP_OP_MUL = 2,
    IR_MAP_OP_SAFE_DIV = 3,
    IR_MAP_OP_VARIANCE_SUBTRACT = 4
} IRMapBinaryOp;

typedef enum IRThresholdMode
{
    IR_THRESH_ABSOLUTE = 0,
    IR_THRESH_MEAN_MULTIPLIER = 1,
    IR_THRESH_MEAN_SIGMA = 2,
    IR_THRESH_UPPER_ONLY = 3,
    IR_THRESH_LOWER_ONLY = 4
} IRThresholdMode;

typedef enum IRMaskOp
{
    IR_MASK_AND = 0,
    IR_MASK_OR = 1,
    IR_MASK_AND_NOT = 2,
    IR_MASK_NOT = 3
} IRMaskOp;

typedef enum IRWindowKind
{
    IR_WINDOW_RECTANGULAR = 0,
    IR_WINDOW_HANN = 1,
    IR_WINDOW_HAMMING = 2
} IRWindowKind;

typedef enum IRSpectrumAxis
{
    IR_SPECTRUM_AXIS_COLUMNS = 0,
    IR_SPECTRUM_AXIS_ROWS = 1
} IRSpectrumAxis;

typedef enum IRNoiseSubtractMode
{
    IR_NOISE_SUBTRACT_NONE = 0,
    IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE = 1
} IRNoiseSubtractMode;

typedef struct IRMapSummary
{
    double mean;
    double pop_std;
    double sample_std;
    double min_val;
    double max_val;
    double span;
    double percentile_low;
    double percentile_high;
    double percentile_span;
    int64_t valid_count;
} IRMapSummary;

/* 单 Burst 轻量 GJB / 温漂评价上下文（由初始两点标定获得并复用）。
 * noise_subtract_mode: 0=不扣除，1=方差正交扣除 sqrt(max(0, sigma_spatial^2 - factor*mean(vn^2)/burst_frame_count))；
 * lrnu_kernel_size <= 1 不计算 LRNU/HRNU；>= 3 须为奇数（如 15 表示 15x15 低频盒式平均）。
 */
typedef struct IRGjbDriftContext
{
    double volts_per_count;
    double delta_t_k;
    double ref_response_count;
    double ref_responsivity;
    double vmax_per_count;
    double noise_subtract_factor;
    int32_t burst_frame_count;
    int32_t noise_subtract_mode;
    int32_t lrnu_kernel_size;
    int32_t reserved;
} IRGjbDriftContext;

typedef struct IRGjbBurstMetrics
{
    double mean_dn;
    double mean_v;
    double span_dn;
    double spatial_std_dn;
    double spatial_std_v;
    double spatial_std_denoised_dn;
    double spatial_std_denoised_v;
    double nonuniformity_pct;
    double avg_temporal_noise_dn;
    double avg_temporal_noise_mv;
    double rms_temporal_noise_dn;
    double estimated_netd_mk;
    double estimated_nep_w;
    double spatial_noise;
    double max_dynamic_range_db;
    double lrnu_std_dn;
    double lrnu_span_dn;
    double lrnu_pct;
    double hrnu_std_dn;
    double hrnu_pct;
    int64_t valid_pixel_count;
} IRGjbBurstMetrics;

/* 两点 NUC 建表：按有效像元均值计算 K=(mean1-mean2)/(v1-v2)，
 * 若提供 single_point_ref_map 则用 B=mean_ref-K*ref，否则 B=mean1-K*v1。
 * valid_mask (1=有效/0=剔除)、single_point_ref_map、out_summary 均可为 NULL；
 * 若 out_summary 非 NULL 须 summary_count >= 4，依次返回 [mean1, mean2, mean_ref_or_mean1, valid_count]。
 */
IR_ANALYSIS_API int32_t IR_BuildTwoPointNucF64(
    const double *v1_map,
    const double *v2_map,
    const uint8_t *valid_mask,
    const double *single_point_ref_map,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    double *out_k_map,
    double *out_b_map,
    double *out_summary,
    int32_t summary_count,
    char *err,
    int32_t err_bytes);

/* 对二维浮点图应用仿射映射 out = k * in + b。 */
IR_ANALYSIS_API int32_t IR_ApplyAffineMapF64(
    const double *in_map,
    const double *k_map,
    const double *b_map,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    double *out_map,
    char *err,
    int32_t err_bytes);

/* 无损 NUC 与可选减暗场帧统计：单遍 Welford 计算 val = k*raw + b - dark。
 * sample_std=0 计算总体标准差（分母 N，frame_count>=1），1 计算样本标准差（分母 N-1，frame_count>=2）。
 * k_map/b_map 须同为 NULL 或同非 NULL；dark_map 与 valid_mask 可为 NULL。
 * summary 需至少 2 个 double，按 valid_mask（若非 NULL）汇总均值与噪声均值。
 */
IR_ANALYSIS_API int32_t IR_FrameStatisticsNucU16(
    const uint16_t *frames,
    int64_t frame_elements,
    int32_t frame_count,
    int32_t rows,
    int32_t cols,
    int32_t sample_std,
    const double *k_map,
    const double *b_map,
    const double *dark_map,
    const uint8_t *valid_mask,
    double *mean_map,
    double *std_map,
    int64_t pixel_capacity,
    double *summary,
    int32_t summary_count,
    char *err,
    int32_t err_bytes);

/* 像元图统计摘要（均值、总体/样本标准差、min/max/span 与分位跨度）。
 * roi_w==0 且 roi_h==0 表示全图；valid_mask 可为 NULL。0 <= p_low <= p_high <= 100。
 */
IR_ANALYSIS_API int32_t IR_MapSummaryF64(
    const double *map,
    const uint8_t *valid_mask,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t roi_x,
    int32_t roi_y,
    int32_t roi_w,
    int32_t roi_h,
    double p_low,
    double p_high,
    IRMapSummary *summary,
    char *err,
    int32_t err_bytes);

/* 掩码感知空间低频/高频分解：仅聚合 valid_mask==1 且有限的邻域像元，边界自动裁剪。
 * filter_kind 支持 IR_SPATIAL_FILTER_BOX_MEAN（盒式均值）与 IR_SPATIAL_FILTER_GAUSSIAN（归一化可分离高斯核）。
 * kernel_w / kernel_h 必须为 >=1 的奇数（例如 15x15 对应 LRNU/HRNU）。
 * out_low_map 与 out_high_map 至少一个非 NULL；high = valid_finite ? (in - low) : 0。
 */
IR_ANALYSIS_API int32_t IR_SpatialLowHighPassF64(
    const double *in_map,
    const uint8_t *valid_mask,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t filter_kind,
    int32_t kernel_w,
    int32_t kernel_h,
    double *out_low_map,
    double *out_high_map,
    char *err,
    int32_t err_bytes);

/* 像元图二元算术与噪声扣除：op 见 IRMapBinaryOp；VARIANCE_SUBTRACT 计算 sqrt(max(0, a^2 - param * b^2))。 */
IR_ANALYSIS_API int32_t IR_MapBinaryOpF64(
    const double *a_map,
    const double *b_map,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t op,
    double param,
    double *out_map,
    char *err,
    int32_t err_bytes);

/* 按指定阈值模式对像元图生成通过掩码（1=通过，0=未通过/非有限/非基础有效）。
 * base_valid_mask、out_pass_count、out_fail_count 可为 NULL。
 */
IR_ANALYSIS_API int32_t IR_ThresholdMaskF64(
    const double *map,
    const uint8_t *base_valid_mask,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t mode,
    double low_param,
    double high_param,
    uint8_t *out_pass_mask,
    int32_t *out_pass_count,
    int32_t *out_fail_count,
    char *err,
    int32_t err_bytes);

/* 布尔掩码组合：op 见 IRMaskOp；IR_MASK_NOT 时 mask_b 可为 NULL。out_nonzero_count 可为 NULL。 */
IR_ANALYSIS_API int32_t IR_MaskCombine(
    const uint8_t *mask_a,
    const uint8_t *mask_b,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t op,
    uint8_t *out_mask,
    int32_t *out_nonzero_count,
    char *err,
    int32_t err_bytes);

/* 1D 实信号单边 FFT（任意 length >= 2，零第三方依赖）：
 * detrend_mean != 0 时先去均值再加窗；out_bins_capacity >= length/2 + 1。
 * out_freqs_hz 与 out_psd 可为 NULL，out_amplitude 必填（与零填充对齐后保持单边幅度口径）。
 */
IR_ANALYSIS_API int32_t IR_ComputeRfft1D(
    const double *signal,
    int32_t length,
    double sample_rate_hz,
    int32_t window_kind,
    int32_t detrend_mean,
    double *out_freqs_hz,
    double *out_amplitude,
    double *out_psd,
    int32_t out_bins_capacity,
    char *err,
    int32_t err_bytes);

/* 多通道图像时间序列频谱分析（对齐按列/按行交叉读出通道）。
 * out_channel_amplitudes 必填，容量 >= channel_count * (frame_count/2 + 1)；
 * out_freqs_hz (容量 >= frame_count/2+1)、out_channel_series (容量 >= channel_count*frame_count)、
 * out_temporal_means / out_temporal_stds (容量 >= channel_count) 均可为 NULL。
 */
IR_ANALYSIS_API int32_t IR_ChannelSpectrumU16(
    const uint16_t *frames,
    int64_t frame_elements,
    int32_t frame_count,
    int32_t rows,
    int32_t cols,
    int32_t axis,
    int32_t channel_count,
    double sample_rate_hz,
    int32_t window_kind,
    double *out_freqs_hz,
    int32_t freq_capacity,
    double *out_channel_series,
    int64_t series_capacity,
    double *out_channel_amplitudes,
    int64_t amplitude_capacity,
    double *out_temporal_means,
    double *out_temporal_stds,
    int32_t channel_stats_capacity,
    char *err,
    int32_t err_bytes);

/* 单 Burst 轻量 GJB/温漂评价：输入单组均值图与总体噪声图（NUC 前或 NUC 后）+ 固定有效掩码与标定上下文，
 * 直接输出空域非均匀性、扣除噪声后的空间起伏 / RFPN、估算 NETD/NEP、动态范围与可选 LRNU/HRNU。
 */
IR_ANALYSIS_API int32_t IR_GjbEvaluateBurstMetrics(
    const double *mean_map,
    const double *vn_map,
    const uint8_t *valid_mask,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    const IRGjbDriftContext *ctx,
    IRGjbBurstMetrics *metrics,
    char *err,
    int32_t err_bytes);

/* ======================== 闪烁像元（闪元 / RTS）检测 ======================== */

typedef struct IRFlickerSummary
{
    double std_dev_multiplier;
    double flicker_ratio_pct;
    double mean_frame_mean_dn;
    double mean_frame_sigma_dn;
    int32_t exceed_count_threshold;
    int32_t frame_count;
    int32_t valid_pixel_count;
    int32_t flicker_count;
} IRFlickerSummary;

/* 闪元检测（逐帧总体标准差阈值统计）：
 * frames 为紧密 [帧][行][列] uint16_t（frame_count >= 1）；若同时提供 k_map 与 b_map 则在库内执行 NUC (k*raw+b)。
 * 跳过 valid_mask==0、原始值==0、或校正后非有限/==0 的像元；仅对当帧观察到的有效像元计算均值与总体标准差 sigma，
 * 若 |val - frame_mean| > std_dev_multiplier * frame_sigma，则该像元超限次数 +1；
 * 若总观察有效像元超限次数 > exceed_count_threshold（0 <= threshold < frame_count），标记为闪元（out_flicker_mask=1）。
 * out_exceed_count_map、out_flicker_mask、out_frame_means、out_frame_sigmas 均可为 NULL。
 */
IR_ANALYSIS_API int32_t IR_FlickerDetectU16(
    const uint16_t *frames,
    int64_t frame_elements,
    int32_t frame_count,
    int32_t rows,
    int32_t cols,
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
    int32_t err_bytes);
/* ======================== 二维 FFT / 空域频谱 ======================== */

typedef struct IRFft2dSummary
{
    double dc_magnitude;            /* 零频幅度 |X[0,0]| / W */
    double peak_ac_magnitude;       /* 非零频最大归一化幅度 max(|X[u,v]| / W) */
    double total_ac_power;          /* 所有非零频归一化功率和 sum((|X[u,v]| / W)^2) */
    double low_freq_power_ratio;    /* 归一化径向频率 <= 0.25 的 AC 功率占比 [0, 1] */
    double high_freq_power_ratio;   /* 归一化径向频率 > 0.25 的 AC 功率占比 [0, 1] */
    int32_t peak_ac_row;            /* 非零频峰值行索引（按输出坐标系） */
    int32_t peak_ac_col;            /* 非零频峰值列索引（按输出坐标系） */
    int32_t rows;
    int32_t cols;
    int32_t valid_pixel_count;
    int32_t reserved;
} IRFft2dSummary;

/* 纯 C++ 任意尺寸二维离散傅里叶幅度/功率谱（支持任意 rows >= 1、cols >= 1）：
 * W 是所选可分离二维窗函数的总权重 sum(w_row) * sum(w_col)；矩形窗时 W=rows*cols，Hann 窗时按 Hann 权重和归一化；
 * 输出幅度图和功率图分别使用 |X|/W 和 (|X|/W)^2，与 summary 字段使用同一归一化约定；
 * 若提供 valid_mask，则无效或非有限像元按有效像元均值填充（配合 detrend_mean=1 等价填零）；
 * window_kind 支持 IR_WINDOW_RECTANGULAR（可分离二维矩形）与 IR_WINDOW_HANN（可分离二维对称 Hann）。
 * Hann 窗在长度为 1 或 2 的轴上各权重取 1，以保留该轴上的全部像元。
 * shift_dc_center != 0 时按 (rows/2, cols/2) 中心平移输出谱图及峰值索引；
 * summary 必填，out_magnitude_map 与 out_power_map 均可为 NULL。
 */
IR_ANALYSIS_API int32_t IR_Fft2dMagnitudeF64(
    const double *in_map,
    const uint8_t *valid_mask,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t window_kind,
    int32_t detrend_mean,
    int32_t shift_dc_center,
    IRFft2dSummary *summary,
    double *out_magnitude_map,
    double *out_power_map,
    char *err,
    int32_t err_bytes);

#ifdef __cplusplus
}
#endif
