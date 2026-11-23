#include "frame_queue.h"
#include "api_common.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace ir::acquisition {
using namespace ir::api;

static_assert(sizeof(IRCaptureStats) == 64, "IRCaptureStats ABI size must be 64 bytes");

namespace {
int64_t steadyNowNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void FrameQueue::validateCapacity(int32_t capacity)
{
    require(capacity >= kMinCapacity && capacity <= kMaxCapacity,
            IR_BAD_ARGUMENT, "FrameQueue capacity must be in [2, 4096]");
}

void FrameQueue::setIdleCapacity(int32_t capacity)
{
    validateCapacity(capacity);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        capacity_ = capacity;
    }
    {
        std::lock_guard<std::mutex> lock(previewMutex_);
        configuredCapacity_.store(capacity, std::memory_order_relaxed);
    }
}

void FrameQueue::configure(IRGeometry geometry, int32_t capacity)
{
    validateCapacity(capacity);
    const int64_t pxCount = pixels(geometry.height, geometry.width);
    const int64_t poolElems = stackSize(pxCount, capacity);

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queueGeometry_ = geometry;
        queuePixelCount_ = pxCount;
        capacity_ = capacity;
        pool_.assign(static_cast<size_t>(poolElems), 0);
        stamps_.assign(static_cast<size_t>(capacity), IRFrameStamp{});
        head_ = 0;
        count_ = 0;
        droppedSinceLastPush_ = 0;
        terminalStatus_ = IR_OK;
        terminalMessage_.clear();
        stopped_ = false;
    }

    {
        std::lock_guard<std::mutex> lock(previewMutex_);
        previewGeometry_ = geometry;
        previewPixelCount_ = pxCount;
        hasLatest_ = false;
        latestPixels_.assign(static_cast<size_t>(pxCount), 0);
        latestStamp_ = {};
        framesReceived_.store(0, std::memory_order_relaxed);
        framesDelivered_.store(0, std::memory_order_relaxed);
        framesDropped_.store(0, std::memory_order_relaxed);
        previewFrames_.store(0, std::memory_order_relaxed);
        configuredCapacity_.store(capacity, std::memory_order_relaxed);
        queueDepth_.store(0, std::memory_order_relaxed);
        running_ = 0;
        fpsTimestampsNs_.fill(0);
        fpsHead_ = 0;
        fpsCount_ = 0;
    }
}

void FrameQueue::resetSession(IRGeometry geometry, int32_t capacity, bool running)
{
    configure(geometry, capacity);
    std::lock_guard<std::mutex> lock(previewMutex_);
    running_ = running ? 1 : 0;
}

void FrameQueue::beginStartSession(IRGeometry geometry, int32_t capacity)
{
    validateCapacity(capacity);
    const int64_t pxCount = pixels(geometry.height, geometry.width);
    const int64_t poolElems = stackSize(pxCount, capacity);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queueGeometry_ = geometry;
        queuePixelCount_ = pxCount;
        capacity_ = capacity;
        pool_.assign(static_cast<size_t>(poolElems), 0);
        stamps_.assign(static_cast<size_t>(capacity), IRFrameStamp{});
        head_ = 0;
        count_ = 0;
        droppedSinceLastPush_ = 0;
        terminalStatus_ = IR_OK;
        terminalMessage_.clear();
        stopped_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(previewMutex_);
        previewGeometry_ = geometry;
        previewPixelCount_ = pxCount;
        hasLatest_ = false;
        latestPixels_.assign(static_cast<size_t>(pxCount), 0);
        latestStamp_ = {};
        configuredCapacity_ = capacity;
        queueDepth_.store(0, std::memory_order_relaxed);
        running_ = 1;
        fpsTimestampsNs_.fill(0);
        fpsHead_ = 0;
        fpsCount_ = 0;
    }
}

void FrameQueue::resumeSession()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        head_ = 0;
        count_ = 0;
        droppedSinceLastPush_ = 0;
        terminalStatus_ = IR_OK;
        terminalMessage_.clear();
        stopped_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(previewMutex_);
        queueDepth_.store(0, std::memory_order_relaxed);
        running_ = 1;
    }
}

void FrameQueue::resetClosed()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queueGeometry_ = {};
        queuePixelCount_ = 0;
        pool_.clear();
        pool_.shrink_to_fit();
        stamps_.clear();
        head_ = 0;
        count_ = 0;
        droppedSinceLastPush_ = 0;
        terminalStatus_ = IR_OK;
        terminalMessage_.clear();
        stopped_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(previewMutex_);
        previewGeometry_ = {};
        previewPixelCount_ = 0;
        hasLatest_ = false;
        latestPixels_.clear();
        latestPixels_.shrink_to_fit();
        latestStamp_ = {};
        queueDepth_ = 0;
        running_ = 0;
        fpsHead_ = 0;
        fpsCount_ = 0;
    }
    queueCv_.notify_all();
}

void FrameQueue::setRunning(bool running)
{
    std::lock_guard<std::mutex> lock(previewMutex_);
    running_ = running ? 1 : 0;
}

void FrameQueue::recordPreviewLocked(const uint16_t *pixelData, const IRFrameStamp &stamp,
                                     int64_t pxCount)
{
    if (!pixelData || pxCount <= 0 || static_cast<int64_t>(latestPixels_.size()) < pxCount)
        return;

    std::copy_n(pixelData, static_cast<size_t>(pxCount), latestPixels_.data());
    latestStamp_ = stamp;
    hasLatest_ = true;
    previewFrames_.fetch_add(1, std::memory_order_relaxed);

    const int64_t nowNs = steadyNowNs();
    const size_t n = fpsTimestampsNs_.size();
    const size_t writeIdx = (fpsHead_ + fpsCount_) % n;
    fpsTimestampsNs_[writeIdx] = nowNs;
    if (fpsCount_ < n) {
        ++fpsCount_;
    } else {
        fpsHead_ = (fpsHead_ + 1) % n;
    }
}

void FrameQueue::push(const uint16_t *pixelData, const IRFrameStamp &stamp)
{
    if (!pixelData)
        return;
    framesReceived_.fetch_add(1, std::memory_order_relaxed);
    framesDropped_.fetch_add(stamp.lost_frames, std::memory_order_relaxed);

    bool overflowed = false;
    int32_t newDepth = 0;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (stopped_ || queuePixelCount_ <= 0 || capacity_ <= 0)
            return;

        if (count_ < capacity_) {
            const int32_t tail = (head_ + count_) % capacity_;
            std::copy_n(pixelData, static_cast<size_t>(queuePixelCount_),
                        pool_.data() + static_cast<size_t>(tail) * static_cast<size_t>(queuePixelCount_));
            IRFrameStamp queuedStamp = stamp;
            if (droppedSinceLastPush_ > 0) {
                const uint64_t totalLost = uint64_t{queuedStamp.lost_frames} + droppedSinceLastPush_;
                queuedStamp.lost_frames = (totalLost > std::numeric_limits<uint32_t>::max())
                    ? std::numeric_limits<uint32_t>::max()
                    : static_cast<uint32_t>(totalLost);
                queuedStamp.fields |= IR_HAS_LOSS;
                droppedSinceLastPush_ = 0;
            }
            stamps_[static_cast<size_t>(tail)] = queuedStamp;
            ++count_;
            newDepth = count_;
        } else {
            // Preserve the unread contiguous prefix on overflow; drop the newly arriving frame.
            ++droppedSinceLastPush_;
            overflowed = true;
            newDepth = count_;
        }
    }

    queueDepth_.store(newDepth, std::memory_order_relaxed);
    if (overflowed)
        framesDropped_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock previewLock(previewMutex_, std::try_to_lock);
    if (previewLock.owns_lock())
        recordPreviewLocked(pixelData, stamp, previewPixelCount_);
    queueCv_.notify_one();
}

void FrameQueue::publishPulledFrame(const uint16_t *pixelData, const IRFrameStamp &stamp, bool delivered)
{
    framesReceived_.fetch_add(1, std::memory_order_relaxed);
    framesDropped_.fetch_add(stamp.lost_frames, std::memory_order_relaxed);
    if (delivered)
        framesDelivered_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(previewMutex_, std::try_to_lock);
    if (lock.owns_lock())
        recordPreviewLocked(pixelData, stamp, previewPixelCount_);
}

void FrameQueue::recordDelivered()
{
    framesDelivered_.fetch_add(1, std::memory_order_relaxed);
}

FrameQueue::PopStatus FrameQueue::pop(uint16_t *outPixels, IRFrameStamp &outStamp, int32_t timeoutMs,
                                      const std::function<bool()> &stopRequested,
                                      int32_t &terminalCode, std::string &terminalMessage)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool attempted = false;

    for (;;) {
        int32_t newDepth = 0;
        bool popped = false;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (stopRequested && stopRequested())
                return PopStatus::cancelled;

            if (count_ > 0) {
                std::copy_n(pool_.data() + static_cast<size_t>(head_) * static_cast<size_t>(queuePixelCount_),
                            static_cast<size_t>(queuePixelCount_), outPixels);
                outStamp = stamps_[static_cast<size_t>(head_)];
                head_ = (head_ + 1) % capacity_;
                --count_;
                newDepth = count_;
                popped = true;
            } else if (droppedSinceLastPush_ > 0) {
                droppedSinceLastPush_ = 0;
                return PopStatus::gap;
            } else if (terminalStatus_ != IR_OK) {
                terminalCode = terminalStatus_;
                terminalMessage = terminalMessage_;
                return PopStatus::terminal_error;
            } else if (stopped_) {
                return PopStatus::stopped;
            } else {
                const auto now = std::chrono::steady_clock::now();
                if (attempted && now >= deadline)
                    return PopStatus::timeout;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                const auto wait = std::clamp<int64_t>(remaining, 0, 50);
                queueCv_.wait_for(lock, std::chrono::milliseconds(wait));
                attempted = true;
                continue;
            }
        }
        if (popped) {
            queueDepth_.store(newDepth, std::memory_order_relaxed);
            return PopStatus::ok;
        }
    }
}

void FrameQueue::clearMeasurementQueue()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        head_ = 0;
        count_ = 0;
        droppedSinceLastPush_ = 0;
        terminalStatus_ = IR_OK;
        terminalMessage_.clear();
    }
    queueDepth_.store(0, std::memory_order_relaxed);
}

void FrameQueue::setTerminalError(int32_t code, std::string message)
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (terminalStatus_ == IR_OK) {
            terminalStatus_ = code;
            terminalMessage_ = std::move(message);
        }
        stopped_ = true;
    }
    queueCv_.notify_all();
}

void FrameQueue::markStopped()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopped_ = true;
    }
    queueCv_.notify_all();
}

void FrameQueue::wakeWaiters()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
    }
    queueCv_.notify_all();
}

void FrameQueue::tryReadLatest(uint16_t *outPixels, int64_t pixelCapacity, IRGeometry *geometry,
                               IRFrameStamp *stamp, int32_t &hasFrame)
{
    hasFrame = 0;
    std::unique_lock<std::mutex> lock(previewMutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    if (geometry)
        *geometry = previewGeometry_;
    if (!hasLatest_)
        return;

    needed(outPixels, pixelCapacity, previewPixelCount_);
    std::copy_n(latestPixels_.data(), static_cast<size_t>(previewPixelCount_), outPixels);
    if (stamp)
        *stamp = latestStamp_;
    hasFrame = 1;
}

IRCaptureStats FrameQueue::stats()
{
    std::lock_guard<std::mutex> lock(previewMutex_);
    IRCaptureStats out{};
    out.frames_received = framesReceived_.load(std::memory_order_relaxed);
    out.frames_delivered = framesDelivered_.load(std::memory_order_relaxed);
    out.frames_dropped = framesDropped_.load(std::memory_order_relaxed);
    out.preview_frames = previewFrames_.load(std::memory_order_relaxed);
    out.queue_capacity = configuredCapacity_.load(std::memory_order_relaxed);
    out.queue_depth = queueDepth_.load(std::memory_order_relaxed);
    out.width = previewGeometry_.width;
    out.height = previewGeometry_.height;
    out.running = running_;
    out.reserved = 0;

    if (fpsCount_ >= 2) {
        const size_t n = fpsTimestampsNs_.size();
        const int64_t oldest = fpsTimestampsNs_[fpsHead_];
        const int64_t newest = fpsTimestampsNs_[(fpsHead_ + fpsCount_ - 1) % n];
        if (newest > oldest) {
            out.estimated_fps = static_cast<double>(fpsCount_ - 1) * 1.0e9 /
                static_cast<double>(newest - oldest);
        }
    }
    return out;
}

} // namespace ir::acquisition
