#ifndef IR_POWER_DRIVER_ABI_H
#define IR_POWER_DRIVER_ABI_H

/*
 * Pure C99/C11 narrow external power-supply driver ABI loaded by IR_station.
 * All strings are UTF-8. All C ABI status returns use int32_t values from IRStatus.
 *
 * Lifetimes:
 * - out_desc and descriptor strings (driver_id) must remain valid until library unload.
 * - config_json_utf8 is valid only during the call.
 * - IRPowerDriverHandle is owned by the driver and must be closed via desc->close().
 */

#include <stdint.h>
#include "IR_device.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#define IR_POWER_DRIVER_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define IR_POWER_DRIVER_EXPORT __attribute__((visibility("default")))
#else
#define IR_POWER_DRIVER_EXPORT
#endif

#define IR_POWER_DRIVER_ABI_VERSION 1u
#define IR_POWER_DRIVER_QUERY_SYMBOL "ir_power_driver_query"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IRPowerCapabilities {
    IR_POWER_CAP_READ = 1u << 0,
    IR_POWER_CAP_OUTPUT = 1u << 1,
    IR_POWER_CAP_SET = 1u << 2
} IRPowerCapabilities;

typedef void *IRPowerDriverHandle;

typedef struct IRPowerDriverDescriptor {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *driver_id;
    uint32_t capabilities;
    int32_t min_channel;
    int32_t max_channel;
    int32_t (*validate_config)(const char *config_json_utf8, char *err, int32_t err_bytes);
    int32_t (*open)(const char *config_json_utf8, IRPowerDriverHandle *out_handle,
                    char *identity_utf8, int32_t identity_bytes,
                    char *err, int32_t err_bytes);
    int32_t (*read)(IRPowerDriverHandle handle, int32_t channel,
                    IRPowerReading *out_reading, char *err, int32_t err_bytes);
    int32_t (*set_output)(IRPowerDriverHandle handle, int32_t channel, int32_t enabled,
                          IRPowerReading *out_readback, char *err, int32_t err_bytes);
    int32_t (*set_channel)(IRPowerDriverHandle handle, int32_t channel,
                           double voltage_v, double current_limit_a,
                           IRPowerReading *out_readback, char *err, int32_t err_bytes);
    int32_t (*close)(IRPowerDriverHandle handle, char *err, int32_t err_bytes);
} IRPowerDriverDescriptor;

typedef int32_t (*IRPowerDriverQueryFn)(const IRPowerDriverDescriptor **out_desc,
                                        char *err, int32_t err_bytes);

#ifdef __cplusplus
}
#endif

#endif /* IR_POWER_DRIVER_ABI_H */
