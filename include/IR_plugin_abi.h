#ifndef IR_PLUGIN_ABI_H
#define IR_PLUGIN_ABI_H

/*
 * Pure C99/C11 plugin ABI for external metric operators loaded by IR_station.
 * All strings are UTF-8. All C ABI status returns use int32_t values from IRStatus.
 * C++ plugins must catch exceptions inside extern "C" callbacks (see IR_plugin_cxx.hpp).
 *
 * Lifetimes:
 * - out_list and IRMetricPlugin descriptor strings must remain valid until library unload.
 * - IRPrepareCallbacks, IRHostCallbacks, and params_json are valid only during the call.
 * - IRSlotView array and opaque pointers, stamps, and alloc_* pointers are valid only during execute().
 */

#include <stddef.h>
#include <stdint.h>
#include "IR_status.h"
#include "IR_types.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#define IR_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define IR_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define IR_PLUGIN_EXPORT
#endif

#define IR_PLUGIN_ABI_VERSION 2u
#define IR_PLUGIN_QUERY_SYMBOL "ir_plugin_query"

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

typedef struct IRSlotView {
    int32_t slot_kind;
    /* Populated for Batch, Map, and Mask slots. */
    IRArrayView array;
    /* Effective raw code bit width for Batch; zero for derived arrays and opaque slots. */
    int32_t valid_bits;
    /* Present only for Batch; NULL for maps, masks, and opaque slots. */
    const IRFrameStamp *stamps;
    /* Populated only for non-array slots such as DriftContext. */
    const void *opaque_data;
    uint64_t opaque_byte_size;
} IRSlotView;

typedef struct IRPrepareCallbacks {
    void *prepare_ctx;
    int32_t (*require_input_slot)(void *prepare_ctx, const char *input_key,
                                  int32_t expected_slot_kind, int32_t required,
                                  int32_t *out_is_bound);
    int32_t (*bind_output_slot)(void *prepare_ctx, const char *output_key,
                                int32_t slot_kind, int32_t required,
                                int32_t *out_is_bound);
    int32_t (*declare_scalar_output)(void *prepare_ctx, const char *suffix_utf8,
                                     const char *unit_utf8);
} IRPrepareCallbacks;

typedef struct IRHostCallbacks {
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
} IRHostCallbacks;

typedef struct IRMetricPlugin {
    uint32_t abi_version;
    const char *plugin_id;
    const char *method_name;
    const char *default_input_key;
    int32_t (*prepare)(const IRPrepareCallbacks *prep, const char *params_json,
                       char *err, int32_t err_bytes);
    int32_t (*execute)(const IRHostCallbacks *host, IRGeometry geometry,
                       const char *params_json, char *err, int32_t err_bytes);
} IRMetricPlugin;

typedef int32_t (*IRPluginQueryFn)(const IRMetricPlugin **out_list,
                                     uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif
