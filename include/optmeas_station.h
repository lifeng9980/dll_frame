#pragma once
#include "optmeas_status.h"

#if defined(_WIN32) && defined(OPTMEAS_DEVICE_BUILD)
#define OPTMEAS_DEVICE_API __declspec(dllexport)
#elif defined(_WIN32)
#define OPTMEAS_DEVICE_API __declspec(dllimport)
#else
#define OPTMEAS_DEVICE_API
#endif

typedef uint64_t OptMeasHandle;
#ifdef __cplusplus
extern "C" {
#endif
enum OptMeasBackend { OPTMEAS_DIRECTSHOW = 1, OPTMEAS_RAW_REPLAY = 2, OPTMEAS_V4L2 = 3 };
enum OptMeasFrameFields {
    OPTMEAS_HAS_SEQUENCE = 1, OPTMEAS_HAS_DRIVER_SEQUENCE = 2, OPTMEAS_HAS_LOSS = 4,
    OPTMEAS_SEQUENCE_IS_CALLBACK = 8, OPTMEAS_PIXELS_INVERTED = 16
};
enum OptMeasTimeOrigin {
    OPTMEAS_TIME_UNKNOWN, OPTMEAS_TIME_SOURCE, OPTMEAS_TIME_HOST,
    OPTMEAS_TIME_RECORDED_MONOTONIC, OPTMEAS_TIME_RECORDED_RELATIVE
};
/* 实际帧几何，宽高以像元计，valid_bits 为每个 U16 中的有效位。 */
typedef struct OptMeasGeometry {
    int32_t width, height, valid_bits;
} OptMeasGeometry;
/* 先检查 fields 中的有效位。timestamp_ns 为纳秒，时基由 time_origin 指明。 */
typedef struct OptMeasFrameStamp {
    uint64_t sequence, driver_sequence;
    int64_t timestamp_ns;
    uint32_t lost_frames, fields;
    int32_t time_origin;
} OptMeasFrameStamp;

/* 一个进程使用一个采集源。除 RequestStop 外，采集和流程调用使用打开源的线程。
   打开 RAW 目录，valid_bits 传 0 时读取索引。成功时 actual 返回宽、高和有效位。 */
OPTMEAS_DEVICE_API int32_t OptMeas_OpenFrames(int32_t backend, const char *name_or_directory,
    int32_t valid_bits, OptMeasGeometry *actual, char *err, int32_t err_bytes);
/* 回放从第 0 帧开始，每次停止后可重新启动。 */
OPTMEAS_DEVICE_API int32_t OptMeas_StartFrames(char *err, int32_t err_bytes);
/* timeout_ms 为实时来源的单帧等待上限，RAW 按文件 I/O 速度读取。
 * pixels 至少有 count*width*height 个 uint16_t，stamps 至少有 count 条记录。
 * 发生错误时，只有首 completed 帧及其记录有效。所有输出缓冲区由调用方持有。
 */
OPTMEAS_DEVICE_API int32_t OptMeas_ReadFrames(int32_t count, int32_t timeout_ms, uint16_t *pixels,
    int64_t pixel_capacity, OptMeasFrameStamp *stamps, int32_t stamp_capacity, int32_t *completed,
    char *err, int32_t err_bytes);
/* 可以从其他线程请求停止。此调用返回后，采集线程仍须完成当前操作并释放资源。 */
OPTMEAS_DEVICE_API void OptMeas_RequestStop(void);
OPTMEAS_DEVICE_API int32_t OptMeas_StopFrames(char *err, int32_t err_bytes);
/* 释放采集源，之后可打开另一来源。 */
OPTMEAS_DEVICE_API int32_t OptMeas_CloseFrames(char *err, int32_t err_bytes);

typedef struct OptMeasNoiseRecipe {
    const char *recipe_id;                 /* 产品配方标识或版本，UTF-8 */
    OptMeasHandle detector, blackbody;     /* 离线流程均填 0。 */
    int32_t integration_us, frame_hz, empty_us;
    double temperature_c, tolerance_c;
    int32_t stable_window_ms, stable_timeout_ms, require_device_stable;
    int32_t frames, frame_timeout_ms;
} OptMeasNoiseRecipe;
typedef struct OptMeasNoiseResult {
    int32_t completed;
    double mean_dn, temporal_noise_dn;      /* 像元样本标准差的全图均值 */
} OptMeasNoiseResult;
/* 同步运行已打开且未启动的 RAW 源。recipe 和 result 由调用方持有。
   recipe_id 使用 UTF-8，frames >= 2。离线结果为完成帧数、平均 DN 和样本时域噪声 DN。
   目录传 NULL 时只计算。传 UTF-8 路径时须是同级父目录下尚未存在的新目录。
   失败时 completed 仍报告已连续交付的帧数，指标字段无效。 */
OPTMEAS_DEVICE_API int32_t OptMeas_RunNoise(const OptMeasNoiseRecipe *recipe,
    const char *new_output_directory, OptMeasNoiseResult *result, char *err, int32_t err_bytes);

#ifdef __cplusplus
}
#endif
