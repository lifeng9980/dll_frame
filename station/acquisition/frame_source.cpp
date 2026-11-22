#include "frame_source.h"
#include "api_common.h"
#include <algorithm>
#include <chrono>
using namespace ir::api;
namespace ir::acquisition {
Capture &capture() { static Capture value; return value; }
std::unique_lock<std::mutex> lockCapture()
{
    std::unique_lock lock(capture().mutex, std::try_to_lock);
    require(lock.owns_lock(), IR_BUSY, "Another capture or measurement is active");
    return lock;
}
IRGeometry Capture::open(int kind, const char *name, int bits)
{
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
    sourceName = name;
    source = std::move(opened);
    owner = std::this_thread::get_id();
    backend = kind;
    return geometry;
}
void Capture::close()
{
    if (!source) return;
    checkOwner();
    // 即使后端报告停流失败，离开作用域时也要释放采集源。
    auto closing = std::move(source);
    const bool wasRunning = running;
    running = false;
    sourceName.clear();
    owner = {};
    backend = 0;
    if (wasRunning) closing->stop();
}
void Capture::checkOwner() const
{
    require(bool(source), IR_BAD_ARGUMENT, "Open a frame source first");
    require(owner == std::this_thread::get_id(), IR_BAD_ARGUMENT, "Use the opening thread");
}
void Capture::beginOperation() noexcept { stopState_.store(StopState::active); }
void Capture::endOperation() noexcept { stopState_.store(StopState::idle); }
bool Capture::tryEnterStandaloneStopScope() noexcept
{
    auto expected = StopState::idle;
    return stopState_.compare_exchange_strong(expected, StopState::active);
}
void Capture::leaveStandaloneStopScope(bool entered) noexcept
{
    if (entered) stopState_.store(StopState::idle);
}
void Capture::requestStop() noexcept
{
    auto expected = StopState::active;
    stopState_.compare_exchange_strong(expected, StopState::requested);
}
void Capture::checkStop() const
{
    require(stopState_.load() != StopState::requested, IR_CANCELLED, "Measurement or acquisition cancelled");
}
void Capture::start()
{
    checkStop();
    require(!running, IR_BUSY, "Source is already running");
    source->start();
    running = true;
}
void Capture::stop()
{
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
        c.beginOperation();
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
