#pragma once

#include "IR_device.h"
#include "IR_recipe.h"
#include "IR_station.h"
#include "api_common.h"
#include "recipe_config.h"
#include "station/acquisition/frame_source.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

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

inline const char *statusName(int32_t status) noexcept
{
    switch (status) {
    case IR_OK: return "IR_OK";
    case IR_NULL_POINTER: return "IR_NULL_POINTER";
    case IR_BAD_ARGUMENT: return "IR_BAD_ARGUMENT";
    case IR_BUFFER_TOO_SMALL: return "IR_BUFFER_TOO_SMALL";
    case IR_COMPUTATION_FAILED: return "IR_COMPUTATION_FAILED";
    case IR_INVALID_HANDLE: return "IR_INVALID_HANDLE";
    case IR_IO_ERROR: return "IR_IO_ERROR";
    case IR_TIMEOUT: return "IR_TIMEOUT";
    case IR_CANCELLED: return "IR_CANCELLED";
    case IR_BUSY: return "IR_BUSY";
    case IR_UNSUPPORTED: return "IR_UNSUPPORTED";
    case IR_END_OF_STREAM: return "IR_END_OF_STREAM";
    case IR_FRAME_GAP: return "IR_FRAME_GAP";
    default: return "IR_UNKNOWN";
    }
}

inline const char *stageName(int32_t stage) noexcept
{
    switch (stage) {
    case IR_RECIPE_STAGE_NONE: return "none";
    case IR_RECIPE_STAGE_VALIDATE: return "validate";
    case IR_RECIPE_STAGE_RESERVE: return "reserve";
    case IR_RECIPE_STAGE_PREPARE: return "prepare";
    case IR_RECIPE_STAGE_INSTRUMENT: return "instrument";
    case IR_RECIPE_STAGE_CAPTURE: return "capture";
    case IR_RECIPE_STAGE_ANALYZE: return "analyze";
    case IR_RECIPE_STAGE_ARCHIVE: return "archive";
    case IR_RECIPE_STAGE_CLEANUP: return "cleanup";
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

struct RecipeReadbacks {
    Json conditions = Json::array();
    Json instruments = Json::array();
};

void runControlBlackbody(const ir::acquisition::Capture &capture, IRHandle handle,
                         const BlackbodyStep &step, RecipeReadbacks &readbacks);
void runControlPower(IRHandle handle, const PowerStep &step, RecipeReadbacks &readbacks,
                     std::vector<const PowerStep *> &powerToSwitchOff);
void runControlPowerWaitStable(const ir::acquisition::Capture &capture, IRHandle handle,
                               const PowerWaitStableStep &step, RecipeReadbacks &readbacks);
void runControlOvenStabilize(const ir::acquisition::Capture &capture, IRHandle handle,
                             const OvenStabilizeStep &step, RecipeReadbacks &readbacks,
                             std::vector<std::string> &ovensToStandby);
void runControlOvenStandby(IRHandle handle, const OvenStandbyStep &step, RecipeReadbacks &readbacks);
void runControlDmmRead(IRHandle handle, const DmmReadStep &step, RecipeReadbacks &readbacks,
                       ExecutionContext &ctx);
void runControlDetectorTiming(IRHandle handle, const DetectorTimingStep &step, RecipeReadbacks &readbacks);
void runControlDetectorGpol(IRHandle handle, const DetectorGpolStep &step, RecipeReadbacks &readbacks);
void runControlDetectorRegister(IRHandle handle, const DetectorRegisterStep &step, RecipeReadbacks &readbacks);
void runControlDetectorCurrent(IRHandle handle, const DetectorCurrentStep &step,
                               RecipeReadbacks &readbacks, ExecutionContext &ctx);
void runControlCoolerRead(IRHandle handle, const CoolerReadStep &step,
                          RecipeReadbacks &readbacks, ExecutionContext &ctx);

} // namespace ir::procedures::recipe

namespace ir::procedures {

struct SavedBatchRef {
    std::string subdir;
    IRGeometry geometry{};
    const std::vector<uint16_t> *frames{};
    const std::vector<IRFrameStamp> *stamps{};
};

void checkDestination(const std::filesystem::path &path);
void writeFrameBatchFiles(const std::filesystem::path &directory, IRGeometry geometry,
    const std::vector<uint16_t> &frames, const std::vector<IRFrameStamp> &stamps);
void saveRecipeArtifacts(const std::filesystem::path &destination,
    const std::vector<SavedBatchRef> &batches, const std::string &resultJson);
void writeTextFileAtomicOrDirect(const std::filesystem::path &filePath, const std::string &content);

} // namespace ir::procedures
