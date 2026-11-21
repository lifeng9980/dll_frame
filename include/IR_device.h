#pragma once
#include "IR_station.h"

#ifdef __cplusplus
extern "C" {
#endif

enum IRDeviceKind { IR_PCN7LT = 2, IR_DCN1000 = 3, IR_IT6332A = 4 };

typedef struct IRBlackbodyReading {
    double actual_c;    /* 实际温度，°C */
    double setpoint_c;  /* 控制器回读的设定温度，°C */
    int32_t stable;     /* 设备稳定标志，0 或 1 */
} IRBlackbodyReading;

/* PCN7-LT 配置 JSON 含 host，可选 tcp_port（默认 2101）、head（默认 -1）
 * 和 expected_head_serial。DCN1000 配置含 port，使用 9600 8N1。
 * IT6332A 配置含 port，可选 baud_rate（默认 115200）。
 * 黑体 timeout_ms 默认 3000，IT6332A 默认 1200；单位均为毫秒。
 * 打开时确认设备身份。handle 由调用方提供，失败时写 0，成功后由 IR_CloseDevice 释放。
 * 字符串使用 UTF-8。操作为同步调用。同一设备同时只执行一个操作，并发读写或流程预约时返回 IR_BUSY。
 * 设备预约只约束当前 SDK 进程内的调用；其他进程的占用由设备或驱动决定。
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

typedef struct IRPowerReading {
    double setpoint_v;
    double current_limit_a;
    double measured_v;
    double measured_a;
    int32_t output_enabled;
} IRPowerReading;

/* IT6332A 串口配置 JSON 含 port，可选 baud_rate（默认 115200）和 timeout_ms（默认 1200）。
 * 打开时确认 *IDN? 返回 IT6332A，并切换到远程控制。每个操作先确认所选通道。
 * 读数单位为 V 和 A。通道号由设备支持范围 1～3 校验。
 * 指定设备正被流程预约时，本组操作返回 IR_BUSY。
 */
IR_STATION_API int32_t IR_SetPowerChannel(IRHandle handle, int32_t channel,
    double voltage_v, double current_limit_a, IRPowerReading *readback,
    char *err, int32_t err_bytes);
IR_STATION_API int32_t IR_SetPowerOutput(IRHandle handle, int32_t channel,
    int32_t enabled, IRPowerReading *readback, char *err, int32_t err_bytes);
IR_STATION_API int32_t IR_ReadPower(IRHandle handle, int32_t channel,
    IRPowerReading *reading, char *err, int32_t err_bytes);

#ifdef __cplusplus
}
#endif
