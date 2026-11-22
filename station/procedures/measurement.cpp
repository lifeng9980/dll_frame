#include "measurement.h"
#include "api_common.h"
#include "station/acquisition/frame_source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <optional>
#include <thread>

namespace ir::procedures {
using namespace ir::api;

Measurement::Measurement() : lock_(acquisition::lockCapture()), capture_(acquisition::capture())
{
    require(!capture_.running, IR_BUSY, "Stop the stream before starting a measurement");
    capture_.beginOperation();
}

Measurement::~Measurement()
{
    capture_.endOperation();
}

IRBlackbodyReading waitForBlackbody(const acquisition::Capture &capture, IRHandle handle,
                                   const BlackbodyTarget &target)
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(target.timeoutMs);
    std::optional<steady_clock::time_point> stableSince;
    for (;;) {
        capture.checkStop();
        require(steady_clock::now() < deadline, IR_TIMEOUT, "Blackbody did not stabilize before the deadline");
        IRBlackbodyReading reading{};
        char detail[1024]{};
        checkStatus(IR_ReadBlackbody(handle, &reading, detail, sizeof detail), detail);
        capture.checkStop();
        const auto now = steady_clock::now();
        require(now <= deadline, IR_TIMEOUT, "Blackbody did not stabilize before the deadline");
        const bool inRange = std::abs(reading.actual_c - target.temperatureC) <= target.toleranceC &&
                             (!target.requireDeviceStable || reading.stable);
        if (inRange) {
            if (!stableSince) stableSince = now;
            if (now - *stableSince >= milliseconds(target.stableWindowMs)) return reading;
        } else {
            stableSince.reset();
        }
        std::this_thread::sleep_until(std::min(deadline, now + milliseconds(100)));
    }
}

std::string utcTimestampNow()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch());
    auto secCount = duration_cast<seconds>(millis).count();
    auto msPart = millis.count() % 1000;
    if (msPart < 0) {
        msPart += 1000;
        --secCount;
    }
    const std::time_t rawTime = static_cast<std::time_t>(secCount);
    std::tm utc{};
#ifdef _WIN32
    const bool ok = gmtime_s(&utc, &rawTime) == 0;
#else
    const bool ok = gmtime_r(&rawTime, &utc) != nullptr;
#endif
    if (!ok) return "1970-01-01T00:00:00.000Z";
    char buffer[32];
    const int written = std::snprintf(
        buffer, sizeof buffer,
        "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
        utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(msPart));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof buffer)
        return "1970-01-01T00:00:00.000Z";
    return std::string(buffer, static_cast<size_t>(written));
}
}

namespace
{
struct StandaloneStopGuard
{
    ir::acquisition::Capture &capture;
    bool entered = false;

    explicit StandaloneStopGuard(ir::acquisition::Capture &c) noexcept
        : capture(c), entered(c.tryEnterStandaloneStopScope())
    {
    }
    ~StandaloneStopGuard()
    {
        capture.leaveStandaloneStopScope(entered);
    }
};
} // namespace

extern "C" IR_STATION_API int32_t IR_WaitBlackbodyStable(
    IRHandle handle, double target_c, double tolerance_c,
    int32_t stable_window_ms, int32_t timeout_ms, int32_t require_device_stable,
    IRBlackbodyReading *reading, char *err, int32_t err_bytes)
{
    if (reading) *reading = IRBlackbodyReading{};
    return ir::api::guarded(err, err_bytes, [&] {
        ir::api::require(handle != 0, IR_INVALID_HANDLE, "Invalid blackbody handle");
        ir::api::needed(reading, 1, 1);
        ir::api::require(std::isfinite(target_c) && std::isfinite(tolerance_c) &&
                             target_c >= -60.0 && target_c <= 100.0 &&
                             tolerance_c > 0.0 && stable_window_ms >= 0 &&
                             timeout_ms >= 1 && timeout_ms >= stable_window_ms,
                         IR_BAD_ARGUMENT, "Invalid blackbody stabilization parameters");
        auto &cap = ir::acquisition::capture();
        StandaloneStopGuard guard(cap);
        const ir::procedures::BlackbodyTarget target{
            target_c, tolerance_c, stable_window_ms, timeout_ms, require_device_stable != 0};
        *reading = ir::procedures::waitForBlackbody(cap, handle, target);
    });
}
