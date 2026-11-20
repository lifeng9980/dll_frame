#pragma once
#include "IR_station.h"

#ifdef __cplusplus
extern "C" {
#endif

enum IRDeviceKind { IR_PCN7LT = 2, IR_DCN1000 = 3 };

typedef struct IRBlackbodyReading {
    double actual_c;    /* 实际温度，°C */
    double setpoint_c;  /* 控制器回读的设定温度，°C */
    int32_t stable;     /* 设备稳定标志，0 或 1 */
} IRBlackbodyReading;

/* PCN7-LT 配置 JSON 含 host，可选 tcp_port（默认 2101）、head（默认 -1）
 * 和 expected_head_serial。DCN1000 配置含 port，使用 9600 8N1。
 * 可选 timeout_ms 以毫秒计，默认 3000，覆盖一组指令写入与应答。
 * 打开时确认设备身份。handle 由调用方提供，失败时写 0，成功后由 IR_CloseDevice 释放。
 * 字符串使用 UTF-8。操作为同步调用。同一设备的读写由 SDK 串行处理。
 */
IR_STATION_API int32_t IR_OpenDevice(int32_t kind, const char *config_json_utf8, IRHandle *handle,
    char *err, int32_t err_bytes);

/* 释放句柄。需要停止加热或修改设定温度时，由调用方先执行对应工位流程。 */
IR_STATION_API int32_t IR_CloseDevice(IRHandle handle, char *err, int32_t err_bytes);

/* 写入目标温度，单位 °C。以控制器读回确认结果。
 * readback_c 由调用方提供，成功时有效。写入超时后，设备状态以随后读回为准。
 */
IR_STATION_API int32_t IR_SetBlackbody(IRHandle handle, double target_c, double *readback_c,
    char *err, int32_t err_bytes);

/* 读取黑体实际温度、设定值和设备稳定标志。reading 由调用方提供，成功时有效。 */
IR_STATION_API int32_t IR_ReadBlackbody(IRHandle handle, IRBlackbodyReading *reading,
    char *err, int32_t err_bytes);

#ifdef __cplusplus
}
#endif
