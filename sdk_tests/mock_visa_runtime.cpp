/* Mock Windows x64 VISA runtime used by sdk_tests/visa_link_test.cpp.
 *
 * The production IR_station.dll always resolves visa64.dll from System32
 * (LOAD_LIBRARY_SEARCH_SYSTEM32). The unit test binary instead points the link at this DLL
 * through the compile-time-only IR_VISA_TEST_OVERRIDE seam, so both the async completion path and
 * the cancellation/timeout teardown path (viTerminate + viClose of the session) can be asserted
 * against observable counters instead of live NI-VISA hardware.
 *
 * Counters are exported as a plain integer array so the DLL and the test executable share one
 * standard C signature: the field order below is mirrored by the MockStat enum in
 * sdk_tests/visa_link_test.cpp, and ir_mock_visa_get_stats() reports the field count so a layout
 * drift fails the test instead of corrupting memory.
 *
 * The mock is single-threaded by contract: the test drives one caller at a time.
 */
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

constexpr int32_t VisaSuccess = 0;
constexpr int32_t VisaTimeout = -1073807339;    // VI_ERROR_TMO
constexpr int32_t VisaUnsupported = -1073807331; // VI_ERROR_NSUP_ATTR / VI_ERROR_INV_OBJECT
constexpr uint32_t TimeoutAttribute = 0x3fff001a;
constexpr uint32_t StatusAttribute = 0x3fff4025;
constexpr uint32_t CountAttribute64 = 0x3fff4028;
constexpr uint32_t IoCompletionEventType = 0x3fff2009;
// VISA object handles: 1 = default resource manager, 2 = device session, >= 0x100 = event context.
constexpr uint32_t ManagerSession = 1;
constexpr uint32_t DeviceSession = 2;
constexpr uint32_t FirstEventHandle = 0x100;

/** Field order of ir_mock_visa_get_stats(); sdk_tests/visa_link_test.cpp mirrors it. */
enum MockStat : uint32_t {
    StatApiVersion = 0, // always 1; lets the test detect a layout drift
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
    StatMaxWaitTimeout,      // largest timeout requested from viWaitOnEvent (25 ms slice cap)
    StatCloseWithPendingJob, // device sessions closed while an async job was still pending
    StatInvalidTerminate,    // viTerminate calls with a foreign session, mode or job id
    MockStatCount
};

struct MockState {
    // stallWaits < 0 never completes; otherwise the first stallWaits viWaitOnEvent calls time out.
    int32_t stallWaits{0};
    int32_t waitCount{0};
    int32_t eventCount{0};
    uint32_t nextJob{0};
    uint32_t pendingJob{0};
    uint32_t pendingKind{0}; // 0 = idle, 1 = write, 2 = read
    unsigned char *pendingBuffer{nullptr};
    uint32_t pendingSize{0};
    uint64_t completedCount{0};
    uint32_t deviceSession{0};
    std::string reply{"IR,MOCK,1.0\n"};
    std::string lastWrite;
    int32_t stats[MockStatCount]{1, 0}; // [StatApiVersion] = 1
};

MockState gState;

int32_t &stat(MockStat index) noexcept
{
    return gState.stats[index];
}

void clearJob() noexcept
{
    gState.pendingJob = 0;
    gState.pendingKind = 0;
    gState.pendingBuffer = nullptr;
    gState.pendingSize = 0;
    stat(StatPendingJobs) = 0;
}

void startJob(uint32_t kind, unsigned char *buffer, uint32_t size) noexcept
{
    gState.pendingKind = kind;
    gState.pendingBuffer = buffer;
    gState.pendingSize = size;
    gState.pendingJob = ++gState.nextJob;
    stat(StatPendingJobs) = 1;
}
}

extern "C" __declspec(dllexport) int32_t WINAPI ir_mock_visa_get_stats(int32_t *outValues,
                                                                      uint32_t count) noexcept
{
    // Returns the field count so the caller can detect a layout drift before reading the values.
    for (uint32_t index = 0; index < MockStatCount && index < count; ++index)
        outValues[index] = gState.stats[index];
    return static_cast<int32_t>(MockStatCount);
}

extern "C" __declspec(dllexport) void WINAPI ir_mock_visa_reset(int32_t stallWaits) noexcept
{
    gState = MockState{};
    gState.stallWaits = stallWaits;
}

extern "C" __declspec(dllexport) int32_t WINAPI ir_mock_visa_copy_last_write(char *out,
                                                                            int32_t bytes) noexcept
{
    const auto size = static_cast<int32_t>(gState.lastWrite.size());
    if (!out || bytes <= 0) return size;
    const auto copy = size < bytes - 1 ? size : bytes - 1;
    if (copy > 0) std::memcpy(out, gState.lastWrite.data(), static_cast<size_t>(copy));
    out[copy] = '\0';
    return size;
}

extern "C" __declspec(dllexport) int32_t WINAPI viOpenDefaultRM(uint32_t *outSession) noexcept
{
    ++stat(StatOpenDefaultRm);
    if (!outSession) return VisaUnsupported;
    *outSession = ManagerSession;
    return VisaSuccess;
}

extern "C" __declspec(dllexport) int32_t WINAPI viOpen(uint32_t session, const char *, uint32_t,
                                                      uint32_t, uint32_t *outSession) noexcept
{
    ++stat(StatOpen);
    if (!outSession || session != ManagerSession) return VisaUnsupported;
    gState.deviceSession = DeviceSession;
    *outSession = DeviceSession;
    return VisaSuccess;
}

extern "C" __declspec(dllexport) int32_t WINAPI viEnableEvent(uint32_t session, uint32_t eventType,
                                                            uint16_t, uint32_t) noexcept
{
    ++stat(StatEnableEvent);
    if (session != gState.deviceSession || eventType != IoCompletionEventType) return VisaUnsupported;
    return VisaSuccess;
}

extern "C" __declspec(dllexport) int32_t WINAPI viSetAttribute(uint32_t session, uint32_t attribute,
                                                             uintptr_t value) noexcept
{
    ++stat(StatSetAttribute);
    if (session != gState.deviceSession || attribute != TimeoutAttribute) return VisaUnsupported;
    return value > 0 ? VisaSuccess : VisaUnsupported;
}

extern "C" __declspec(dllexport) int32_t WINAPI viWriteAsync(uint32_t session, unsigned char *buffer,
                                                            uint32_t size, uint32_t *outJob) noexcept
{
    ++stat(StatWriteAsync);
    if (session != gState.deviceSession || !outJob) return VisaUnsupported;
    if (buffer && size > 0) {
        gState.lastWrite.assign(reinterpret_cast<const char *>(buffer), static_cast<size_t>(size));
    } else {
        gState.lastWrite.clear();
    }
    startJob(1, buffer, size);
    *outJob = gState.pendingJob;
    return VisaSuccess;
}

extern "C" __declspec(dllexport) int32_t WINAPI viReadAsync(uint32_t session, unsigned char *buffer,
                                                           uint32_t size, uint32_t *outJob) noexcept
{
    ++stat(StatReadAsync);
    if (session != gState.deviceSession || !outJob) return VisaUnsupported;
    startJob(2, buffer, size);
    *outJob = gState.pendingJob;
    return VisaSuccess;
}

extern "C" __declspec(dllexport) int32_t WINAPI viWaitOnEvent(uint32_t session, uint32_t eventType,
                                                             uint32_t timeout, uint32_t *outType,
                                                             uint32_t *outEvent) noexcept
{
    ++stat(StatWaitOnEvent);
    if (static_cast<int32_t>(timeout) > stat(StatMaxWaitTimeout))
        stat(StatMaxWaitTimeout) = static_cast<int32_t>(timeout);
    if (session != gState.deviceSession || eventType != IoCompletionEventType) return VisaUnsupported;
    const bool stall = gState.stallWaits < 0 || ++gState.waitCount <= gState.stallWaits;
    if (stall || gState.pendingJob == 0) return VisaTimeout;
    if (gState.pendingKind == 2 && gState.pendingBuffer) {
        const size_t capacity = gState.pendingSize;
        const size_t copy = gState.reply.size() < capacity ? gState.reply.size() : capacity;
        if (copy > 0)
            std::memcpy(gState.pendingBuffer, gState.reply.data(), copy);
        gState.pendingSize = static_cast<uint32_t>(copy);
    }
    gState.completedCount = gState.pendingSize;
    // The job stays pending until the host closes the event context or the device session, so the
    // test observes the same teardown ordering as the real runtime.
    if (outType) *outType = IoCompletionEventType;
    if (outEvent) *outEvent = ++gState.eventCount + FirstEventHandle - 1;
    return VisaSuccess;
}

extern "C" __declspec(dllexport) int32_t WINAPI viGetAttribute(uint32_t object, uint32_t attribute,
                                                              void *value) noexcept
{
    ++stat(StatGetAttribute);
    if (!value || object < FirstEventHandle) return VisaUnsupported;
    if (attribute == StatusAttribute) {
        *static_cast<int32_t *>(value) = VisaSuccess;
        return VisaSuccess;
    }
    if (attribute == CountAttribute64) {
        *static_cast<uint64_t *>(value) = gState.completedCount;
        return VisaSuccess;
    }
    return VisaUnsupported;
}

extern "C" __declspec(dllexport) int32_t WINAPI viTerminate(uint32_t session, uint16_t mode,
                                                           uint32_t jobId) noexcept
{
    ++stat(StatTerminate);
    // The host must abort the exact job it started, on the open device session, in mode 0. A wrong
    // id is recorded instead of clearing the job, so the test can fail on it.
    const bool expected = gState.deviceSession != 0 && session == gState.deviceSession && mode == 0 &&
                          jobId != 0 && jobId == gState.pendingJob;
    if (!expected) {
        ++stat(StatInvalidTerminate);
        return VisaUnsupported;
    }
    clearJob();
    return VisaSuccess;
}

extern "C" __declspec(dllexport) int32_t WINAPI viClose(uint32_t object) noexcept
{
    ++stat(StatClose);
    if (object == ManagerSession) {
        ++stat(StatCloseManager);
    } else if (object == DeviceSession) {
        ++stat(StatCloseDevice);
        if (gState.pendingJob != 0) ++stat(StatCloseWithPendingJob);
        gState.deviceSession = 0;
    } else if (object >= FirstEventHandle) {
        ++stat(StatCloseEvent);
    } else {
        return VisaUnsupported;
    }
    clearJob();
    return VisaSuccess;
}
