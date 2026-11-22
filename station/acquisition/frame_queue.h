#pragma once

#include "IR_station.h"
#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ir::acquisition {

class FrameQueue {
public:
    enum class PopStatus { ok, timeout, cancelled, gap, terminal_error, stopped };

    static constexpr int32_t kDefaultCapacity = 64;
    static constexpr int32_t kMinCapacity = 2;
    static constexpr int32_t kMaxCapacity = 4096;

    static void validateCapacity(int32_t capacity);

    void setIdleCapacity(int32_t capacity);
    void configure(IRGeometry geometry, int32_t capacity);
    void resetSession(IRGeometry geometry, int32_t capacity, bool running);
    void beginStartSession(IRGeometry geometry, int32_t capacity);
    void resumeSession();
    void resetClosed();
    void setRunning(bool running);

    void push(const uint16_t *pixels, const IRFrameStamp &stamp);
    void publishPulledFrame(const uint16_t *pixels, const IRFrameStamp &stamp, bool delivered);
    void recordDelivered();

    PopStatus pop(uint16_t *outPixels, IRFrameStamp &outStamp, int32_t timeoutMs,
                  const std::function<bool()> &stopRequested,
                  int32_t &terminalCode, std::string &terminalMessage);

    void clearMeasurementQueue();
    void setTerminalError(int32_t code, std::string message);
    void markStopped();
    void wakeWaiters();

    void tryReadLatest(uint16_t *pixels, int64_t pixelCapacity, IRGeometry *geometry,
                       IRFrameStamp *stamp, int32_t &hasFrame);
    IRCaptureStats stats();

private:
    void recordPreviewLocked(const uint16_t *pixels, const IRFrameStamp &stamp,
                             int64_t pxCount, bool countDelivered);

    // Strictly separate leaf locks: never acquire queueMutex_ and previewMutex_ simultaneously.
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    IRGeometry queueGeometry_{};
    int64_t queuePixelCount_{0};
    int32_t capacity_{kDefaultCapacity};
    std::vector<uint16_t> pool_;
    std::vector<IRFrameStamp> stamps_;
    int32_t head_{0};
    int32_t count_{0};
    uint64_t droppedSinceLastPush_{0};
    int32_t terminalStatus_{IR_OK};
    std::string terminalMessage_;
    bool stopped_{false};

    std::mutex previewMutex_;
    IRGeometry previewGeometry_{};
    int64_t previewPixelCount_{0};
    bool hasLatest_{false};
    std::vector<uint16_t> latestPixels_;
    IRFrameStamp latestStamp_{};
    uint64_t framesReceived_{0};
    uint64_t framesDelivered_{0};
    uint64_t framesDropped_{0};
    uint64_t previewFrames_{0};
    int32_t configuredCapacity_{kDefaultCapacity};
    int32_t queueDepth_{0};
    int32_t running_{0};
    std::array<int64_t, 16> fpsTimestampsNs_{};
    size_t fpsHead_{0};
    size_t fpsCount_{0};
};

} // namespace ir::acquisition
