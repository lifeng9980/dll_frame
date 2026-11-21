#include "measurement.h"
#include "api_common.h"
#include "station/acquisition/frame_source.h"
#include <chrono>
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
}
