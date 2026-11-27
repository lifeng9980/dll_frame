#include "IR_power_driver_abi.h"
#include <cstring>
#include <string>

namespace {

struct MockPowerSession {
    bool enabled{false};
    bool failClose{false};
    double setV{12.0};
    double setA{2.0};
};

void writeErr(char *err, int32_t errBytes, const char *msg) noexcept
{
    if (!err || errBytes <= 0) return;
    const size_t n = std::strlen(msg);
    const size_t copy = n < static_cast<size_t>(errBytes - 1) ? n : static_cast<size_t>(errBytes - 1);
    if (copy > 0) std::memcpy(err, msg, copy);
    err[copy] = '\0';
}

int32_t mockValidate(const char *configJson, char *err, int32_t errBytes) noexcept
{
    if (!configJson || !*configJson || std::strstr(configJson, "invalid") != nullptr) {
        writeErr(err, errBytes, "mock_power invalid config");
        return IR_BAD_ARGUMENT;
    }
    return IR_OK;
}

int32_t mockOpen(const char *configJson, IRPowerDriverHandle *outHandle,
                 char *identityUtf8, int32_t identityBytes,
                 char *err, int32_t errBytes) noexcept
{
    if (!outHandle) return IR_NULL_POINTER;
    *outHandle = nullptr;
    if (mockValidate(configJson, err, errBytes) != IR_OK) return IR_BAD_ARGUMENT;
    auto *s = new MockPowerSession();
    s->failClose = std::strstr(configJson, "\"fail_close\":true") != nullptr ||
                   std::strstr(configJson, "\"fail_close\": true") != nullptr;
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
    outReading->setpoint_v = s->setV;
    outReading->current_limit_a = s->setA;
    outReading->measured_v = s->enabled ? s->setV : 0.0;
    outReading->measured_a = s->enabled ? 0.25 : 0.0;
    outReading->output_enabled = s->enabled ? 1 : 0;
    outReading->reserved = 0;
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
    s->enabled = enabled != 0;
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

const IRPowerDriverDescriptor kMockDesc{
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
    &mockClose
};

} // namespace

extern "C" IR_POWER_DRIVER_EXPORT int32_t ir_power_driver_query(
    const IRPowerDriverDescriptor **out_desc, char *err, int32_t errBytes) noexcept
{
    (void)err;
    (void)errBytes;
    if (!out_desc) return IR_NULL_POINTER;
    *out_desc = &kMockDesc;
    return IR_OK;
}
