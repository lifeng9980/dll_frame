#pragma once
#include "IR_status.h"

#if defined(_WIN32) && defined(IR_STATION_BUILD)
#define IR_STATION_API __declspec(dllexport)
#elif defined(_WIN32)
#define IR_STATION_API __declspec(dllimport)
#elif defined(IR_STATION_BUILD) && defined(__GNUC__)
#define IR_STATION_API __attribute__((visibility("default")))
#else
#define IR_STATION_API
#endif

typedef uint64_t IRHandle;
#ifdef __cplusplus
extern "C" {
#endif
enum IRBackend {
    IR_DIRECTSHOW = 1, /* Windows DirectShow device name, e.g. FX3. */
    IR_RAW_REPLAY = 2,
    IR_V4L2 = 3 /* Linux /dev/videoN, for the FX3 YUYV 16-bit raw stream. */
};
enum IRFrameFields {
    IR_HAS_SEQUENCE = 1, IR_HAS_DRIVER_SEQUENCE = 2, IR_HAS_LOSS = 4,
    IR_SEQUENCE_IS_CALLBACK = 8, IR_PIXELS_INVERTED = 16
};
enum IRTimeOrigin {
    IR_TIME_UNKNOWN, IR_TIME_SOURCE, IR_TIME_HOST,
    IR_TIME_RECORDED_MONOTONIC, IR_TIME_RECORDED_RELATIVE
};
/* 实际帧几何，宽高以像元计，valid_bits 为每个 U16 中的有效位；0 表示来源没有给出。 */
typedef struct IRGeometry {
    int32_t width, height, valid_bits;
} IRGeometry;
/* 先检查 fields 中的有效位。DirectShow 的 sequence 标记为 callback 序号；
   timestamp_ns 为纳秒，时基由 time_origin 指明。 */
typedef struct IRFrameStamp {
    uint64_t sequence, driver_sequence;
    int64_t timestamp_ns;
    uint32_t lost_frames, fields;
    int32_t time_origin;
} IRFrameStamp;

/* 一个进程使用一个采集源。除 RequestStop 外，采集和流程调用使用打开源的线程。
   RAW 目录的 valid_bits 传 0 时读取索引。实时源的 valid_bits 由调用方按探测器配置提供，
   不清楚时传 0。Windows 可传 IR_DIRECTSHOW 和相机名；Linux 可传 IR_V4L2 和 /dev/videoN。
   FX3 必须提供 16 位原始传输格式：Windows DirectShow RGB565/YUY2，Linux V4L2 YUYV。
   成功时 actual 返回宽、高和有效位。 */
IR_STATION_API int32_t IR_OpenFrames(int32_t backend, const char *name_or_directory,
    int32_t valid_bits, IRGeometry *actual, char *err, int32_t err_bytes);
/* 回放从第 0 帧开始，每次停止后可重新启动。 */
IR_STATION_API int32_t IR_StartFrames(char *err, int32_t err_bytes);
/* timeout_ms 为实时来源的单帧等待上限，RAW 按文件 I/O 速度读取。
 * pixels 至少有 count*width*height 个 uint16_t，stamps 至少有 count 条记录。
 * 发生错误时，只有首 completed 帧及其记录有效。所有输出缓冲区由调用方持有。
 */
IR_STATION_API int32_t IR_ReadFrames(int32_t count, int32_t timeout_ms, uint16_t *pixels,
    int64_t pixel_capacity, IRFrameStamp *stamps, int32_t stamp_capacity, int32_t *completed,
    char *err, int32_t err_bytes);
/* 可以从其他线程请求停止。此调用返回后，采集线程仍须完成当前操作并释放资源。 */
IR_STATION_API void IR_RequestStop(void);
/* 在采集线程停止源，保留打开状态，可再次启动。 */
IR_STATION_API int32_t IR_StopFrames(char *err, int32_t err_bytes);
/* 释放采集源，之后可打开另一来源。 */
IR_STATION_API int32_t IR_CloseFrames(char *err, int32_t err_bytes);

typedef struct IRNoiseRecipe {
    const char *recipe_id;                 /* 产品配方标识或版本，UTF-8 */
    IRHandle detector, blackbody;         /* 离线均填 0。blackbody 可指定已打开的黑体。 */
    int32_t integration_us, frame_hz, empty_us;
    double temperature_c, tolerance_c;
    int32_t stable_window_ms, stable_timeout_ms, require_device_stable;
    int32_t frames, frame_timeout_ms;
} IRNoiseRecipe;
typedef struct IRNoiseResult {
    int32_t completed;
    double mean_dn, temporal_noise_dn;      /* 像元样本标准差的全图均值 */
} IRNoiseResult;
/* 在打开源的线程同步运行配方。采集源已打开且未启动，recipe 和 result 由调用方持有。
   recipe_id 使用 UTF-8，frames >= 2。指定黑体时，先设温并等待稳定，再从当前采集源取帧。
   温度和温差单位 °C，稳定窗口和等待上限单位 ms，以设温回读成功为起点。通信调用结束后检查超时和取消。
   detector 现阶段填 0。
   结果为完成帧数、平均 DN 和样本时域噪声 DN。
   目录传 NULL 时只计算。传 UTF-8 路径时须是同级父目录下尚未存在的新目录。
   失败时 completed 仍报告已连续交付的帧数，指标字段无效。 */
IR_STATION_API int32_t IR_RunNoise(const IRNoiseRecipe *recipe,
    const char *new_output_directory, IRNoiseResult *result, char *err, int32_t err_bytes);

#ifdef __cplusplus
}
#endif
