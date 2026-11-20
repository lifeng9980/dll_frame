#include "save_raw.h"
#include "station/acquisition/frame_source.h"
#include "api_common.h"
#include "IR_analysis.h"
#include "IR_device.h"
#include <chrono>
#include <optional>
#include <thread>
using namespace ir;
using namespace ir::api;
namespace {
void checked(int32_t code, const char *message)
{
    if (code != IR_OK) throw Error(code,message);
}

void settleBlackbody(const IRNoiseRecipe &recipe, acquisition::Capture &capture)
{
    using namespace std::chrono;
    require(std::isfinite(recipe.temperature_c) && std::isfinite(recipe.tolerance_c) &&
            recipe.tolerance_c > 0 && recipe.stable_window_ms >= 0 &&
            recipe.stable_timeout_ms > 0 && recipe.stable_timeout_ms >= recipe.stable_window_ms &&
            (recipe.require_device_stable == 0 || recipe.require_device_stable == 1),
            IR_BAD_ARGUMENT, "Invalid blackbody stability parameters");
    char detail[1024]{};
    double readback{};
    capture.checkStop();
    checked(IR_SetBlackbody(recipe.blackbody, recipe.temperature_c, &readback, detail, sizeof detail), detail);
    const auto deadline = steady_clock::now() + milliseconds(recipe.stable_timeout_ms);
    std::optional<steady_clock::time_point> stableSince;
    for (;;) {
        capture.checkStop();
        require(steady_clock::now() < deadline, IR_TIMEOUT, "Blackbody did not stabilize before the deadline");
        IRBlackbodyReading reading{};
        checked(IR_ReadBlackbody(recipe.blackbody, &reading, detail, sizeof detail), detail);
        capture.checkStop();
        const auto now = steady_clock::now();
        require(now <= deadline, IR_TIMEOUT, "Blackbody did not stabilize before the deadline");
        const bool inRange = std::abs(reading.actual_c - recipe.temperature_c) <= recipe.tolerance_c &&
                             (!recipe.require_device_stable || reading.stable);
        if (inRange) {
            if (!stableSince) stableSince = now;
            if (now - *stableSince >= milliseconds(recipe.stable_window_ms)) return;
        } else {
            stableSince.reset();
        }
        std::this_thread::sleep_until(std::min(deadline, now + milliseconds(100)));
    }
}
}
extern "C" int32_t IR_RunNoise(const IRNoiseRecipe *recipe, const char *output,
    IRNoiseResult *result, char *err, int32_t bytes)
{
    if (result) *result = {};
    return guarded(err, bytes, [&] {
        needed(recipe, 1, 1);
        needed(result, 1, 1);
        require(recipe->recipe_id && *recipe->recipe_id && recipe->frames >= 2 && recipe->frame_timeout_ms >= 0,
                IR_BAD_ARGUMENT, "Specify recipe ID, at least two frames, and a nonnegative timeout");
        require(!recipe->detector, IR_UNSUPPORTED, "Detector control is not available in this build");
        auto lock = acquisition::lockCapture();
        auto &capture = acquisition::capture();
        capture.checkOwner();
        require(!capture.running, IR_BUSY, "Stop the stream before starting a measurement");
        capture.cancelled = false;
        const auto directory = QString::fromUtf8(output);
        if (output) procedures::checkDestination(directory);

        const auto geometry = capture.source->geometry();
        const auto pixelCount = pixels(geometry.height, geometry.width);
        const auto elements = stackSize(pixelCount, recipe->frames);
        std::vector<uint16_t> frames(static_cast<size_t>(elements));
        std::vector<IRFrameStamp> stamps(recipe->frames);
        std::vector<double> means(static_cast<size_t>(pixelCount));
        std::vector<double> deviations(static_cast<size_t>(pixelCount));

        // 等稳期间尚未启动采集，避免把升温过程中的帧计入本次测量。
        if (recipe->blackbody) settleBlackbody(*recipe, capture);
        capture.checkStop();
        capture.start();
        try {
            capture.read(recipe->frames, recipe->frame_timeout_ms, frames.data(), elements,
                         stamps.data(), recipe->frames, result->completed);
        } catch (...) {
            capture.stop();
            throw;
        }
        capture.stop();

        // 固定配方使用像元样本标准差，算法和保存共用刚采到的同一批像素。
        char detail[1024]{};
        double summary[2]{};
        checked(IR_FrameStatisticsU16(frames.data(), elements, recipe->frames, geometry.height,
            geometry.width, 1, means.data(), deviations.data(), pixelCount, summary, 2, detail, sizeof detail), detail);
        capture.checkStop();
        IRNoiseResult measured{result->completed, summary[0], summary[1]};
        if (output) procedures::saveNoiseRun(directory, *recipe, capture.backend, geometry, frames, stamps, measured);
        *result = measured;
    });
}
