#include "frame_source.h"
#include "api_common.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <utility>
#include <vector>
using namespace ir::api;
namespace ir::acquisition {
static_assert(sizeof(IRCaptureStats) == 64, "IRCaptureStats ABI size must be 64 bytes");
Capture &capture() { static Capture value; return value; }

namespace {
uint64_t checkedMultiply(uint64_t left, uint64_t right)
{
    require(right == 0 || left <= std::numeric_limits<uint64_t>::max() / right,
            IR_BAD_ARGUMENT, "Capture memory estimate overflows");
    return left * right;
}

uint64_t checkedAdd(uint64_t left, uint64_t right)
{
    require(right <= std::numeric_limits<uint64_t>::max() - left,
            IR_BAD_ARGUMENT, "Capture memory estimate overflows");
    return left + right;
}

uint64_t pixelBytes(int32_t dataType);

uint64_t estimateBufferedBytes(IRGeometry geometry, int32_t queueCapacity,
                               int32_t dataType, uint64_t backendBytes)
{
    const uint64_t frameBytes = checkedMultiply(
        static_cast<uint64_t>(pixels(geometry.height, geometry.width)), pixelBytes(dataType));
    uint64_t queueBytes = checkedMultiply(
        frameBytes, static_cast<uint64_t>(queueCapacity) + 2);
    queueBytes = checkedAdd(queueBytes, checkedMultiply(
        static_cast<uint64_t>(queueCapacity), sizeof(IRFrameStamp)));
    return checkedAdd(queueBytes, backendBytes);
}

uint64_t pixelBytes(int32_t dataType)
{
    switch (dataType) {
    case IR_TYPE_U16: return sizeof(uint16_t);
    case IR_TYPE_U32: return sizeof(uint32_t);
    default: throw Error(IR_UNSUPPORTED, "Frame sources support U16 and U32 pixels");
    }
}

int32_t maxValidBits(int32_t dataType)
{
    switch (dataType) {
    case IR_TYPE_U16: return 16;
    case IR_TYPE_U32: return 32;
    default: throw Error(IR_UNSUPPORTED, "Frame sources support U16 and U32 pixels");
    }
}

class FrameScratch {
public:
    explicit FrameScratch(const FrameSource &source)
    {
        const auto geometry = source.geometry();
        const auto pixelCount = static_cast<size_t>(pixels(geometry.height, geometry.width));
        const auto dataType = source.dataType();
        if (dataType == IR_TYPE_U16) {
            pixels16_.resize(pixelCount);
        } else if (dataType == IR_TYPE_U32) {
            pixels32_.resize(pixelCount);
        } else {
            throw Error(IR_UNSUPPORTED, "Frame sources support U16 and U32 pixels");
        }
    }

    void *data() noexcept
    {
        return pixels32_.empty() ? static_cast<void *>(pixels16_.data()) : pixels32_.data();
    }

private:
    std::vector<uint16_t> pixels16_;
    std::vector<uint32_t> pixels32_;
};

}

std::unique_lock<std::mutex> lockCapture()
{
    std::unique_lock lock(capture().mutex, std::try_to_lock);
    require(lock.owns_lock(), IR_BUSY, "Another capture or measurement is active");
    return lock;
}
IRFrameSourceInfo Capture::open(int kind, const char *name, int bits)
{
    return open(kind, name, bits, queueCapacity_, kDefaultMemoryLimitBytes);
}

IRFrameSourceInfo Capture::open(int kind, const char *name, int bits, int32_t queueCapacity,
                                uint64_t memoryLimitBytes)
{
    checkOperationAccess();
    require(!source, IR_BUSY, "A frame source is open");
    require(name && *name && bits >= 0 && bits <= 32, IR_BAD_ARGUMENT, "Invalid source name or valid bits");
    std::unique_ptr<FrameSource> opened;
    if (kind == IR_RAW_REPLAY) {
        opened = openReplay(name);
        require(!bits || bits == opened->geometry().valid_bits, IR_BAD_ARGUMENT, "Depth differs from RAW index");
    }
#if defined(__linux__)
    else if (kind == IR_V4L2) {
        opened = openV4l2(name, bits);
    }
#endif
#if defined(_WIN32)
    else if (kind == IR_DIRECTSHOW) {
        opened = openDirectShow(name, bits);
    }
#endif
    else throw Error(IR_UNSUPPORTED, "Frame backend is not available on this platform");
    const auto geometry = opened->geometry();
    const int32_t dataType = opened->dataType();
    require(geometry.valid_bits >= 0 && geometry.valid_bits <= maxValidBits(dataType),
            IR_BAD_ARGUMENT, "Source valid_bits exceeds its pixel type");
    const uint64_t bufferedBytes = estimateBufferedBytes(
        geometry, queueCapacity, dataType, opened->bufferedBytes());
    require(bufferedBytes <= memoryLimitBytes,
            IR_BAD_ARGUMENT, "Capture buffers exceed station.acquisition.max_inflight_bytes");
    std::string openedName{name};
    frameQueue_.configure(geometry, dataType, queueCapacity);
    queueCapacity_ = queueCapacity;
    memoryLimitBytes_ = memoryLimitBytes;
    sourceName = std::move(openedName);
    source = std::move(opened);
    owner = std::this_thread::get_id();
    backend = kind;
    return {geometry, dataType};
}
void Capture::close()
{
    checkOperationAccess();
    if (!source) return;
    checkOwner();
    stopPump();
    // 即使后端报告停流失败，离开作用域时也要释放采集源。
    auto closing = std::move(source);
    const bool wasRunning = running;
    running = false;
    sourceName.clear();
    owner = {};
    backend = 0;
    memoryLimitBytes_ = kDefaultMemoryLimitBytes;
    try {
        if (wasRunning) closing->stop();
    } catch (...) {
        frameQueue_.resetClosed();
        throw;
    }
    frameQueue_.resetClosed();
}
void Capture::checkOwner() const
{
    checkOperationAccess();
    require(bool(source), IR_BAD_ARGUMENT, "Open a frame source first");
    require(owner == std::this_thread::get_id(), IR_BAD_ARGUMENT, "Use the opening thread");
}
void Capture::checkOperationAccess() const
{
    std::lock_guard lock(operationMutex_);
    const auto state = stopState_.load(std::memory_order_acquire);
    require(state == StopState::idle || operationOwner_ == std::this_thread::get_id(),
            IR_BUSY, "Another thread owns the active SDK operation");
}
bool Capture::beginOperation() noexcept
{
    std::lock_guard lock(operationMutex_);
    if (stopState_.load(std::memory_order_acquire) != StopState::idle)
        return false;
    operationOwner_ = std::this_thread::get_id();
    stopState_.store(StopState::active, std::memory_order_release);
    return true;
}
void Capture::endOperation() noexcept
{
    std::lock_guard lock(operationMutex_);
    if (operationOwner_ != std::this_thread::get_id())
        return;
    operationOwner_ = {};
    stopState_.store(StopState::idle, std::memory_order_release);
}
Capture::OperationScope Capture::enterOperationScope() noexcept
{
    std::lock_guard lock(operationMutex_);
    const auto state = stopState_.load(std::memory_order_acquire);
    if (state == StopState::idle) {
        operationOwner_ = std::this_thread::get_id();
        stopState_.store(StopState::active, std::memory_order_release);
        return OperationScope::owned;
    }
    if (operationOwner_ == std::this_thread::get_id())
        return OperationScope::joined;
    return OperationScope::unavailable;
}
void Capture::leaveOperationScope(OperationScope scope) noexcept
{
    if (scope == OperationScope::owned)
        endOperation();
}
void Capture::requestStop() noexcept
{
    auto expected = StopState::active;
    if (!stopState_.compare_exchange_strong(expected, StopState::requested,
                                            std::memory_order_acq_rel))
        return;
    frameQueue_.wakeWaiters();
}
bool Capture::isStopRequested() const noexcept
{
    return stopState_.load(std::memory_order_acquire) == StopState::requested;
}
void Capture::checkStop() const
{
    require(stopState_.load() != StopState::requested, IR_CANCELLED, "Measurement or acquisition cancelled");
}
void Capture::setQueueCapacity(int32_t capacity)
{
    checkOperationAccess();
    FrameQueue::validateCapacity(capacity);
    require(!running, IR_BUSY, "Cannot change frame queue capacity while capture is running");
    if (source) {
        const uint64_t bufferedBytes = estimateBufferedBytes(
            source->geometry(), capacity, source->dataType(), source->bufferedBytes());
        require(bufferedBytes <= memoryLimitBytes_, IR_BAD_ARGUMENT,
                "Capture buffers exceed station.acquisition.max_inflight_bytes");
        frameQueue_.configure(source->geometry(), source->dataType(), capacity);
    } else {
        frameQueue_.setIdleCapacity(capacity);
    }
    queueCapacity_ = capacity;
}
uint64_t Capture::bufferedBytes() const
{
    if (!source)
        return 0;
    return estimateBufferedBytes(source->geometry(), queueCapacity_, source->dataType(), source->bufferedBytes());
}
void Capture::startPump()
{
    FrameScratch scratch(*source);
    pumpStop_.store(false, std::memory_order_release);
    frameQueue_.setRunning(true);
    pumpThread_ = std::thread([this, scratch = std::move(scratch)]() mutable {
        pumpLoop(scratch.data());
    });
    pumpActive_ = true;
}
void Capture::stopPump()
{
    pumpStop_.store(true, std::memory_order_release);
    frameQueue_.markStopped();
    if (pumpThread_.joinable())
        pumpThread_.join();
    pumpActive_ = false;
}
void Capture::pumpLoop(void *frame)
{
    while (!pumpStop_.load(std::memory_order_acquire)) {
        IRFrameStamp stamp{};
        try {
            source->read(frame, stamp, 50);
            if (pumpStop_.load(std::memory_order_acquire))
                break;
            frameQueue_.push(frame, stamp);
        } catch (const Error &err) {
            if (pumpStop_.load(std::memory_order_acquire))
                break;
            if (err.code == IR_TIMEOUT)
                continue;
            if (err.code == IR_END_OF_STREAM) {
                frameQueue_.markStopped();
                break;
            }
            frameQueue_.setTerminalError(err.code, err.what());
            break;
        } catch (const std::exception &ex) {
            if (pumpStop_.load(std::memory_order_acquire))
                break;
            frameQueue_.setTerminalError(IR_IO_ERROR, ex.what());
            break;
        } catch (...) {
            if (pumpStop_.load(std::memory_order_acquire))
                break;
            frameQueue_.setTerminalError(IR_IO_ERROR, "Unknown capture pump error");
            break;
        }
    }
}
void Capture::start()
{
    checkOperationAccess();
    checkStop();
    require(!running, IR_BUSY, "Source is already running");
    frameQueue_.beginStartSession();
    try {
        source->start();
        running = true;
        if (source->isLive())
            startPump();
        else
            frameQueue_.setRunning(true);
    } catch (...) {
        stopPump();
        running = false;
        try { source->stop(); } catch (...) {}
        throw;
    }
}
void Capture::stop()
{
    stopPump();
    const bool wasRunning = running;
    running = false;
    if (wasRunning) source->stop();
}
void Capture::read(int timeoutMs, IRFrameBatchView &batch)
{
    const int32_t count = batch.pixels.frames;
    batch.pixels.frames = 0;
    require(running && timeoutMs >= 0, IR_BAD_ARGUMENT, "Start source and specify a nonnegative timeout");
    const auto g = source->geometry();
    const auto n = pixels(g.height, g.width);
    const int32_t dataType = source->dataType();
    const uint64_t elementSize = pixelBytes(dataType);
    require(count > 0, IR_BAD_ARGUMENT, "Specify a positive frame count");
    require(batch.pixels.data_type == dataType, IR_BAD_ARGUMENT, "Pixel data type differs from source");
    require(batch.pixels.width == g.width && batch.pixels.height == g.height,
            IR_BAD_ARGUMENT, "Pixel geometry differs from source");
    needed(batch.pixels.data, 1, 1);
    require(reinterpret_cast<uintptr_t>(batch.pixels.data) % elementSize == 0,
            IR_BAD_ARGUMENT, "Pixel buffer is not aligned for its data type");
    require(static_cast<uint64_t>(n) <= std::numeric_limits<uint64_t>::max() / elementSize,
            IR_BAD_ARGUMENT, "Frame byte size overflows");
    const uint64_t oneFrameBytes = static_cast<uint64_t>(n) * elementSize;
    require(static_cast<uint64_t>(count) <= std::numeric_limits<uint64_t>::max() / oneFrameBytes,
            IR_BAD_ARGUMENT, "Batch byte size overflows");
    const uint64_t requiredBytes = oneFrameBytes * static_cast<uint64_t>(count);
    require(batch.pixels.byte_size >= requiredBytes, IR_BUFFER_TOO_SMALL, "Pixel buffer is too small");
    needed(batch.stamps, batch.stamp_capacity, count);
    batch.valid_bits = g.valid_bits;
    auto *pixelBytes = static_cast<uint8_t *>(batch.pixels.data);
    int completed = 0;
    while (completed < count) {
        checkStop();
        void *frameOut = pixelBytes + static_cast<uint64_t>(completed) * oneFrameBytes;
        if (pumpActive_) {
            IRFrameStamp stamp{};
            int32_t termCode = IR_OK;
            std::string termMsg;
            const auto popStatus = frameQueue_.pop(
                frameOut, stamp, timeoutMs,
                [this] { return stopState_.load() == StopState::requested; },
                termCode, termMsg);
            switch (popStatus) {
            case FrameQueue::PopStatus::ok:
                break;
            case FrameQueue::PopStatus::cancelled:
                throw Error(IR_CANCELLED, "Measurement or acquisition cancelled");
            case FrameQueue::PopStatus::timeout:
                throw Error(IR_TIMEOUT, "Frame wait timed out");
            case FrameQueue::PopStatus::gap:
                throw Error(IR_FRAME_GAP, "Frame queue overflow or sequence discontinuity");
            case FrameQueue::PopStatus::terminal_error:
                throw Error(termCode, termMsg);
            case FrameQueue::PopStatus::stopped:
                throw Error(IR_END_OF_STREAM, "End of RAW capture");
            }
            bool gap = stamp.lost_frames != 0;
            if (completed) {
                const auto &last = batch.stamps[completed - 1];
                const auto both = last.fields & stamp.fields;
                gap |= (both & IR_HAS_SEQUENCE) && stamp.sequence != last.sequence + 1;
                gap |= (both & IR_HAS_DRIVER_SEQUENCE) && stamp.driver_sequence != last.driver_sequence + 1;
            }
            require(!gap, IR_FRAME_GAP, "Frame sequence discontinuity");
            frameQueue_.recordDelivered();
            batch.stamps[completed++] = stamp;
            batch.pixels.frames = completed;
            checkStop();
            continue;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        IRFrameStamp stamp{};
        bool attempted = false;
        for (;;) {
            checkStop();
            const auto now = std::chrono::steady_clock::now();
            if (attempted && now >= deadline)
                throw Error(IR_TIMEOUT, "Frame wait timed out");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const auto wait = std::clamp<int64_t>(remaining, 0, 50);
            try {
                source->read(frameOut, stamp, int(wait));
                break;
            } catch (const Error &error) {
                if (error.code != IR_TIMEOUT || std::chrono::steady_clock::now() >= deadline) throw;
            }
            attempted = true;
        }
        bool gap = stamp.lost_frames != 0;
        if (completed) {
            const auto &last = batch.stamps[completed - 1];
            const auto both = last.fields & stamp.fields;
            gap |= (both & IR_HAS_SEQUENCE) && stamp.sequence != last.sequence + 1;
            gap |= (both & IR_HAS_DRIVER_SEQUENCE) && stamp.driver_sequence != last.driver_sequence + 1;
        }
        require(!gap, IR_FRAME_GAP, "Frame sequence discontinuity");
        frameQueue_.publishPulledFrame(frameOut, stamp, true);
        batch.stamps[completed++] = stamp;
        batch.pixels.frames = completed;
        checkStop();
    }
}
void Capture::flush(int32_t discardFrames, int32_t timeoutMs, int32_t &discarded)
{
    discarded = 0;
    require(running && discardFrames >= 0 && timeoutMs >= 0, IR_BAD_ARGUMENT,
            "Start source and specify nonnegative discard count and timeout");
    checkStop();
    if (pumpActive_) {
        stopPump();
        try {
            source->flush();
            frameQueue_.resumeSession();
            startPump();
        } catch (...) {
            try { stop(); } catch (...) {}
            throw;
        }
        while (discarded < discardFrames) {
            checkStop();
            IRFrameStamp stamp{};
            int32_t termCode = IR_OK;
            std::string termMsg;
            const auto popStatus = frameQueue_.pop(
                nullptr, stamp, timeoutMs,
                [this] { return stopState_.load() == StopState::requested; },
                termCode, termMsg);
            switch (popStatus) {
            case FrameQueue::PopStatus::ok:
                ++discarded;
                break;
            case FrameQueue::PopStatus::cancelled:
                throw Error(IR_CANCELLED, "Measurement or acquisition cancelled");
            case FrameQueue::PopStatus::timeout:
                throw Error(IR_TIMEOUT, "Frame flush wait timed out");
            case FrameQueue::PopStatus::terminal_error:
                throw Error(termCode, termMsg);
            case FrameQueue::PopStatus::stopped:
                throw Error(IR_END_OF_STREAM, "End of RAW capture");
            case FrameQueue::PopStatus::gap:
                break;
            }
            checkStop();
        }
        return;
    }
    source->flush();
    if (discardFrames == 0)
        return;

    FrameScratch scratch(*source);
    for (int32_t i = 0; i < discardFrames; ++i) {
        checkStop();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        IRFrameStamp stamp{};
        bool attempted = false;
        for (;;) {
            checkStop();
            const auto now = std::chrono::steady_clock::now();
            if (attempted && now >= deadline)
                throw Error(IR_TIMEOUT, "Frame flush wait timed out");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const auto wait = std::clamp<int64_t>(remaining, 0, 50);
            try {
                source->read(scratch.data(), stamp, int(wait));
                break;
            } catch (const Error &error) {
                if (error.code != IR_TIMEOUT || std::chrono::steady_clock::now() >= deadline) throw;
            }
            attempted = true;
        }
        frameQueue_.publishPulledFrame(scratch.data(), stamp, false);
        ++discarded;
        checkStop();
    }
}
}
using namespace ir::acquisition;
extern "C" int32_t IR_OpenFrames(int32_t backend, const char *name, int32_t bits,
    IRFrameSourceInfo *actual, char *err, int32_t size)
noexcept{
    return guarded(err, size, [&] {
        auto lock = lockCapture();
        needed(actual, 1, 1);
        *actual = capture().open(backend, name, bits);
    });
}
extern "C" int32_t IR_StartFrames(char *err, int32_t size)
noexcept{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner();
        require(!c.running, IR_BUSY, "Source is already running");
        require(c.beginOperation(), IR_BUSY, "Another cancellable SDK operation is active");
        try {
            c.start();
        } catch (...) {
            c.endOperation();
            throw;
        }
    });
}
extern "C" int32_t IR_ReadFrames(int32_t ms, IRFrameBatchView *batch, char *err, int32_t size)
noexcept{
    const int32_t requestedFrames = batch ? batch->pixels.frames : 0;
    IRFrameBatchView work{};
    if (batch) {
        work = *batch;
        work.pixels.frames = 0;
        batch->pixels.frames = 0;
    }
    const int32_t status = guarded(err, size, [&] {
        needed(batch, 1, 1);
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner();
        work.pixels.frames = requestedFrames;
        c.read(ms, work);
    });
    if (batch) {
        batch->pixels.frames = work.pixels.frames;
        batch->valid_bits = work.valid_bits;
    }
    return status;
}
extern "C" int32_t IR_FlushFrames(int32_t discard_frames, int32_t timeout_ms,
    int32_t *discarded, char *err, int32_t size)
noexcept{
    int32_t localDiscarded = 0;
    if (discarded) *discarded = 0;
    const auto status = guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner();
        try {
            c.flush(discard_frames, timeout_ms, localDiscarded);
        } catch (...) {
            if (!c.running)
                c.endOperation();
            throw;
        }
    });
    if (discarded) *discarded = localDiscarded;
    return status;
}
extern "C" void IR_RequestStop(void) noexcept{ capture().requestStop(); }
extern "C" int32_t IR_StopFrames(char *err, int32_t size)
noexcept{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner();
        try {
            c.stop();
        } catch (...) {
            c.endOperation();
            throw;
        }
        c.endOperation();
    });
}
extern "C" int32_t IR_CloseFrames(char *err, int32_t size)
noexcept{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        if (!c.source) return;
        c.checkOwner();
        c.endOperation();
        c.close();
    });
}
extern "C" int32_t IR_SetFrameQueueCapacity(int32_t capacity, char *err, int32_t size)
noexcept{
    return guarded(err, size, [&] {
        auto lock = lockCapture();
        capture().setQueueCapacity(capacity);
    });
}
extern "C" int32_t IR_TryReadLatestFrame(IRFrameBatchView *batch, char *err, int32_t size)
noexcept{
    const int32_t requestedFrames = batch ? batch->pixels.frames : 0;
    IRFrameBatchView work{};
    if (batch) {
        work = *batch;
        work.pixels.frames = 0;
        batch->pixels.frames = 0;
    }
    const int32_t status = guarded(err, size, [&] {
        needed(batch, 1, 1);
        work.pixels.frames = requestedFrames;
        capture().frameQueue().tryReadLatest(work);
    });
    if (batch) {
        batch->pixels.frames = work.pixels.frames;
        batch->pixels.data_type = work.pixels.data_type;
        batch->pixels.width = work.pixels.width;
        batch->pixels.height = work.pixels.height;
        batch->valid_bits = work.valid_bits;
    }
    return status;
}
extern "C" int32_t IR_GetCaptureStats(IRCaptureStats *stats, char *err, int32_t size)
noexcept{
    if (stats) *stats = {};
    return guarded(err, size, [&] {
        needed(stats, 1, 1);
        *stats = capture().frameQueue().stats();
    });
}
