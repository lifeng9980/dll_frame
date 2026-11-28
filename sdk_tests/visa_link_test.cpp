/* Windows-only unit test for the VISA SCPI link against a mock VISA runtime DLL.
 *
 * The test binary compiles station/instruments/visa_link.cpp with IR_VISA_TEST_OVERRIDE so the
 * runtime path comes from the test instead of System32. IR_station.dll keeps the production
 * LOAD_LIBRARY_SEARCH_SYSTEM32 lookup, so nothing here can weaken the shipped library.
 *
 * Covered: (a) async write/read through viWaitOnEvent timeout polling, (b) procedure cancellation
 * interrupting the 25 ms wait slice with IR_CANCELLED, (c) the viTerminate + viClose teardown of
 * pending async I/O, (d) the plain deadline path and (e) a missing runtime.
 */
#include "api_common.h"
#include "asio_link.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iterator>
#include <string>
#include <type_traits>

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

/** Mirrors the counter order exported by sdk_tests/mock_visa_runtime.cpp. */
enum MockStat : uint32_t {
    StatApiVersion = 0,
    StatOpenDefaultRm,
    StatOpen,
    StatEnableEvent,
    StatSetAttribute,
    StatWriteAsync,
    StatReadAsync,
    StatWaitOnEvent,
    StatGetAttribute,
    StatTerminate,
    StatClose,
    StatCloseManager,
    StatCloseDevice,
    StatCloseEvent,
    StatPendingJobs,
    StatMaxWaitTimeout,
    StatCloseWithPendingJob,
    StatInvalidTerminate,
    MockStatCount
};

using MockStats = std::array<int32_t, static_cast<std::size_t>(MockStatCount)>;

class MockVisa {
public:
    explicit MockVisa(const std::filesystem::path &path)
    {
        module_ = LoadLibraryW(path.c_str());
        if (!module_) return;
        reset_ = function<void (*)(int32_t)>("ir_mock_visa_reset");
        getStats_ = function<int32_t (*)(int32_t *, uint32_t)>("ir_mock_visa_get_stats");
        copyLastWrite_ = function<int32_t (*)(char *, int32_t)>("ir_mock_visa_copy_last_write");
    }

    ~MockVisa()
    {
        if (module_) FreeLibrary(module_);
    }

    MockVisa(const MockVisa &) = delete;
    MockVisa &operator=(const MockVisa &) = delete;

    bool valid() const { return reset_ && getStats_ && copyLastWrite_; }

    void reset(int32_t stallWaits) const { reset_(stallWaits); }

    /** Counter snapshot; an export/field mismatch leaves it zeroed so the apiVersion gate trips. */
    MockStats stats() const
    {
        MockStats result{};
        if (getStats_(result.data(), static_cast<uint32_t>(result.size())) !=
            static_cast<int32_t>(result.size()))
            return MockStats{};
        return result;
    }

    int32_t waitCount() const noexcept { return stats()[StatWaitOnEvent]; }

    std::string lastWrite() const
    {
        char buffer[512]{};
        copyLastWrite_(buffer, static_cast<int32_t>(sizeof(buffer)));
        return buffer;
    }

private:
    template<class Pointer> Pointer function(const char *name) const
    {
        static_assert(std::is_pointer_v<Pointer>);
        return reinterpret_cast<Pointer>(GetProcAddress(module_, name));
    }

    HMODULE module_{};
    void (*reset_)(int32_t){};
    int32_t (*getStats_)(int32_t *, uint32_t){};
    int32_t (*copyLastWrite_)(char *, int32_t){};
};

const MockVisa *gMock = nullptr;
int32_t gCancelAfterWaits = 0;

/** Procedure-style stop predicate: the hook recipe_runner installs for IR_RequestStop. */
bool cancelAfterWaitLoop(const void *) noexcept
{
    return gMock != nullptr && gMock->waitCount() >= gCancelAfterWaits;
}

ir::transport::Deadline after(std::chrono::milliseconds budget)
{
    return std::chrono::steady_clock::now() + budget;
}

std::string toUtf8(const std::filesystem::path &path)
{
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

std::filesystem::path anchoredPath(const std::filesystem::path &path)
{
    std::error_code errorCode;
    const auto absolute = std::filesystem::absolute(path, errorCode);
    if (errorCode) return path.lexically_normal();
    const auto resolved = std::filesystem::weakly_canonical(absolute, errorCode);
    return errorCode ? absolute.lexically_normal() : resolved;
}

/** Mock runtime: argv[1] when given, else the DLL CMake places next to this executable. */
std::filesystem::path mockRuntimePath(int argc, char **argv)
{
    if (argc >= 2) return anchoredPath(std::filesystem::path(argv[1]));
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (length == 0 || length >= std::size(module)) return {};
    return anchoredPath(std::filesystem::path(module).parent_path() / L"ir_test_mock_visa.dll");
}

/** Runs one open/action cycle and reports the SDK status instead of throwing. */
int32_t runLink(const std::function<void(ir::transport::ScpiLink &, ir::transport::Deadline)> &action,
                std::chrono::milliseconds budget)
{
    try {
        const auto deadline = after(budget);
        auto link = ir::transport::openVisaLink("USB0::MOCK::INSTR", deadline);
        action(*link, deadline);
        return IR_OK;
    } catch (const ir::api::Error &error) {
        return error.code;
    }
}

constexpr const char *Command = "MEAS:CURR?\n";
constexpr const char *Reply = "IR,MOCK,1.0\n";

void testAsyncCompletion(const MockVisa &mock)
{
    // The first two viWaitOnEvent calls time out, so the host must keep polling in 25 ms slices.
    mock.reset(2);
    const auto status = runLink([](ir::transport::ScpiLink &link, ir::transport::Deadline deadline) {
        link.writeAll(Command, deadline);
        CHECK(link.readDelimited('\n', deadline) == Reply);
    }, std::chrono::seconds(2));
    CHECK(status == IR_OK);
    CHECK(mock.lastWrite() == Command);

    const auto stats = mock.stats();
    CHECK(stats[StatWriteAsync] == 1 && stats[StatReadAsync] == 1);
    CHECK(stats[StatWaitOnEvent] >= 4);   // two polled timeouts plus one completion per transfer
    CHECK(stats[StatGetAttribute] == 4);  // status + count for the write and the read events
    CHECK(stats[StatCloseEvent] == 2);
    CHECK(stats[StatPendingJobs] == 0);
    CHECK(stats[StatCloseDevice] == 1 && stats[StatCloseManager] == 1);
    CHECK(stats[StatTerminate] == 0);
    CHECK(stats[StatInvalidTerminate] == 0);
    // Every viWaitOnEvent call must stay within the 25 ms cancellation slice.
    CHECK(stats[StatMaxWaitTimeout] <= 25);
}

void testCancellation(const MockVisa &mock)
{
    // The mock never completes, so only the cancellation predicate can end the wait loop.
    mock.reset(-1);
    gMock = &mock;
    gCancelAfterWaits = 3;
    int32_t status = IR_OK;
    std::chrono::milliseconds elapsed{};
    {
        const ir::transport::CancellationScope scope(nullptr, cancelAfterWaitLoop);
        const auto begin = std::chrono::steady_clock::now();
        status = runLink([](ir::transport::ScpiLink &link, ir::transport::Deadline deadline) {
            link.readDelimited('\n', deadline);
        }, std::chrono::seconds(5));
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin);
    }
    gMock = nullptr;

    CHECK(status == IR_CANCELLED);
    // Generous wall-clock bound: the 5 s deadline must not be reached, while slow CI or an
    // antivirus scan between polls may stretch the three 25 ms slices.
    CHECK(elapsed < std::chrono::milliseconds(3000));
    const auto stats = mock.stats();
    CHECK(stats[StatWaitOnEvent] >= 3 && stats[StatWaitOnEvent] <= 40); // bounded polling
    CHECK(stats[StatTerminate] == 1);   // pending async I/O torn down before returning
    CHECK(stats[StatCloseDevice] == 1); // session closed while the caller's buffer was still live
    CHECK(stats[StatPendingJobs] == 0);
    CHECK(stats[StatCloseManager] == 1);
    CHECK(stats[StatMaxWaitTimeout] <= 25);
    CHECK(stats[StatInvalidTerminate] == 0);      // the exact job id was aborted
    CHECK(stats[StatCloseWithPendingJob] == 0);   // viTerminate ran before viClose(device)
}

void testDeadlineTimeout(const MockVisa &mock)
{
    mock.reset(-1);
    const auto begin = std::chrono::steady_clock::now();
    const auto status = runLink([](ir::transport::ScpiLink &link, ir::transport::Deadline) {
        link.readDelimited('\n', after(std::chrono::milliseconds(150)));
    }, std::chrono::milliseconds(150));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);

    CHECK(status == IR_TIMEOUT);
    CHECK(elapsed >= std::chrono::milliseconds(150));
    const auto stats = mock.stats();
    CHECK(stats[StatTerminate] == 1);
    CHECK(stats[StatCloseDevice] == 1 && stats[StatPendingJobs] == 0);
    CHECK(stats[StatInvalidTerminate] == 0);
    CHECK(stats[StatCloseWithPendingJob] == 0);
}

void testMissingRuntime()
{
    ir::transport::setVisaRuntimePathForTest("");
    int32_t status = IR_OK;
    try {
        ir::transport::openVisaLink("USB0::MOCK::INSTR", after(std::chrono::seconds(1)));
    } catch (const ir::api::Error &error) {
        status = error.code;
    }
    CHECK(status == IR_UNSUPPORTED);
}
}

int main(int argc, char **argv)
{
    const auto mockPath = mockRuntimePath(argc, argv);
    const auto mockUtf8 = toUtf8(mockPath);
    MockVisa mock(mockPath);
    if (!mock.valid()) {
        std::fprintf(stderr, "cannot load mock VISA runtime from %s (Win32 error %lu)\n",
                     mockUtf8.c_str(), GetLastError());
        return 2;
    }
    ir::transport::setVisaRuntimePathForTest(mockUtf8);
    // The counter export only answers with a matching field count, so this also guards the layout.
    if (mock.stats()[StatApiVersion] != 1) {
        std::fprintf(stderr, "mock VISA statistics layout mismatch (expected apiVersion 1)\n");
        return 2;
    }

    testAsyncCompletion(mock);
    testCancellation(mock);
    testDeadlineTimeout(mock);
    testMissingRuntime();

    if (gFailures != 0) {
        std::fprintf(stderr, "%d VISA link check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("visa link checks passed\n");
    return 0;
}
