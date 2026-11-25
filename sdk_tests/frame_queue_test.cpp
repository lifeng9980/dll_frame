#include "IR_station.h"
#include "frame_queue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
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

void testPreviewContentionDoesNotBlockMeasurementQueue()
{
    FrameQueue queue;
    const IRGeometry geometry{4, 4, 16};
    queue.configure(geometry, 8);
    queue.resetSession(geometry, 8, true);

    std::promise<void> locked;
    std::promise<void> release;
    const auto releaseFuture = release.get_future().share();
    std::thread previewHolder([&] {
        ir::acquisition::FrameQueueTestAccess::holdPreviewLock(queue, locked, releaseFuture);
    });
    locked.get_future().wait();

    std::vector<uint16_t> previewPixels(16);
    IRGeometry previewGeometry{};
    int32_t hasFrame = 1;
    queue.tryReadLatest(previewPixels.data(), 16, &previewGeometry, nullptr, hasFrame);
    CHECK(hasFrame == 0);

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
    fq.configure(geom, 64);
    fq.resetSession(geom, 64, true);

    std::atomic<bool> stopPreview{false};
    std::atomic<bool> previewSeen{false};
    std::atomic<bool> previewError{false};
    std::thread previewThread([&] {
        std::vector<uint16_t> buf(16, 0);
        while (!stopPreview.load(std::memory_order_acquire)) {
            try {
                IRGeometry pg{};
                IRFrameStamp ps{};
                int32_t hasFrame = 0;
                fq.tryReadLatest(buf.data(), 16, &pg, &ps, hasFrame);
                if (hasFrame == 1) {
                    if (pg.width == 4 && pg.height == 4 && ps.sequence >= 1)
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
    fq.configure(geom, 8);
    fq.resetSession(geom, 8, true);

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
    IRGeometry pg{};
    IRFrameStamp ps{};
    int32_t hasFrame = 0;
    fq.tryReadLatest(prevBuf.data(), 16, &pg, &ps, hasFrame);
    CHECK(hasFrame == 1);
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
    fq.resetSession(geom, 4, true);
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
    fq.configure(geom, 8);
    fq.resetSession(geom, 8, true);

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

    IRGeometry geom{};
    CHECK(IR_OpenFrames(IR_RAW_REPLAY, pathStr.c_str(), 16, &geom, err, sizeof err) == IR_OK);
    CHECK(geom.width == 4 && geom.height == 4 && geom.valid_bits == 16);

    std::vector<uint16_t> previewBuf(static_cast<size_t>(nPix), 0);
    int32_t hasFrame = 1;
    CHECK(IR_TryReadLatestFrame(previewBuf.data(), nPix, &geom, nullptr, &hasFrame, err, sizeof err) == IR_OK);
    CHECK(hasFrame == 0);

    std::atomic<bool> stopPoll{false};
    std::atomic<bool> sawPreview{false};
    std::atomic<bool> sawTooSmallGeom{false};
    std::atomic<int> busyErrors{0};
    std::atomic<int> pollOtherErrors{0};

    std::thread pollThread([&] {
        std::vector<uint16_t> localBuf(static_cast<size_t>(nPix), 0);
        uint16_t tinyBuf[1]{};
        char localErr[256]{};
        while (!stopPoll.load(std::memory_order_acquire)) {
            IRGeometry lg{};
            IRFrameStamp ls{};
            int32_t lHas = 0;
            const int32_t stPreview = IR_TryReadLatestFrame(
                localBuf.data(), nPix, &lg, &ls, &lHas, localErr, sizeof localErr);
            if (stPreview == IR_BUSY)
                ++busyErrors;
            else if (stPreview != IR_OK)
                ++pollOtherErrors;
            else if (lHas == 1) {
                sawPreview.store(true, std::memory_order_release);
                IRGeometry tg{};
                int32_t tHas = 0;
                const int32_t stTiny = IR_TryReadLatestFrame(
                    tinyBuf, 1, &tg, nullptr, &tHas, localErr, sizeof localErr);
                if (stTiny == IR_BUFFER_TOO_SMALL && tg.width == 4 && tg.height == 4 && tg.valid_bits == 16)
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
    IRFrameStamp oneStamp{};
    int32_t foreignCompleted = 0;
    int32_t foreignStatus = IR_OK;
    std::thread foreignReader([&] {
        uint16_t foreignBuffer[16]{};
        IRFrameStamp foreignStamp{};
        char foreignError[256]{};
        foreignStatus = IR_ReadFrames(1, 1000, foreignBuffer, 16, &foreignStamp, 1,
                                      &foreignCompleted, foreignError, sizeof foreignError);
    });
    foreignReader.join();
    CHECK(foreignStatus == IR_BUSY);
    CHECK(foreignCompleted == 0);

    for (int i = 0; i < 10; ++i) {
        int32_t completed = 0;
        CHECK(IR_ReadFrames(1, 1000, readBuf.data(), static_cast<int64_t>(readBuf.size()),
                            &oneStamp, 1, &completed, err, sizeof err) == IR_OK);
        CHECK(completed == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    for (int wait = 0; wait < 200 && (!sawPreview.load() || !sawTooSmallGeom.load()); ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::thread canceler([] { IR_RequestStop(); });
    canceler.join();
    int32_t cancelledCompleted = 0;
    CHECK(IR_ReadFrames(1, 1000, readBuf.data(), static_cast<int64_t>(readBuf.size()),
                        &oneStamp, 1, &cancelledCompleted, err, sizeof err) == IR_CANCELLED);
    CHECK(cancelledCompleted == 0);
    CHECK(IR_StopFrames(err, sizeof err) == IR_OK);

    CHECK(IR_StartFrames(err, sizeof err) == IR_OK);
    std::vector<uint16_t> batchBuf(20ULL * nPix);
    std::vector<IRFrameStamp> batchStamps(20);
    int32_t batchCompleted = 0;
    CHECK(IR_ReadFrames(20, 0, batchBuf.data(), static_cast<int64_t>(batchBuf.size()),
                        batchStamps.data(), 20, &batchCompleted, err, sizeof err) == IR_OK);
    CHECK(batchCompleted == 20);
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
    CHECK(IR_TryReadLatestFrame(previewBuf.data(), nPix, &geom, nullptr, &hasFrame, err, sizeof err) == IR_OK);
    CHECK(hasFrame == 0);
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
    CHECK(IR_OpenFrames(IR_RAW_REPLAY, largePath.c_str(), 16, &geom, err, sizeof err) == IR_OK);
    CHECK(IR_SetFrameQueueCapacity(4096, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_GetCaptureStats(&finalStats, err, sizeof err) == IR_OK);
    CHECK(finalStats.queue_capacity == 64);
    CHECK(IR_CloseFrames(err, sizeof err) == IR_OK);
}

} // namespace

int main()
{
    testPreviewContentionDoesNotBlockMeasurementQueue();
    testConcurrentLiveProducerAndSlowPreview();
    testOverflowPrefixAndGapRecovery();
    testCancelWakesBlockedConsumer();
    testPublicCApiConcurrentPreviewAndStats();
    if (gFailures == 0)
        std::printf("frame_queue_test: ALL PASSED\n");
    return gFailures == 0 ? 0 : 1;
}
