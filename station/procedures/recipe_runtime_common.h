#pragma once

#include "IR_recipe.h"
#include "api_common.h"
#include "recipe_config.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <system_error>

namespace ir::procedures::recipe {

struct RecipeProgress {
    IRRecipeResult &result;
    const StepRef *step{};

    void enter(IRRecipeStage stage, const StepRef *ref = nullptr) noexcept
    {
        step = ref;
        result.failed_stage = stage;
        result.failed_step_index = ref ? ref->index : -1;
    }
};

struct RecipeCleanupFailure {
    int32_t status{IR_OK};
    const StepRef *step{};
    char detail[1024]{};

    template <class Action>
    void attempt(const StepRef *ref, Action action) noexcept
    {
        char message[1024]{};
        const auto code = ir::api::guarded(message, sizeof message, action);
        if (code != IR_OK && status == IR_OK) {
            status = code;
            step = ref;
            ir::api::copyText(detail, sizeof detail, message);
        }
    }
};

inline double elapsedMsSince(std::chrono::steady_clock::time_point start) noexcept
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

inline int32_t exceptionToStatus(const std::exception &e) noexcept
{
    if (dynamic_cast<const std::system_error *>(&e) != nullptr)
        return IR_IO_ERROR;
    if (dynamic_cast<const std::invalid_argument *>(&e) != nullptr)
        return IR_BAD_ARGUMENT;
    return IR_COMPUTATION_FAILED;
}

inline void emitRecipeStepEvent(const IRRecipeRunOptions *options, int32_t kind,
                                 const StepRef &ref, int32_t stage, int32_t status,
                                 double durationMs, const char *stepType,
                                 const char *detail = "") noexcept
{
    if (!options || !options->on_step_event)
        return;
    IRRecipeStepEvent ev{};
    ev.struct_size = sizeof(IRRecipeStepEvent);
    ev.event_kind = kind;
    ev.step_index = ref.index;
    ev.stage = stage;
    ev.status = status;
    ev.reserved = 0;
    ev.duration_ms = durationMs;
    ev.step_id_utf8 = ref.id.c_str();
    ev.step_type_utf8 = stepType ? stepType : "";
    ev.detail_utf8 = detail ? detail : "";
    try {
        options->on_step_event(&ev, options->user_data);
    } catch (...) {
    }
}

inline const char *backendName(int backend) noexcept
{
    switch (backend) {
    case IR_RAW_REPLAY: return "raw_replay";
    case IR_DIRECTSHOW: return "directshow";
    case IR_V4L2: return "v4l2";
    default: return "unknown";
    }
}

inline Json powerJson(const IRPowerReading &reading, int channel)
{
    return {
        {"channel", channel},
        {"setpoint_v", reading.setpoint_v},
        {"current_limit_a", reading.current_limit_a},
        {"measured_v", reading.measured_v},
        {"measured_a", reading.measured_a},
        {"output_enabled", reading.output_enabled != 0}
    };
}

} // namespace ir::procedures::recipe
