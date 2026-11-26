#ifndef IR_TYPES_H
#define IR_TYPES_H

#include <stdint.h>
#include "IR_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IRFrameFields {
    IR_HAS_SEQUENCE = 1,
    IR_HAS_DRIVER_SEQUENCE = 2,
    IR_HAS_LOSS = 4,
    IR_SEQUENCE_IS_CALLBACK = 8,
    IR_PIXELS_INVERTED = 16
} IRFrameFields;

typedef enum IRTimeOrigin {
    IR_TIME_UNKNOWN,
    IR_TIME_SOURCE,
    IR_TIME_HOST,
    IR_TIME_RECORDED_MONOTONIC,
    IR_TIME_RECORDED_RELATIVE
} IRTimeOrigin;

typedef enum IRDataType {
    IR_TYPE_U8 = 1,
    IR_TYPE_U16 = 2,
    IR_TYPE_U32 = 3,
    IR_TYPE_F64 = 4
} IRDataType;

/* 连续数组视图，像素按 [frame][row][column] 紧密排列。内存由调用方持有。 */
typedef struct IRArrayView {
    const void *data;
    uint64_t byte_size;
    int32_t data_type;
    int32_t width;
    int32_t height;
    int32_t frames;
} IRArrayView;

/* 实际帧几何，宽高以像元计，valid_bits 描述采集码的有效位宽；0 表示来源没有给出。 */
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
    int32_t reserved;
} IRFrameStamp;

/* 可写连续数组视图，容量以字节计，dimensions/data_type 描述调用方缓冲区。 */
typedef struct IRMutableArrayView {
    void *data;
    uint64_t byte_size;
    int32_t data_type;
    int32_t width;
    int32_t height;
    int32_t frames;
} IRMutableArrayView;

/* 打开采集源后返回的像素表示和几何；data_type 与 valid_bits 含义不同。 */
typedef struct IRFrameSourceInfo {
    IRGeometry geometry;
    int32_t data_type;
} IRFrameSourceInfo;

/* 采集批次视图。IR_ReadFrames 将 pixels.frames 当作容量并返回实际完成帧数。 */
typedef struct IRFrameBatchView {
    IRMutableArrayView pixels;
    int32_t valid_bits;
    IRFrameStamp *stamps;
    int32_t stamp_capacity;
} IRFrameBatchView;

#ifdef __cplusplus
}
#endif

#endif /* IR_TYPES_H */
