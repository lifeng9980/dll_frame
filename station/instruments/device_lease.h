#pragma once
#include "IR_station.h"
#include <cstddef>
#include <vector>

namespace ir::instruments {

/* Reserves the requested SDK instrument handles for the current procedure thread. */
class DeviceLease {
public:
    DeviceLease(const IRHandle *handles, std::size_t count);
    ~DeviceLease();
    DeviceLease(const DeviceLease &) = delete;
    DeviceLease &operator=(const DeviceLease &) = delete;

private:
    std::vector<IRHandle> handles_;
};

}
