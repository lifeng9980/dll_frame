#ifndef IR_PLUGIN_ABI_H
#define IR_PLUGIN_ABI_H

/*
 * Pure C plugin ABI for external metric operators loaded by IR_station.
 * All strings are UTF-8. All C ABI status returns use int32_t values from IRStatus.
 * C++ plugins must catch exceptions inside extern "C" callbacks (see ir_plugin_guard).
 */

#include <stddef.h>
#include <stdint.h>
#include "IR_status.h"
#include "IR_station.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#define IR_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define IR_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define IR_PLUGIN_EXPORT
#endif

#define IR_PLUGIN_ABI_VERSION_1 1u
#define IR_PLUGIN_QUERY_V1_SYMBOL "ir_plugin_query_v1"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IRPluginSlotKind {
    IR_PLUGIN_SLOT_BATCH = 1,
    IR_PLUGIN_SLOT_DOUBLE_MAP = 2,
    IR_PLUGIN_SLOT_BYTE_MASK = 3,
    IR_PLUGIN_SLOT_DRIFT_CONTEXT = 4
} IRPluginSlotKind;

typedef enum IRPluginLogLevel {
    IR_PLUGIN_LOG_DEBUG = 0,
    IR_PLUGIN_LOG_INFO = 1,
    IR_PLUGIN_LOG_WARN = 2,
    IR_PLUGIN_LOG_ERROR = 3
} IRPluginLogLevel;

typedef struct IRPluginHeader {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *plugin_id;
    const char *plugin_version;
    const char *author;
} IRPluginHeader;

typedef struct IRSlotView {
    uint32_t struct_size;
    int32_t slot_kind;
    int32_t width;
    int32_t height;
    int32_t frames;
    int32_t valid_bits;
    const void *data;
    size_t byte_size;
    const IRFrameStamp *stamps;
} IRSlotView;

typedef struct IRPrepareCallbacksV1 {
    uint32_t struct_size;
    void *prepare_ctx;
    int32_t (*require_input_slot)(void *prepare_ctx, const char *input_key,
                                  int32_t expected_slot_kind, int32_t required,
                                  int32_t *out_is_bound);
    int32_t (*bind_output_slot)(void *prepare_ctx, const char *output_key,
                                int32_t slot_kind, int32_t required,
                                int32_t *out_is_bound);
    int32_t (*declare_scalar_output)(void *prepare_ctx, const char *suffix_utf8,
                                     const char *unit_utf8);
} IRPrepareCallbacksV1;

typedef struct IRHostCallbacksV1 {
    uint32_t struct_size;
    void *host_ctx;
    int32_t (*has_slot)(void *host_ctx, const char *io_key);
    int32_t (*get_slot)(void *host_ctx, const char *input_key, IRSlotView *out_view);
    int32_t (*alloc_double_map)(void *host_ctx, const char *output_key,
                                  int32_t width, int32_t height, double **out_data);
    int32_t (*alloc_byte_mask)(void *host_ctx, const char *output_key,
                                 int32_t width, int32_t height, uint8_t **out_data);
    int32_t (*emit_scalar)(void *host_ctx, const char *suffix_utf8, double value);
    int32_t (*is_cancelled)(void *host_ctx);
    void (*log_message)(void *host_ctx, int32_t level, const char *message_utf8);
} IRHostCallbacksV1;

typedef struct IRMetricPluginV1 {
    IRPluginHeader header;
    const char *method_name;
    int32_t method_version;
    int32_t reserved;
    const char *default_input_key;
    int32_t (*prepare)(const IRPrepareCallbacksV1 *prep, const char *params_json,
                       char *err, int32_t err_bytes);
    int32_t (*check_geometry)(const char *params_json, IRGeometry geometry,
                              char *err, int32_t err_bytes);
    int32_t (*execute)(const IRHostCallbacksV1 *host, const char *params_json,
                       char *err, int32_t err_bytes);
} IRMetricPluginV1;

typedef int32_t (*IRPluginQueryV1Fn)(const IRMetricPluginV1 **out_list,
                                     uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <cstring>
#include <exception>

/*
 * Helper for C++ plugin implementations so exceptions never unwind across
 * the extern "C" ABI boundary.
 */
template <class Fn>
inline int32_t ir_plugin_guard(char *err, int32_t err_bytes, Fn &&fn) noexcept
{
    if (err && err_bytes > 0)
        err[0] = '\0';
    auto copy_err = [err, err_bytes](const char *msg) noexcept {
        if (!err || err_bytes <= 0 || !msg)
            return;
        size_t len = std::strlen(msg);
        const size_t max_len = static_cast<size_t>(err_bytes - 1);
        if (len > max_len)
            len = max_len;
        std::memcpy(err, msg, len);
        err[len] = '\0';
    };
    try {
        return fn();
    } catch (const std::exception &e) {
        copy_err(e.what());
        return IR_COMPUTATION_FAILED;
    } catch (...) {
        copy_err("Unhandled C++ exception in plugin");
        return IR_COMPUTATION_FAILED;
    }
}
#endif

#endif /* IR_PLUGIN_ABI_H */
