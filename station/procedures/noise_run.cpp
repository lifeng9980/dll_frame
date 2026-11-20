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
    return guarded(err,bytes,[&] {
        needed(recipe,1,1); needed(result,1,1);
        const auto &r = *recipe;
        require(r.recipe_id && *r.recipe_id && r.frames >= 2 && r.frame_timeout_ms >= 0,
            IR_BAD_ARGUMENT,"Specify recipe ID, at least two frames, and a nonnegative timeout");
        require(!r.detector, IR_UNSUPPORTED, "Detector control is not available in this build");
        auto lock = acquisition::lockCapture();
        auto &c = acquisition::capture();
        c.checkOwner();
        require(!c.running,IR_BUSY,"Stop the stream before starting a measurement");
        c.cancelled = false;
        const auto path = QString::fromUtf8(output);
        if (output) procedures::checkDestination(path);
        const auto g = c.source->geometry();
        const auto n = pixels(g.height,g.width);
        const auto total = stackSize(n,r.frames);
        std::vector<uint16_t> frames(static_cast<size_t>(total));
        std::vector<IRFrameStamp> stamps(r.frames);
        std::vector<double> means(static_cast<size_t>(n)), stds(static_cast<size_t>(n));
        char detail[1024]{};
        if (r.blackbody) settleBlackbody(r, c);
        c.checkStop();
        c.start();
        try {
            c.read(r.frames,r.frame_timeout_ms,frames.data(),total,stamps.data(),r.frames,result->completed);
        } catch (...) { c.stop(); throw; }
        c.stop();
        double summary[2]{};
        checked(IR_FrameStatisticsU16(frames.data(),total,r.frames,g.height,g.width,1,means.data(),
            stds.data(),n,summary,2,detail,sizeof detail),detail);
        c.checkStop();
        IRNoiseResult measured{result->completed,summary[0],summary[1]};
        if (output) procedures::saveNoiseRun(path,r,c.backend,g,frames,stamps,measured);
        *result = measured;
    });
}
