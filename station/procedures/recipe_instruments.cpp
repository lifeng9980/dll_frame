#include "recipe_runtime_common.h"
#include "IR_device.h"
#include "api_common.h"
#include <algorithm>

namespace ir::procedures::recipe {
using namespace ir::api;

namespace {
const char *dmmModeName(int32_t mode) noexcept
{
    switch (mode) {
    case IR_DMM_MODE_DCV: return "dcv";
    case IR_DMM_MODE_DCI: return "dci";
    case IR_DMM_MODE_RES: return "res";
    case IR_DMM_MODE_DIODE: return "diode";
    default: return "any";
    }
}

const char *ovenModeName(int32_t mode) noexcept
{
    switch (mode) {
    case IR_OVEN_MODE_STANDBY: return "STANDBY";
    case IR_OVEN_MODE_CONSTANT: return "CONSTANT";
    case IR_OVEN_MODE_PROGRAM: return "PROGRAM";
    case IR_OVEN_MODE_OTHER: return "OTHER";
    default: return "UNKNOWN";
    }
}
} // namespace


void runControlBlackbody(const ir::acquisition::Capture &capture, IRHandle handle,
                         const BlackbodyStep &step, RecipeReadbacks &readbacks)
{
    const auto &target = step.target;
    char error[1024]{};
    double setpoint{};
    checkStatus(IR_SetBlackbody(handle, target.temperatureC, &setpoint, error, sizeof error), error);
    const auto reading = waitForBlackbody(capture, handle, target);
    readbacks.conditions.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"target_c", target.temperatureC},
        {"setpoint_readback_c", setpoint},
        {"actual_c", reading.actual_c},
        {"device_stable", reading.stable != 0},
        {"tolerance_c", target.toleranceC},
        {"stable_window_ms", target.stableWindowMs},
        {"timeout_ms", target.timeoutMs}
    });
}

void runControlPower(IRHandle handle, const PowerStep &step, RecipeReadbacks &readbacks,
                     std::vector<const PowerStep *> &powerToSwitchOff)
{
    using Operation = PowerStep::Operation;
    char error[1024]{};
    IRPowerReading reading{};
    const char *action = "read";
    switch (step.operation) {
    case Operation::set:
        action = "set";
        checkStatus(IR_SetPowerChannel(handle, step.channel, step.voltageV, step.currentLimitA,
                                       &reading, error, sizeof error), error);
        break;
    case Operation::output:
        action = "output";
        if (step.offAfterRun && std::none_of(powerToSwitchOff.begin(), powerToSwitchOff.end(),
            [&](const PowerStep *pending) {
                return pending->device == step.device && pending->channel == step.channel;
            })) {
            powerToSwitchOff.push_back(&step);
        }
        checkStatus(IR_SetPowerOutput(handle, step.channel, step.enabled ? 1 : 0,
                                      &reading, error, sizeof error), error);
        break;
    case Operation::read:
        checkStatus(IR_ReadPower(handle, step.channel, &reading, error, sizeof error), error);
        break;
    }
    readbacks.instruments.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"operation", action},
        {"reading", powerJson(reading, step.channel)}
    });
}

void runControlPowerWaitStable(const ir::acquisition::Capture &capture, IRHandle handle,
                               const PowerWaitStableStep &step, RecipeReadbacks &readbacks)
{
    const auto reading = waitForPowerCurrentStable(capture, handle, step.channel, step.spec);
    readbacks.instruments.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"operation", "power_wait_stable"},
        {"reading", powerJson(reading, step.channel)}
    });
}

void runControlOvenStabilize(const ir::acquisition::Capture &capture, IRHandle handle,
                             const OvenStabilizeStep &step, RecipeReadbacks &readbacks,
                             std::vector<std::string> &ovensToStandby)
{
    if (step.standbyAfterRun &&
        std::find(ovensToStandby.begin(), ovensToStandby.end(), step.device) == ovensToStandby.end()) {
        ovensToStandby.push_back(step.device);
    }
    char error[1024]{};
    double setpoint{};
    checkStatus(IR_SetOven(handle, step.target.temperatureC, &setpoint, error, sizeof error), error);
    const auto reading = waitForOven(capture, handle, step.target);
    readbacks.conditions.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"kind", "oven"},
        {"target_c", step.target.temperatureC},
        {"setpoint_readback_c", setpoint},
        {"actual_c", reading.actual_c},
        {"mode", ovenModeName(reading.mode)},
        {"alarm_active", reading.alarm_active != 0},
        {"tolerance_c", step.target.toleranceC},
        {"stable_window_ms", step.target.stableWindowMs},
        {"timeout_ms", step.target.timeoutMs}
    });
}

void runControlOvenStandby(IRHandle handle, const OvenStandbyStep &step, RecipeReadbacks &readbacks)
{
    char error[1024]{};
    checkStatus(IR_StopOven(handle, error, sizeof error), error);
    IROvenReading reading{};
    checkStatus(IR_ReadOven(handle, &reading, error, sizeof error), error);
    readbacks.instruments.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"operation", "standby"},
        {"reading", {
            {"actual_c", reading.actual_c},
            {"setpoint_c", reading.setpoint_c},
            {"mode", ovenModeName(reading.mode)},
            {"alarm_active", reading.alarm_active != 0}
        }}
    });
}

void runControlDmmRead(IRHandle handle, const DmmReadStep &step,
                       RecipeReadbacks &readbacks, ExecutionContext &ctx)
{
    char error[1024]{};
    if (step.setMode && step.mode != IR_DMM_MODE_ANY)
        checkStatus(IR_SetDmmMode(handle, step.mode, error, sizeof error), error);
    IRDmmReading reading{};
    checkStatus(IR_ReadDmm(handle, step.mode, &reading, error, sizeof error), error);
    readbacks.instruments.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"operation", "dmm_read"},
        {"reading", {
            {"mode", dmmModeName(reading.mode)},
            {"value", reading.value},
            {"unit", step.unit},
            {"valid", reading.valid != 0}
        }}
    });
    const std::string key = step.name + ".value";
    ctx.scalarValues[key] = reading.value;
    ctx.metrics[key] = {
        {"value", reading.value},
        {"unit", step.unit},
        {"method", "dmm6500_" + std::string(dmmModeName(reading.mode))},
        {"version", 1}
    };
}

void runControlDetectorTiming(IRHandle handle, const DetectorTimingStep &step, RecipeReadbacks &readbacks)
{
    char error[1024]{};
    checkStatus(IR_SetDetectorTiming(handle, step.integrationUs, step.frameHz,
                                     step.emptyUs, error, sizeof error), error);
    readbacks.conditions.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"kind", "detector_timing"},
        {"integration_us", step.integrationUs},
        {"frame_hz", step.frameHz},
        {"empty_us", step.emptyUs}
    });
}

void runControlDetectorGpol(IRHandle handle, const DetectorGpolStep &step, RecipeReadbacks &readbacks)
{
    char error[1024]{};
    checkStatus(IR_SetDetectorGpol(handle, step.gpolV, error, sizeof error), error);
    readbacks.conditions.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"kind", "detector_gpol"},
        {"gpol_v", step.gpolV}
    });
}

void runControlDetectorRegister(IRHandle handle, const DetectorRegisterStep &step, RecipeReadbacks &readbacks)
{
    char error[1024]{};
    checkStatus(IR_SetDetectorRegister(handle, step.commandCode, step.payload.data(),
                                       static_cast<int32_t>(step.payload.size()), error, sizeof error), error);
    readbacks.conditions.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"kind", "detector_register"},
        {"command_code", step.commandCode},
        {"payload_hex", step.payloadHex}
    });
}

void runControlDetectorCurrent(IRHandle handle, const DetectorCurrentStep &step,
                               RecipeReadbacks &readbacks, ExecutionContext &ctx)
{
    char error[1024]{};
    IRDetectorCurrentReading reading{};
    checkStatus(IR_MeasureDetectorCurrent(handle, step.gainCode, step.channelCode,
                                          &reading, error, sizeof error), error);
    readbacks.instruments.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"operation", "detector_current"},
        {"gain_code", step.gainCode},
        {"channel_code", step.channelCode},
        {"reading", {
            {"raw_code", reading.raw_code},
            {"sense_voltage_v", reading.sense_voltage_v},
            {"current_ma", reading.current_ma}
        }}
    });
    const std::string kCur = step.name + ".current_ma";
    const std::string kSense = step.name + ".sense_voltage_v";
    ctx.scalarValues[kCur] = reading.current_ma;
    ctx.scalarValues[kSense] = reading.sense_voltage_v;
    ctx.metrics[kCur] = {
        {"value", reading.current_ma},
        {"unit", "mA"},
        {"method", "detector_current_adc"},
        {"version", 1}
    };
    ctx.metrics[kSense] = {
        {"value", reading.sense_voltage_v},
        {"unit", "V"},
        {"method", "detector_current_adc"},
        {"version", 1}
    };
}

void runControlCoolerRead(IRHandle handle, const CoolerReadStep &step,
                          RecipeReadbacks &readbacks, ExecutionContext &ctx)
{
    char error[1024]{};
    IRCoolerReading reading{};
    checkStatus(IR_ReadCooler(handle, &reading, error, sizeof error), error);
    readbacks.instruments.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"operation", "cooler_read"},
        {"reading", {
            {"raw_value", reading.raw_value},
            {"voltage_mv", reading.voltage_mv},
            {"valid", reading.valid != 0}
        }}
    });
    const std::string key = step.name + ".voltage_mv";
    ctx.scalarValues[key] = reading.voltage_mv;
    ctx.metrics[key] = {
        {"value", reading.voltage_mv},
        {"unit", "mV"},
        {"method", "cooler_modbus"},
        {"version", 1}
    };
}

} // namespace ir::procedures::recipe
