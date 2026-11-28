#include "asio_link.h"
#include "api_common.h"
#include "utf8_path.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <array>

namespace ir::transport {
namespace {
using api::Error;
using api::require;

// VISA 64-bit C ABI, kept private so the NI development kit is optional.
using Status = int32_t;
using Object = uint32_t;
#ifdef IR_VISA_TEST_OVERRIDE
std::wstring &testVisaRuntimePath()
{
    static std::wstring path;
    return path;
}
#endif
constexpr Status VisaTimeout = -1073807339;
constexpr Status VisaLocked = -1073807345;
constexpr Status VisaBusy = -1073807246;
constexpr Status VisaMaxCount = 0x3fff0006;
constexpr uint32_t IoCompletion = 0x3fff2009;
constexpr uint32_t TimeoutAttribute = 0x3fff001a;
constexpr uint32_t StatusAttribute = 0x3fff4025;
constexpr uint32_t CountAttribute64 = 0x3fff4028;

void check(Status status, const char *operation)
{
    if (status >= 0) return;
    const auto code = status == VisaTimeout ? IR_TIMEOUT :
                      (status == VisaLocked || status == VisaBusy) ? IR_BUSY : IR_IO_ERROR;
    throw Error(code, std::string(operation) + " returned VISA status " + std::to_string(status));
}

uint32_t remaining(Deadline deadline)
{
    const auto now = std::chrono::steady_clock::now();
    waitUntil(now); // Check the procedure's cancellation predicate.
    require(now < deadline, IR_TIMEOUT, "VISA operation timed out");
    return static_cast<uint32_t>(std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
}

struct VisaModule {
#ifdef IR_VISA_TEST_OVERRIDE
    // Test-only seam (see asio_link.h): IR_station.dll is never compiled with this override, so
    // production always resolves the NI-VISA runtime from System32 only. The test passes an
    // already anchored absolute path, so dependencies resolve next to the mock runtime.
    HMODULE handle = testVisaRuntimePath().empty()
        ? nullptr
        : LoadLibraryExW(testVisaRuntimePath().c_str(), nullptr,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    HMODULE handle = LoadLibraryExW(L"visa64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#endif
    ~VisaModule() { if (handle) FreeLibrary(handle); }

    template<typename... Args>
    auto function(const char *name) const
    {
        require(handle != nullptr, IR_UNSUPPORTED, "VISA transport requires the Windows x64 NI-VISA runtime (visa64.dll)");
        auto address = GetProcAddress(handle, name);
        require(address != nullptr, IR_UNSUPPORTED, std::string("VISA runtime is missing ") + name);
        return reinterpret_cast<Status(WINAPI *)(Args...)>(address);
    }
};

class VisaLink final : public ScpiLink {
    VisaModule module_;
    using AsyncIo = Status(WINAPI *)(Object, unsigned char *, uint32_t, Object *);
    const AsyncIo write_ = module_.function<Object, unsigned char *, uint32_t, Object *>("viWriteAsync");
    const AsyncIo read_ = module_.function<Object, unsigned char *, uint32_t, Object *>("viReadAsync");
    using Close = Status(WINAPI *)(Object);
    const Close close_ = module_.function<Object>("viClose");
    Object manager_{};
    Object device_{};

    uint64_t transfer(AsyncIo start, unsigned char *buffer, uint32_t size, Deadline deadline)
    {
        const auto set = module_.function<Object, uint32_t, uintptr_t>("viSetAttribute");
        const auto wait = module_.function<Object, uint32_t, uint32_t, uint32_t *, Object *>("viWaitOnEvent");
        const auto get = module_.function<Object, uint32_t, void *>("viGetAttribute");
        const auto terminate = module_.function<Object, uint16_t, Object>("viTerminate");
        check(set(device_, TimeoutAttribute, remaining(deadline)), "viSetAttribute(timeout)");
        Object job{};
        check(start(device_, buffer, size, &job), start == read_ ? "viReadAsync" : "viWriteAsync");
        Object event{};
        try {
            uint32_t type{};
            Status status;
            do {
                status = wait(device_, IoCompletion, std::min(remaining(deadline), 25u), &type, &event);
            } while (status == VisaTimeout);
            check(status, "viWaitOnEvent");
            Status result{};
            uint64_t count{};
            check(get(event, StatusAttribute, &result), "viGetAttribute(status)");
            check(result, "VISA transfer");
            check(get(event, CountAttribute64, &count), "viGetAttribute(count)");
            require(count <= size, IR_IO_ERROR, "Invalid VISA transfer count");
            require(start != read_ || result != VisaMaxCount, IR_IO_ERROR, "VISA response exceeds buffer limit");
            close_(event);
            return count;
        } catch (...) {
            if (event) close_(event);
            terminate(device_, 0, job);
            close_(device_); // End pending operations before releasing the caller's buffer.
            device_ = 0;
            throw;
        }
    }

public:
    ~VisaLink() override
    {
        if (device_) close_(device_);
        if (manager_) close_(manager_);
    }

    void open(const std::string &resource, Deadline deadline)
    {
        const auto openManager = module_.function<Object *>("viOpenDefaultRM");
        const auto openResource = module_.function<Object, const char *, uint32_t, uint32_t, Object *>("viOpen");
        const auto enable = module_.function<Object, uint32_t, uint16_t, uint32_t>("viEnableEvent");
        check(openManager(&manager_), "viOpenDefaultRM");
        check(openResource(manager_, resource.c_str(), 1, remaining(deadline), &device_), "viOpen");
        check(enable(device_, IoCompletion, 1, 0), "viEnableEvent");
    }

    void discardSerialInput() override {}

    void writeAll(std::string_view bytes, Deadline deadline) override
    {
        auto data = reinterpret_cast<unsigned char *>(const_cast<char *>(bytes.data()));
        require(transfer(write_, data, static_cast<uint32_t>(bytes.size()), deadline) == bytes.size(),
                IR_IO_ERROR, "Incomplete VISA command write");
    }

    std::string readDelimited(char delimiter, Deadline deadline) override
    {
        std::array<unsigned char, 4096> buffer{};
        const auto count = transfer(read_, buffer.data(), static_cast<uint32_t>(buffer.size()), deadline);
        std::string reply(reinterpret_cast<const char *>(buffer.data()), static_cast<size_t>(count));
        const auto end = reply.find(delimiter);
        if (end != std::string::npos)
            require(end + 1 == reply.size(), IR_IO_ERROR, "Unexpected data after VISA reply");
        return reply; // USB INSTR also supports end-of-message without LF.
    }
};
}

std::unique_ptr<ScpiLink> openVisaLink(const std::string &resource, Deadline deadline)
{
    auto link = std::make_unique<VisaLink>();
    link->open(resource, deadline);
    return link;
}

#ifdef IR_VISA_TEST_OVERRIDE
void setVisaRuntimePathForTest(const std::string &utf8Path)
{
    testVisaRuntimePath() = utf8::pathFromUtf8(utf8Path).wstring();
}
#endif
}
#else
namespace ir::transport {
std::unique_ptr<ScpiLink> openVisaLink(const std::string &, Deadline)
{
    throw api::Error(IR_UNSUPPORTED, "VISA transport requires Windows x64");
}
}
#endif
