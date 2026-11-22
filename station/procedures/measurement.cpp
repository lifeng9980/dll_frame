#include "measurement.h"
#include "api_common.h"
#include "station/acquisition/frame_source.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
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

IROvenReading waitForOven(const acquisition::Capture &capture, IRHandle handle,
                          const OvenTarget &target)
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(target.timeoutMs);
    std::optional<steady_clock::time_point> stableSince;
    for (;;) {
        capture.checkStop();
        require(steady_clock::now() < deadline, IR_TIMEOUT, "Oven did not stabilize before the deadline");
        IROvenReading reading{};
        char detail[1024]{};
        checkStatus(IR_ReadOven(handle, &reading, detail, sizeof detail), detail);
        capture.checkStop();
        const auto now = steady_clock::now();
        require(now <= deadline, IR_TIMEOUT, "Oven did not stabilize before the deadline");
        const bool alarmBlocked = target.requireNoAlarm && reading.alarm_active != 0;
        require(!alarmBlocked, IR_IO_ERROR, "Oven reported an active alarm while stabilizing");
        const bool inRange = std::abs(reading.actual_c - target.temperatureC) <= target.toleranceC &&
                             reading.mode == IR_OVEN_MODE_CONSTANT;
        if (inRange) {
            if (!stableSince) stableSince = now;
            if (now - *stableSince >= milliseconds(target.stableWindowMs)) return reading;
        } else {
            stableSince.reset();
        }
        std::this_thread::sleep_until(std::min(deadline, now + milliseconds(100)));
    }
}

namespace {
void validatePowerCurrentStableSpec(int32_t channel, const IRPowerCurrentStableSpec &spec)
{
    require(channel >= 1 && channel <= 3, IR_BAD_ARGUMENT, "IT6332A channel must be from 1 to 3");
    require(std::isfinite(spec.expected_voltage_v) &&
                std::isfinite(spec.voltage_tolerance_v) && spec.voltage_tolerance_v >= 0.0 &&
                std::isfinite(spec.max_current_a) && spec.max_current_a >= 0.0 &&
                std::isfinite(spec.startup_threshold_a) && spec.startup_threshold_a >= 0.0 &&
                std::isfinite(spec.stable_threshold_a) && spec.stable_threshold_a > 0.0 &&
                std::isfinite(spec.max_variation_a) && spec.max_variation_a >= 0.0,
            IR_BAD_ARGUMENT, "Invalid power current stabilization thresholds");
    require(spec.startup_required_samples >= 0 &&
                spec.stable_required_samples >= 1 &&
                spec.sample_interval_ms >= 0 &&
                spec.timeout_ms >= 1,
            IR_BAD_ARGUMENT, "Invalid power current stabilization sample counts or timeouts");
    if (spec.startup_required_samples > 0) {
        require(spec.startup_threshold_a > 0.0 && spec.startup_timeout_ms >= 1,
                IR_BAD_ARGUMENT, "Power startup confirmation requires startup_threshold_a > 0 and startup_timeout_ms >= 1");
    }
}
} // namespace

IRPowerReading waitForPowerCurrentStable(const acquisition::Capture &capture, IRHandle handle,
                                         int32_t channel, const IRPowerCurrentStableSpec &spec)
{
    static_assert(sizeof(IRPowerCurrentStableSpec) == 72, "Unexpected IRPowerCurrentStableSpec size");
    validatePowerCurrentStableSpec(channel, spec);

    using namespace std::chrono;
    const auto started = steady_clock::now();
    const auto deadline = started + milliseconds(spec.timeout_ms);
    const auto startupDeadline = started + milliseconds(std::max<int32_t>(spec.startup_timeout_ms, 0));
    bool startupConfirmed = (spec.startup_required_samples == 0);
    int32_t startupSamples = 0;
    std::deque<double> recentCurrents;

    for (;;) {
        capture.checkStop();
        require(steady_clock::now() < deadline, IR_TIMEOUT,
                "Power channel current did not stabilize before deadline");

        IRPowerReading reading{};
        char detail[1024]{};
        checkStatus(IR_ReadPower(handle, channel, &reading, detail, sizeof detail), detail);
        capture.checkStop();
        const auto now = steady_clock::now();

        require(reading.output_enabled != 0, IR_IO_ERROR,
                "Power channel output must be enabled while waiting for stabilization");
        if (spec.max_current_a > 0.0) {
            require(std::isfinite(reading.measured_a) && reading.measured_a >= 0.0 &&
                        reading.measured_a <= spec.max_current_a,
                    IR_IO_ERROR, "Power channel current exceeded max_current_a limit");
        }

        const bool voltageNormal = (spec.expected_voltage_v <= 0.0) ||
                                   (std::isfinite(reading.measured_v) &&
                                    std::abs(reading.measured_v - spec.expected_voltage_v) <= spec.voltage_tolerance_v);

        if (!startupConfirmed) {
            if (voltageNormal && std::isfinite(reading.measured_a) &&
                reading.measured_a > spec.startup_threshold_a) {
                ++startupSamples;
            } else {
                startupSamples = 0;
            }
            if (startupSamples >= spec.startup_required_samples) {
                startupConfirmed = true;
                recentCurrents.clear();
            } else {
                require(now < startupDeadline && now < deadline, IR_TIMEOUT,
                        "Power channel startup load current was not observed before timeout");
            }
        } else {
            if (voltageNormal && std::isfinite(reading.measured_a) &&
                reading.measured_a < spec.stable_threshold_a) {
                recentCurrents.push_back(reading.measured_a);
                while (static_cast<int32_t>(recentCurrents.size()) > spec.stable_required_samples)
                    recentCurrents.pop_front();
                if (static_cast<int32_t>(recentCurrents.size()) == spec.stable_required_samples) {
                    const auto [minIt, maxIt] =
                        std::minmax_element(recentCurrents.cbegin(), recentCurrents.cend());
                    if ((*maxIt - *minIt) <= spec.max_variation_a)
                        return reading;
                }
            } else {
                recentCurrents.clear();
            }
        }

        require(now < deadline, IR_TIMEOUT, "Power channel current did not stabilize before deadline");
        if (spec.sample_interval_ms > 0) {
            const auto sleepEnd = std::min(deadline, now + milliseconds(spec.sample_interval_ms));
            while (steady_clock::now() < sleepEnd) {
                capture.checkStop();
                const auto remaining = duration_cast<milliseconds>(sleepEnd - steady_clock::now()).count();
                if (remaining <= 0) break;
                std::this_thread::sleep_for(milliseconds(std::min<int64_t>(remaining, 50)));
            }
        }
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

extern "C" IR_STATION_API int32_t IR_WaitOvenStable(
    IRHandle handle, double target_c, double tolerance_c,
    int32_t stable_window_ms, int32_t timeout_ms, int32_t require_no_alarm,
    IROvenReading *reading, char *err, int32_t err_bytes)
{
    if (reading) *reading = IROvenReading{};
    return ir::api::guarded(err, err_bytes, [&] {
        ir::api::require(handle != 0, IR_INVALID_HANDLE, "Invalid oven handle");
        ir::api::needed(reading, 1, 1);
        ir::api::require(std::isfinite(target_c) && std::isfinite(tolerance_c) &&
                             tolerance_c > 0.0 && stable_window_ms >= 0 &&
                             timeout_ms >= 1 && timeout_ms >= stable_window_ms &&
                             (require_no_alarm == 0 || require_no_alarm == 1),
                         IR_BAD_ARGUMENT, "Invalid oven stabilization parameters");
        auto &cap = ir::acquisition::capture();
        StandaloneStopGuard guard(cap);
        const ir::procedures::OvenTarget target{
            target_c, tolerance_c, stable_window_ms, timeout_ms, require_no_alarm != 0};
        *reading = ir::procedures::waitForOven(cap, handle, target);
    });
}

extern "C" IR_STATION_API int32_t IR_WaitPowerCurrentStable(
    IRHandle handle, int32_t channel,
    const IRPowerCurrentStableSpec *spec, IRPowerReading *reading,
    char *err, int32_t err_bytes)
{
    if (reading) *reading = IRPowerReading{};
    return ir::api::guarded(err, err_bytes, [&] {
        ir::api::require(handle != 0, IR_INVALID_HANDLE, "Invalid power handle");
        ir::api::needed(spec, 1, 1);
        ir::api::needed(reading, 1, 1);
        auto &cap = ir::acquisition::capture();
        StandaloneStopGuard guard(cap);
        *reading = ir::procedures::waitForPowerCurrentStable(cap, handle, channel, *spec);
    });
}
