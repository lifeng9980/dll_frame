#pragma once
#include "IR_station.h"
#include "frame_queue.h"
#include <cstdint>
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
    virtual int32_t dataType() const noexcept { return IR_TYPE_U16; }
    virtual bool isLive() const noexcept { return true; }
    virtual uint64_t bufferedBytes() const noexcept { return 0; }
    virtual void start() = 0;
    virtual void read(void *pixels, IRFrameStamp &stamp, int timeoutMs) = 0;
    virtual void flush() {}
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
    enum class OperationScope { owned, joined, unavailable };

    std::mutex mutex;
    std::unique_ptr<FrameSource> source;
    std::string sourceName;
    std::thread::id owner;
    int backend{};
    bool running{}; // The owner must stop this backend session even after the pump exits.
    IRFrameSourceInfo open(int backend, const char *name, int validBits);
    IRFrameSourceInfo open(int backend, const char *name, int validBits,
                           int32_t queueCapacity, uint64_t memoryLimitBytes);
    void close();
    void checkOperationAccess() const;
    void checkOwner() const;
    // 采集启动和流程开始各建立一次停止范围。取帧阶段不重置流程的停止请求。
    bool beginOperation() noexcept;
    void endOperation() noexcept;
    OperationScope enterOperationScope() noexcept;
    void leaveOperationScope(OperationScope scope) noexcept;
    void requestStop() noexcept;
    bool isStopRequested() const noexcept;
    void checkStop() const;
    void start();
    void stop();
    void read(int timeoutMs, IRFrameBatchView &batch);
    void flush(int32_t discardFrames, int32_t timeoutMs, int32_t &discarded);
    void setQueueCapacity(int32_t capacity);
    uint64_t bufferedBytes() const;
    FrameQueue &frameQueue() noexcept { return frameQueue_; }

private:
    static constexpr uint64_t kDefaultMemoryLimitBytes = 1024ULL * 1024ULL * 1024ULL;

    void startPump();
    void stopPump();
    void pumpLoop(void *frame);

    enum class StopState { idle, active, requested };
    mutable std::mutex operationMutex_;
    std::atomic<StopState> stopState_{StopState::idle};
    std::thread::id operationOwner_{};
    int32_t queueCapacity_{FrameQueue::kDefaultCapacity};
    uint64_t memoryLimitBytes_{kDefaultMemoryLimitBytes};
    FrameQueue frameQueue_;
    std::thread pumpThread_;
    std::atomic<bool> pumpStop_{false};
    bool pumpActive_{false};
};
Capture &capture();
std::unique_lock<std::mutex> lockCapture();
}
