#pragma once
#include "IR_station.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IRDeviceKind {
    IR_PCN7LT = 2,
    IR_DCN1000 = 3,
    IR_IT6332A = 4,
    IR_OVEN_GWS_STEN = 5,
    IR_DMM6500 = 6,
    IR_DETECTOR_SERIAL = 7,
    IR_COOLER_MODBUS = 8
} IRDeviceKind;

typedef struct IRBlackbodyReading {
    double actual_c;    /* 实际温度，°C */
    double setpoint_c;  /* 控制器回读的设定温度，°C */
    int32_t stable;     /* 设备稳定标志，0 或 1 */
    int32_t reserved;   /* 对齐保留，置 0 */
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
    char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* 释放句柄。需要停止加热或修改设定温度时，由调用方先执行对应工位流程。 */
IR_STATION_API int32_t IR_CloseDevice(IRHandle handle, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* 写入目标温度，单位 °C。以控制器读回确认结果。
 * readback_c 由调用方提供，成功时有效。写入超时后，设备状态以随后读回为准。
 */
IR_STATION_API int32_t IR_SetBlackbody(IRHandle handle, double target_c, double *readback_c,
    char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* 读取黑体实际温度、设定值和设备稳定标志。reading 由调用方提供，成功时有效。 */
IR_STATION_API int32_t IR_ReadBlackbody(IRHandle handle, IRBlackbodyReading *reading,
    char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* 等待已打开黑体在 tolerance_c 容差内连续保持 stable_window_ms 毫秒（可选要求设备自身稳定标志）。
 * 支持从其他线程调用 IR_RequestStop() 中止等待；reading 不可为 NULL，成功时返回最后一次读数。
 */
IR_STATION_API int32_t IR_WaitBlackbodyStable(IRHandle handle, double target_c, double tolerance_c,
    int32_t stable_window_ms, int32_t timeout_ms, int32_t require_device_stable,
    IRBlackbodyReading *reading, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

typedef struct IRPowerReading {
    double setpoint_v;
    double current_limit_a;
    double measured_v;
    double measured_a;
    int32_t output_enabled;
    int32_t reserved;   /* 对齐保留，置 0 */
} IRPowerReading;

typedef struct IRPowerCurrentStableSpec {
    double expected_voltage_v;        /* <= 0 时不校验电压，> 0 时要求 |measured_v - expected_voltage_v| <= voltage_tolerance_v */
    double voltage_tolerance_v;       /* 允许电压偏差，V（例如 0.15） */
    double max_current_a;             /* 过流硬上限，A（> 0 时超过立即报 IR_IO_ERROR） */
    double startup_threshold_a;       /* 启动确认阈值，A（> 0 且 startup_required_samples > 0 时生效） */
    double stable_threshold_a;        /* 稳定目标上限电流，A（须连续低于此值） */
    double max_variation_a;           /* 稳定窗口极差上限，A（max - min <= max_variation_a） */
    int32_t startup_required_samples; /* 启动确认连续样本数（0 表示跳过启动阶段） */
    int32_t startup_timeout_ms;       /* 启动阶段超时，ms（startup_required_samples > 0 时须 >= 1） */
    int32_t stable_required_samples;  /* 稳定窗口连续样本数，>= 1 */
    int32_t sample_interval_ms;       /* 采样轮询间隔，ms，>= 0 */
    int32_t timeout_ms;               /* 总等待超时，ms，>= 1 */
    int32_t reserved;                 /* 对齐保留，置 0 */
} IRPowerCurrentStableSpec;

/* IT6332A 配置 JSON 含串口 port（baud_rate 默认 115200，可选 data_bits/parity/stop_bits）
 * 或 TCP host/tcp_port（默认 5026）二选一，以及可选 timeout_ms（默认 1200）。
 * 打开时确认 *IDN? 返回 IT6332A，并切换到远程控制。每个操作先确认所选通道。
 * 读数单位为 V 和 A。通道号由设备支持范围 1～3 校验。
 * 指定设备正被流程预约时，本组操作返回 IR_BUSY。
 */
IR_STATION_API int32_t IR_SetPowerChannel(IRHandle handle, int32_t channel,
    double voltage_v, double current_limit_a, IRPowerReading *readback,
    char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;
IR_STATION_API int32_t IR_SetPowerOutput(IRHandle handle, int32_t channel,
    int32_t enabled, IRPowerReading *readback, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;
IR_STATION_API int32_t IR_ReadPower(IRHandle handle, int32_t channel,
    IRPowerReading *reading, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* 等待已开启电源通道先通过可选的负载启动确认，再连续保持电流稳定低于 stable_threshold_a。
 * 支持从其他线程调用 IR_RequestStop() 中止等待；spec 与 reading 不可为 NULL，成功时返回最终读数。
 */
IR_STATION_API int32_t IR_WaitPowerCurrentStable(IRHandle handle, int32_t channel,
    const IRPowerCurrentStableSpec *spec, IRPowerReading *reading,
    char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* 枚举本机可用串口，输出 UTF-8 JSON 数组（例如 [{"port":"COM1","device":"..."}]）。
 * required_bytes 可为 NULL；缓冲区不足时返回 IR_BUFFER_TOO_SMALL 并写入所需总字节数（含结尾 '\0'）。
 */
IR_STATION_API int32_t IR_ListSerialPorts(char *json_out, int32_t capacity,
    int32_t *required_bytes, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* 枚举指定采集后端的可用设备，输出 UTF-8 JSON 数组（例如 [{"name":"FX3","device_path":"..."}]）。
 * Windows 支持 IR_DIRECTSHOW，Linux 支持 IR_V4L2；其余已知后端返回 IR_UNSUPPORTED。
 * required_bytes 可为 NULL；缓冲区不足时返回 IR_BUFFER_TOO_SMALL 并写入所需总字节数（含结尾 '\0'）。
 */
IR_STATION_API int32_t IR_ListCaptureDevices(int32_t backend, char *json_out, int32_t capacity,
    int32_t *required_bytes, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* ======================== 温箱 (GWS STEN) ======================== */

typedef enum IROvenMode {
    IR_OVEN_MODE_UNKNOWN = 0,
    IR_OVEN_MODE_STANDBY = 1,
    IR_OVEN_MODE_CONSTANT = 2,
    IR_OVEN_MODE_PROGRAM = 3,
    IR_OVEN_MODE_OTHER = 4
} IROvenMode;

typedef struct IROvenReading {
    double actual_c;      /* 当前箱体温度，°C */
    double setpoint_c;    /* 控制器回读设定温度，°C */
    int32_t mode;         /* IROvenMode */
    int32_t alarm_active; /* 0=无告警，1=存在告警 */
} IROvenReading;

/* GWS STEN 烘箱配置 JSON 含 serial port（9600 默认）或 TCP host/tcp_port（二选一），
 * 可选 address（默认 1）、delimiter（crlf/cr/lf，默认 crlf）、min_setpoint_c（默认 -45）、
 * max_setpoint_c（默认 70）及 timeout_ms（默认 2000）。
 */
IR_STATION_API int32_t IR_SetOven(IRHandle handle, double target_c, double *readback_c,
    char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;
IR_STATION_API int32_t IR_ReadOven(IRHandle handle, IROvenReading *reading,
    char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;
IR_STATION_API int32_t IR_StopOven(IRHandle handle, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;
IR_STATION_API int32_t IR_WaitOvenStable(IRHandle handle, double target_c, double tolerance_c,
    int32_t stable_window_ms, int32_t timeout_ms, int32_t require_no_alarm,
    IROvenReading *reading, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* ======================== 万用表 (Keithley DMM6500 SCPI) ======================== */

typedef enum IRDmmMode {
    IR_DMM_MODE_ANY = 0,
    IR_DMM_MODE_DCV = 1,
    IR_DMM_MODE_DCI = 2,
    IR_DMM_MODE_RES = 3,
    IR_DMM_MODE_DIODE = 4
} IRDmmMode;

typedef struct IRDmmReading {
    double value;         /* 测量数值（V / A / Ohm，二极管模式为正向压降 V） */
    int32_t mode;         /* IRDmmMode */
    int32_t valid;        /* 1=有效 */
} IRDmmReading;

/* DMM6500 配置 JSON 含 port（默认 115200 baud）或 host/tcp_port（默认 5025）二选一及可选 timeout_ms（默认 2000）。 */
IR_STATION_API int32_t IR_SetDmmMode(IRHandle handle, int32_t mode, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;
IR_STATION_API int32_t IR_ReadDmm(IRHandle handle, int32_t expected_mode,
    IRDmmReading *reading, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* ======================== 探测器串口控制 (CC 帧协议) ======================== */

typedef struct IRDetectorCurrentReading {
    uint32_t raw_code;          /* 24-bit ADC 原始码 */
    uint32_t reserved;          /* 对齐保留，置 0 */
    double sense_voltage_v;     /* 采样电阻端电压，V */
    double current_ma;          /* 换算电流，mA（R = 1 Ohm） */
} IRDetectorCurrentReading;

/* 探测器串口控制配置 JSON 含 port（默认 115200 baud）或 host/tcp_port（默认 2102）二选一，
 * 可选 timeout_ms（默认 1200）与 inter_command_ms（默认 0）。
 */
IR_STATION_API int32_t IR_SetDetectorTiming(IRHandle handle, int32_t integration_us,
    int32_t frame_hz, int32_t empty_us, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;
IR_STATION_API int32_t IR_SetDetectorGpol(IRHandle handle, double gpol_v,
    char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;
IR_STATION_API int32_t IR_SetDetectorRegister(IRHandle handle, uint8_t command_code,
    const uint8_t *payload, int32_t payload_bytes, char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;
IR_STATION_API int32_t IR_MeasureDetectorCurrent(IRHandle handle, int32_t gain_code,
    uint16_t channel_code, IRDetectorCurrentReading *reading,
    char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

/* ======================== 制冷机监测 (Modbus RTU) ======================== */

typedef struct IRCoolerReading {
    uint32_t raw_value;         /* Modbus 保持寄存器 32 位无符号值 */
    int32_t valid;              /* 1=有效 */
    double voltage_mv;          /* 温度电压，mV = raw_value / 100.0 */
} IRCoolerReading;

/* 制冷机 Modbus RTU 配置 JSON 含 port（默认 9600 baud 8N2）或 host/tcp_port（默认 5020）二选一，
 * 可选 slave_address（1..247，默认 1）、register_address（0..65535，默认 0x7A52）与 timeout_ms（默认 2000）。
 */
IR_STATION_API int32_t IR_ReadCooler(IRHandle handle, IRCoolerReading *reading,
    char *err, int32_t err_bytes) IR_STATION_NOEXCEPT;

#ifdef __cplusplus
}
#endif
