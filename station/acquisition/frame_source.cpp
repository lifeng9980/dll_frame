#include "frame_source.h"
#include "api_common.h"
using namespace optmeas::api;
namespace optmeas::acquisition {
Capture &capture() { static Capture value; return value; }
std::unique_lock<std::mutex> lockCapture()
{
    std::unique_lock lock(capture().mutex, std::try_to_lock);
    require(lock.owns_lock(), OPTMEAS_BUSY, "Another capture call is active");
    return lock;
}
void Capture::checkOwner() const
{
    require(bool(source), OPTMEAS_BAD_ARGUMENT, "Open a frame source first");
    require(owner == std::this_thread::get_id(), OPTMEAS_BAD_ARGUMENT, "Use the opening thread");
}
void Capture::checkStop() const
{
    require(!cancelled.load(), OPTMEAS_CANCELLED, "Acquisition cancelled");
}
void Capture::start()
{
    require(!running, OPTMEAS_BUSY, "Source is already running");
    source->start();
    running = true;
}
void Capture::stop()
{
    if (running) source->stop();
    running = false;
}
void Capture::read(int count, int timeoutMs, uint16_t *out, int64_t capacity,
                   OptMeasFrameStamp *stamps, int stampCapacity, int &completed)
{
    require(running && timeoutMs >= 0, OPTMEAS_BAD_ARGUMENT, "Start source and specify a nonnegative timeout");
    const auto g = source->geometry();
    const auto n = pixels(g.height, g.width);
    needed(out, capacity, stackSize(n, count));
    needed(stamps, stampCapacity, count);
    while (completed < count) {
        checkStop();
        OptMeasFrameStamp stamp{};
        source->read(out + completed * n, stamp, timeoutMs);
        checkStop();
        bool gap = stamp.lost_frames != 0;
        if (completed) {
            const auto &last = stamps[completed - 1];
            const auto both = last.fields & stamp.fields;
            gap |= (both & OPTMEAS_HAS_SEQUENCE) && stamp.sequence != last.sequence + 1;
            gap |= (both & OPTMEAS_HAS_DRIVER_SEQUENCE) && stamp.driver_sequence != last.driver_sequence + 1;
        }
        require(!gap, OPTMEAS_FRAME_GAP, "Frame sequence discontinuity");
        stamps[completed++] = stamp;
    }
}
}
using namespace optmeas::acquisition;
extern "C" int32_t OptMeas_OpenFrames(int32_t backend, const char *name, int32_t bits,
    OptMeasGeometry *actual, char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture();
        auto &c = capture();
        needed(actual, 1, 1);
        require(!c.source, OPTMEAS_BUSY, "A frame source is open");
        require(name && *name && bits >= 0 && bits <= 16, OPTMEAS_BAD_ARGUMENT, "Invalid source name or valid bits");
        std::unique_ptr<FrameSource> source;
        if (backend == OPTMEAS_RAW_REPLAY) {
            source = openReplay(name);
            require(!bits || bits == source->geometry().valid_bits, OPTMEAS_BAD_ARGUMENT, "Depth differs from RAW index");
        }
        else throw Error(OPTMEAS_UNSUPPORTED, "Frame backend is not available on this platform");
        *actual = source->geometry();
        c.source = std::move(source);
        c.owner = std::this_thread::get_id();
        c.backend = backend;
    });
}
extern "C" int32_t OptMeas_StartFrames(char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner();
        require(!c.running, OPTMEAS_BUSY, "Source is already running");
        c.cancelled = false;
        c.start();
    });
}
extern "C" int32_t OptMeas_ReadFrames(int32_t count, int32_t ms, uint16_t *out, int64_t cap,
    OptMeasFrameStamp *stamps, int32_t sc, int32_t *completed, char *err, int32_t size)
{
    if (completed) *completed = 0;
    return guarded(err, size, [&] {
        needed(completed, 1, 1);
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner(); c.read(count, ms, out, cap, stamps, sc, *completed);
    });
}
extern "C" void OptMeas_RequestStop(void) { capture().cancelled = true; }
extern "C" int32_t OptMeas_StopFrames(char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        c.checkOwner(); c.stop();
    });
}
extern "C" int32_t OptMeas_CloseFrames(char *err, int32_t size)
{
    return guarded(err, size, [&] {
        auto lock = lockCapture(); auto &c = capture();
        if (c.source) { c.checkOwner(); c.stop(); c.source.reset(); }
    });
}
