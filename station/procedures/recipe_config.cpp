#include "recipe_config.h"
#include "api_common.h"
#include <algorithm>
#include <set>

namespace ir::procedures::recipe {
using namespace ir::api;

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

void optionalInteger(const Json &object, const char *key, int low, int high, const std::string &path)
{
    const auto found = object.find(key);
    if (found == object.end()) return;
    const auto value = numberField(object, key, path);
    require(value >= low && value <= high && std::trunc(value) == value,
            IR_BAD_ARGUMENT, path + "." + key + " is outside its integer range");
}

int integerField(const Json &object, const char *key, int low, int high, const std::string &path)
{
    const auto value = numberField(object, key, path);
    require(value >= low && value <= high && std::trunc(value) == value,
            IR_BAD_ARGUMENT, path + "." + key + " is outside its integer range");
    return static_cast<int>(value);
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
    allowFields(root, {"$schema", "schema_version", "acquisition", "devices"}, "station");
    (void)optionalString(root, "$schema", "", "station");
    require(integerField(root, "schema_version", 1, 1, "station") == 1,
            IR_BAD_ARGUMENT, "station.schema_version is unsupported");

    const auto &acquisition = objectField(root, "acquisition", "station");
    allowFields(acquisition, {"backend", "source", "valid_bits", "max_batch_bytes"}, "station.acquisition");
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
        else throw Error(IR_BAD_ARGUMENT, path + ".kind is unsupported");
        const auto &connection = objectField(device.value(), "connection", path);
        config.connectionJson = connection.dump();
        if (config.kind == IR_PCN7LT) {
            allowFields(connection, {"host", "tcp_port", "head", "expected_head_serial", "timeout_ms"},
                        path + ".connection");
            (void)stringField(connection, "host", path + ".connection");
            optionalInteger(connection, "tcp_port", 1, 65535, path + ".connection");
            optionalInteger(connection, "head", -1, 1, path + ".connection");
            optionalInteger(connection, "timeout_ms", 1, 60000, path + ".connection");
            const auto expected = optionalString(connection, "expected_head_serial", "",
                                                 path + ".connection");
            const auto head = connection.contains("head")
                ? integerField(connection, "head", -1, 1, path + ".connection") : -1;
            require(expected.empty() || head >= 0, IR_BAD_ARGUMENT,
                    path + ".connection.expected_head_serial requires a head index");
        } else if (config.kind == IR_IT6332A) {
            allowFields(connection, {"port", "baud_rate", "timeout_ms"}, path + ".connection");
            (void)stringField(connection, "port", path + ".connection");
            optionalInteger(connection, "baud_rate", 1200, 921600, path + ".connection");
            optionalInteger(connection, "timeout_ms", 1, 60000, path + ".connection");
        } else {
            allowFields(connection, {"port", "timeout_ms"}, path + ".connection");
            (void)stringField(connection, "port", path + ".connection");
            optionalInteger(connection, "timeout_ms", 1, 60000, path + ".connection");
        }
        station.devices.emplace(device.key(), std::move(config));
    }
    return station;
}

namespace {
BlackbodyStep parseBlackbody(StepRef ref, const Json &fields, const std::string &path,
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

PowerStep parsePower(StepRef ref, const std::string &type, const Json &fields,
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

RecipeConfig parseRecipe(const char *text, const StationConfig &station)
{
    auto root = parseJson(text, "recipe JSON");
    require(root.is_object(), IR_BAD_ARGUMENT, "recipe JSON must contain an object");
    allowFields(root, {"$schema", "schema_version", "recipe_id", "steps", "limits"}, "recipe");
    (void)optionalString(root, "$schema", "", "recipe");
    (void)integerField(root, "schema_version", 1, 1, "recipe");

    RecipeConfig recipe;
    recipe.id = stringField(root, "recipe_id", "recipe");
    const auto steps = root.find("steps");
    require(steps != root.end() && steps->is_array() && !steps->empty() && steps->size() <= INT32_MAX,
            IR_BAD_ARGUMENT, "recipe.steps must be a nonempty array");

    std::set<std::string> ids;
    std::set<std::string> analysisNames;
    std::set<std::string> usedDevices;
    std::map<std::string, std::string> metricUnits;
    for (size_t index = 0; index < steps->size(); ++index) {
        const auto &fields = (*steps)[index];
        const auto path = "recipe.steps[" + std::to_string(index) + "]";
        require(fields.is_object(), IR_BAD_ARGUMENT, path + " must be an object");
        StepRef ref{static_cast<int32_t>(index), stringField(fields, "id", path)};
        require(ids.insert(ref.id).second, IR_BAD_ARGUMENT, path + ".id is duplicated");
        const auto type = stringField(fields, "type", path);
        const bool captureSeen = recipe.capture.ref.index >= 0;
        const bool saveSeen = recipe.archive.index >= 0;

        if (type == "blackbody_stabilize") {
            require(!captureSeen, IR_BAD_ARGUMENT, path + " must precede capture");
            auto step = parseBlackbody(std::move(ref), fields, path, station);
            usedDevices.insert(step.device);
            recipe.controls.emplace_back(std::move(step));
        } else if (type == "power_set" || type == "power_output" || type == "power_read") {
            require(!captureSeen, IR_BAD_ARGUMENT, path + " must precede capture");
            auto step = parsePower(std::move(ref), type, fields, path, station);
            usedDevices.insert(step.device);
            recipe.controls.emplace_back(std::move(step));
        } else if (type == "capture") {
            allowFields(fields, {"id", "type", "frames", "frame_timeout_ms"}, path);
            require(!captureSeen, IR_BAD_ARGUMENT, "recipe must contain exactly one capture before analysis");
            recipe.capture = {std::move(ref), integerField(fields, "frames", 1, INT32_MAX, path),
                              integerField(fields, "frame_timeout_ms", 0, 86400000, path)};
        } else if (type == "analyze") {
            require(captureSeen && !saveSeen, IR_BAD_ARGUMENT, path + " must follow capture and precede save_raw");
            require(stringField(fields, "input", path) == recipe.capture.ref.id,
                    IR_BAD_ARGUMENT, path + ".input must name the captured batch");
            auto step = prepareAnalysis(std::move(ref), fields, path, recipe.capture.frames);
            require(analysisNames.insert(step.name).second, IR_BAD_ARGUMENT, path + ".name is duplicated");
            for (const auto &output : step.outputs) {
                require(metricUnits.emplace(step.name + output.suffix, output.unit).second,
                        IR_BAD_ARGUMENT, path + " produces a duplicate metric name");
            }
            recipe.analysis.push_back(std::move(step));
        } else if (type == "save_raw") {
            allowFields(fields, {"id", "type", "input"}, path);
            require(captureSeen && !saveSeen && index == steps->size() - 1,
                    IR_BAD_ARGUMENT, path + " must be the final step after capture");
            require(stringField(fields, "input", path) == recipe.capture.ref.id,
                    IR_BAD_ARGUMENT, path + ".input must name the captured batch");
            recipe.archive = std::move(ref);
        } else {
            throw Error(IR_BAD_ARGUMENT, path + ".type is unsupported: " + type);
        }
    }
    require(recipe.capture.ref.index >= 0 && recipe.archive.index >= 0, IR_BAD_ARGUMENT,
            "recipe needs one capture step and one final save_raw step");
    require(!recipe.analysis.empty(), IR_BAD_ARGUMENT, "recipe needs at least one analyze step");

    if (const auto limits = root.find("limits"); limits != root.end()) {
        require(limits->is_array(), IR_BAD_ARGUMENT, "recipe.limits must be an array");
        for (size_t index = 0; index < limits->size(); ++index) {
            const auto &limit = (*limits)[index];
            const auto path = "recipe.limits[" + std::to_string(index) + "]";
            require(limit.is_object(), IR_BAD_ARGUMENT, path + " must be an object");
            allowFields(limit, {"metric", "operator", "value", "unit"}, path);
            auto metric = stringField(limit, "metric", path);
            const auto found = metricUnits.find(metric);
            require(found != metricUnits.end(), IR_BAD_ARGUMENT, path + ".metric is not produced by an analyze step");
            const auto operation = stringField(limit, "operator", path);
            require(operation == "max" || operation == "min", IR_BAD_ARGUMENT,
                    path + ".operator must be max or min");
            auto unit = stringField(limit, "unit", path);
            require(unit == found->second, IR_BAD_ARGUMENT, path + ".unit does not match the metric unit");
            recipe.limits.push_back({std::move(metric), std::move(unit), operation == "max",
                                     numberField(limit, "value", path)});
        }
    }

    recipe.deviceAliases.assign(usedDevices.begin(), usedDevices.end());
    recipe.definition = std::move(root);
    return recipe;
}
}
