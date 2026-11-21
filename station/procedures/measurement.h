#pragma once

#include "IR_device.h"
#include <mutex>

namespace ir::acquisition { class Capture; }

namespace ir::procedures {
// 一次同步测量独占采集入口。实际采集源和仪器可以在分析前释放。
class Measurement {
public:
    Measurement();
    ~Measurement();
    acquisition::Capture &frames() noexcept { return capture_; }

private:
    std::unique_lock<std::mutex> lock_;
    acquisition::Capture &capture_;
};

struct BlackbodyTarget {
    double temperatureC;
    double toleranceC;
    int32_t stableWindowMs;
    int32_t timeoutMs;
    bool requireDeviceStable;
};

// 从设温回读成功后开始计算等稳时限。
IRBlackbodyReading waitForBlackbody(const acquisition::Capture &capture, IRHandle handle,
                                   const BlackbodyTarget &target);
}
