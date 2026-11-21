#pragma once
#include "IR_station.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
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
#if defined(__linux__)
std::unique_ptr<FrameSource> openV4l2(const char *devicePath, int validBits);
#endif
#if defined(_WIN32)
std::unique_ptr<FrameSource> openDirectShow(const char *cameraName, int validBits);
#endif

class Capture {
public:
    std::mutex mutex;
    std::unique_ptr<FrameSource> source;
    std::string sourceName;
    std::thread::id owner;
    int backend{};
    bool running{};
    IRGeometry open(int backend, const char *name, int validBits);
    void close();
    void checkOwner() const;
    // 采集启动和流程开始各建立一次停止范围。取帧阶段不重置流程的停止请求。
    void beginOperation() noexcept;
    void endOperation() noexcept;
    void requestStop() noexcept;
    void checkStop() const;
    void start();
    void stop();
    void read(int count, int timeoutMs, uint16_t *pixels, int64_t capacity,
              IRFrameStamp *stamps, int stampCapacity, int &completed);

private:
    enum class StopState { idle, active, requested };
    std::atomic<StopState> stopState_{StopState::idle};
};
Capture &capture();
std::unique_lock<std::mutex> lockCapture();
}
