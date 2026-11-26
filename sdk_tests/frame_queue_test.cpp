#include "IR_station.h"
#include "api_common.h"
#include "frame_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace ir::acquisition {
struct FrameQueueTestAccess {
    static void holdPreviewLock(FrameQueue &queue, std::promise<void> &locked,
                                std::shared_future<void> release)
    {
        std::unique_lock lock(queue.previewMutex_);
        locked.set_value();
        release.wait();
    }
};

namespace {
class TestLiveSource final : public FrameSource {
public:
    IRGeometry geometry() const override
    {
        // The first geometry request after start() is part of pump scratch setup.
        if (started_ && std::exchange(failScratchSetup_, false))
            throw std::bad_alloc{};
        return {2, 2, 16};
    }

    void start() override
    {
        ir::api::require(!started_, IR_BUSY, "Live test source is still started");
        started_ = true;
        delivered_ = false;
    }
    void stop() override { started_ = false; }
    void flush() override { failScratchSetup_ = true; }

    void read(void *frame, IRFrameStamp &stamp, int) override
    {
        if (std::exchange(delivered_, true))
            throw ir::api::Error(IR_END_OF_STREAM, "End of live test data");
        std::fill_n(static_cast<uint16_t *>(frame), 4, uint16_t{17});
        stamp.sequence = 1;
        stamp.fields = IR_HAS_SEQUENCE;
    }

private:
    bool started_{};
    bool delivered_{};
    mutable bool failScratchSetup_{true};
};
}

#if defined(_WIN32)
std::unique_ptr<FrameSource> openDirectShow(const char *, int)
{
    return std::make_unique<TestLiveSource>();
}
#elif defined(__linux__)
std::unique_ptr<FrameSource> openV4l2(const char *, int)
{
    return std::make_unique<TestLiveSource>();
}
#endif
}

namespace {

int gFailures = 0;

void checkTrue(bool cond, const char *expr, int line)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
        ++gFailures;
    }
}

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)

IRFrameBatchView makeU16Batch(void *pixels, uint64_t byteSize, int32_t width, int32_t height,
                              int32_t frames, IRFrameStamp *stamps = nullptr,
                              int32_t stampCapacity = 0)
{
    return {{pixels, byteSize, IR_TYPE_U16, width, height, frames}, 0, stamps, stampCapacity};
}

struct TempDirGuard {
    fs::path dir;
    explicit TempDirGuard(fs::path p) : dir(std::move(p))
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    ~TempDirGuard()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

using ir::acquisition::FrameQueue;

#if defined(_WIN32) || defined(__linux__)
void testLiveScratchStartupFailure()
{
#ifdef _WIN32
    constexpr auto backend = IR_DIRECTSHOW;
#else
    constexpr auto backend = IR_V4L2;
#endif
    char error[512]{};
    IRFrameSourceInfo sourceInfo{};
    CHECK(IR_OpenFrames(backend, "fake-live", 16, &sourceInfo, error, sizeof error) == IR_OK);
    CHECK(IR_StartFrames(error, sizeof error) == IR_COMPUTATION_FAILED);
    IRCaptureStats stats{};
    CHECK(IR_GetCaptureStats(&stats, error, sizeof error) == IR_OK);
    CHECK(stats.running == 0);

    CHECK(IR_StartFrames(error, sizeof error) == IR_OK);
    uint16_t pixels[4]{};
    IRFrameStamp stamp{};
    auto batch = makeU16Batch(pixels, sizeof pixels, 2, 2, 1, &stamp, 1);
    CHECK(IR_ReadFrames(1000, &batch, error, sizeof error) == IR_OK);
    CHECK(batch.pixels.frames == 1 && pixels[0] == 17 && pixels[3] == 17);

    int32_t discarded = -1;
    CHECK(IR_FlushFrames(0, 0, &discarded, error, sizeof error) == IR_COMPUTATION_FAILED);
    CHECK(discarded == 0);
    CHECK(IR_GetCaptureStats(&stats, error, sizeof error) == IR_OK);
    CHECK(stats.running == 0);
    CHECK(IR_StartFrames(error, sizeof error) == IR_OK);
    batch.pixels.frames = 1;
    CHECK(IR_ReadFrames(1000, &batch, error, sizeof error) == IR_OK);
    CHECK(batch.pixels.frames == 1 && pixels[0] == 17 && pixels[3] == 17);
    CHECK(IR_CloseFrames(error, sizeof error) == IR_OK);
}
#endif

void testTerminalErrorFollowsQueuedFrames()
{
    FrameQueue queue;
    queue.resetSession({2, 2, 16}, IR_TYPE_U16, 2, true);
    const uint16_t pixels[]{4, 5, 6, 7};
    IRFrameStamp stamp{};
    stamp.sequence = 3;
    stamp.fields = IR_HAS_SEQUENCE;
    queue.push(pixels, stamp);

    const std::string prefix(1022, 'x');
    const std::string message = prefix + "\xe4\xb8\xad";
    queue.setTerminalError(IR_IO_ERROR, message.c_str());
    queue.setTerminalError(IR_TIMEOUT, "Later failure");

    uint16_t out[4]{};
    IRFrameStamp outStamp{};
    int32_t code = IR_OK;
    std::string detail;
    const auto neverStop = [] { return false; };
    CHECK(queue.pop(out, outStamp, 0, neverStop, code, detail) == FrameQueue::PopStatus::ok);
    CHECK(outStamp.sequence == 3 && out[0] == 4 && out[3] == 7);
    CHECK(queue.pop(out, outStamp, 0, neverStop, code, detail) == FrameQueue::PopStatus::terminal_error);
    CHECK(code == IR_IO_ERROR && detail == prefix);
}

void testPreviewContentionDoesNotBlockMeasurementQueue()
{
    FrameQueue queue;
    const IRGeometry geometry{4, 4, 16};
    queue.configure(geometry, IR_TYPE_U16, 8);
    queue.resetSession(geometry, IR_TYPE_U16, 8, true);

    std::promise<void> locked;
    std::promise<void> release;
    const auto releaseFuture = release.get_future().share();
    std::thread previewHolder([&] {
        ir::acquisition::FrameQueueTestAccess::holdPreviewLock(queue, locked, releaseFuture);
    });
    locked.get_future().wait();

    std::vector<uint16_t> previewPixels(16);
    auto preview = makeU16Batch(previewPixels.data(), previewPixels.size() * sizeof(uint16_t), 4, 4, 1);
    queue.tryReadLatest(preview);
    CHECK(preview.pixels.frames == 0);

    std::promise<void> producerDone;
    auto producerFuture = producerDone.get_future();
    std::thread producer([&] {
        std::vector<uint16_t> pixels(16, 7);
        for (uint64_t sequence = 1; sequence <= 100; ++sequence) {
            IRFrameStamp stamp{};
            stamp.sequence = sequence;
            stamp.fields = IR_HAS_SEQUENCE;
            queue.push(pixels.data(), stamp);
        }
        producerDone.set_value();
    });

    const bool producedWhilePreviewHeld =
        producerFuture.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    release.set_value();
    producer.join();
    previewHolder.join();

    const auto stats = queue.stats();
    CHECK(producedWhilePreviewHeld);
    CHECK(stats.frames_received == 100);
    CHECK(stats.queue_depth == 8);
    CHECK(stats.frames_dropped >= 92);
}

void testConcurrentLiveProducerAndSlowPreview()
{
    FrameQueue fq;
    const IRGeometry geom{4, 4, 16};
    fq.configure(geom, IR_TYPE_U16, 64);
    fq.resetSession(geom, IR_TYPE_U16, 64, true);

    std::atomic<bool> stopPreview{false};
    std::atomic<bool> previewSeen{false};
    std::atomic<bool> previewError{false};
    std::thread previewThread([&] {
        std::vector<uint16_t> buf(16, 0);
        IRFrameStamp stamp{};
        while (!stopPreview.load(std::memory_order_acquire)) {
            try {
                auto preview = makeU16Batch(buf.data(), buf.size() * sizeof(uint16_t), 4, 4, 1, &stamp, 1);
                fq.tryReadLatest(preview);
                if (preview.pixels.frames == 1) {
                    if (preview.pixels.width == 4 && preview.pixels.height == 4 && stamp.sequence >= 1)
                        previewSeen.store(true, std::memory_order_release);
                    else
                        previewError.store(true, std::memory_order_release);
                }
            } catch (...) {
                previewError.store(true, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    });

    std::thread producer([&] {
        std::vector<uint16_t> frame(16, 0);
        for (uint64_t seq = 1; seq <= 80; ++seq) {
            std::fill(frame.begin(), frame.end(), static_cast<uint16_t>(seq));
            IRFrameStamp st{};
            st.sequence = seq;
            st.fields = IR_HAS_SEQUENCE;
            fq.push(frame.data(), st);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        fq.markStopped();
    });

    std::vector<uint16_t> out(16, 0);
    int32_t delivered = 0;
    for (uint64_t expectedSeq = 1; expectedSeq <= 80; ++expectedSeq) {
        IRFrameStamp st{};
        int32_t termCode = IR_OK;
        std::string termMsg;
        const auto popRes = fq.pop(out.data(), st, 2000, [] { return false; }, termCode, termMsg);
        CHECK(popRes == FrameQueue::PopStatus::ok);
        if (popRes != FrameQueue::PopStatus::ok)
            break;
        fq.recordDelivered();
        ++delivered;
        CHECK(st.sequence == expectedSeq);
        CHECK(st.lost_frames == 0);
        CHECK(out[0] == static_cast<uint16_t>(expectedSeq));
    }

    producer.join();
    stopPreview.store(true, std::memory_order_release);
    previewThread.join();

    const IRCaptureStats stats = fq.stats();
    CHECK(delivered == 80);
    CHECK(!previewError.load());
    CHECK(previewSeen.load());
    CHECK(stats.frames_received == 80);
    CHECK(stats.frames_delivered == 80);
    CHECK(stats.preview_frames > 0 && stats.preview_frames <= 80);
    CHECK(stats.frames_dropped == 0);
    CHECK(stats.estimated_fps > 0.0);
}

void testOverflowPrefixAndGapRecovery()
{
    FrameQueue fq;
    const IRGeometry geom{4, 4, 16};
    fq.configure(geom, IR_TYPE_U16, 8);
    fq.resetSession(geom, IR_TYPE_U16, 8, true);

    std::vector<uint16_t> frame(16, 0);
    for (uint64_t seq = 1; seq <= 20; ++seq) {
        std::fill(frame.begin(), frame.end(), static_cast<uint16_t>(seq));
        IRFrameStamp st{};
        st.sequence = seq;
        st.fields = IR_HAS_SEQUENCE;
        fq.push(frame.data(), st);
    }

    // Non-blocking preview sees latest frame 20 immediately despite ring overflow.
    std::vector<uint16_t> prevBuf(16, 0);
    IRFrameStamp ps{};
    auto preview = makeU16Batch(prevBuf.data(), prevBuf.size() * sizeof(uint16_t), 4, 4, 1, &ps, 1);
    fq.tryReadLatest(preview);
    CHECK(preview.pixels.frames == 1);
    CHECK(ps.sequence == 20 && prevBuf[0] == 20);

    // Consumer pops intact contiguous prefix 1..8, then receives gap on empty ring.
    std::vector<uint16_t> popBuf(16, 0);
    for (uint64_t seq = 1; seq <= 8; ++seq) {
        IRFrameStamp st{};
        int32_t tc = IR_OK;
        std::string tm;
        const auto res = fq.pop(popBuf.data(), st, 200, [] { return false; }, tc, tm);
        CHECK(res == FrameQueue::PopStatus::ok);
        CHECK(st.sequence == seq && st.lost_frames == 0 && popBuf[0] == static_cast<uint16_t>(seq));
    }
    {
        IRFrameStamp st{};
        int32_t tc = IR_OK;
        std::string tm;
        const auto res = fq.pop(popBuf.data(), st, 200, [] { return false; }, tc, tm);
        CHECK(res == FrameQueue::PopStatus::gap);
    }
    const IRCaptureStats stOverflow = fq.stats();
    CHECK(stOverflow.frames_dropped >= 12);

    // Test attached loss flag when a post-overflow frame eventually enters freed tail slot.
    fq.resetSession(geom, IR_TYPE_U16, 4, true);
    for (uint64_t seq = 1; seq <= 6; ++seq) {
        std::fill(frame.begin(), frame.end(), static_cast<uint16_t>(seq));
        IRFrameStamp st{};
        st.sequence = seq;
        fq.push(frame.data(), st);
    }
    IRFrameStamp st{};
    int32_t tc = IR_OK;
    std::string tm;
    CHECK(fq.pop(popBuf.data(), st, 200, [] { return false; }, tc, tm) == FrameQueue::PopStatus::ok);
    CHECK(st.sequence == 1 && st.lost_frames == 0);
    {
        std::fill(frame.begin(), frame.end(), uint16_t{7});
        IRFrameStamp s7{};
        s7.sequence = 7;
        fq.push(frame.data(), s7);
    }
    for (uint64_t seq = 2; seq <= 4; ++seq) {
        CHECK(fq.pop(popBuf.data(), st, 200, [] { return false; }, tc, tm) == FrameQueue::PopStatus::ok);
        CHECK(st.sequence == seq && st.lost_frames == 0);
    }
    CHECK(fq.pop(popBuf.data(), st, 200, [] { return false; }, tc, tm) == FrameQueue::PopStatus::ok);
    CHECK(st.sequence == 7 && st.lost_frames == 2 && (st.fields & IR_HAS_LOSS) != 0);

    // Clear measurement queue and verify clean recovery.
    fq.clearMeasurementQueue();
    for (uint64_t seq = 101; seq <= 104; ++seq) {
        std::fill(frame.begin(), frame.end(), static_cast<uint16_t>(seq));
        IRFrameStamp s{};
        s.sequence = seq;
        fq.push(frame.data(), s);
    }
    for (uint64_t seq = 101; seq <= 104; ++seq) {
        CHECK(fq.pop(popBuf.data(), st, 200, [] { return false; }, tc, tm) == FrameQueue::PopStatus::ok);
        CHECK(st.sequence == seq && st.lost_frames == 0);
    }
}

void testCancelWakesBlockedConsumer()
{
    FrameQueue fq;
    const IRGeometry geom{4, 4, 16};
    fq.configure(geom, IR_TYPE_U16, 8);
    fq.resetSession(geom, IR_TYPE_U16, 8, true);

    std::atomic<bool> stopFlag{false};
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stopFlag.store(true, std::memory_order_release);
        fq.wakeWaiters();
    });

    std::vector<uint16_t> buf(16, 0);
    IRFrameStamp st{};
    int32_t tc = IR_OK;
    std::string tm;
    const auto t0 = std::chrono::steady_clock::now();
    const auto res = fq.pop(buf.data(), st, 5000, [&] { return stopFlag.load(std::memory_order_acquire); }, tc, tm);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    stopper.join();

    CHECK(res == FrameQueue::PopStatus::cancelled);
    CHECK(elapsedMs < 1000);
}

void testPublicCApiConcurrentPreviewAndStats()
{
    static_assert(sizeof(IRCaptureStats) == 64, "IRCaptureStats size must be 64 bytes");
    CHECK(sizeof(IRCaptureStats) == 64);

    char err[512]{};
    CHECK(IR_SetFrameQueueCapacity(1, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_SetFrameQueueCapacity(5000, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_SetFrameQueueCapacity(32, err, sizeof err) == IR_OK);

    const TempDirGuard fixture(fs::temp_directory_path() / "ir_sdk_frame_queue_test");
    const int32_t rows = 4, cols = 4, totalFrames = 20;
    const int32_t nPix = rows * cols;
    std::vector<uint16_t> raw(static_cast<size_t>(totalFrames * nPix));
    for (int32_t f = 0; f < totalFrames; ++f) {
        const uint16_t val = static_cast<uint16_t>(1000 + f * 10);
        std::fill_n(raw.data() + static_cast<size_t>(f * nPix), nPix, val);
    }
    {
        std::ofstream rawOut(fixture.dir / "frames.raw", std::ios::binary);
        rawOut.write(reinterpret_cast<const char *>(raw.data()),
                     static_cast<std::streamsize>(raw.size() * sizeof(uint16_t)));
        std::ofstream csvOut(fixture.dir / "frames.csv", std::ios::binary);
        csvOut << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n";
        for (int32_t f = 0; f < totalFrames; ++f)
            csvOut << f << ",4,4,8,RAW16_LE,16,little\n";
    }

    const auto u8path = fixture.dir.u8string();
    const std::string pathStr(reinterpret_cast<const char *>(u8path.data()), u8path.size());

    IRFrameSourceInfo sourceInfo{};
    CHECK(IR_OpenFrames(IR_RAW_REPLAY, pathStr.c_str(), 16, &sourceInfo, err, sizeof err) == IR_OK);
    CHECK(sourceInfo.data_type == IR_TYPE_U16);
    CHECK(sourceInfo.geometry.width == 4 && sourceInfo.geometry.height == 4 &&
          sourceInfo.geometry.valid_bits == 16);

    std::vector<uint16_t> previewBuf(static_cast<size_t>(nPix), 0);
    auto preview = makeU16Batch(previewBuf.data(), previewBuf.size() * sizeof(uint16_t), 4, 4, 1);
    CHECK(IR_TryReadLatestFrame(&preview, err, sizeof err) == IR_OK);
    CHECK(preview.pixels.frames == 0);

    std::atomic<bool> stopPoll{false};
    std::atomic<bool> sawPreview{false};
    std::atomic<bool> sawTooSmallGeom{false};
    std::atomic<int> busyErrors{0};
    std::atomic<int> pollOtherErrors{0};

    std::thread pollThread([&] {
        std::vector<uint16_t> localBuf(static_cast<size_t>(nPix), 0);
        IRFrameStamp localStamp{};
        uint16_t tinyBuf[1]{};
        char localErr[256]{};
        while (!stopPoll.load(std::memory_order_acquire)) {
            auto localPreview = makeU16Batch(localBuf.data(), localBuf.size() * sizeof(uint16_t),
                                             4, 4, 1, &localStamp, 1);
            const int32_t stPreview = IR_TryReadLatestFrame(&localPreview, localErr, sizeof localErr);
            if (stPreview == IR_BUSY)
                ++busyErrors;
            else if (stPreview != IR_OK)
                ++pollOtherErrors;
            else if (localPreview.pixels.frames == 1) {
                sawPreview.store(true, std::memory_order_release);
                auto tinyPreview = makeU16Batch(tinyBuf, sizeof(tinyBuf), 4, 4, 1);
                const int32_t stTiny = IR_TryReadLatestFrame(&tinyPreview, localErr, sizeof localErr);
                if (stTiny == IR_BUFFER_TOO_SMALL && tinyPreview.pixels.width == 4 &&
                    tinyPreview.pixels.height == 4 && tinyPreview.valid_bits == 16)
                    sawTooSmallGeom.store(true, std::memory_order_release);
            }

            IRCaptureStats stats{};
            const int32_t stStats = IR_GetCaptureStats(&stats, localErr, sizeof localErr);
            if (stStats == IR_BUSY)
                ++busyErrors;
            else if (stStats != IR_OK)
                ++pollOtherErrors;

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    CHECK(IR_StartFrames(err, sizeof err) == IR_OK);
    CHECK(IR_SetFrameQueueCapacity(16, err, sizeof err) == IR_BUSY);

    std::vector<uint16_t> readBuf(static_cast<size_t>(nPix), 0);
    int32_t foreignStatus = IR_OK;
    std::thread foreignReader([&] {
        uint16_t foreignBuffer[16]{};
        IRFrameStamp foreignStamp{};
        char foreignError[256]{};
        auto foreignBatch = makeU16Batch(foreignBuffer, sizeof(foreignBuffer), 4, 4, 1,
                                         &foreignStamp, 1);
        foreignStatus = IR_ReadFrames(1000, &foreignBatch, foreignError, sizeof foreignError);
        CHECK(foreignBatch.pixels.frames == 0);
    });
    foreignReader.join();
    CHECK(foreignStatus == IR_BUSY);

    for (int i = 0; i < 10; ++i) {
        IRFrameStamp oneStamp{};
        auto readBatch = makeU16Batch(readBuf.data(), readBuf.size() * sizeof(uint16_t), 4, 4, 1,
                                      &oneStamp, 1);
        CHECK(IR_ReadFrames(1000, &readBatch, err, sizeof err) == IR_OK);
        CHECK(readBatch.pixels.frames == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    for (int wait = 0; wait < 200 && (!sawPreview.load() || !sawTooSmallGeom.load()); ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::thread canceler([] { IR_RequestStop(); });
    canceler.join();
    IRFrameStamp cancelledStamp{};
    auto cancelledBatch = makeU16Batch(readBuf.data(), readBuf.size() * sizeof(uint16_t), 4, 4, 1,
                                       &cancelledStamp, 1);
    CHECK(IR_ReadFrames(1000, &cancelledBatch, err, sizeof err) == IR_CANCELLED);
    CHECK(cancelledBatch.pixels.frames == 0);
    CHECK(IR_StopFrames(err, sizeof err) == IR_OK);

    CHECK(IR_StartFrames(err, sizeof err) == IR_OK);
    std::vector<uint16_t> batchBuf(20ULL * nPix);
    std::vector<IRFrameStamp> batchStamps(20);
    auto batch = makeU16Batch(batchBuf.data(), batchBuf.size() * sizeof(uint16_t), 4, 4, 20,
                              batchStamps.data(), static_cast<int32_t>(batchStamps.size()));
    CHECK(IR_ReadFrames(0, &batch, err, sizeof err) == IR_OK);
    CHECK(batch.pixels.frames == 20);
    CHECK(IR_StopFrames(err, sizeof err) == IR_OK);

    stopPoll.store(true, std::memory_order_release);
    pollThread.join();

    IRCaptureStats finalStats{};
    CHECK(IR_GetCaptureStats(&finalStats, err, sizeof err) == IR_OK);
    CHECK(busyErrors.load() == 0);
    CHECK(pollOtherErrors.load() == 0);
    CHECK(sawPreview.load());
    CHECK(sawTooSmallGeom.load());
    CHECK(finalStats.queue_capacity == 32);
    CHECK(finalStats.frames_delivered >= 30);

    CHECK(IR_CloseFrames(err, sizeof err) == IR_OK);
    preview.pixels.frames = 1;
    CHECK(IR_TryReadLatestFrame(&preview, err, sizeof err) == IR_OK);
    CHECK(preview.pixels.frames == 0);
    CHECK(IR_GetCaptureStats(&finalStats, err, sizeof err) == IR_OK);
    CHECK(finalStats.running == 0);
    CHECK(IR_SetFrameQueueCapacity(64, err, sizeof err) == IR_OK);

    const TempDirGuard largeFixture(fs::temp_directory_path() / "ir_sdk_frame_queue_budget_test");
    constexpr int32_t largeRows = 512;
    constexpr int32_t largeCols = 640;
    std::vector<uint16_t> largeFrame(static_cast<size_t>(largeRows) * largeCols, 0);
    {
        std::ofstream rawOut(largeFixture.dir / "frames.raw", std::ios::binary);
        rawOut.write(reinterpret_cast<const char *>(largeFrame.data()),
                     static_cast<std::streamsize>(largeFrame.size() * sizeof(uint16_t)));
        std::ofstream csvOut(largeFixture.dir / "frames.csv", std::ios::binary);
        csvOut << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n";
        csvOut << "0,640,512,1280,RAW16_LE,16,little\n";
    }
    const auto largeU8Path = largeFixture.dir.u8string();
    const std::string largePath(reinterpret_cast<const char *>(largeU8Path.data()), largeU8Path.size());
    CHECK(IR_OpenFrames(IR_RAW_REPLAY, largePath.c_str(), 16, &sourceInfo, err, sizeof err) == IR_OK);
    CHECK(IR_SetFrameQueueCapacity(4096, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_GetCaptureStats(&finalStats, err, sizeof err) == IR_OK);
    CHECK(finalStats.queue_capacity == 64);
    CHECK(IR_CloseFrames(err, sizeof err) == IR_OK);
}

} // namespace

int main()
{
#if defined(_WIN32) || defined(__linux__)
    testLiveScratchStartupFailure();
#endif
    testTerminalErrorFollowsQueuedFrames();
    testPreviewContentionDoesNotBlockMeasurementQueue();
    testConcurrentLiveProducerAndSlowPreview();
    testOverflowPrefixAndGapRecovery();
    testCancelWakesBlockedConsumer();
    testPublicCApiConcurrentPreviewAndStats();
    if (gFailures == 0)
        std::printf("frame_queue_test: ALL PASSED\n");
    return gFailures == 0 ? 0 : 1;
}
