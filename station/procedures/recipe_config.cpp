#include "recipe_config.h"
#include "api_common.h"
#include <algorithm>
#include <set>

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
}
