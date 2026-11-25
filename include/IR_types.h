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
    int32_t reserved;
} IRFrameStamp;

#ifdef __cplusplus
}
#endif

#endif /* IR_TYPES_H */
