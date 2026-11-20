#include "frame_source.h"
#include "api_common.h"
using namespace ir::api;
namespace ir::acquisition {
Capture &capture() { static Capture value; return value; }
std::unique_lock<std::mutex> lockCapture()
{
    std::unique_lock lock(capture().mutex, std::try_to_lock);
    require(lock.owns_lock(), IR_BUSY, "Another capture call is active");
    return lock;
}
void Capture::checkOwner() const
{
    require(bool(source), IR_BAD_ARGUMENT, "Open a frame source first");
    require(owner == std::this_thread::get_id(), IR_BAD_ARGUMENT, "Use the opening thread");
}
void Capture::checkStop() const
{
    require(!cancelled.load(), IR_CANCELLED, "Acquisition cancelled");
}
void Capture::start()
{
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
        IRFrameStamp stamp{};
        source->read(out + completed * n, stamp, timeoutMs);
        checkStop();
        bool gap = stamp.lost_frames != 0;
        if (completed) {
            const auto &last = stamps[completed - 1];
            const auto both = last.fields & stamp.fields;
            gap |= (both & IR_HAS_SEQUENCE) && stamp.sequence != last.sequence + 1;
            gap |= (both & IR_HAS_DRIVER_SEQUENCE) && stamp.driver_sequence != last.driver_sequence + 1;
        }
        require(!gap, IR_FRAME_GAP, "Frame sequence discontinuity");
        stamps[completed++] = stamp;
    }
}
}
using namespace ir::acquisition;
extern "C" int32_t IR_OpenFrames(int32_t backend, const char *name, int32_t bits,
    IRGeometry *actual, char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture();
        auto &c = capture();
        needed(actual, 1, 1);
        require(!c.source, IR_BUSY, "A frame source is open");
        require(name && *name && bits >= 0 && bits <= 16, IR_BAD_ARGUMENT, "Invalid source name or valid bits");
        std::unique_ptr<FrameSource> source;
        if (backend == IR_RAW_REPLAY) {
            source = openReplay(name);
            require(!bits || bits == source->geometry().valid_bits, IR_BAD_ARGUMENT, "Depth differs from RAW index");
        }
#if defined(__linux__)
        else if (backend == IR_V4L2) {
            source = openV4l2(name, bits);
        }
#endif
#if defined(_WIN32)
        else if (backend == IR_DIRECTSHOW) {
            source = openDirectShow(name, bits);
        }
#endif
        else throw Error(IR_UNSUPPORTED, "Frame backend is not available on this platform");
        *actual = source->geometry();
        c.source = std::move(source);
        c.owner = std::this_thread::get_id();
        c.backend = backend;
    });
}
extern "C" int32_t IR_StartFrames(char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner();
        require(!c.running, IR_BUSY, "Source is already running");
        c.cancelled = false;
        c.start();
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
extern "C" void IR_RequestStop(void) { capture().cancelled = true; }
extern "C" int32_t IR_StopFrames(char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner(); c.stop();
    });
}
extern "C" int32_t IR_CloseFrames(char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        if (c.source) { c.checkOwner(); c.stop(); c.source.reset(); }
    });
}
