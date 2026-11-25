#pragma once
#include "IR_status.h"
#include "IR_types.h"

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
/* 版本字符串为 UTF-8，由库持有，库卸载前有效。 */
IR_STATION_API const char *IR_StationVersion(void);

typedef enum IRBackend {
    IR_DIRECTSHOW = 1, /* Windows DirectShow device name, e.g. FX3. */
    IR_RAW_REPLAY = 2,
    IR_V4L2 = 3, /* Linux /dev/videoN, for the FX3 YUYV 16-bit raw stream. */
    IR_NI_IMAQ = 4 /* Reserved for future NI-IMAQ support; currently returns IR_UNSUPPORTED. */
} IRBackend;

typedef struct IRCaptureStats {
    uint64_t frames_received;     /* 后端累计交付到 SDK 队列/采集层的帧数 */
    uint64_t frames_delivered;    /* 累计成功交付给测量调用方的帧数 */
    uint64_t frames_dropped;      /* 队列溢出或后端报告的丢帧累计 */
    uint64_t preview_frames;      /* 预览快照槽累计更新帧数 */
    int32_t queue_capacity;       /* 测量有界队列容量（默认 64） */
    int32_t queue_depth;          /* 当前测量队列中待取帧数 */
    int32_t width;                /* 当前打开采集源宽度，未打开为 0 */
    int32_t height;               /* 当前打开采集源高度，未打开为 0 */
    int32_t running;              /* 采集源是否运行中（0 或 1） */
    int32_t reserved;             /* 对齐保留字段，置 0 */
    double estimated_fps;         /* 主机单调时钟估算实测帧率（Hz） */
} IRCaptureStats;

/* 一个进程使用一个采集源。除 RequestStop 外，采集和流程调用使用打开源的线程。
   打开、启动、读取、停止和关闭必须由同一个工作线程执行；并发操作返回 IR_BUSY。
   RAW 目录的 valid_bits 传 0 时读取索引。实时源的 valid_bits 由调用方按探测器配置提供，
   不清楚时传 0。Windows 可传 IR_DIRECTSHOW 和相机名；Linux 可传 IR_V4L2 和 /dev/videoN。
   IR_NI_IMAQ 为保留编号，当前构建返回 IR_UNSUPPORTED。
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
/* 清空底层已排队帧并重置连续性检查起点，同时按 timeout_ms 读取并丢弃 discard_frames 个稳定过渡帧（不因组间停顿触发 IR_FRAME_GAP）。
 * 适用于长时多组采集中保持采集源不断流、按 Burst 分段读取。discarded 可为 NULL。
 */
IR_STATION_API int32_t IR_FlushFrames(int32_t discard_frames, int32_t timeout_ms,
    int32_t *discarded, char *err, int32_t err_bytes);
/* 可以从其他线程请求停止当前采集或自动流程。等待取帧时分段检查取消，
   同步设备事务结束后也会检查。原工作线程负责停流和收尾。空闲时的请求不影响下一次运行。 */
IR_STATION_API void IR_RequestStop(void);
/* 在采集线程停止源，保留打开状态，可再次启动。 */
IR_STATION_API int32_t IR_StopFrames(char *err, int32_t err_bytes);
/* 释放采集源，之后可打开另一来源。 */
IR_STATION_API int32_t IR_CloseFrames(char *err, int32_t err_bytes);

/* 配置测量有界 FrameQueue 容量（2~4096，默认 64）。须在未启动采集时调用，运行中返回 IR_BUSY。 */
IR_STATION_API int32_t IR_SetFrameQueueCapacity(int32_t capacity, char *err, int32_t err_bytes);

/* 无阻塞读取最新一帧预览快照（丢旧保新，不消耗测量有界队列，也不因配方/测量取帧返回 IR_BUSY）。
 * 暂无帧时返回 IR_OK 且 *has_frame = 0；有帧但 pixel_capacity 不足时返回 IR_BUFFER_TOO_SMALL（geometry 仍写入）。
 * geometry 与 stamp 可为 NULL。
 */
IR_STATION_API int32_t IR_TryReadLatestFrame(uint16_t *pixels, int64_t pixel_capacity,
    IRGeometry *geometry, IRFrameStamp *stamp, int32_t *has_frame, char *err, int32_t err_bytes);

/* 获取当前采集会话统计与 FrameQueue 诊断快照（线程安全、非阻塞）。 */
IR_STATION_API int32_t IR_GetCaptureStats(IRCaptureStats *stats, char *err, int32_t err_bytes);

#ifdef __cplusplus
}
#endif
