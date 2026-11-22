#include "save_raw.h"
#include "measurement.h"
#include "api_common.h"
#include "IR_analysis.h"
#include "IR_device.h"
#include "station/acquisition/frame_source.h"
#include "station/instruments/device_lease.h"
#include <exception>
#include <filesystem>
using namespace ir;
using namespace ir::api;
namespace {
void settleBlackbody(const IRNoiseRecipe &recipe, acquisition::Capture &capture,
                     int32_t &step, double &setpointReadback,
                     IRBlackbodyReading &stableReading)
{
    require(std::isfinite(recipe.temperature_c) && std::isfinite(recipe.tolerance_c) &&
            recipe.tolerance_c > 0 && recipe.stable_window_ms >= 0 &&
            recipe.stable_timeout_ms > 0 && recipe.stable_timeout_ms >= recipe.stable_window_ms &&
            (recipe.require_device_stable == 0 || recipe.require_device_stable == 1),
            IR_BAD_ARGUMENT, "Invalid blackbody stability parameters");
    char detail[1024]{};
    capture.checkStop();
    step = IR_NOISE_STEP_SET_BLACKBODY;
    checkStatus(IR_SetBlackbody(recipe.blackbody, recipe.temperature_c, &setpointReadback, detail, sizeof detail), detail);
    step = IR_NOISE_STEP_WAIT_BLACKBODY;
    stableReading = procedures::waitForBlackbody(capture, recipe.blackbody,
        {recipe.temperature_c, recipe.tolerance_c, recipe.stable_window_ms,
         recipe.stable_timeout_ms, recipe.require_device_stable != 0});
}

int32_t currentExceptionStatus() noexcept
{
    try {
        throw;
    } catch (const Error &error) {
        return error.code;
    } catch (const std::system_error &) {
        return IR_IO_ERROR;
    } catch (...) {
        return IR_COMPUTATION_FAILED;
    }
}
}
extern "C" int32_t IR_RunNoise(const IRNoiseRecipe *recipe, const char *output,
    IRNoiseResult *result, char *err, int32_t bytes)
{
    if (result) *result = {};
    int32_t step = IR_NOISE_STEP_VALIDATE;
    const auto status = guarded(err, bytes, [&] {
        try {
            needed(recipe, 1, 1);
            needed(result, 1, 1);
            require(recipe->recipe_id && *recipe->recipe_id && recipe->frames >= 2 && recipe->frame_timeout_ms >= 0,
                    IR_BAD_ARGUMENT, "Specify recipe ID, at least two frames, and a nonnegative timeout");
            require(!recipe->detector, IR_UNSUPPORTED, "Detector control is not available in this build");
            std::filesystem::path directory;
            if (output) {
#ifdef _WIN32
                directory = std::filesystem::u8path(output);
#else
                directory = std::filesystem::path(output);
#endif
            }
            if (output) procedures::checkDestination(directory);

            const auto startedUtc = procedures::utcTimestampNow();
            step = IR_NOISE_STEP_RESERVE;
            procedures::Measurement measurement;
            auto &capture = measurement.frames();
            capture.checkOwner();
            const IRHandle requestedDevices[]{recipe->blackbody};
            ir::instruments::DeviceLease deviceLease(requestedDevices, recipe->blackbody ? 1 : 0);

            const auto geometry = capture.source->geometry();
            const auto pixelCount = pixels(geometry.height, geometry.width);
            const auto elements = stackSize(pixelCount, recipe->frames);
            std::vector<uint16_t> frames(static_cast<size_t>(elements));
            std::vector<IRFrameStamp> stamps(recipe->frames);
            std::vector<double> means(static_cast<size_t>(pixelCount));
            std::vector<double> deviations(static_cast<size_t>(pixelCount));
            double blackbodySetpointReadback{};
            IRBlackbodyReading blackbodyReading{};
            bool captureStarted = false;

            try {
                // 等稳期间尚未启动采集，避免把升温过程中的帧计入本次测量。
                if (recipe->blackbody) {
                    settleBlackbody(*recipe, capture, step, blackbodySetpointReadback, blackbodyReading);
                }
                capture.checkStop();
                step = IR_NOISE_STEP_START_CAPTURE;
                capture.start();
                captureStarted = true;
                step = IR_NOISE_STEP_READ_FRAMES;
                capture.read(recipe->frames, recipe->frame_timeout_ms, frames.data(), elements,
                             stamps.data(), recipe->frames, result->completed);
                step = IR_NOISE_STEP_STOP_CAPTURE;
                capture.stop();
                captureStarted = false;

                // 固定配方使用像元样本标准差，算法和保存共用刚采到的同一批像素。
                step = IR_NOISE_STEP_ANALYZE;
                char detail[1024]{};
                double summary[2]{};
                checkStatus(IR_FrameStatisticsU16(frames.data(), elements, recipe->frames, geometry.height,
                    geometry.width, 1, means.data(), deviations.data(), pixelCount, summary, 2,
                    detail, sizeof detail), detail);
                capture.checkStop();
                IRNoiseResult measured{};
                measured.completed = result->completed;
                measured.mean_dn = summary[0];
                measured.temporal_noise_dn = summary[1];
                if (output) {
                    step = IR_NOISE_STEP_SAVE;
                    procedures::saveNoiseRun(directory, *recipe, capture.backend, capture.sourceName, geometry,
                                             frames, stamps, measured, blackbodySetpointReadback,
                                             recipe->blackbody ? &blackbodyReading : nullptr, startedUtc);
                }
                *result = measured;
                result->failed_step = IR_NOISE_STEP_NONE;
                result->cleanup_status = IR_OK;
            } catch (...) {
                const auto failure = std::current_exception();
                result->failed_step = step;
                if (captureStarted) {
                    try {
                        capture.stop();
                    } catch (...) {
                        result->cleanup_status = currentExceptionStatus();
                    }
                }
                std::rethrow_exception(failure);
            }
        } catch (...) {
            if (result) result->failed_step = step;
            throw;
        }
    });
    if (status != IR_OK && result && result->failed_step == IR_NOISE_STEP_NONE)
        result->failed_step = step;
    return status;
}
