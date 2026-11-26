#include "frame_queue.h"
#include "api_common.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace ir::acquisition {
using namespace ir::api;

static_assert(sizeof(IRCaptureStats) == 64, "IRCaptureStats ABI size must be 64 bytes");

namespace {
int64_t steadyNowNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t bytesPerPixel(int32_t dataType)
{
    switch (dataType) {
    case IR_TYPE_U16: return sizeof(uint16_t);
    case IR_TYPE_U32: return sizeof(uint32_t);
    default: throw Error(IR_UNSUPPORTED, "FrameQueue supports U16 and U32 pixels");
    }
}

uint64_t frameBytes(IRGeometry geometry, int32_t dataType)
{
    const auto count = pixels(geometry.height, geometry.width);
    const auto elementSize = bytesPerPixel(dataType);
    require(static_cast<uint64_t>(count) <= std::numeric_limits<uint64_t>::max() / elementSize,
            IR_BAD_ARGUMENT, "Frame byte size overflows");
    return static_cast<uint64_t>(count) * elementSize;
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

void FrameQueue::configure(IRGeometry geometry, int32_t dataType, int32_t capacity)
{
    validateCapacity(capacity);
    const int64_t pxCount = pixels(geometry.height, geometry.width);
    const uint64_t oneFrameBytes = frameBytes(geometry, dataType);
    require(oneFrameBytes <= std::numeric_limits<size_t>::max() / static_cast<size_t>(capacity),
            IR_BAD_ARGUMENT, "FrameQueue storage size overflows");
    const size_t poolBytes = static_cast<size_t>(oneFrameBytes) * static_cast<size_t>(capacity);

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queuePixelCount_ = pxCount;
        queueFrameBytes_ = oneFrameBytes;
        capacity_ = capacity;
        pool_.assign(poolBytes, 0);
        stamps_.assign(static_cast<size_t>(capacity), IRFrameStamp{});
        head_ = 0;
        count_ = 0;
        queueDepth_.store(0, std::memory_order_relaxed);
        setRunning(false);
        droppedSinceLastPush_ = 0;
        terminalStatus_ = IR_OK;
        terminalMessage_[0] = '\0';
        stopped_ = false;
    }

    {
        std::lock_guard<std::mutex> lock(previewMutex_);
        previewGeometry_ = geometry;
        previewDataType_ = dataType;
        previewFrameBytes_ = oneFrameBytes;
        previewRequested_.store(false, std::memory_order_relaxed);
        hasLatest_ = false;
        latestPixels_.assign(static_cast<size_t>(oneFrameBytes), 0);
        latestStamp_ = {};
        framesReceived_.store(0, std::memory_order_relaxed);
        framesDelivered_.store(0, std::memory_order_relaxed);
        framesDropped_.store(0, std::memory_order_relaxed);
        previewFrames_.store(0, std::memory_order_relaxed);
        configuredCapacity_.store(capacity, std::memory_order_relaxed);
        fpsTimestampsNs_.fill(0);
        fpsHead_ = 0;
        fpsCount_ = 0;
    }
}

void FrameQueue::resetSession(IRGeometry geometry, int32_t dataType, int32_t capacity, bool running)
{
    configure(geometry, dataType, capacity);
    setRunning(running);
}

void FrameQueue::beginStartSession()
{
    resumeSession();
    std::lock_guard<std::mutex> lock(previewMutex_);
    hasLatest_ = false;
    latestStamp_ = {};
    fpsTimestampsNs_.fill(0);
    fpsHead_ = 0;
    fpsCount_ = 0;
}

void FrameQueue::resumeSession()
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    head_ = 0;
    count_ = 0;
    queueDepth_.store(0, std::memory_order_relaxed);
    setRunning(false);
    droppedSinceLastPush_ = 0;
    terminalStatus_ = IR_OK;
    terminalMessage_[0] = '\0';
    stopped_ = false;
}

void FrameQueue::resetClosed()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queuePixelCount_ = 0;
        queueFrameBytes_ = 0;
        pool_.clear();
        pool_.shrink_to_fit();
        stamps_.clear();
        head_ = 0;
        count_ = 0;
        queueDepth_.store(0, std::memory_order_relaxed);
        setRunning(false);
        droppedSinceLastPush_ = 0;
        terminalStatus_ = IR_OK;
        terminalMessage_[0] = '\0';
        stopped_ = true;
    }
    {
        std::lock_guard<std::mutex> lock(previewMutex_);
        previewGeometry_ = {};
        previewDataType_ = IR_TYPE_U16;
        previewFrameBytes_ = 0;
        previewRequested_.store(false, std::memory_order_relaxed);
        hasLatest_ = false;
        latestPixels_.clear();
        latestPixels_.shrink_to_fit();
        latestStamp_ = {};
        fpsHead_ = 0;
        fpsCount_ = 0;
    }
    queueCv_.notify_all();
}

void FrameQueue::setRunning(bool running) noexcept
{
    running_.store(running, std::memory_order_relaxed);
}

void FrameQueue::recordPreviewLocked(const void *pixelData, const IRFrameStamp &stamp,
                                     bool snapshotRequested)
{
    if (!pixelData || previewFrameBytes_ == 0 || latestPixels_.size() < previewFrameBytes_)
        return;

    const int64_t nowNs = steadyNowNs();
    if (snapshotRequested) {
        std::memcpy(latestPixels_.data(), pixelData, static_cast<size_t>(previewFrameBytes_));
        latestStamp_ = stamp;
        hasLatest_ = true;
        previewFrames_.fetch_add(1, std::memory_order_relaxed);
    }

    const size_t n = fpsTimestampsNs_.size();
    const size_t writeIdx = (fpsHead_ + fpsCount_) % n;
    fpsTimestampsNs_[writeIdx] = nowNs;
    if (fpsCount_ < n) {
        ++fpsCount_;
    } else {
        fpsHead_ = (fpsHead_ + 1) % n;
    }
}

void FrameQueue::push(const void *pixelData, const IRFrameStamp &stamp)
{
    if (!pixelData)
        return;
    framesReceived_.fetch_add(1, std::memory_order_relaxed);
    framesDropped_.fetch_add(stamp.lost_frames, std::memory_order_relaxed);

    bool overflowed = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (stopped_ || queuePixelCount_ <= 0 || queueFrameBytes_ == 0 || capacity_ <= 0)
            return;

        if (count_ < capacity_) {
            const int32_t tail = (head_ + count_) % capacity_;
            std::memcpy(pool_.data() + static_cast<size_t>(tail) * static_cast<size_t>(queueFrameBytes_),
                        pixelData, static_cast<size_t>(queueFrameBytes_));
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
            queueDepth_.store(count_, std::memory_order_relaxed);
        } else {
            // Preserve the unread contiguous prefix on overflow; drop the newly arriving frame.
            ++droppedSinceLastPush_;
            overflowed = true;
        }
    }

    if (overflowed)
        framesDropped_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock previewLock(previewMutex_, std::try_to_lock);
    if (previewLock.owns_lock())
        recordPreviewLocked(pixelData, stamp, previewRequested_.exchange(false, std::memory_order_relaxed));
    queueCv_.notify_one();
}

void FrameQueue::publishPulledFrame(const void *pixelData, const IRFrameStamp &stamp, bool delivered)
{
    framesReceived_.fetch_add(1, std::memory_order_relaxed);
    framesDropped_.fetch_add(stamp.lost_frames, std::memory_order_relaxed);
    if (delivered)
        framesDelivered_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(previewMutex_, std::try_to_lock);
    if (lock.owns_lock())
        recordPreviewLocked(pixelData, stamp, true);
}

void FrameQueue::recordDelivered()
{
    framesDelivered_.fetch_add(1, std::memory_order_relaxed);
}

FrameQueue::PopStatus FrameQueue::pop(void *outPixels, IRFrameStamp &outStamp, int32_t timeoutMs,
                                      const std::function<bool()> &stopRequested,
                                      int32_t &terminalCode, std::string &terminalMessage)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool attempted = false;

    std::unique_lock<std::mutex> lock(queueMutex_);
    for (;;) {
        if (stopRequested && stopRequested())
            return PopStatus::cancelled;

        if (count_ > 0) {
            if (outPixels)
                std::memcpy(outPixels,
                            pool_.data() + static_cast<size_t>(head_) * static_cast<size_t>(queueFrameBytes_),
                            static_cast<size_t>(queueFrameBytes_));
            outStamp = stamps_[static_cast<size_t>(head_)];
            head_ = (head_ + 1) % capacity_;
            --count_;
            queueDepth_.store(count_, std::memory_order_relaxed);
            return PopStatus::ok;
        }
        if (droppedSinceLastPush_ > 0) {
            droppedSinceLastPush_ = 0;
            return PopStatus::gap;
        }
        if (terminalStatus_ != IR_OK) {
            terminalCode = terminalStatus_;
            terminalMessage = terminalMessage_.data();
            return PopStatus::terminal_error;
        }
        if (stopped_)
            return PopStatus::stopped;

        const auto now = std::chrono::steady_clock::now();
        if (attempted && now >= deadline)
            return PopStatus::timeout;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const auto wait = std::clamp<int64_t>(remaining, 0, 50);
        queueCv_.wait_for(lock, std::chrono::milliseconds(wait));
        attempted = true;
    }
}

void FrameQueue::clearMeasurementQueue()
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    head_ = 0;
    count_ = 0;
    queueDepth_.store(0, std::memory_order_relaxed);
    droppedSinceLastPush_ = 0;
    terminalStatus_ = IR_OK;
    terminalMessage_[0] = '\0';
}

void FrameQueue::setTerminalError(int32_t code, const char *message)
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (terminalStatus_ == IR_OK) {
            terminalStatus_ = code;
            copyText(terminalMessage_.data(), static_cast<int32_t>(terminalMessage_.size()), message);
        }
        stopped_ = true;
        setRunning(false);
    }
    queueCv_.notify_all();
}

void FrameQueue::markStopped()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopped_ = true;
        setRunning(false);
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

void FrameQueue::tryReadLatest(IRFrameBatchView &batch)
{
    const int32_t requestedType = batch.pixels.data_type;
    const int32_t requestedWidth = batch.pixels.width;
    const int32_t requestedHeight = batch.pixels.height;
    const int32_t requestedFrames = batch.pixels.frames;
    batch.pixels.frames = 0;
    // 即使本次拿不到锁也登记需求：下一帧起推送路径开始保留预览像素。
    previewRequested_.store(true, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(previewMutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    batch.pixels.data_type = previewDataType_;
    batch.pixels.width = previewGeometry_.width;
    batch.pixels.height = previewGeometry_.height;
    batch.valid_bits = previewGeometry_.valid_bits;
    if (!hasLatest_)
        return;

    require(requestedType == previewDataType_, IR_BAD_ARGUMENT, "Preview data type differs from source");
    require(requestedWidth == previewGeometry_.width && requestedHeight == previewGeometry_.height,
            IR_BAD_ARGUMENT, "Preview geometry differs from source");
    require(requestedFrames >= 1, IR_BAD_ARGUMENT, "Preview batch must have capacity for one frame");
    needed(batch.pixels.data, 1, 1);
    require(batch.pixels.byte_size >= previewFrameBytes_, IR_BUFFER_TOO_SMALL,
            "Preview pixel buffer is too small");
    const uint64_t elementSize = bytesPerPixel(previewDataType_);
    require(reinterpret_cast<std::uintptr_t>(batch.pixels.data) % elementSize == 0,
            IR_BAD_ARGUMENT, "Preview buffer is not aligned for its data type");
    if (batch.stamps) {
        needed(batch.stamps, batch.stamp_capacity, 1);
        batch.stamps[0] = latestStamp_;
    } else {
        require(batch.stamp_capacity == 0, IR_NULL_POINTER, "Missing preview stamp buffer");
    }
    std::memcpy(batch.pixels.data, latestPixels_.data(), static_cast<size_t>(previewFrameBytes_));
    batch.pixels.frames = 1;
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
    out.running = running_.load(std::memory_order_relaxed) ? 1 : 0;
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
