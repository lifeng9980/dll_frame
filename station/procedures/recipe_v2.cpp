#include "recipe_config.h"
#include "IR_analysis.h"
#include "IR_device.h"
#include "api_common.h"
#include "metric_provider_v2.h"
#include "recipe_runtime_common.h"
#include "save_raw.h"
#include "station/acquisition/frame_source.h"
#include "station/instruments/device_lease.h"
#include "station/version.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace ir::procedures::recipe {
using namespace ir::api;
namespace fs = std::filesystem;

namespace {

fs::path pathFromUtf8(const std::string &utf8)
{
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string portableNameKey(std::string value)
{
    for (auto &character : value) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return value;
}

bool reservedOutputName(const std::string &value)
{
    const auto key = portableNameKey(value);
    return key == "frames.csv" || key == "frames.raw" || key == "result.json" ||
           key == "result.json.tmp";
}

void validateRelativeSingleName(const std::string &value, const std::string &path)
{
    require(!value.empty() && value != "." && value != "..", IR_BAD_ARGUMENT,
            path + " must be a valid single path segment");
    for (unsigned char ch : value) {
        require(ch != '\0' && ch != '/' && ch != '\\' && ch != ':', IR_BAD_ARGUMENT,
                path + " cannot contain path separators or drive markers");
    }
    require(value.find("..") == std::string::npos, IR_BAD_ARGUMENT,
            path + " cannot contain '..'");
    require(value.back() != ' ' && value.back() != '.', IR_BAD_ARGUMENT,
            path + " cannot end with a space or period");
    std::string base = portableNameKey(value.substr(0, value.find('.')));
    static const std::set<std::string> reservedDevices{
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
        "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4",
        "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    require(!reservedDevices.contains(base), IR_BAD_ARGUMENT,
            path + " cannot use a reserved Windows device name");
}

void registerBatchSlot(const std::string &slot, std::map<std::string, SlotKind> &knownSlots,
                       const std::string &path)
{
    const auto [it, inserted] = knownSlots.emplace(slot, SlotKind::batch);
    if (!inserted) {
        require(it->second == SlotKind::batch, IR_BAD_ARGUMENT,
                path + " cannot overwrite non-batch slot '" + slot + "'");
    }
}

void requireDeviceKind(const StationConfig &station, const std::string &alias, int expectedKind,
                       const std::string &path, const char *label)
{
    const auto it = station.devices.find(alias);
    require(it != station.devices.end() && it->second.kind == expectedKind,
            IR_BAD_ARGUMENT, path + ".device must name a configured " + label);
}

const char *dmmModeName(int32_t mode)
{
    switch (mode) {
    case IR_DMM_MODE_DCV: return "dcv";
    case IR_DMM_MODE_DCI: return "dci";
    case IR_DMM_MODE_RES: return "res";
    case IR_DMM_MODE_DIODE: return "diode";
    default: return "any";
    }
}

const char *ovenModeName(int32_t mode)
{
    switch (mode) {
    case IR_OVEN_MODE_STANDBY: return "STANDBY";
    case IR_OVEN_MODE_CONSTANT: return "CONSTANT";
    case IR_OVEN_MODE_PROGRAM: return "PROGRAM";
    case IR_OVEN_MODE_OTHER: return "OTHER";
    default: return "UNKNOWN";
    }
}

template <class VariantType>
VariantType parseSharedControlOrCaptureOrFlushOrAnalyze(
    StepRef ref, const std::string &type, const Json &fields, const std::string &path,
    const StationConfig &station, std::map<std::string, SlotKind> &knownSlots,
    std::set<std::string> &usedDevices, std::set<std::string> &analysisNames,
    std::map<std::string, std::string> &metricUnits,
    std::vector<std::string> *producedMetricsOrder,
    const IMetricProviderV2 *externalMetrics)
{
    if (type == "blackbody_stabilize") {
        auto step = parseBlackbodyStep(std::move(ref), fields, path, station);
        usedDevices.insert(step.device);
        return step;
    }
    if (type == "power_set" || type == "power_output" || type == "power_read") {
        auto step = parsePowerStep(std::move(ref), type, fields, path, station);
        usedDevices.insert(step.device);
        return step;
    }
    if (type == "power_wait_stable") {
        allowFields(fields, {"id", "type", "device", "channel",
                             "expected_voltage_v", "voltage_tolerance_v", "max_current_a",
                             "startup_threshold_a", "startup_required_samples", "startup_timeout_ms",
                             "stable_threshold_a", "max_variation_a", "stable_required_samples",
                             "sample_interval_ms", "timeout_ms"}, path);
        const auto alias = stringField(fields, "device", path);
        requireDeviceKind(station, alias, IR_IT6332A, path, "it6332a");
        const int32_t channel = integerField(fields, "channel", 1, 3, path);
        IRPowerCurrentStableSpec spec{};
        spec.expected_voltage_v = optionalNumber(fields, "expected_voltage_v", 0.0, path);
        spec.voltage_tolerance_v = optionalNumber(fields, "voltage_tolerance_v", 0.15, path);
        spec.max_current_a = optionalNumber(fields, "max_current_a", 0.0, path);
        spec.startup_threshold_a = optionalNumber(fields, "startup_threshold_a", 0.3, path);
        spec.stable_threshold_a = optionalNumber(fields, "stable_threshold_a", 0.3, path);
        spec.max_variation_a = optionalNumber(fields, "max_variation_a", 0.02, path);
        spec.startup_required_samples = optionalInteger(fields, "startup_required_samples", 3, 0, 100000, path);
        spec.startup_timeout_ms = optionalInteger(fields, "startup_timeout_ms", 30000, 0, 86400000, path);
        spec.stable_required_samples = optionalInteger(fields, "stable_required_samples", 5, 1, 100000, path);
        spec.sample_interval_ms = optionalInteger(fields, "sample_interval_ms", 200, 0, 60000, path);
        spec.timeout_ms = optionalInteger(fields, "timeout_ms", 600000, 1, 86400000, path);
        spec.reserved = 0;
        require(spec.expected_voltage_v >= 0.0 && spec.voltage_tolerance_v >= 0.0 &&
                spec.max_current_a >= 0.0 && spec.startup_threshold_a >= 0.0 &&
                spec.stable_threshold_a > 0.0 && spec.max_variation_a >= 0.0,
                IR_BAD_ARGUMENT, path + " has invalid power stabilization thresholds");
        if (spec.startup_required_samples > 0) {
            require(spec.startup_threshold_a > 0.0 && spec.startup_timeout_ms >= 1,
                    IR_BAD_ARGUMENT, path + " requires startup_threshold_a > 0 and startup_timeout_ms >= 1");
        }
        usedDevices.insert(alias);
        return PowerWaitStableStep{std::move(ref), alias, channel, spec};
    }
    if (type == "oven_stabilize") {
        allowFields(fields, {"id", "type", "device", "target_c", "tolerance_c",
                             "stable_window_ms", "timeout_ms", "require_no_alarm", "standby_after_run"}, path);
        const auto alias = stringField(fields, "device", path);
        requireDeviceKind(station, alias, IR_OVEN_GWS_STEN, path, "gws-sten oven");
        const OvenTarget target{
            numberField(fields, "target_c", path),
            numberField(fields, "tolerance_c", path),
            integerField(fields, "stable_window_ms", 0, 3600000, path),
            integerField(fields, "timeout_ms", 1, 86400000, path),
            optionalBoolean(fields, "require_no_alarm", true, path)
        };
        require(target.toleranceC > 0.0 && target.timeoutMs >= target.stableWindowMs &&
                target.temperatureC >= -100.0 && target.temperatureC <= 250.0,
                IR_BAD_ARGUMENT, path + " has invalid oven temperature or stability limits");
        const bool standbyAfter = optionalBoolean(fields, "standby_after_run", false, path);
        usedDevices.insert(alias);
        return OvenStabilizeStep{std::move(ref), alias, target, standbyAfter};
    }
    if (type == "oven_standby") {
        allowFields(fields, {"id", "type", "device"}, path);
        const auto alias = stringField(fields, "device", path);
        requireDeviceKind(station, alias, IR_OVEN_GWS_STEN, path, "gws-sten oven");
        usedDevices.insert(alias);
        return OvenStandbyStep{std::move(ref), alias};
    }
    if (type == "dmm_read") {
        allowFields(fields, {"id", "type", "device", "name", "mode", "set_mode"}, path);
        const auto alias = stringField(fields, "device", path);
        requireDeviceKind(station, alias, IR_DMM6500, path, "dmm6500");
        const std::string stepName = fields.contains("name") ? stringField(fields, "name", path) : ref.id;
        const std::string modeText = optionalString(fields, "mode", "any", path);
        const bool setMode = optionalBoolean(fields, "set_mode", false, path);
        int32_t mode = IR_DMM_MODE_ANY;
        std::string unit = "reading";
        if (modeText == "any") { mode = IR_DMM_MODE_ANY; unit = "reading"; }
        else if (modeText == "dcv") { mode = IR_DMM_MODE_DCV; unit = "V"; }
        else if (modeText == "dci") { mode = IR_DMM_MODE_DCI; unit = "A"; }
        else if (modeText == "res") { mode = IR_DMM_MODE_RES; unit = "Ohm"; }
        else if (modeText == "diode") { mode = IR_DMM_MODE_DIODE; unit = "V"; }
        else throw Error(IR_BAD_ARGUMENT, path + ".mode is unsupported: " + modeText);
        require(!setMode || mode != IR_DMM_MODE_ANY, IR_BAD_ARGUMENT,
                path + ".set_mode=true requires mode in {dcv, dci, res, diode}");
        require(analysisNames.insert(stepName).second, IR_BAD_ARGUMENT,
                path + ".name is duplicated: " + stepName);
        const std::string metricKey = stepName + ".value";
        require(metricUnits.emplace(metricKey, unit).second, IR_BAD_ARGUMENT,
                path + " produces duplicate metric name: " + metricKey);
        if (producedMetricsOrder)
            producedMetricsOrder->push_back(metricKey);
        usedDevices.insert(alias);
        return DmmReadStep{std::move(ref), alias, stepName, mode, setMode, std::move(unit)};
    }
    if (type == "detector_timing") {
        allowFields(fields, {"id", "type", "device", "integration_us", "frame_hz", "empty_us"}, path);
        const auto alias = stringField(fields, "device", path);
        requireDeviceKind(station, alias, IR_DETECTOR_SERIAL, path, "detector-serial");
        const int32_t intUs = integerField(fields, "integration_us", 1, 1000000, path);
        const int32_t hz = integerField(fields, "frame_hz", 1, 1000000, path);
        const int32_t emptyUs = optionalInteger(fields, "empty_us", 0, 0, 0xFFFFFF, path);
        require(intUs <= 1000000 / hz, IR_BAD_ARGUMENT,
                path + ": integration_us exceeds frame period (1e6 / frame_hz)");
        usedDevices.insert(alias);
        return DetectorTimingStep{std::move(ref), alias, intUs, hz, emptyUs};
    }
    if (type == "detector_gpol") {
        allowFields(fields, {"id", "type", "device", "gpol_v"}, path);
        const auto alias = stringField(fields, "device", path);
        requireDeviceKind(station, alias, IR_DETECTOR_SERIAL, path, "detector-serial");
        const double gpol = numberField(fields, "gpol_v", path);
        require(std::isfinite(gpol) && gpol >= 0.0 && gpol <= 5.0, IR_BAD_ARGUMENT,
                path + ".gpol_v must be within [0.0, 5.0]");
        usedDevices.insert(alias);
        return DetectorGpolStep{std::move(ref), alias, gpol};
    }
    if (type == "detector_register") {
        allowFields(fields, {"id", "type", "device", "command_code", "payload_hex"}, path);
        const auto alias = stringField(fields, "device", path);
        requireDeviceKind(station, alias, IR_DETECTOR_SERIAL, path, "detector-serial");
        const int32_t cmd = integerField(fields, "command_code", 0, 255, path);
        const auto hexText = stringField(fields, "payload_hex", path);
        require(hexText.size() >= 2 && hexText.size() <= 510 && (hexText.size() % 2) == 0,
                IR_BAD_ARGUMENT, path + ".payload_hex must contain 1..255 hex bytes");
        auto hexVal = [&](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        std::vector<uint8_t> payload;
        payload.reserve(hexText.size() / 2);
        for (size_t i = 0; i < hexText.size(); i += 2) {
            const int hi = hexVal(hexText[i]);
            const int lo = hexVal(hexText[i + 1]);
            require(hi >= 0 && lo >= 0, IR_BAD_ARGUMENT, path + ".payload_hex contains non-hex characters");
            payload.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        usedDevices.insert(alias);
        return DetectorRegisterStep{std::move(ref), alias, static_cast<uint8_t>(cmd), std::move(payload), hexText};
    }
    if (type == "detector_current") {
        allowFields(fields, {"id", "type", "device", "name", "gain_code", "channel_code"}, path);
        const auto alias = stringField(fields, "device", path);
        requireDeviceKind(station, alias, IR_DETECTOR_SERIAL, path, "detector-serial");
        const std::string stepName = fields.contains("name") ? stringField(fields, "name", path) : ref.id;
        const int32_t gainCode = integerField(fields, "gain_code", 0, 7, path);
        const uint16_t channelCode = static_cast<uint16_t>(integerField(fields, "channel_code", 0, 65535, path));
        require(analysisNames.insert(stepName).second, IR_BAD_ARGUMENT,
                path + ".name is duplicated: " + stepName);
        const std::string mCurrent = stepName + ".current_ma";
        const std::string mSense = stepName + ".sense_voltage_v";
        require(metricUnits.emplace(mCurrent, "mA").second, IR_BAD_ARGUMENT,
                path + " produces duplicate metric name: " + mCurrent);
        require(metricUnits.emplace(mSense, "V").second, IR_BAD_ARGUMENT,
                path + " produces duplicate metric name: " + mSense);
        if (producedMetricsOrder) {
            producedMetricsOrder->push_back(mCurrent);
            producedMetricsOrder->push_back(mSense);
        }
        usedDevices.insert(alias);
        return DetectorCurrentStep{std::move(ref), alias, stepName, gainCode, channelCode};
    }
    if (type == "cooler_read") {
        allowFields(fields, {"id", "type", "device", "name"}, path);
        const auto alias = stringField(fields, "device", path);
        requireDeviceKind(station, alias, IR_COOLER_MODBUS, path, "cooler-modbus");
        const std::string stepName = fields.contains("name") ? stringField(fields, "name", path) : ref.id;
        require(analysisNames.insert(stepName).second, IR_BAD_ARGUMENT,
                path + ".name is duplicated: " + stepName);
        const std::string metricKey = stepName + ".voltage_mv";
        require(metricUnits.emplace(metricKey, "mV").second, IR_BAD_ARGUMENT,
                path + " produces duplicate metric name: " + metricKey);
        if (producedMetricsOrder)
            producedMetricsOrder->push_back(metricKey);
        usedDevices.insert(alias);
        return CoolerReadStep{std::move(ref), alias, stepName};
    }
    if (type == "flush_frames") {
        allowFields(fields, {"id", "type", "discard_frames", "timeout_ms"}, path);
        return FlushStep{
            std::move(ref),
            optionalInteger(fields, "discard_frames", 1, 0, 10000, path),
            optionalInteger(fields, "timeout_ms", 2000, 0, 86400000, path)
        };
    }
    if (type == "capture") {
        allowFields(fields, {"id", "type", "frames", "frame_timeout_ms", "output"}, path);
        std::string outputSlot = fields.contains("output")
            ? stringField(fields, "output", path)
            : ref.id;
        registerBatchSlot(outputSlot, knownSlots, path + ".output");
        return CaptureStepV2{
            std::move(ref),
            integerField(fields, "frames", 1, INT32_MAX, path),
            integerField(fields, "frame_timeout_ms", 0, 86400000, path),
            std::move(outputSlot)
        };
    }
    if (type == "analyze") {
        auto step = prepareAnalysisV2(std::move(ref), fields, path, knownSlots, externalMetrics);
        require(analysisNames.insert(step.name).second, IR_BAD_ARGUMENT,
                path + ".name is duplicated: " + step.name);
        for (const auto &out : step.outputs) {
            const std::string fullMetric = step.name + out.suffix;
            require(metricUnits.emplace(fullMetric, out.unit).second, IR_BAD_ARGUMENT,
                    path + " produces a duplicate metric name: " + fullMetric);
            if (producedMetricsOrder)
                producedMetricsOrder->push_back(fullMetric);
        }
        return step;
    }
    throw Error(IR_BAD_ARGUMENT, path + ".type is unsupported: " + type);
}

struct V2Readbacks {
    Json conditions = Json::array();
    Json instruments = Json::array();
};

struct SavedBatchSnapshot {
    std::string inputSlot;
    std::string subdir;
    FrameBatch batch;
};

uint64_t retainedRecipeStorage(const RecipeConfigV2 &recipe, IRGeometry geometry,
                               int64_t maxBatchBytes)
{
    struct BatchSlot {
        int32_t currentFrames{};
        int32_t maxFrames{};
    };

    uint64_t total = 0;
    std::map<std::string, BatchSlot> batchSlots;
    const auto recordCapture = [&](const CaptureStepV2 &step) {
        auto &slot = batchSlots[step.outputSlot];
        slot.currentFrames = step.frames;
        slot.maxFrames = std::max(slot.maxFrames, step.frames);
    };

    for (const auto &step : recipe.steps) {
        if (const auto *capture = std::get_if<CaptureStepV2>(&step)) {
            recordCapture(*capture);
        } else if (const auto *burst = std::get_if<RepeatBurstStep>(&step)) {
            for (const auto &inner : burst->steps)
                if (const auto *capture = std::get_if<CaptureStepV2>(&inner))
                    recordCapture(*capture);
        } else if (const auto *save = std::get_if<SaveRawStepV2>(&step)) {
            const auto frames = batchSlots.at(save->inputSlot).currentFrames;
            total = addBytes(total, batchStorageBytes(geometry, frames, maxBatchBytes));
        }
    }

    for (const auto &[name, slot] : batchSlots) {
        (void)name;
        total = addBytes(total, batchStorageBytes(geometry, slot.maxFrames, maxBatchBytes));
    }

    const uint64_t pixelCount = static_cast<uint64_t>(pixels(geometry.height, geometry.width));
    total = addBytes(total, multiplyBytes(
        multiplyBytes(pixelCount, sizeof(double)), recipe.doubleMapSlotCount));
    total = addBytes(total, multiplyBytes(pixelCount, recipe.byteMaskSlotCount));
    total = addBytes(total, multiplyBytes(sizeof(IRGjbDriftContext), recipe.driftContextSlotCount));
    return total;
}

void runControlBlackbody(const ir::acquisition::Capture &capture, IRHandle handle,
                         const BlackbodyStep &step, V2Readbacks &readbacks)
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

void runControlPower(IRHandle handle, const PowerStep &step, V2Readbacks &readbacks,
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
                               const PowerWaitStableStep &step, V2Readbacks &readbacks)
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
                             const OvenStabilizeStep &step, V2Readbacks &readbacks,
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

void runControlOvenStandby(IRHandle handle, const OvenStandbyStep &step, V2Readbacks &readbacks)
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
                       V2Readbacks &readbacks, ExecutionContext &ctx)
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

void runControlDetectorTiming(IRHandle handle, const DetectorTimingStep &step, V2Readbacks &readbacks)
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

void runControlDetectorGpol(IRHandle handle, const DetectorGpolStep &step, V2Readbacks &readbacks)
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

void runControlDetectorRegister(IRHandle handle, const DetectorRegisterStep &step, V2Readbacks &readbacks)
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
                               V2Readbacks &readbacks, ExecutionContext &ctx)
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
                          V2Readbacks &readbacks, ExecutionContext &ctx)
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

void validateInnerGeometry(const InnerStepV2 &step, IRGeometry geom, int64_t maxBatchElements)
{
    if (const auto *cap = std::get_if<CaptureStepV2>(&step)) {
        const auto elements = stackSize(pixels(geom.height, geom.width), cap->frames);
        require(elements <= maxBatchElements, IR_BAD_ARGUMENT,
                "Frame batch exceeds station.acquisition.max_batch_bytes");
    } else if (const auto *an = std::get_if<AnalysisStepV2>(&step)) {
        if (an->operations.checkGeometry)
            an->operations.checkGeometry(geom);
    }
}

std::string formatDoubleClassic(double val)
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::setprecision(15) << val;
    return oss.str();
}

struct TopStepInfo {
    const StepRef *ref{nullptr};
    const char *typeName{""};
    int32_t initialStage{IR_RECIPE_STAGE_NONE};
};

TopStepInfo topStepInfo(const StepV2 &step) noexcept
{
    return std::visit([](const auto &s) -> TopStepInfo {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, BlackbodyStep>) {
            return {&s.ref, "blackbody_stabilize", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, PowerStep>) {
            const char *name = "power_read";
            if (s.operation == PowerStep::Operation::set) name = "power_set";
            else if (s.operation == PowerStep::Operation::output) name = "power_output";
            return {&s.ref, name, IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, PowerWaitStableStep>) {
            return {&s.ref, "power_wait_stable", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, OvenStabilizeStep>) {
            return {&s.ref, "oven_stabilize", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, OvenStandbyStep>) {
            return {&s.ref, "oven_standby", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, DmmReadStep>) {
            return {&s.ref, "dmm_read", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, DetectorTimingStep>) {
            return {&s.ref, "detector_timing", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, DetectorGpolStep>) {
            return {&s.ref, "detector_gpol", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, DetectorRegisterStep>) {
            return {&s.ref, "detector_register", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, DetectorCurrentStep>) {
            return {&s.ref, "detector_current", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, CoolerReadStep>) {
            return {&s.ref, "cooler_read", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, FlushStep>) {
            return {&s.ref, "flush_frames", IR_RECIPE_STAGE_CAPTURE};
        } else if constexpr (std::is_same_v<T, CaptureStepV2>) {
            return {&s.ref, "capture", IR_RECIPE_STAGE_CAPTURE};
        } else if constexpr (std::is_same_v<T, AnalysisStepV2>) {
            return {&s.ref, "analyze", IR_RECIPE_STAGE_ANALYZE};
        } else if constexpr (std::is_same_v<T, SaveRawStepV2>) {
            return {&s.ref, "save_raw", IR_RECIPE_STAGE_ARCHIVE};
        } else if constexpr (std::is_same_v<T, RepeatBurstStep>) {
            return {&s.ref, "repeat_burst", IR_RECIPE_STAGE_CAPTURE};
        }
        return {};
    }, step);
}

} // namespace

RecipeConfigV2 parseRecipeV2(const char *text, const StationConfig &station,
                             const IMetricProviderV2 *externalMetrics)
{
    auto root = parseJson(text, "recipe JSON");
    require(root.is_object(), IR_BAD_ARGUMENT, "recipe JSON must contain an object");
    allowFields(root, {"schemav", "recipe_id", "steps", "safety", "limits"}, "recipe");
    (void)optionalInteger(root, "schemav", 1, 1, 1, "recipe");

    RecipeConfigV2 recipe;
    recipe.id = stringField(root, "recipe_id", "recipe");

    const auto stepsField = root.find("steps");
    require(stepsField != root.end() && stepsField->is_array() && !stepsField->empty() &&
            stepsField->size() <= INT32_MAX,
            IR_BAD_ARGUMENT, "recipe.steps must be a nonempty array");

    std::set<std::string> ids;
    std::set<std::string> analysisNames;
    std::set<std::string> usedDevices;
    std::set<std::string> savedSubdirs;
    std::set<std::string> csvFiles;
    std::set<std::string> csvPaths;
    std::map<std::string, SlotKind> knownSlots;
    std::map<std::string, std::string> metricUnits;

    for (size_t index = 0; index < stepsField->size(); ++index) {
        const auto &fields = (*stepsField)[index];
        const auto path = "recipe.steps[" + std::to_string(index) + "]";
        require(fields.is_object(), IR_BAD_ARGUMENT, path + " must be an object");
        StepRef ref{static_cast<int32_t>(index), stringField(fields, "id", path)};
        require(ids.insert(ref.id).second, IR_BAD_ARGUMENT, path + ".id is duplicated");
        const auto type = stringField(fields, "type", path);

        if (type == "save_raw") {
            allowFields(fields, {"id", "type", "input", "subdir"}, path);
            const auto inputSlot = stringField(fields, "input", path);
            const auto foundSlot = knownSlots.find(inputSlot);
            require(foundSlot != knownSlots.end() && foundSlot->second == SlotKind::batch,
                    IR_BAD_ARGUMENT, path + ".input must reference a captured batch slot");
            const auto subdir = optionalString(fields, "subdir", "", path);
            if (!subdir.empty()) {
                validateRelativeSingleName(subdir, path + ".subdir");
                require(!reservedOutputName(subdir), IR_BAD_ARGUMENT,
                        path + ".subdir conflicts with a reserved output name");
            }
            require(savedSubdirs.insert(portableNameKey(subdir)).second, IR_BAD_ARGUMENT,
                    path + ": duplicate save_raw target directory '" + subdir + "'");
            recipe.steps.emplace_back(SaveRawStepV2{std::move(ref), inputSlot, subdir});
        } else if (type == "repeat_burst") {
            allowFields(fields, {"id", "type", "max_iterations", "duration_sec", "interval_sec",
                                 "csv_file", "record_metrics", "steps"}, path);
            RepeatBurstStep burst;
            burst.ref = std::move(ref);
            burst.maxIterations = optionalInteger(fields, "max_iterations", 0, 0, INT32_MAX, path);
            burst.durationSec = optionalNumber(fields, "duration_sec", 0.0, path);
            burst.intervalSec = optionalNumber(fields, "interval_sec", 0.0, path);
            require(burst.durationSec >= 0.0 && burst.intervalSec >= 0.0, IR_BAD_ARGUMENT,
                    path + ": duration_sec and interval_sec must be nonnegative");
            require(burst.maxIterations > 0 || burst.durationSec > 0.0, IR_BAD_ARGUMENT,
                    path + ": specify positive max_iterations or duration_sec");
            burst.csvFile = optionalString(fields, "csv_file", "burst_summary.csv", path);
            validateRelativeSingleName(burst.csvFile, path + ".csv_file");
            const auto csvKey = portableNameKey(burst.csvFile);
            require(csvKey.size() > 4 && csvKey.ends_with(".csv"), IR_BAD_ARGUMENT,
                    path + ".csv_file must use the .csv extension");
            require(!reservedOutputName(burst.csvFile), IR_BAD_ARGUMENT,
                    path + ".csv_file conflicts with a reserved output name");
            require(csvFiles.insert(csvKey).second, IR_BAD_ARGUMENT,
                    path + ": duplicated csv_file '" + burst.csvFile + "'");
            csvPaths.insert(csvKey);

            const auto innerIt = fields.find("steps");
            require(innerIt != fields.end() && innerIt->is_array() && !innerIt->empty() &&
                    innerIt->size() <= INT32_MAX,
                    IR_BAD_ARGUMENT, path + ".steps must be a nonempty array");

            std::vector<std::string> innerMetrics;
            for (size_t innerIdx = 0; innerIdx < innerIt->size(); ++innerIdx) {
                const auto &innerFields = (*innerIt)[innerIdx];
                const auto innerPath = path + ".steps[" + std::to_string(innerIdx) + "]";
                require(innerFields.is_object(), IR_BAD_ARGUMENT, innerPath + " must be an object");
                StepRef innerRef{static_cast<int32_t>(recipe.steps.size()),
                                 stringField(innerFields, "id", innerPath)};
                require(ids.insert(innerRef.id).second, IR_BAD_ARGUMENT,
                        innerPath + ".id is duplicated: " + innerRef.id);
                const auto innerType = stringField(innerFields, "type", innerPath);
                require(innerType != "save_raw" && innerType != "repeat_burst", IR_BAD_ARGUMENT,
                        innerPath + ".type '" + innerType + "' is not allowed inside repeat_burst");

                burst.steps.emplace_back(parseSharedControlOrCaptureOrFlushOrAnalyze<InnerStepV2>(
                    std::move(innerRef), innerType, innerFields, innerPath, station,
                    knownSlots, usedDevices, analysisNames, metricUnits, &innerMetrics, externalMetrics));
            }

            if (fields.contains("record_metrics")) {
                const auto &rm = fields.at("record_metrics");
                require(rm.is_array() && !rm.empty(), IR_BAD_ARGUMENT,
                        path + ".record_metrics must be a nonempty string array");
                std::set<std::string> innerSet(innerMetrics.begin(), innerMetrics.end());
                for (size_t ri = 0; ri < rm.size(); ++ri) {
                    require(rm[ri].is_string(), IR_BAD_ARGUMENT,
                            path + ".record_metrics[" + std::to_string(ri) + "] must be a string");
                    const auto mName = rm[ri].get<std::string>();
                    require(innerSet.contains(mName), IR_BAD_ARGUMENT,
                            path + ".record_metrics references unknown loop metric: " + mName);
                    burst.recordMetrics.push_back(mName);
                }
            } else {
                burst.recordMetrics = std::move(innerMetrics);
            }

            recipe.hasRepeatBurst = true;
            recipe.steps.emplace_back(std::move(burst));
        } else {
            recipe.steps.emplace_back(parseSharedControlOrCaptureOrFlushOrAnalyze<StepV2>(
                std::move(ref), type, fields, path, station,
                knownSlots, usedDevices, analysisNames, metricUnits, nullptr, externalMetrics));
        }
    }

    if (const auto safetyIt = root.find("safety"); safetyIt != root.end()) {
        require(safetyIt->is_object(), IR_BAD_ARGUMENT, "recipe.safety must be an object");
        allowFields(*safetyIt, {"blackbody_standby", "oven_standby"}, "recipe.safety");
        if (const auto bbIt = safetyIt->find("blackbody_standby"); bbIt != safetyIt->end()) {
            require(bbIt->is_array(), IR_BAD_ARGUMENT, "recipe.safety.blackbody_standby must be an array");
            for (size_t i = 0; i < bbIt->size(); ++i) {
                const auto &entry = (*bbIt)[i];
                const auto sPath = "recipe.safety.blackbody_standby[" + std::to_string(i) + "]";
                require(entry.is_object(), IR_BAD_ARGUMENT, sPath + " must be an object");
                allowFields(entry, {"device", "target_c"}, sPath);
                const auto alias = stringField(entry, "device", sPath);
                const auto devIt = station.devices.find(alias);
                require(devIt != station.devices.end() &&
                        (devIt->second.kind == IR_PCN7LT || devIt->second.kind == IR_DCN1000),
                        IR_BAD_ARGUMENT, sPath + ".device must name a configured blackbody");
                const double targetC = numberField(entry, "target_c", sPath);
                require(targetC >= -60.0 && targetC <= 100.0, IR_BAD_ARGUMENT,
                        sPath + ".target_c must be within [-60, 100]");
                usedDevices.insert(alias);
                recipe.standbyBlackbodies.push_back({alias, targetC});
            }
        }
        if (const auto ovIt = safetyIt->find("oven_standby"); ovIt != safetyIt->end()) {
            require(ovIt->is_array(), IR_BAD_ARGUMENT, "recipe.safety.oven_standby must be an array");
            for (size_t i = 0; i < ovIt->size(); ++i) {
                const auto &entry = (*ovIt)[i];
                const auto sPath = "recipe.safety.oven_standby[" + std::to_string(i) + "]";
                require(entry.is_string(), IR_BAD_ARGUMENT, sPath + " must be a string device alias");
                const auto alias = entry.get<std::string>();
                requireDeviceKind(station, alias, IR_OVEN_GWS_STEN, sPath, "gws-sten oven");
                usedDevices.insert(alias);
                if (std::find(recipe.standbyOvens.begin(), recipe.standbyOvens.end(), alias) == recipe.standbyOvens.end())
                    recipe.standbyOvens.push_back(alias);
            }
        }
    }

    if (const auto limitsIt = root.find("limits"); limitsIt != root.end()) {
        require(limitsIt->is_array(), IR_BAD_ARGUMENT, "recipe.limits must be an array");
        for (size_t index = 0; index < limitsIt->size(); ++index) {
            const auto &limit = (*limitsIt)[index];
            const auto path = "recipe.limits[" + std::to_string(index) + "]";
            require(limit.is_object(), IR_BAD_ARGUMENT, path + " must be an object");
            allowFields(limit, {"metric", "operator", "value", "unit"}, path);
            auto metric = stringField(limit, "metric", path);
            const auto found = metricUnits.find(metric);
            require(found != metricUnits.end(), IR_BAD_ARGUMENT,
                    path + ".metric is not produced by any analyze step: " + metric);
            const auto operation = stringField(limit, "operator", path);
            require(operation == "max" || operation == "min", IR_BAD_ARGUMENT,
                    path + ".operator must be max or min");
            auto unit = stringField(limit, "unit", path);
            require(unit == found->second, IR_BAD_ARGUMENT,
                    path + ".unit does not match metric unit '" + found->second + "'");
            recipe.limits.push_back({std::move(metric), std::move(unit), operation == "max",
                                     numberField(limit, "value", path)});
        }
    }

    recipe.deviceAliases.assign(usedDevices.begin(), usedDevices.end());
    for (const auto &subdir : savedSubdirs)
        require(!csvPaths.contains(subdir), IR_BAD_ARGUMENT,
                "A save_raw subdir conflicts with a repeat_burst csv_file");
    for (const auto &[slot, kind] : knownSlots) {
        (void)slot;
        if (kind == SlotKind::double_map) ++recipe.doubleMapSlotCount;
        else if (kind == SlotKind::byte_mask) ++recipe.byteMaskSlotCount;
        else if (kind == SlotKind::drift_context) ++recipe.driftContextSlotCount;
    }
    recipe.definition = std::move(root);
    return recipe;
}

struct V2RuntimeState {
    ir::acquisition::Capture &capture;
    const StationConfig &station;
    const RecipeConfigV2 &recipe;
    const fs::path &destination;
    RecipeProgress &progress;
    bool sourceOpened{false};
    bool sourceStarted{false};
    std::map<std::string, IRHandle> devices;
    std::optional<ir::instruments::DeviceLease> lease;
    std::vector<const PowerStep *> powerToSwitchOff;
    std::vector<std::string> ovensToStandby;
    ExecutionContext ctx;
    V2Readbacks readbacks;
    std::vector<SavedBatchSnapshot> savedSnapshots;
    uint64_t savedSnapshotBytes{0};
    std::vector<Json> burstRunsSummary;
    Json stepRecords = Json::array();
    bool destinationCreated{false};
    bool freshFramesRequired{false};

    void ensureCaptureStarted()
    {
        if (!sourceStarted) {
            capture.start();
            sourceStarted = true;
        }
    }

    void clearPendingFrames()
    {
        if (!freshFramesRequired || !capture.source || !capture.source->isLive())
            return;
        int32_t discarded = 0;
        capture.flush(0, 0, discarded);
        freshFramesRequired = false;
    }
};

void preflightRecipeV2(V2RuntimeState &rt)
{
    rt.progress.enter(IR_RECIPE_STAGE_PREPARE);
    rt.ctx.geometry = rt.capture.open(rt.station.backend, rt.station.source.c_str(),
                                      rt.station.validBits, rt.station.queueCapacity,
                                      rt.station.maxInflightBytes);
    rt.sourceOpened = true;
    rt.capture.checkStop();

    const int64_t maxBatchElements = rt.station.maxBatchBytes / int64_t(sizeof(uint16_t));
    for (const auto &step : rt.recipe.steps) {
        std::visit([&](const auto &s) {
            using T = std::decay_t<decltype(s)>;
            rt.progress.enter(IR_RECIPE_STAGE_PREPARE, &s.ref);
            if constexpr (std::is_same_v<T, CaptureStepV2>) {
                const auto elements = stackSize(
                    pixels(rt.ctx.geometry.height, rt.ctx.geometry.width), s.frames);
                require(elements <= maxBatchElements, IR_BAD_ARGUMENT,
                        "Frame batch exceeds station.acquisition.max_batch_bytes");
            } else if constexpr (std::is_same_v<T, AnalysisStepV2>) {
                if (s.operations.checkGeometry)
                    s.operations.checkGeometry(rt.ctx.geometry);
            } else if constexpr (std::is_same_v<T, RepeatBurstStep>) {
                for (const auto &inner : s.steps) {
                    std::visit([&](const auto &is) {
                        rt.progress.enter(IR_RECIPE_STAGE_PREPARE, &is.ref);
                        validateInnerGeometry(inner, rt.ctx.geometry, maxBatchElements);
                    }, inner);
                }
            }
        }, step);
    }
    uint64_t requiredStorage = rt.capture.bufferedBytes();
    requiredStorage = addBytes(requiredStorage,
                               retainedRecipeStorage(rt.recipe, rt.ctx.geometry, rt.station.maxBatchBytes));
    requireWithinMemoryLimit(requiredStorage, rt.station.maxInflightBytes);
    rt.capture.checkStop();
}

void openAndLeaseDevicesV2(V2RuntimeState &rt)
{
    rt.progress.enter(IR_RECIPE_STAGE_RESERVE);
    std::vector<IRHandle> handles;
    handles.reserve(rt.recipe.deviceAliases.size());
    for (const auto &alias : rt.recipe.deviceAliases) {
        rt.capture.checkStop();
        const auto &cfg = rt.station.devices.at(alias);
        auto &handle = rt.devices[alias];
        char error[1024]{};
        const auto st = IR_OpenDevice(cfg.kind, cfg.connectionJson.c_str(), &handle, error, sizeof error);
        if (st != IR_OK)
            throw Error(st, "Cannot open device '" + alias + "': " + error);
        handles.push_back(handle);
    }
    rt.capture.checkStop();
    rt.lease.emplace(handles.data(), handles.size());
}

void executeCommonStepV2(V2RuntimeState &rt, const auto &s)
{
    using T = std::decay_t<decltype(s)>;
    if constexpr (std::is_same_v<T, BlackbodyStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlBlackbody(rt.capture, rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, PowerStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlPower(rt.devices.at(s.device), s, rt.readbacks, rt.powerToSwitchOff);
        if (s.operation != PowerStep::Operation::read)
            rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, PowerWaitStableStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlPowerWaitStable(rt.capture, rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, OvenStabilizeStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlOvenStabilize(rt.capture, rt.devices.at(s.device), s, rt.readbacks, rt.ovensToStandby);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, OvenStandbyStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlOvenStandby(rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, DmmReadStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlDmmRead(rt.devices.at(s.device), s, rt.readbacks, rt.ctx);
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, DetectorTimingStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlDetectorTiming(rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, DetectorGpolStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlDetectorGpol(rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, DetectorRegisterStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlDetectorRegister(rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, DetectorCurrentStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlDetectorCurrent(rt.devices.at(s.device), s, rt.readbacks, rt.ctx);
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, CoolerReadStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlCoolerRead(rt.devices.at(s.device), s, rt.readbacks, rt.ctx);
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, FlushStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &s.ref);
        rt.capture.checkStop();
        rt.ensureCaptureStarted();
        int32_t discarded = 0;
        rt.capture.flush(s.discardFrames, s.timeoutMs, discarded);
        rt.freshFramesRequired = false;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, CaptureStepV2>) {
        rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &s.ref);
        rt.capture.checkStop();
        rt.ensureCaptureStarted();
        rt.clearPendingFrames();
        const int64_t elements = stackSize(
            pixels(rt.ctx.geometry.height, rt.ctx.geometry.width), s.frames);
        auto &batch = rt.ctx.batches[s.outputSlot];
        batch.geometry = rt.ctx.geometry;
        batch.pixels.resize(static_cast<size_t>(elements));
        batch.stamps.resize(static_cast<size_t>(s.frames));
        int32_t stepCompleted = 0;
        try {
            rt.capture.read(s.frames, s.timeoutMs, batch.pixels.data(), elements,
                            batch.stamps.data(), s.frames, stepCompleted);
        } catch (...) {
            rt.progress.result.completed_frames += stepCompleted;
            throw;
        }
        rt.progress.result.completed_frames += stepCompleted;
        rt.capture.checkStop();
        requireWithinMemoryLimit(addBytes(
            addBytes(rt.capture.bufferedBytes(), rt.ctx.retainedBytes()), rt.savedSnapshotBytes),
            rt.station.maxInflightBytes);
    } else if constexpr (std::is_same_v<T, AnalysisStepV2>) {
        rt.progress.enter(IR_RECIPE_STAGE_ANALYZE, &s.ref);
        rt.capture.checkStop();
        AnalysisRunContext runCtx{
            rt.ctx,
            rt.capture,
            addBytes(rt.capture.bufferedBytes(), rt.savedSnapshotBytes),
            rt.station.maxInflightBytes
        };
        s.operations.execute(runCtx);
        rt.capture.checkStop();
        requireWithinMemoryLimit(addBytes(
            addBytes(rt.capture.bufferedBytes(), rt.ctx.retainedBytes()), rt.savedSnapshotBytes),
            rt.station.maxInflightBytes);
    }
}

int32_t executeRepeatBurstStepV2(V2RuntimeState &rt, const RepeatBurstStep &rb)
{
    rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &rb.ref);
    const auto loopStart = std::chrono::steady_clock::now();
    std::ofstream csvStream;
    int32_t iterationsDone = 0;
    for (;;) {
        rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &rb.ref);
        rt.capture.checkStop();
        if (rb.maxIterations > 0 && iterationsDone >= rb.maxIterations)
            break;
        const double elapsedBefore = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - loopStart).count();
        if (rb.maxIterations == 0 && iterationsDone > 0 && elapsedBefore >= rb.durationSec)
            break;
        if (rb.durationSec > 0.0 && iterationsDone > 0 && elapsedBefore >= rb.durationSec)
            break;

        for (const auto &inner : rb.steps) {
            std::visit([&](const auto &s) { executeCommonStepV2(rt, s); }, inner);
        }

        rt.progress.enter(IR_RECIPE_STAGE_ARCHIVE, &rb.ref);
        if (!csvStream.is_open()) {
            if (!rt.destinationCreated) {
                std::error_code createError;
                const bool created = fs::create_directory(rt.destination, createError);
                require(created && !createError, IR_IO_ERROR,
                        "Cannot exclusively create the recipe output directory");
                rt.destinationCreated = true;
            }
            const fs::path csvPath = rt.destination / pathFromUtf8(rb.csvFile);
            csvStream.open(csvPath, std::ios::binary | std::ios::out);
            require(csvStream.is_open(), IR_IO_ERROR,
                    "Cannot open burst summary CSV: " + rb.csvFile);
            csvStream << "iteration,timestamp_utc,elapsed_s";
            for (const auto &col : rb.recordMetrics)
                csvStream << ',' << col;
            csvStream << '\n';
        }
        const double iterElapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - loopStart).count();
        csvStream << iterationsDone << ',' << utcTimestampNow() << ','
                  << formatDoubleClassic(iterElapsed);
        for (const auto &col : rb.recordMetrics) {
            const auto itVal = rt.ctx.scalarValues.find(col);
            require(itVal != rt.ctx.scalarValues.end(), IR_COMPUTATION_FAILED,
                    "repeat_burst record metric missing: " + col);
            csvStream << ',' << formatDoubleClassic(itVal->second);
        }
        csvStream << '\n';
        csvStream.flush();
        require(csvStream.good(), IR_IO_ERROR,
                "Failed flushing burst summary CSV: " + rb.csvFile);
        ++iterationsDone;
        if (rt.capture.source && rt.capture.source->isLive())
            rt.freshFramesRequired = true;

        const bool moreByCount = (rb.maxIterations == 0) || (iterationsDone < rb.maxIterations);
        const double elapsedAfter = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - loopStart).count();
        const bool moreByTime = (rb.durationSec <= 0.0) || (elapsedAfter < rb.durationSec);
        if (!moreByCount || !moreByTime)
            break;

        if (rb.intervalSec > 0.0) {
            rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &rb.ref);
            const auto sleepEnd = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(rb.intervalSec));
            while (std::chrono::steady_clock::now() < sleepEnd) {
                rt.capture.checkStop();
                const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                    sleepEnd - std::chrono::steady_clock::now()).count();
                if (rem <= 0) break;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::min<int64_t>(rem, 50)));
            }
        }
    }
    rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &rb.ref);
    rt.burstRunsSummary.push_back({
        {"id", rb.ref.id},
        {"csv_file", rb.csvFile},
        {"iterations", iterationsDone}
    });
    return iterationsDone;
}

void performSafetyCleanupV2(V2RuntimeState &rt, std::exception_ptr failure)
{
    RecipeCleanupFailure cleanup;
    if (rt.sourceOpened)
        cleanup.attempt(nullptr, [&] { rt.capture.close(); });
    for (auto it = rt.powerToSwitchOff.rbegin(); it != rt.powerToSwitchOff.rend(); ++it) {
        const auto &step = **it;
        const auto itDev = rt.devices.find(step.device);
        if (itDev == rt.devices.end() || !itDev->second)
            continue;
        cleanup.attempt(&step.ref, [&] {
            char error[1024]{};
            IRPowerReading reading{};
            checkStatus(IR_SetPowerOutput(itDev->second, step.channel, 0, &reading,
                                          error, sizeof error), error);
        });
    }
    if (rt.lease.has_value()) {
        for (const auto &sb : rt.recipe.standbyBlackbodies) {
            const auto itDev = rt.devices.find(sb.device);
            if (itDev == rt.devices.end() || !itDev->second)
                continue;
            cleanup.attempt(nullptr, [&] {
                char error[1024]{};
                double readback{};
                checkStatus(IR_SetBlackbody(itDev->second, sb.targetC, &readback,
                                            error, sizeof error), error);
            });
        }
        std::vector<std::string> uniqueStandbyOvens = rt.ovensToStandby;
        for (const auto &ov : rt.recipe.standbyOvens) {
            if (std::find(uniqueStandbyOvens.begin(), uniqueStandbyOvens.end(), ov) == uniqueStandbyOvens.end())
                uniqueStandbyOvens.push_back(ov);
        }
        for (auto it = uniqueStandbyOvens.rbegin(); it != uniqueStandbyOvens.rend(); ++it) {
            const auto itDev = rt.devices.find(*it);
            if (itDev == rt.devices.end() || !itDev->second)
                continue;
            cleanup.attempt(nullptr, [&] {
                char error[1024]{};
                checkStatus(IR_StopOven(itDev->second, error, sizeof error), error);
            });
        }
    }
    rt.lease.reset();
    for (auto it = rt.devices.rbegin(); it != rt.devices.rend(); ++it) {
        if (!it->second) continue;
        cleanup.attempt(nullptr, [&] {
            char error[1024]{};
            checkStatus(IR_CloseDevice(it->second, error, sizeof error), error);
        });
    }
    rt.progress.result.cleanup_status = cleanup.status;
    if (failure) std::rethrow_exception(failure);
    if (cleanup.status != IR_OK) {
        rt.progress.enter(IR_RECIPE_STAGE_CLEANUP, cleanup.step);
        throw Error(cleanup.status, cleanup.detail);
    }
}

void archiveRecipeV2Result(V2RuntimeState &rt, const std::string &startedUtc)
{
    rt.progress.enter(IR_RECIPE_STAGE_ARCHIVE);
    rt.capture.checkStop();

    bool limitsPass = true;
    Json evaluatedLimits = Json::array();
    for (const auto &limit : rt.recipe.limits) {
        const auto itVal = rt.ctx.scalarValues.find(limit.metric);
        require(itVal != rt.ctx.scalarValues.end(), IR_COMPUTATION_FAILED,
                "Limit metric was not computed: " + limit.metric);
        const double actual = itVal->second;
        const bool pass = limit.isMaximum ? (actual <= limit.threshold) : (actual >= limit.threshold);
        limitsPass &= pass;
        evaluatedLimits.push_back({
            {"metric", limit.metric},
            {"operator", limit.isMaximum ? "max" : "min"},
            {"value", limit.threshold},
            {"unit", limit.unit},
            {"actual", actual},
            {"result", pass ? "PASS" : "NG"}
        });
    }
    const char *runStatus = rt.recipe.limits.empty() ? "COMPLETED" : (limitsPass ? "PASS" : "NG");

    Json metricsJson = Json::object();
    for (const auto &[key, val] : rt.ctx.metrics)
        metricsJson[key] = val;

    Json savedBatchesMeta = Json::array();
    std::vector<SavedBatchRef> batchRefs;
    batchRefs.reserve(rt.savedSnapshots.size());
    for (const auto &snap : rt.savedSnapshots) {
        batchRefs.push_back({snap.subdir, snap.batch.geometry,
                             &snap.batch.pixels, &snap.batch.stamps});
        savedBatchesMeta.push_back({
            {"input", snap.inputSlot},
            {"subdir", snap.subdir},
            {"frames", snap.batch.stamps.size()},
            {"width", snap.batch.geometry.width},
            {"height", snap.batch.geometry.height}
        });
    }

    const Json record{
        {"schemav", 1},
        {"recipe_id", rt.recipe.id},
        {"recipe", rt.recipe.definition},
        {"run_status", runStatus},
        {"backend", backendName(rt.station.backend)},
        {"source_name", rt.station.source},
        {"width", rt.ctx.geometry.width},
        {"height", rt.ctx.geometry.height},
        {"valid_bits", rt.ctx.geometry.valid_bits},
        {"completed_frames", rt.progress.result.completed_frames},
        {"cleanup_status", rt.progress.result.cleanup_status},
        {"raw_saved", !rt.savedSnapshots.empty()},
        {"saved_batches", std::move(savedBatchesMeta)},
        {"burst_runs", std::move(rt.burstRunsSummary)},
        {"step_records", std::move(rt.stepRecords)},
        {"station_version", IR_StationVersion()},
        {"analysis_version", IR_AnalysisVersion()},
        {"source_commit", ir::station::sourceCommit()},
        {"started_utc", startedUtc},
        {"finished_utc", utcTimestampNow()},
        {"conditions", rt.readbacks.conditions},
        {"instrument_readings", rt.readbacks.instruments},
        {"metrics", std::move(metricsJson)},
        {"limits", std::move(evaluatedLimits)}
    };
    const auto text = record.dump(2) + '\n';

    if (!rt.destinationCreated) {
        saveRecipeArtifacts(rt.destination, batchRefs, text);
    } else {
        for (const auto &ref : batchRefs) {
            const auto targetDir = ref.subdir.empty()
                ? rt.destination
                : (rt.destination / pathFromUtf8(ref.subdir));
            writeFrameBatchFiles(targetDir, ref.geometry, *ref.frames, *ref.stamps);
        }
        writeTextFileAtomicOrDirect(rt.destination / "result.json", text);
    }
    rt.progress.enter(IR_RECIPE_STAGE_NONE);
}

void runRecipeV2(ir::acquisition::Capture &capture, const StationConfig &station,
                 const RecipeConfigV2 &recipe, const char *directory, IRRecipeResult &result,
                 const IRRecipeRunOptions *options)
{
    require(directory && *directory, IR_BAD_ARGUMENT, "Specify a new output directory");
    const auto destination = pathFromUtf8(directory);
    checkDestination(destination);
    capture.checkStop();
    const auto startedUtc = utcTimestampNow();

    RecipeProgress progress{result};
    V2RuntimeState rt{capture, station, recipe, destination, progress};
    std::exception_ptr failure;

    try {
        try {
            preflightRecipeV2(rt);
            openAndLeaseDevicesV2(rt);

            for (const auto &step : recipe.steps) {
                const TopStepInfo sInfo = topStepInfo(step);
                const std::string stepStartUtc = utcTimestampNow();
                const auto stepStartSteady = std::chrono::steady_clock::now();
                emitRecipeStepEvent(options, IR_STEP_EVENT_STARTED, *sInfo.ref,
                                    sInfo.initialStage, IR_OK, 0.0, sInfo.typeName);
                int32_t burstIterationsForRecord = -1;
                try {
                    if (const auto *sr = std::get_if<SaveRawStepV2>(&step)) {
                        rt.progress.enter(IR_RECIPE_STAGE_ARCHIVE, &sr->ref);
                        rt.capture.checkStop();
                        const auto &batch = rt.ctx.batches.at(sr->inputSlot);
                        const uint64_t snapshotBytes = batchStorageBytes(
                            batch.geometry, static_cast<int32_t>(batch.stamps.size()),
                            station.maxBatchBytes);
                        const uint64_t nextSavedSnapshotBytes = addBytes(
                            rt.savedSnapshotBytes, snapshotBytes);
                        const uint64_t requiredStorage = addBytes(
                            addBytes(rt.capture.bufferedBytes(), rt.ctx.retainedBytes()),
                            nextSavedSnapshotBytes);
                        requireWithinMemoryLimit(requiredStorage, station.maxInflightBytes);
                        rt.savedSnapshots.push_back({sr->inputSlot, sr->subdir, batch});
                        rt.savedSnapshotBytes = nextSavedSnapshotBytes;
                    } else if (const auto *rb = std::get_if<RepeatBurstStep>(&step)) {
                        burstIterationsForRecord = executeRepeatBurstStepV2(rt, *rb);
                    } else {
                        std::visit([&](const auto &s) { executeCommonStepV2(rt, s); }, step);
                    }
                } catch (const Error &e) {
                    emitRecipeStepEvent(options, IR_STEP_EVENT_FAILED, *sInfo.ref,
                                        result.failed_stage, e.code, elapsedMsSince(stepStartSteady),
                                        sInfo.typeName, e.what());
                    throw;
                } catch (const std::exception &e) {
                    emitRecipeStepEvent(options, IR_STEP_EVENT_FAILED, *sInfo.ref,
                                        result.failed_stage, exceptionToStatus(e),
                                        elapsedMsSince(stepStartSteady), sInfo.typeName, e.what());
                    throw;
                } catch (...) {
                    emitRecipeStepEvent(options, IR_STEP_EVENT_FAILED, *sInfo.ref,
                                        result.failed_stage, IR_COMPUTATION_FAILED,
                                        elapsedMsSince(stepStartSteady), sInfo.typeName, "Unknown error");
                    throw;
                }
                const double durationMs = elapsedMsSince(stepStartSteady);
                const int32_t finalStage = (burstIterationsForRecord >= 0)
                    ? sInfo.initialStage
                    : result.failed_stage;
                Json rec{
                    {"index", sInfo.ref->index},
                    {"id", sInfo.ref->id},
                    {"type", sInfo.typeName},
                    {"stage", finalStage},
                    {"started_utc", stepStartUtc},
                    {"finished_utc", utcTimestampNow()},
                    {"duration_ms", durationMs},
                    {"status", "OK"}
                };
                if (burstIterationsForRecord >= 0)
                    rec["iterations"] = burstIterationsForRecord;
                rt.stepRecords.push_back(std::move(rec));
                emitRecipeStepEvent(options, IR_STEP_EVENT_COMPLETED, *sInfo.ref,
                                    finalStage, IR_OK, durationMs, sInfo.typeName);
            }
        } catch (...) {
            failure = std::current_exception();
        }

        performSafetyCleanupV2(rt, failure);
        archiveRecipeV2Result(rt, startedUtc);
    } catch (const Error &error) {
        if (progress.step)
            throw Error(error.code, "Step '" + progress.step->id + "' failed: " + error.what());
        throw;
    }
}

} // namespace ir::procedures::recipe
