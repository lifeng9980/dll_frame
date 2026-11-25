#include "recipe_config.h"
#include "IR_device.h"
#include "api_common.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ir::procedures::recipe {
using namespace ir::api;


uint64_t addBytes(uint64_t total, uint64_t additional)
{
    require(additional <= std::numeric_limits<uint64_t>::max() - total,
            IR_BAD_ARGUMENT, "Estimated capture memory overflows");
    return total + additional;
}

uint64_t multiplyBytes(uint64_t count, uint64_t itemBytes)
{
    require(itemBytes == 0 || count <= std::numeric_limits<uint64_t>::max() / itemBytes,
            IR_BAD_ARGUMENT, "Estimated capture memory overflows");
    return count * itemBytes;
}

uint64_t batchStorageBytes(IRGeometry geometry, int32_t frames, int64_t maxBatchBytes)
{
    const uint64_t pixelsPerFrame = static_cast<uint64_t>(pixels(geometry.height, geometry.width));
    const uint64_t pixelElements = multiplyBytes(pixelsPerFrame, static_cast<uint64_t>(frames));
    const uint64_t pixelBytes = multiplyBytes(pixelElements, sizeof(uint16_t));
    require(pixelBytes <= static_cast<uint64_t>(maxBatchBytes), IR_BAD_ARGUMENT,
            "Frame batch exceeds station.acquisition.max_batch_bytes");
    const uint64_t stampBytes = multiplyBytes(static_cast<uint64_t>(frames), sizeof(IRFrameStamp));
    return addBytes(pixelBytes, stampBytes);
}

void requireWithinMemoryLimit(uint64_t required, uint64_t limit)
{
    if (required <= limit)
        return;
    throw Error(IR_BAD_ARGUMENT, "Estimated SDK frame storage " + std::to_string(required) +
                " bytes exceeds station.acquisition.max_inflight_bytes " +
                std::to_string(limit) + " bytes");
}

const Json &objectField(const Json &object, const char *key, const std::string &path)
{
    const auto found = object.find(key);
    require(found != object.end() && found->is_object(), IR_BAD_ARGUMENT,
            path + "." + key + " must be an object");
    return *found;
}

std::string stringField(const Json &object, const char *key, const std::string &path)
{
    const auto found = object.find(key);
    require(found != object.end() && found->is_string(), IR_BAD_ARGUMENT,
            path + "." + key + " must be a string");
    auto value = found->get<std::string>();
    require(!value.empty() && value.find('\0') == std::string::npos, IR_BAD_ARGUMENT,
            path + "." + key + " must be nonempty UTF-8 text without NUL");
    return value;
}

std::string optionalString(const Json &object, const char *key, const std::string &fallback,
                          const std::string &path)
{
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    require(found->is_string(), IR_BAD_ARGUMENT, path + "." + key + " must be a string");
    const auto value = found->get<std::string>();
    require(value.find('\0') == std::string::npos, IR_BAD_ARGUMENT,
            path + "." + key + " contains NUL");
    return value;
}

double numberField(const Json &object, const char *key, const std::string &path)
{
    const auto found = object.find(key);
    require(found != object.end() && found->is_number(), IR_BAD_ARGUMENT,
            path + "." + key + " must be a number");
    const auto value = found->get<double>();
    require(std::isfinite(value), IR_BAD_ARGUMENT, path + "." + key + " must be finite");
    return value;
}

double optionalNumber(const Json &object, const char *key, double fallback, const std::string &path)
{
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    return numberField(object, key, path);
}

int integerField(const Json &object, const char *key, int low, int high, const std::string &path)
{
    const auto value = numberField(object, key, path);
    require(value >= low && value <= high && std::trunc(value) == value,
            IR_BAD_ARGUMENT, path + "." + key + " is outside its integer range");
    return static_cast<int>(value);
}

int optionalInteger(const Json &object, const char *key, int fallback, int low, int high,
                    const std::string &path)
{
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    return integerField(object, key, low, high, path);
}

int64_t optionalBytes(const Json &object, const char *key, int64_t fallback, const std::string &path)
{
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    require(found->is_number_integer() || found->is_number_unsigned(), IR_BAD_ARGUMENT,
            path + "." + key + " must be an integer");
    if (found->is_number_unsigned()) {
        const auto value = found->get<uint64_t>();
        require(value >= 2 && value <= uint64_t(INT64_MAX), IR_BAD_ARGUMENT,
                path + "." + key + " is outside its integer range");
        return static_cast<int64_t>(value);
    }
    const auto value = found->get<int64_t>();
    require(value >= 2, IR_BAD_ARGUMENT, path + "." + key + " is outside its integer range");
    return value;
}

bool booleanField(const Json &object, const char *key, const std::string &path)
{
    const auto found = object.find(key);
    require(found != object.end() && found->is_boolean(), IR_BAD_ARGUMENT,
            path + "." + key + " must be true or false");
    return found->get<bool>();
}

bool optionalBoolean(const Json &object, const char *key, bool fallback, const std::string &path)
{
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    return booleanField(object, key, path);
}

void allowFields(const Json &object, std::initializer_list<const char *> allowed, const std::string &path)
{
    for (auto field = object.begin(); field != object.end(); ++field) {
        const auto known = std::find_if(allowed.begin(), allowed.end(), [&](const char *name) {
            return field.key() == name;
        });
        require(known != allowed.end(), IR_BAD_ARGUMENT, path + " has unknown field: " + field.key());
    }
}

Json parseJson(const char *text, const char *label)
{
    require(text && *text, IR_BAD_ARGUMENT, std::string(label) + " is empty");
    try {
        return Json::parse(text);
    } catch (const Json::parse_error &) {
        throw Error(IR_BAD_ARGUMENT, std::string(label) + " contains invalid JSON");
    }
}

StationConfig parseStation(const char *text)
{
    const auto root = parseJson(text, "station JSON");
    require(root.is_object(), IR_BAD_ARGUMENT, "station JSON must contain an object");
    allowFields(root, {"schemav", "acquisition", "devices", "plugins"}, "station");
    (void)optionalInteger(root, "schemav", 1, 1, 1, "station");

    const auto &acquisition = objectField(root, "acquisition", "station");
    allowFields(acquisition, {"backend", "source", "valid_bits", "max_batch_bytes", "max_inflight_bytes", "queue_capacity"},
                "station.acquisition");
    const auto backend = stringField(acquisition, "backend", "station.acquisition");
    StationConfig station;
    if (backend == "raw_replay") station.backend = IR_RAW_REPLAY;
    else if (backend == "directshow") {
#ifdef _WIN32
        station.backend = IR_DIRECTSHOW;
#else
        throw Error(IR_UNSUPPORTED, "station.acquisition.backend directshow is not available on this platform");
#endif
    } else if (backend == "v4l2") {
#if defined(__linux__)
        station.backend = IR_V4L2;
#else
        throw Error(IR_UNSUPPORTED, "station.acquisition.backend v4l2 is not available on this platform");
#endif
    } else throw Error(IR_BAD_ARGUMENT, "station.acquisition.backend is unsupported");
    station.source = stringField(acquisition, "source", "station.acquisition");
    station.validBits = integerField(acquisition, "valid_bits", 0, 16, "station.acquisition");
    station.maxBatchBytes = optionalBytes(acquisition, "max_batch_bytes", station.maxBatchBytes,
                                          "station.acquisition");
    station.maxInflightBytes = static_cast<uint64_t>(optionalBytes(
        acquisition, "max_inflight_bytes", static_cast<int64_t>(station.maxInflightBytes),
        "station.acquisition"));
    station.queueCapacity = optionalInteger(acquisition, "queue_capacity", station.queueCapacity,
                                            2, 4096, "station.acquisition");

    if (const auto pluginsField = root.find("plugins"); pluginsField != root.end()) {
        require(pluginsField->is_array(), IR_BAD_ARGUMENT, "station.plugins must be an array");
        station.pluginPaths.reserve(pluginsField->size());
        for (size_t i = 0; i < pluginsField->size(); ++i) {
            const auto pPath = "station.plugins[" + std::to_string(i) + "]";
            require((*pluginsField)[i].is_string(), IR_BAD_ARGUMENT, pPath + " must be a string");
            auto val = (*pluginsField)[i].get<std::string>();
            require(!val.empty() && val.find('\0') == std::string::npos, IR_BAD_ARGUMENT,
                    pPath + " must be nonempty UTF-8 text without NUL");
            station.pluginPaths.push_back(std::move(val));
        }
    }

    const auto devicesField = root.find("devices");
    if (devicesField == root.end()) return station;
    require(devicesField->is_object(), IR_BAD_ARGUMENT, "station.devices must be an object");
    for (auto device = devicesField->begin(); device != devicesField->end(); ++device) {
        const auto path = "station.devices." + device.key();
        require(device.value().is_object(), IR_BAD_ARGUMENT, path + " must be an object");
        allowFields(device.value(), {"kind", "connection"}, path);
        const auto kind = stringField(device.value(), "kind", path);
        DeviceConfig config;
        if (kind == "pcn7-lt") config.kind = IR_PCN7LT;
        else if (kind == "dcn1000") config.kind = IR_DCN1000;
        else if (kind == "it6332a") config.kind = IR_IT6332A;
        else if (kind == "gws-sten") config.kind = IR_OVEN_GWS_STEN;
        else if (kind == "dmm6500") config.kind = IR_DMM6500;
        else if (kind == "detector-serial") config.kind = IR_DETECTOR_SERIAL;
        else if (kind == "cooler-modbus") config.kind = IR_COOLER_MODBUS;
        else throw Error(IR_BAD_ARGUMENT, path + ".kind is unsupported");
        const auto &connection = objectField(device.value(), "connection", path);
        config.connectionJson = connection.dump();
        const auto validateTcpOrSerial = [&](std::vector<const char *> extraFields, int defaultTcpPort, int defaultBaud) {
            const std::string connPath = path + ".connection";
            const bool hasHost = connection.contains("host");
            const bool hasPort = connection.contains("port");
            require(hasHost != hasPort, IR_BAD_ARGUMENT,
                    connPath + " must specify either 'host' or 'port', not both");
            std::vector<const char *> allowed;
            if (hasHost) {
                allowed = {"host", "tcp_port", "timeout_ms"};
                allowed.insert(allowed.end(), extraFields.begin(), extraFields.end());
                for (auto it = connection.begin(); it != connection.end(); ++it) {
                    require(std::any_of(allowed.begin(), allowed.end(), [&](const char *k) { return it.key() == k; }),
                            IR_BAD_ARGUMENT, connPath + " has unknown or serial-only field: " + it.key());
                }
                (void)stringField(connection, "host", connPath);
                (void)optionalInteger(connection, "tcp_port", defaultTcpPort, 1, 65535, connPath);
            } else {
                allowed = {"port", "baud_rate", "data_bits", "parity", "stop_bits", "timeout_ms"};
                allowed.insert(allowed.end(), extraFields.begin(), extraFields.end());
                for (auto it = connection.begin(); it != connection.end(); ++it) {
                    require(std::any_of(allowed.begin(), allowed.end(), [&](const char *k) { return it.key() == k; }),
                            IR_BAD_ARGUMENT, connPath + " has unknown or TCP-only field: " + it.key());
                }
                (void)stringField(connection, "port", connPath);
                (void)optionalInteger(connection, "baud_rate", defaultBaud, 1200, 921600, connPath);
                (void)optionalInteger(connection, "data_bits", 8, 5, 8, connPath);
                const auto pText = optionalString(connection, "parity", "none", connPath);
                require(pText == "none" || pText == "odd" || pText == "even",
                        IR_BAD_ARGUMENT, connPath + ".parity must be none, odd, or even");
                (void)optionalInteger(connection, "stop_bits", 1, 1, 2, connPath);
            }
            (void)optionalInteger(connection, "timeout_ms", 2000, 1, 60000, connPath);
        };
        if (config.kind == IR_PCN7LT) {
            allowFields(connection, {"host", "tcp_port", "head", "expected_head_serial", "timeout_ms"},
                        path + ".connection");
            (void)stringField(connection, "host", path + ".connection");
            (void)optionalInteger(connection, "tcp_port", 2101, 1, 65535, path + ".connection");
            (void)optionalInteger(connection, "head", -1, -1, 1, path + ".connection");
            (void)optionalInteger(connection, "timeout_ms", 3000, 1, 60000, path + ".connection");
            const auto expected = optionalString(connection, "expected_head_serial", "",
                                                 path + ".connection");
            const auto head = connection.contains("head")
                ? integerField(connection, "head", -1, 1, path + ".connection") : -1;
            require(expected.empty() || head >= 0, IR_BAD_ARGUMENT,
                    path + ".connection.expected_head_serial requires a head index");
        } else if (config.kind == IR_IT6332A) {
            validateTcpOrSerial({}, 5026, 115200);
        } else if (config.kind == IR_OVEN_GWS_STEN) {
            validateTcpOrSerial({"address", "delimiter", "min_setpoint_c", "max_setpoint_c"}, 5000, 9600);
            (void)optionalInteger(connection, "address", 1, 1, 255, path + ".connection");
            const auto delim = optionalString(connection, "delimiter", "crlf", path + ".connection");
            require(delim == "crlf" || delim == "cr" || delim == "lf",
                    IR_BAD_ARGUMENT, path + ".connection.delimiter must be crlf, cr, or lf");
            const double minSp = optionalNumber(connection, "min_setpoint_c", -45.0, path + ".connection");
            const double maxSp = optionalNumber(connection, "max_setpoint_c", 70.0, path + ".connection");
            require(minSp <= maxSp, IR_BAD_ARGUMENT,
                    path + ".connection min_setpoint_c must be <= max_setpoint_c");
        } else if (config.kind == IR_DMM6500) {
            validateTcpOrSerial({}, 5025, 115200);
        } else if (config.kind == IR_DETECTOR_SERIAL) {
            validateTcpOrSerial({"inter_command_ms"}, 2102, 115200);
            (void)optionalInteger(connection, "inter_command_ms", 0, 0, 2000, path + ".connection");
        } else if (config.kind == IR_COOLER_MODBUS) {
            validateTcpOrSerial({"slave_address", "register_address"}, 5020, 9600);
            (void)optionalInteger(connection, "slave_address", 1, 1, 247, path + ".connection");
            (void)optionalInteger(connection, "register_address", 0x7A52, 0, 65535, path + ".connection");
        } else {
            allowFields(connection, {"port", "timeout_ms"}, path + ".connection");
            (void)stringField(connection, "port", path + ".connection");
            (void)optionalInteger(connection, "timeout_ms", 3000, 1, 60000, path + ".connection");
        }
        station.devices.emplace(device.key(), std::move(config));
    }
    return station;
}

BlackbodyStep parseBlackbodyStep(StepRef ref, const Json &fields, const std::string &path,
                                 const StationConfig &station)
{
    allowFields(fields, {"id", "type", "device", "target_c", "tolerance_c",
        "stable_window_ms", "timeout_ms", "require_device_stable"}, path);
    auto alias = stringField(fields, "device", path);
    const auto found = station.devices.find(alias);
    require(found != station.devices.end() &&
            (found->second.kind == IR_PCN7LT || found->second.kind == IR_DCN1000),
            IR_BAD_ARGUMENT, path + ".device must name a configured blackbody");
    const BlackbodyTarget target{
        numberField(fields, "target_c", path),
        numberField(fields, "tolerance_c", path),
        integerField(fields, "stable_window_ms", 0, 3600000, path),
        integerField(fields, "timeout_ms", 1, 86400000, path),
        booleanField(fields, "require_device_stable", path)
    };
    require(target.toleranceC > 0.0 && target.timeoutMs >= target.stableWindowMs &&
            target.temperatureC >= -60.0 && target.temperatureC <= 100.0,
            IR_BAD_ARGUMENT, path + " has invalid temperature or stability limits");
    return {std::move(ref), std::move(alias), target};
}

PowerStep parsePowerStep(StepRef ref, const std::string &type, const Json &fields,
                         const std::string &path, const StationConfig &station)
{
    PowerStep step;
    step.ref = std::move(ref);
    step.device = stringField(fields, "device", path);
    const auto found = station.devices.find(step.device);
    require(found != station.devices.end() && found->second.kind == IR_IT6332A,
            IR_BAD_ARGUMENT, path + ".device must name a configured IT6332A");
    step.channel = integerField(fields, "channel", 1, 3, path);
    if (type == "power_set") {
        allowFields(fields, {"id", "type", "device", "channel", "voltage_v", "current_limit_a"}, path);
        step.operation = PowerStep::Operation::set;
        step.voltageV = numberField(fields, "voltage_v", path);
        step.currentLimitA = numberField(fields, "current_limit_a", path);
        require(step.voltageV >= 0.0 && step.currentLimitA > 0.0,
                IR_BAD_ARGUMENT, path + " has invalid voltage or current limit");
    } else if (type == "power_output") {
        allowFields(fields, {"id", "type", "device", "channel", "enabled", "off_after_run"}, path);
        step.operation = PowerStep::Operation::output;
        step.enabled = booleanField(fields, "enabled", path);
        step.offAfterRun = fields.contains("off_after_run") && booleanField(fields, "off_after_run", path);
        require(!step.enabled || step.offAfterRun, IR_BAD_ARGUMENT,
                path + ".off_after_run must be true when enabling power output");
    } else {
        allowFields(fields, {"id", "type", "device", "channel"}, path);
        step.operation = PowerStep::Operation::read;
    }
    return step;
}

namespace {
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

template <class VariantType>
VariantType parseSharedControlOrCaptureOrFlushOrAnalyze(
    StepRef ref, const std::string &type, const Json &fields, const std::string &path,
    const StationConfig &station, std::map<std::string, SlotKind> &knownSlots,
    std::set<std::string> &usedDevices, std::set<std::string> &analysisNames,
    std::map<std::string, std::string> &metricUnits,
    std::vector<std::string> *producedMetricsOrder,
    const ExternalMetricResolver *externalMetrics)
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
        return CaptureStep{
            std::move(ref),
            integerField(fields, "frames", 1, INT32_MAX, path),
            integerField(fields, "frame_timeout_ms", 0, 86400000, path),
            std::move(outputSlot)
        };
    }
    if (type == "analyze") {
        auto step = prepareAnalysis(std::move(ref), fields, path, knownSlots, externalMetrics);
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

} // namespace


RecipeConfig parseRecipe(const char *text, const StationConfig &station,
                             const ExternalMetricResolver *externalMetrics)
{
    auto root = parseJson(text, "recipe JSON");
    require(root.is_object(), IR_BAD_ARGUMENT, "recipe JSON must contain an object");
    allowFields(root, {"schemav", "recipe_id", "steps", "safety", "limits"}, "recipe");
    (void)optionalInteger(root, "schemav", 1, 1, 1, "recipe");

    RecipeConfig recipe;
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
            recipe.steps.emplace_back(SaveRawStep{std::move(ref), inputSlot, subdir});
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

                burst.steps.emplace_back(parseSharedControlOrCaptureOrFlushOrAnalyze<InnerStep>(
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
            recipe.steps.emplace_back(parseSharedControlOrCaptureOrFlushOrAnalyze<RecipeStep>(
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
    recipe.metricUnits = std::move(metricUnits);
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

} // namespace ir::procedures::recipe
