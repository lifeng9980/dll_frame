#include "frame_source.h"
#include "api_common.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
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

uint64_t estimateBufferedBytes(IRGeometry geometry, int32_t queueCapacity,
                               uint64_t backendBytes)
{
    const uint64_t frameBytes = checkedMultiply(
        static_cast<uint64_t>(pixels(geometry.height, geometry.width)), sizeof(uint16_t));
    uint64_t queueBytes = checkedMultiply(
        frameBytes, static_cast<uint64_t>(queueCapacity) + 2);
    queueBytes = checkedAdd(queueBytes, checkedMultiply(
        static_cast<uint64_t>(queueCapacity), sizeof(IRFrameStamp)));
    return checkedAdd(queueBytes, backendBytes);
}

}

std::unique_lock<std::mutex> lockCapture()
{
    std::unique_lock lock(capture().mutex, std::try_to_lock);
    require(lock.owns_lock(), IR_BUSY, "Another capture or measurement is active");
    return lock;
}
IRGeometry Capture::open(int kind, const char *name, int bits)
{
    return open(kind, name, bits, queueCapacity_, kDefaultMemoryLimitBytes);
}

IRGeometry Capture::open(int kind, const char *name, int bits, int32_t queueCapacity,
                         uint64_t memoryLimitBytes)
{
    checkOperationAccess();
    require(!source, IR_BUSY, "A frame source is open");
    require(name && *name && bits >= 0 && bits <= 16, IR_BAD_ARGUMENT, "Invalid source name or valid bits");
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
    const uint64_t bufferedBytes = estimateBufferedBytes(
        geometry, queueCapacity, opened->bufferedBytes());
    require(bufferedBytes <= memoryLimitBytes,
            IR_BAD_ARGUMENT, "Capture buffers exceed station.acquisition.max_inflight_bytes");
    std::string openedName{name};
    frameQueue_.configure(geometry, queueCapacity);
    queueCapacity_ = queueCapacity;
    memoryLimitBytes_ = memoryLimitBytes;
    sourceName = std::move(openedName);
    source = std::move(opened);
    owner = std::this_thread::get_id();
    backend = kind;
    return geometry;
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
            source->geometry(), capacity, source->bufferedBytes());
        require(bufferedBytes <= memoryLimitBytes_, IR_BAD_ARGUMENT,
                "Capture buffers exceed station.acquisition.max_inflight_bytes");
        frameQueue_.configure(source->geometry(), capacity);
    } else {
        frameQueue_.setIdleCapacity(capacity);
    }
    queueCapacity_ = capacity;
}
uint64_t Capture::bufferedBytes() const
{
    if (!source)
        return 0;
    return estimateBufferedBytes(source->geometry(), queueCapacity_, source->bufferedBytes());
}
void Capture::startPump()
{
    pumpStop_.store(false, std::memory_order_release);
    pumpActive_ = true;
    pumpThread_ = std::thread([this] { pumpLoop(); });
}
void Capture::stopPump()
{
    pumpStop_.store(true, std::memory_order_release);
    frameQueue_.markStopped();
    if (pumpThread_.joinable())
        pumpThread_.join();
    pumpActive_ = false;
    frameQueue_.setRunning(false);
}
void Capture::pumpLoop()
{
    const auto g = source->geometry();
    const auto n = pixels(g.height, g.width);
    std::vector<uint16_t> scratch(static_cast<size_t>(n), 0);
    while (!pumpStop_.load(std::memory_order_acquire)) {
        IRFrameStamp stamp{};
        try {
            source->read(scratch.data(), stamp, 50);
            if (pumpStop_.load(std::memory_order_acquire))
                break;
            frameQueue_.push(scratch.data(), stamp);
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
    frameQueue_.beginStartSession(source->geometry(), queueCapacity_);
    source->start();
    running = true;
    if (source->isLive()) {
        try {
            startPump();
        } catch (...) {
            running = false;
            try { source->stop(); } catch (...) {}
            frameQueue_.markStopped();
            frameQueue_.setRunning(false);
            throw;
        }
    }
}
void Capture::stop()
{
    stopPump();
    if (running) source->stop();
    running = false;
}
void Capture::read(int count, int timeoutMs, uint16_t *out, int64_t capacity,
                   IRFrameStamp *stamps, int stampCapacity, int &completed)
{
    require(running && timeoutMs >= 0, IR_BAD_ARGUMENT, "Start source and specify a nonnegative timeout");
    const auto g = source->geometry();
    const auto n = pixels(g.height, g.width);
    needed(out, capacity, stackSize(n, count));
    needed(stamps, stampCapacity, count);
    while (completed < count) {
        checkStop();
        if (pumpActive_) {
            IRFrameStamp stamp{};
            int32_t termCode = IR_OK;
            std::string termMsg;
            const auto popStatus = frameQueue_.pop(
                out + completed * n, stamp, timeoutMs,
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
                const auto &last = stamps[completed - 1];
                const auto both = last.fields & stamp.fields;
                gap |= (both & IR_HAS_SEQUENCE) && stamp.sequence != last.sequence + 1;
                gap |= (both & IR_HAS_DRIVER_SEQUENCE) && stamp.driver_sequence != last.driver_sequence + 1;
            }
            require(!gap, IR_FRAME_GAP, "Frame sequence discontinuity");
            frameQueue_.recordDelivered();
            stamps[completed++] = stamp;
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
                source->read(out + completed * n, stamp, int(wait));
                break;
            } catch (const Error &error) {
                if (error.code != IR_TIMEOUT || std::chrono::steady_clock::now() >= deadline) throw;
            }
            attempted = true;
        }
        bool gap = stamp.lost_frames != 0;
        if (completed) {
            const auto &last = stamps[completed - 1];
            const auto both = last.fields & stamp.fields;
            gap |= (both & IR_HAS_SEQUENCE) && stamp.sequence != last.sequence + 1;
            gap |= (both & IR_HAS_DRIVER_SEQUENCE) && stamp.driver_sequence != last.driver_sequence + 1;
        }
        require(!gap, IR_FRAME_GAP, "Frame sequence discontinuity");
        frameQueue_.publishPulledFrame(out + completed * n, stamp, true);
        stamps[completed++] = stamp;
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
        source->flush();
        frameQueue_.resumeSession();
        startPump();
        if (discardFrames == 0)
            return;

        const auto g = source->geometry();
        const auto n = pixels(g.height, g.width);
        std::vector<uint16_t> scratch(static_cast<size_t>(n), 0);
        int32_t i = 0;
        while (i < discardFrames) {
            checkStop();
            IRFrameStamp stamp{};
            int32_t termCode = IR_OK;
            std::string termMsg;
            const auto popStatus = frameQueue_.pop(
                scratch.data(), stamp, timeoutMs,
                [this] { return stopState_.load() == StopState::requested; },
                termCode, termMsg);
            if (popStatus == FrameQueue::PopStatus::gap)
                continue;
            switch (popStatus) {
            case FrameQueue::PopStatus::ok:
                ++i;
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

    const auto g = source->geometry();
    const auto n = pixels(g.height, g.width);
    std::vector<uint16_t> scratch(static_cast<size_t>(n), 0);
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
    IRGeometry *actual, char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture();
        needed(actual, 1, 1);
        *actual = capture().open(backend, name, bits);
    });
}
extern "C" int32_t IR_StartFrames(char *err, int32_t size)
{
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
extern "C" int32_t IR_ReadFrames(int32_t count, int32_t ms, uint16_t *out, int64_t cap,
    IRFrameStamp *stamps, int32_t sc, int32_t *completed, char *err, int32_t size)
{
    if (completed) *completed = 0;
    return guarded(err, size, [&] {
        needed(completed, 1, 1);
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner(); c.read(count, ms, out, cap, stamps, sc, *completed);
    });
}
extern "C" int32_t IR_FlushFrames(int32_t discard_frames, int32_t timeout_ms,
    int32_t *discarded, char *err, int32_t size)
{
    int32_t localDiscarded = 0;
    if (discarded) *discarded = 0;
    const auto status = guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner();
        c.flush(discard_frames, timeout_ms, localDiscarded);
    });
    if (discarded) *discarded = localDiscarded;
    return status;
}
extern "C" void IR_RequestStop(void) { capture().requestStop(); }
extern "C" int32_t IR_StopFrames(char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner();
        c.stop();
        c.endOperation();
    });
}
extern "C" int32_t IR_CloseFrames(char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        if (!c.source) return;
        c.checkOwner();
        c.endOperation();
        c.close();
    });
}
extern "C" int32_t IR_SetFrameQueueCapacity(int32_t capacity, char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture();
        capture().setQueueCapacity(capacity);
    });
}
extern "C" int32_t IR_TryReadLatestFrame(uint16_t *pixels, int64_t pixel_capacity,
    IRGeometry *geometry, IRFrameStamp *stamp, int32_t *has_frame, char *err, int32_t size)
{
    if (has_frame) *has_frame = 0;
    return guarded(err, size, [&] {
        needed(has_frame, 1, 1);
        capture().frameQueue().tryReadLatest(pixels, pixel_capacity, geometry, stamp, *has_frame);
    });
}
extern "C" int32_t IR_GetCaptureStats(IRCaptureStats *stats, char *err, int32_t size)
{
    if (stats) *stats = {};
    return guarded(err, size, [&] {
        needed(stats, 1, 1);
        *stats = capture().frameQueue().stats();
    });
}
