#pragma once
#include "IR_station.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace ir::acquisition {
class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual IRGeometry geometry() const = 0;
    virtual void start() = 0;
    virtual void read(uint16_t *pixels, IRFrameStamp &stamp, int timeoutMs) = 0;
    virtual void stop() = 0;
};
std::unique_ptr<FrameSource> openReplay(const char *directory);

class Capture {
public:
    std::mutex mutex;
    std::unique_ptr<FrameSource> source;
    std::thread::id owner;
    int backend{};
    bool running{};
    std::atomic_bool cancelled{};
    void checkOwner() const;
    void checkStop() const;
    void start();
    void stop();
    void read(int count, int timeoutMs, uint16_t *pixels, int64_t capacity,
              IRFrameStamp *stamps, int stampCapacity, int &completed);
};
Capture &capture();
std::unique_lock<std::mutex> lockCapture();
}
