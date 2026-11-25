#pragma once
#include "IR_station.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 检查工位 JSON 与流程 JSON 的结构和步骤关系，不打开设备、不创建文件。
 * 两个 JSON 参数均为 UTF-8。此接口可供 C、C# 和 Python 在运行前校验配置。
 */
IR_STATION_API int32_t IR_CheckRecipeJson(const char *station_json_utf8,
    const char *recipe_json_utf8, char *err, int32_t err_bytes);

enum IRRecipeStage {
    IR_RECIPE_STAGE_NONE = 0,
    IR_RECIPE_STAGE_VALIDATE = 1,
    IR_RECIPE_STAGE_RESERVE = 2,
    IR_RECIPE_STAGE_PREPARE = 3,
    IR_RECIPE_STAGE_INSTRUMENT = 4,
    IR_RECIPE_STAGE_CAPTURE = 5,
    IR_RECIPE_STAGE_ANALYZE = 6,
    IR_RECIPE_STAGE_ARCHIVE = 7,
    IR_RECIPE_STAGE_CLEANUP = 8
};

typedef struct IRRecipeResult {
    int32_t completed_frames;   /* 失败时也报告已连续交付的帧数。 */
    int32_t failed_stage;       /* IRRecipeStage，成功时为 NONE。 */
    int32_t failed_step_index;  /* recipe.steps 的下标，从 0 开始；无对应步骤时为 -1。 */
    int32_t cleanup_status;    /* 第一个收尾错误的 IRStatus；收尾成功或无需收尾时为 IR_OK。 */
} IRRecipeResult;

/* 在当前线程执行一份有序流程。成功测量后在 new_output_directory 中写入 frames.raw、
 * frames.csv 和 result.json。路径必须是现存父目录下尚不存在的新目录，字符串使用 UTF-8。
 * IR_OK 表示测量完整完成；产品限值未满足时仍返回 IR_OK，result.json 的 run_status 为 NG。
 * 设备只在本配方引用时连接。同一进程一次只运行一条配方，其间其他采集调用返回 IR_BUSY。
 * 其他线程可以调用 IR_RequestStop。当前同步事务或计算结束后检查停止，由原线程完成收尾。
 * result 由调用方持有，可传 NULL。执行和收尾均失败时，返回执行错误，收尾状态单独写入 result。
 */
IR_STATION_API int32_t IR_RunRecipeJson(const char *station_json_utf8,
    const char *recipe_json_utf8, const char *new_output_directory_utf8,
    IRRecipeResult *result, char *err, int32_t err_bytes);

enum IRRecipeStepEventKind {
    IR_STEP_EVENT_STARTED = 1,
    IR_STEP_EVENT_COMPLETED = 2,
    IR_STEP_EVENT_FAILED = 3
};

typedef struct IRRecipeStepEvent {
    uint32_t struct_size;
    int32_t event_kind;        /* IRRecipeStepEventKind */
    int32_t step_index;        /* recipe.steps 下标，从 0 开始；无对应步骤时为 -1。 */
    int32_t stage;             /* IRRecipeStage */
    int32_t status;            /* IRStatus */
    int32_t reserved;          /* 对齐保留字段，置 0。 */
    double duration_ms;        /* 步骤耗时（毫秒）；STARTED 时为 0。 */
    const char *step_id_utf8;  /* 仅在本次回调调用期间有效。 */
    const char *step_type_utf8;/* 仅在本次回调调用期间有效。 */
    const char *detail_utf8;   /* 失败详情或附加说明，仅在本次回调调用期间有效。 */
} IRRecipeStepEvent;

/* 在 IR_RunRecipeJsonEx 调用线程同步触发；严禁向 C ABI 边界外抛出 C++ 异常。 */
typedef void (*IRRecipeStepCallback)(const IRRecipeStepEvent *event, void *user_data);

typedef struct IRRecipeRunOptions {
    uint32_t struct_size;
    uint32_t flags;            /* 保留标志位，必须为 0。 */
    IRRecipeStepCallback on_step_event;
    void *user_data;
} IRRecipeRunOptions;

/* 与 IR_RunRecipeJson 行为一致，额外支持通过 options 接收步骤级运行事件回调（可传 NULL）。 */
IR_STATION_API int32_t IR_RunRecipeJsonEx(const char *station_json_utf8,
    const char *recipe_json_utf8, const char *new_output_directory_utf8,
    const IRRecipeRunOptions *options, IRRecipeResult *result, char *err, int32_t err_bytes);

#ifdef __cplusplus
}
#endif
