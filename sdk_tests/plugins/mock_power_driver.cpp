#include "IR_power_driver_abi.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace {

struct MockPowerSession {
    bool enabled{false};
    bool failClose{false};
    bool useControlIpc{false};
    // Opt-in readback corruptions used by the power_mock_test.py regression gates.
    bool mismatchOutput{false};
    bool badOutputFlag{false};
    bool nanReading{false};
    // 0 = none, 1 = setpoint_v, 2 = current_limit_a, 3 = measured_v, 4 = measured_a.
    int32_t nanField{0};
    int32_t controlPort{0};
    int32_t timeoutMs{1500};
    double setV{12.0};
    double setA{2.0};
};

/**
 * Two immutable descriptors: the default one only advertises read/output, the ``power_set``
 * capable one is selected by the ``IR_MOCK_POWER_ALLOW_SET_CHANNEL=1`` environment variable so
 * the host's descriptor never mutates between connections.
 */
int32_t mockValidate(const char *configJson, char *err, int32_t errBytes) noexcept;
int32_t mockOpen(const char *configJson, IRPowerDriverHandle *outHandle, char *identityUtf8,
                 int32_t identityBytes, char *err, int32_t errBytes) noexcept;
int32_t mockRead(IRPowerDriverHandle handle, int32_t channel, IRPowerReading *outReading,
                 char *err, int32_t errBytes) noexcept;
int32_t mockSetOutput(IRPowerDriverHandle handle, int32_t channel, int32_t enabled,
                      IRPowerReading *outReadback, char *err, int32_t errBytes) noexcept;
int32_t mockSetChannel(IRPowerDriverHandle handle, int32_t channel, double voltageV,
                       double currentLimitA, IRPowerReading *outReadback,
                       char *err, int32_t errBytes) noexcept;
int32_t mockClose(IRPowerDriverHandle handle, char *err, int32_t errBytes) noexcept;

const IRPowerDriverDescriptor kReadOutputDescriptor{
    IR_POWER_DRIVER_ABI_VERSION,
    static_cast<uint32_t>(sizeof(IRPowerDriverDescriptor)),
    "mock_power",
    IR_POWER_CAP_READ | IR_POWER_CAP_OUTPUT,
    1,
    1,
    &mockValidate,
    &mockOpen,
    &mockRead,
    &mockSetOutput,
    nullptr,
    &mockClose};

const IRPowerDriverDescriptor kFullDescriptor{
    IR_POWER_DRIVER_ABI_VERSION,
    static_cast<uint32_t>(sizeof(IRPowerDriverDescriptor)),
    "mock_power",
    IR_POWER_CAP_READ | IR_POWER_CAP_OUTPUT | IR_POWER_CAP_SET,
    1,
    1,
    &mockValidate,
    &mockOpen,
    &mockRead,
    &mockSetOutput,
    &mockSetChannel,
    &mockClose};

/** ``IR_MOCK_POWER_ALLOW_SET_CHANNEL=1`` selects the ``power_set`` capable descriptor. */
bool allowSetChannelFromEnvironment() noexcept
{
#ifdef _WIN32
    char value[8]{};
    // GetEnvironmentVariableA keeps MSVC from flagging GetEnvironmentVariableW's CRT twin.
    const DWORD written = GetEnvironmentVariableA("IR_MOCK_POWER_ALLOW_SET_CHANNEL", value,
                                                  static_cast<DWORD>(sizeof(value)));
    return written > 0 && std::strcmp(value, "1") == 0;
#else
    const char *value = std::getenv("IR_MOCK_POWER_ALLOW_SET_CHANNEL");
    return value != nullptr && std::strcmp(value, "1") == 0;
#endif
}

/** Apply the opt-in readback corruptions requested by the test configuration. */
void applyCorruptions(const MockPowerSession &cfg, IRPowerReading &reading) noexcept
{
    const double notANumber = std::nan("");
    if (cfg.nanReading) {
        // Host must reject non-finite readbacks instead of feeding them into metrics.
        reading.measured_v = notANumber;
        reading.measured_a = notANumber;
    }
    switch (cfg.nanField) {
    case 1: reading.setpoint_v = notANumber; break;
    case 2: reading.current_limit_a = notANumber; break;
    case 3: reading.measured_v = notANumber; break;
    case 4: reading.measured_a = notANumber; break;
    default: break;
    }
    if (cfg.badOutputFlag) {
        // Out-of-contract flag value (only 0/1 is legal).
        reading.output_enabled = 2;
    }
}

void writeErr(char *err, int32_t errBytes, const char *msg) noexcept
{
    if (!err || errBytes <= 0) return;
    const size_t n = std::strlen(msg);
    const size_t copy = n < static_cast<size_t>(errBytes - 1) ? n : static_cast<size_t>(errBytes - 1);
    if (copy > 0) std::memcpy(err, msg, copy);
    err[copy] = '\0';
}

const char *skipWs(const char *p) noexcept
{
    while (p && *p && std::isspace(static_cast<unsigned char>(*p))) {
        ++p;
    }
    return p;
}

bool findJsonField(const char *json, const char *key, const char **outValueStart) noexcept
{
    if (!json || !key) return false;
    std::string pattern = std::string("\"") + key + "\"";
    const char *p = json;
    while ((p = std::strstr(p, pattern.c_str())) != nullptr) {
        const char *afterKey = skipWs(p + pattern.size());
        if (afterKey && *afterKey == ':') {
            *outValueStart = skipWs(afterKey + 1);
            return true;
        }
        p += pattern.size();
    }
    return false;
}

bool parseStrictIntField(const char *valStart, int64_t &outVal) noexcept
{
    if (!valStart || !*valStart) return false;
    if (*valStart == '"' || *valStart == 't' || *valStart == 'f' || *valStart == 'n') return false;
    char *endPtr = nullptr;
    const long long parsed = std::strtoll(valStart, &endPtr, 10);
    if (endPtr == valStart) return false;
    if (*endPtr == '.' || *endPtr == 'e' || *endPtr == 'E') return false;
    const char *tail = skipWs(endPtr);
    if (*tail != ',' && *tail != '}' && *tail != '\0') return false;
    outVal = static_cast<int64_t>(parsed);
    return true;
}

bool parseDoubleField(const char *json, const char *key, double &outVal) noexcept
{
    const char *valStart = nullptr;
    if (!findJsonField(json, key, &valStart)) return false;
    char *endPtr = nullptr;
    const double parsed = std::strtod(valStart, &endPtr);
    if (endPtr == valStart) return false;
    outVal = parsed;
    return true;
}

bool parseBoolField(const char *json, const char *key, bool &outVal) noexcept
{
    const char *valStart = nullptr;
    if (!findJsonField(json, key, &valStart)) return false;
    if (std::strncmp(valStart, "true", 4) == 0) {
        outVal = true;
        return true;
    }
    if (std::strncmp(valStart, "false", 5) == 0) {
        outVal = false;
        return true;
    }
    return false;
}

int32_t parseMockConfig(const char *configJson, MockPowerSession &cfg, char *err, int32_t errBytes) noexcept
{
    if (!configJson || !*configJson || std::strstr(configJson, "invalid") != nullptr) {
        writeErr(err, errBytes, "mock_power invalid config");
        return IR_BAD_ARGUMENT;
    }

    const char *unused = nullptr;
    if (findJsonField(configJson, "host", &unused) ||
        findJsonField(configJson, "control_host", &unused) ||
        findJsonField(configJson, "url", &unused) ||
        findJsonField(configJson, "control_url", &unused)) {
        writeErr(err, errBytes, "mock_power only accepts numeric control_port on 127.0.0.1");
        return IR_BAD_ARGUMENT;
    }

    cfg.failClose = std::strstr(configJson, "\"fail_close\":true") != nullptr ||
                    std::strstr(configJson, "\"fail_close\": true") != nullptr;
    // Opt-in readback corruption flags; absent means a well-behaved driver.
    (void)parseBoolField(configJson, "mismatch_output", cfg.mismatchOutput);
    (void)parseBoolField(configJson, "bad_output_flag", cfg.badOutputFlag);
    (void)parseBoolField(configJson, "nan_reading", cfg.nanReading);

    const char *nanFieldVal = nullptr;
    if (findJsonField(configJson, "nan_field", &nanFieldVal)) {
        const auto matches = [nanFieldVal](const char *literal) {
            const size_t len = std::strlen(literal);
            return std::strncmp(nanFieldVal, "\"", 1) == 0 &&
                   std::strncmp(nanFieldVal + 1, literal, len) == 0 &&
                   nanFieldVal[len + 1] == '\"';
        };
        if (matches("setpoint_v")) cfg.nanField = 1;
        else if (matches("current_limit_a")) cfg.nanField = 2;
        else if (matches("measured_v")) cfg.nanField = 3;
        else if (matches("measured_a")) cfg.nanField = 4;
        else {
            writeErr(err, errBytes,
                     "mock_power nan_field must be one of setpoint_v, current_limit_a, measured_v, "
                     "measured_a");
            return IR_BAD_ARGUMENT;
        }
    }

    const char *portVal = nullptr;
    if (findJsonField(configJson, "control_port", &portVal)) {
        int64_t port = 0;
        if (!parseStrictIntField(portVal, port) || port < 1 || port > 65535) {
            writeErr(err, errBytes, "mock_power control_port must be an integer in [1, 65535]");
            return IR_BAD_ARGUMENT;
        }
        cfg.useControlIpc = true;
        cfg.controlPort = static_cast<int32_t>(port);
    }

    const char *timeoutVal = nullptr;
    if (findJsonField(configJson, "timeout_ms", &timeoutVal)) {
        int64_t timeout = 0;
        if (!parseStrictIntField(timeoutVal, timeout) || timeout < 50 || timeout > 30000) {
            writeErr(err, errBytes, "mock_power timeout_ms must be an integer in [50, 30000]");
            return IR_BAD_ARGUMENT;
        }
        cfg.timeoutMs = static_cast<int32_t>(timeout);
    }
    return IR_OK;
}

bool parseDp100ReadingJson(const std::string &json, IRPowerReading &outReading) noexcept
{
    double spV = 0.0;
    double limA = 0.0;
    double measV = 0.0;
    double measA = 0.0;
    bool enabled = false;
    if (!parseDoubleField(json.c_str(), "setpoint_v", spV) ||
        !parseDoubleField(json.c_str(), "current_limit_a", limA) ||
        !parseDoubleField(json.c_str(), "measured_v", measV) ||
        !parseDoubleField(json.c_str(), "measured_a", measA) ||
        !parseBoolField(json.c_str(), "output_enabled", enabled)) {
        return false;
    }
    outReading.setpoint_v = spV;
    outReading.current_limit_a = limA;
    outReading.measured_v = measV;
    outReading.measured_a = measA;
    outReading.output_enabled = enabled ? 1 : 0;
    outReading.reserved = 0;
    return true;
}

#ifdef _WIN32
struct WinHttpHandleGuard {
    HINTERNET handle{nullptr};
    ~WinHttpHandleGuard()
    {
        if (handle) WinHttpCloseHandle(handle);
    }
};

int32_t winHttpRequest(int32_t port, int32_t timeoutMs, const wchar_t *method, const wchar_t *path,
                       const std::string *body, std::string &outResponse,
                       char *err, int32_t errBytes) noexcept
{
    WinHttpHandleGuard session{WinHttpOpen(
        L"IR_mock_power/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.handle) {
        writeErr(err, errBytes, "mock_power WinHttpOpen failed");
        return IR_IO_ERROR;
    }
    WinHttpSetTimeouts(session.handle, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    WinHttpHandleGuard connect{WinHttpConnect(
        session.handle, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0)};
    if (!connect.handle) {
        writeErr(err, errBytes, "mock_power WinHttpConnect failed");
        return IR_IO_ERROR;
    }

    WinHttpHandleGuard request{WinHttpOpenRequest(
        connect.handle, method, path, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0)};
    if (!request.handle) {
        writeErr(err, errBytes, "mock_power WinHttpOpenRequest failed");
        return IR_IO_ERROR;
    }

    const wchar_t *headers = body ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    const DWORD headersLen = body ? static_cast<DWORD>(-1) : 0;
    LPVOID bodyPtr = body ? const_cast<char *>(body->data()) : WINHTTP_NO_REQUEST_DATA;
    const DWORD bodyLen = body ? static_cast<DWORD>(body->size()) : 0;

    if (!WinHttpSendRequest(request.handle, headers, headersLen, bodyPtr, bodyLen, bodyLen, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        const DWORD lastErr = GetLastError();
        if (lastErr == ERROR_WINHTTP_TIMEOUT) {
            writeErr(err, errBytes, "mock_power control HTTP request timed out");
            return IR_TIMEOUT;
        }
        writeErr(err, errBytes, "mock_power control HTTP request failed");
        return IR_IO_ERROR;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        writeErr(err, errBytes, "mock_power failed to read HTTP status");
        return IR_IO_ERROR;
    }

    outResponse.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &avail)) {
            writeErr(err, errBytes, "mock_power WinHttpQueryDataAvailable failed");
            return IR_IO_ERROR;
        }
        if (avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD readBytes = 0;
        if (!WinHttpReadData(request.handle, chunk.data(), avail, &readBytes)) {
            const DWORD lastErr = GetLastError();
            writeErr(err, errBytes, "mock_power WinHttpReadData failed");
            return lastErr == ERROR_WINHTTP_TIMEOUT ? IR_TIMEOUT : IR_IO_ERROR;
        }
        chunk.resize(readBytes);
        outResponse.append(chunk);
    }

    if (statusCode != 200) {
        std::string msg = "mock_power control HTTP status " + std::to_string(statusCode);
        if (!outResponse.empty()) {
            msg += ": " + outResponse;
        }
        writeErr(err, errBytes, msg.c_str());
        return IR_IO_ERROR;
    }
    return IR_OK;
}
#endif

int32_t mockValidate(const char *configJson, char *err, int32_t errBytes) noexcept
{
    MockPowerSession cfg;
    return parseMockConfig(configJson, cfg, err, errBytes);
}

int32_t mockOpen(const char *configJson, IRPowerDriverHandle *outHandle,
                 char *identityUtf8, int32_t identityBytes,
                 char *err, int32_t errBytes) noexcept
{
    if (!outHandle) return IR_NULL_POINTER;
    *outHandle = nullptr;
    auto *s = new MockPowerSession();
    const int32_t cfgRc = parseMockConfig(configJson, *s, err, errBytes);
    if (cfgRc != IR_OK) {
        delete s;
        return cfgRc;
    }

    if (s->useControlIpc) {
#ifdef _WIN32
        std::string body;
        const int32_t rc = winHttpRequest(
            s->controlPort, s->timeoutMs, L"GET", L"/dp100", nullptr, body, err, errBytes);
        if (rc != IR_OK) {
            delete s;
            return rc;
        }
        IRPowerReading reading{};
        if (!parseDp100ReadingJson(body, reading)) {
            delete s;
            writeErr(err, errBytes, "mock_power received invalid DP100 JSON from control HTTP");
            return IR_IO_ERROR;
        }
#else
        delete s;
        writeErr(err, errBytes, "mock_power control_port IPC is only supported on Windows");
        return IR_UNSUPPORTED;
#endif
    }

    writeErr(identityUtf8, identityBytes, "MOCK-POWER-1");
    *outHandle = s;
    return IR_OK;
}

int32_t mockRead(IRPowerDriverHandle handle, int32_t channel,
                 IRPowerReading *outReading, char *err, int32_t errBytes) noexcept
{
    if (!handle) return IR_INVALID_HANDLE;
    if (!outReading) return IR_NULL_POINTER;
    if (channel != 1) {
        writeErr(err, errBytes, "mock_power only supports channel 1");
        return IR_BAD_ARGUMENT;
    }
    auto *s = static_cast<MockPowerSession *>(handle);
    if (s->useControlIpc) {
#ifdef _WIN32
        std::string body;
        const int32_t rc = winHttpRequest(
            s->controlPort, s->timeoutMs, L"GET", L"/dp100", nullptr, body, err, errBytes);
        if (rc != IR_OK) return rc;
        if (!parseDp100ReadingJson(body, *outReading)) {
            writeErr(err, errBytes, "mock_power received invalid DP100 JSON from control HTTP");
            return IR_IO_ERROR;
        }
        return IR_OK;
#else
        writeErr(err, errBytes, "mock_power control_port IPC is only supported on Windows");
        return IR_UNSUPPORTED;
#endif
    }
    outReading->setpoint_v = s->setV;
    outReading->current_limit_a = s->setA;
    outReading->measured_v = s->enabled ? s->setV : 0.0;
    outReading->measured_a = s->enabled ? 0.25 : 0.0;
    outReading->output_enabled = s->enabled ? 1 : 0;
    outReading->reserved = 0;
    applyCorruptions(*s, *outReading);
    return IR_OK;
}

int32_t mockSetOutput(IRPowerDriverHandle handle, int32_t channel, int32_t enabled,
                      IRPowerReading *outReadback, char *err, int32_t errBytes) noexcept
{
    if (!handle) return IR_INVALID_HANDLE;
    if (!outReadback) return IR_NULL_POINTER;
    if (channel != 1) {
        writeErr(err, errBytes, "mock_power only supports channel 1");
        return IR_BAD_ARGUMENT;
    }
    auto *s = static_cast<MockPowerSession *>(handle);
    if (s->useControlIpc) {
#ifdef _WIN32
        const std::string payload = enabled != 0 ? "{\"enabled\":true}" : "{\"enabled\":false}";
        std::string body;
        const int32_t rc = winHttpRequest(
            s->controlPort, s->timeoutMs, L"POST", L"/dp100/output", &payload, body, err, errBytes);
        if (rc != IR_OK) return rc;
        if (!parseDp100ReadingJson(body, *outReadback)) {
            writeErr(err, errBytes, "mock_power received invalid DP100 JSON from control HTTP");
            return IR_IO_ERROR;
        }
        return IR_OK;
#else
        writeErr(err, errBytes, "mock_power control_port IPC is only supported on Windows");
        return IR_UNSUPPORTED;
#endif
    }
    s->enabled = enabled != 0;
    const int32_t readRc = mockRead(handle, channel, outReadback, err, errBytes);
    if (readRc == IR_OK && s->mismatchOutput) {
        // Report the opposite of the requested state so the host readback gate must fire.
        outReadback->output_enabled = enabled != 0 ? 0 : 1;
    }
    return readRc;
}

int32_t mockSetChannel(IRPowerDriverHandle handle, int32_t channel, double voltageV,
                       double currentLimitA, IRPowerReading *outReadback,
                       char *err, int32_t errBytes) noexcept
{
    if (!handle) return IR_INVALID_HANDLE;
    if (!outReadback) return IR_NULL_POINTER;
    if (channel != 1) {
        writeErr(err, errBytes, "mock_power only supports channel 1");
        return IR_BAD_ARGUMENT;
    }
    auto *s = static_cast<MockPowerSession *>(handle);
    s->setV = voltageV;
    s->setA = currentLimitA;
    return mockRead(handle, channel, outReadback, err, errBytes);
}

int32_t mockClose(IRPowerDriverHandle handle, char *err, int32_t errBytes) noexcept
{
    if (!handle) return IR_INVALID_HANDLE;
    auto *s = static_cast<MockPowerSession *>(handle);
    const bool shouldFail = s->failClose;
    delete s;
    if (shouldFail) {
        writeErr(err, errBytes, "mock_power simulated close failure");
        return IR_IO_ERROR;
    }
    return IR_OK;
}

} // namespace

extern "C" IR_POWER_DRIVER_EXPORT int32_t ir_power_driver_query(
    const IRPowerDriverDescriptor **out_desc, char *err, int32_t errBytes) noexcept
{
    (void)err;
    (void)errBytes;
    if (!out_desc) return IR_NULL_POINTER;
    *out_desc = allowSetChannelFromEnvironment() ? &kFullDescriptor : &kReadOutputDescriptor;
    return IR_OK;
}
