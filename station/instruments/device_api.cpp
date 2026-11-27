#include "IR_device.h"
#include "api_common.h"
#include "device_config.h"
#include "device_lease.h"
#include "drivers.h"
#include "utf8_path.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using Json = nlohmann::json;
using ir::instruments::BlackbodyConfig;
using ir::instruments::CoolerModbus;
using ir::instruments::CoolerModbusConfig;
using ir::instruments::DetectorSerial;
using ir::instruments::DetectorSerialConfig;
using ir::instruments::Dmm6500;
using ir::instruments::Dmm6500Config;
using ir::instruments::Doc058Blackbody;
using ir::instruments::ExternalPowerConfig;
using ir::instruments::ExternalPowerSupply;
using ir::instruments::GwsStenConfig;
using ir::instruments::GwsStenOven;
using ir::instruments::It6332aConfig;
using ir::instruments::It6332aPower;
using ir::instruments::PowerSupply;
using ir::instruments::Udp6730Config;
using ir::instruments::Udp6730Power;
using ir::transport::SerialEndpoint;
using ir::transport::TcpEndpoint;
using namespace ir::api;

struct Session {
    std::vector<std::string> endpointKeys;
    std::mutex mutex;
    std::unique_ptr<Doc058Blackbody> blackbody;
    std::unique_ptr<PowerSupply> power;
    std::unique_ptr<GwsStenOven> oven;
    std::unique_ptr<Dmm6500> dmm;
    std::unique_ptr<DetectorSerial> detector;
    std::unique_ptr<CoolerModbus> cooler;
    std::thread::id workflowOwner;
};
std::mutex devicesMutex;
std::map<IRHandle, std::shared_ptr<Session>> devices;
std::set<std::string> openingEndpoints;
IRHandle nextHandle{1};

using Endpoint = BlackbodyConfig::Endpoint;
using DeviceOptions = std::variant<BlackbodyConfig, It6332aConfig, Udp6730Config, ExternalPowerConfig,
                                   GwsStenConfig, Dmm6500Config, DetectorSerialConfig, CoolerModbusConfig>;

class ConnectionFields {
public:
    ConnectionFields(const Json &json, std::string_view path) : json_(json), path_(path)
    {
        if (!json_.is_object()) fail({}, "must be an object");
    }

    [[noreturn]] void fail(std::string_view key, std::string_view message) const
    {
        std::string path(path_);
        if (!key.empty()) {
            path += '.';
            path += key;
        }
        path += ' ';
        path += message;
        throw Error(IR_BAD_ARGUMENT, path);
    }

    bool contains(const char *key) const { return json_.contains(key); }

    void allow(std::initializer_list<std::string_view> fields,
               std::initializer_list<std::string_view> extra = {}) const
    {
        for (auto field = json_.begin(); field != json_.end(); ++field) {
            const std::string_view key = field.key();
            if (std::find(fields.begin(), fields.end(), key) == fields.end() &&
                std::find(extra.begin(), extra.end(), key) == extra.end())
                fail(key, "is unsupported for this instrument or transport");
        }
    }

    std::string text(const char *key, bool required = false) const
    {
        const auto value = json_.find(key);
        if (value == json_.end()) {
            if (required) fail(key, "is required");
            return {};
        }
        if (!value->is_string()) fail(key, "must be a string");
        const auto &input = value->get_ref<const std::string &>();
        if (input.find('\0') != std::string::npos) fail(key, "must not contain NUL");
        constexpr std::string_view whitespace = " \t\r\n\f\v";
        const auto first = input.find_first_not_of(whitespace);
        if (first == std::string::npos) {
            if (required) fail(key, "must be nonempty");
            return {};
        }
        return input.substr(first, input.find_last_not_of(whitespace) - first + 1);
    }

    double number(const char *key, double fallback) const
    {
        const auto value = json_.find(key);
        if (value == json_.end()) return fallback;
        if (!value->is_number()) fail(key, "must be a number");
        const auto result = value->get<double>();
        if (!std::isfinite(result)) fail(key, "must be finite");
        return result;
    }

    int integer(const char *key, int fallback, int low, int high) const
    {
        const auto result = number(key, fallback);
        if (result < low || result > high || std::trunc(result) != result)
            fail(key, "must be an integer in [" + std::to_string(low) + ", " + std::to_string(high) + "]");
        return static_cast<int>(result);
    }

    std::chrono::milliseconds timeout(std::chrono::milliseconds fallback) const
    {
        return std::chrono::milliseconds{integer("timeout_ms", static_cast<int>(fallback.count()), 1, 60000)};
    }

private:
    const Json &json_;
    std::string_view path_;
};

std::string lowercaseAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return value;
}

int parseParity(const ConnectionFields &fields)
{
    const auto parity = lowercaseAscii(fields.text("parity"));
    if (parity.empty() || parity == "none") return 0;
    if (parity == "odd") return 1;
    if (parity == "even") return 2;
    fields.fail("parity", "must be none, odd, or even");
}

TcpEndpoint parseTcpEndpoint(const ConnectionFields &fields, int defaultPort)
{
    return {fields.text("host", true),
            static_cast<uint16_t>(fields.integer("tcp_port", defaultPort, 1, 65535)), {}};
}

Endpoint parseTcpOrSerialEndpoint(const ConnectionFields &fields, int defaultTcpPort,
                                  int defaultBaud, int defaultStopBits = 1,
                                  std::initializer_list<std::string_view> extra = {})
{
    const bool hasHost = fields.contains("host");
    if (hasHost == fields.contains("port"))
        fields.fail({}, "must specify exactly one of 'host' or 'port'");
    if (hasHost) {
        fields.allow({"host", "tcp_port", "timeout_ms"}, extra);
        return parseTcpEndpoint(fields, defaultTcpPort);
    }
    fields.allow({"port", "baud_rate", "data_bits", "parity", "stop_bits", "timeout_ms"}, extra);
    return SerialEndpoint{fields.text("port", true),
                          static_cast<unsigned>(fields.integer("baud_rate", defaultBaud, 1200, 921600)),
                          static_cast<unsigned>(fields.integer("data_bits", 8, 5, 8)),
                          parseParity(fields), fields.integer("stop_bits", defaultStopBits, 1, 2)};
}

BlackbodyConfig parseBlackbodyConfig(int32_t kind, const ConnectionFields &fields)
{
    BlackbodyConfig result;
    result.timeout = fields.timeout(result.timeout);
    result.expectedHead = fields.text("expected_head_serial");
    if (kind == IR_PCN7LT) {
        fields.allow({"host", "tcp_port", "head", "expected_head_serial", "timeout_ms"});
        result.endpoint = parseTcpEndpoint(fields, 2101);
        result.commandInterval = std::chrono::milliseconds{100};
        result.head = fields.integer("head", result.head, -1, 1);
        if (!result.expectedHead.empty() && result.head < 0)
            fields.fail("expected_head_serial", "requires a head index of 0 or 1");
    } else {
        fields.allow({"port", "expected_head_serial", "timeout_ms"});
        result.endpoint = SerialEndpoint{fields.text("port", true), 9600};
        result.commandInterval = std::chrono::milliseconds{200};
    }
    return result;
}

GwsStenConfig parseOvenConfig(const ConnectionFields &fields)
{
    GwsStenConfig result;
    result.endpoint = parseTcpOrSerialEndpoint(fields, 5000, 9600, 1,
                                               {"address", "delimiter", "min_setpoint_c", "max_setpoint_c"});
    result.timeout = fields.timeout(result.timeout);
    result.address = fields.integer("address", result.address, 1, 255);
    result.minSetpointC = fields.number("min_setpoint_c", result.minSetpointC);
    result.maxSetpointC = fields.number("max_setpoint_c", result.maxSetpointC);
    if (result.minSetpointC > result.maxSetpointC)
        fields.fail("min_setpoint_c", "must be <= max_setpoint_c");
    const auto delimiter = lowercaseAscii(fields.text("delimiter"));
    if (delimiter == "cr") result.delimiter = "\r";
    else if (delimiter == "lf") result.delimiter = "\n";
    else if (!delimiter.empty() && delimiter != "crlf")
        fields.fail("delimiter", "must be crlf, cr, or lf");
    return result;
}

DeviceOptions parseDeviceConnection(int32_t kind, const Json &json, std::string_view path)
{
    const ConnectionFields fields(json, path);
    switch (kind) {
    case IR_PCN7LT:
    case IR_DCN1000:
        return parseBlackbodyConfig(kind, fields);
    case IR_IT6332A: {
        It6332aConfig result;
        result.endpoint = parseTcpOrSerialEndpoint(fields, 5026, 115200);
        result.timeout = fields.timeout(result.timeout);
        return result;
    }
    case IR_UDP6730: {
        Udp6730Config result;
        result.endpoint = parseTcpOrSerialEndpoint(fields, 5026, 9600);
        result.timeout = fields.timeout(result.timeout);
        return result;
    }
    case IR_EXTERNAL_POWER: {
        fields.allow({"driver_path", "driver_id", "driver_config"});
        ExternalPowerConfig result;
        result.driverPath = fields.text("driver_path", true);
        result.expectedDriverId = fields.text("driver_id", false);
        const auto driverCfgIt = json.find("driver_config");
        if (driverCfgIt == json.end() || !driverCfgIt->is_object())
            fields.fail("driver_config", "must be an object");
        result.driverConfigJson = driverCfgIt->dump();
        result.inspected = ir::instruments::inspectExternalPowerDriver(result, path);
        return result;
    }
    case IR_OVEN_GWS_STEN:
        return parseOvenConfig(fields);
    case IR_DMM6500: {
        Dmm6500Config result;
        if (fields.contains("visa_resource")) {
            fields.allow({"visa_resource", "timeout_ms"});
            result.visaResource = fields.text("visa_resource", true);
        } else {
            result.endpoint = parseTcpOrSerialEndpoint(fields, 5025, 115200);
        }
        result.timeout = fields.timeout(result.timeout);
        return result;
    }
    case IR_DETECTOR_SERIAL: {
        DetectorSerialConfig result;
        result.endpoint = parseTcpOrSerialEndpoint(fields, 2102, 115200, 1, {"inter_command_ms"});
        result.timeout = fields.timeout(result.timeout);
        result.interCommandDelay = std::chrono::milliseconds{fields.integer("inter_command_ms", 0, 0, 2000)};
        return result;
    }
    case IR_COOLER_MODBUS: {
        CoolerModbusConfig result;
        result.endpoint = parseTcpOrSerialEndpoint(fields, 5020, 9600, 2,
                                                   {"slave_address", "register_address"});
        result.timeout = fields.timeout(result.timeout);
        result.slaveAddress = static_cast<uint8_t>(fields.integer("slave_address", result.slaveAddress, 1, 247));
        result.registerAddress = static_cast<uint16_t>(fields.integer("register_address", result.registerAddress, 0, 65535));
        return result;
    }
    default:
        throw Error(IR_UNSUPPORTED, "Unsupported instrument kind");
    }
}

Json parseConfigJson(const char *config)
{
    require(config && *config, IR_BAD_ARGUMENT, "connection JSON is empty");
    try {
        return Json::parse(config);
    } catch (const Json::parse_error &) {
        throw Error(IR_BAD_ARGUMENT, "connection contains invalid JSON");
    }
}

std::string serialEndpointKey(const std::string &name)
{
#ifdef _WIN32
    auto key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) {
        return character >= 'a' && character <= 'z' ? char(character - 'a' + 'A') : char(character);
    });
    if (key.starts_with(R"(\\.\)")) key.erase(0, 4);
    return "serial:" + key;
#else
    std::error_code errorCode;
    const auto resolved = std::filesystem::canonical(ir::utf8::pathFromUtf8(name), errorCode);
    return "serial:" + (errorCode ? name : ir::utf8::pathToUtf8(resolved));
#endif
}

std::vector<std::string> resolveEndpointKeys(Endpoint &endpoint, std::chrono::milliseconds timeout)
{
    if (const auto *serial = std::get_if<SerialEndpoint>(&endpoint))
        return {serialEndpointKey(serial->port)};
    auto &tcp = std::get<TcpEndpoint>(endpoint);
    tcp.resolvedAddresses = ir::transport::resolveTcpAddresses(tcp, std::chrono::steady_clock::now() + timeout);
    std::vector<std::string> keys;
    keys.reserve(tcp.resolvedAddresses.size());
    for (const auto &address : tcp.resolvedAddresses) {
        const auto formattedAddress = address.find(':') == std::string::npos ? address : '[' + address + ']';
        keys.push_back("tcp:" + formattedAddress + ':' + std::to_string(tcp.port));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

auto findDevice(IRHandle handle)
{
    const auto pos = devices.find(handle);
    require(pos != devices.end(), IR_INVALID_HANDLE, "Unknown device handle");
    return pos;
}

template<class Action> void useSession(IRHandle handle, Action action)
{
    std::shared_ptr<Session> device;
    std::unique_lock<std::mutex> operationLock;
    {
        std::lock_guard lock(devicesMutex);
        device = findDevice(handle)->second;
        require(device->workflowOwner == std::thread::id{} ||
                device->workflowOwner == std::this_thread::get_id(),
                IR_BUSY, "Instrument is reserved by a measurement procedure");
        operationLock = std::unique_lock<std::mutex>(device->mutex, std::try_to_lock);
        require(operationLock.owns_lock(), IR_BUSY, "Another instrument operation is active");
    }
    action(*device);
}

template<class Action> void useBlackbody(IRHandle handle, Action action)
{
    useSession(handle, [&](Session &device) {
        require(bool(device.blackbody), IR_BAD_ARGUMENT, "Device handle is not a blackbody");
        action(*device.blackbody);
    });
}

template<class Action> void usePower(IRHandle handle, Action action)
{
    useSession(handle, [&](Session &device) {
        require(bool(device.power), IR_BAD_ARGUMENT, "Device handle is not a power supply");
        action(*device.power);
    });
}

template<class Action> void useOven(IRHandle handle, Action action)
{
    useSession(handle, [&](Session &device) {
        require(bool(device.oven), IR_BAD_ARGUMENT, "Device handle is not a GWS STEN oven");
        action(*device.oven);
    });
}

template<class Action> void useDmm(IRHandle handle, Action action)
{
    useSession(handle, [&](Session &device) {
        require(bool(device.dmm), IR_BAD_ARGUMENT, "Device handle is not a DMM6500 multimeter");
        action(*device.dmm);
    });
}

template<class Action> void useDetector(IRHandle handle, Action action)
{
    useSession(handle, [&](Session &device) {
        require(bool(device.detector), IR_BAD_ARGUMENT, "Device handle is not a serial detector controller");
        action(*device.detector);
    });
}

template<class Action> void useCooler(IRHandle handle, Action action)
{
    useSession(handle, [&](Session &device) {
        require(bool(device.cooler), IR_BAD_ARGUMENT, "Device handle is not a cooler Modbus monitor");
        action(*device.cooler);
    });
}
}

namespace ir::instruments {
using ir::api::require;

void validateDeviceConnection(int32_t kind, const nlohmann::json &connection, std::string_view path)
{
    (void)parseDeviceConnection(kind, connection, path);
}

PowerCapabilitiesInfo queryPowerDeviceCapabilities(int32_t kind, const nlohmann::json &connection,
                                                   std::string_view path)
{
    auto parsed = parseDeviceConnection(kind, connection, path);
    if (std::holds_alternative<It6332aConfig>(parsed)) {
        return {IR_POWER_CAP_READ | IR_POWER_CAP_OUTPUT | IR_POWER_CAP_SET, 1, 3};
    }
    if (std::holds_alternative<Udp6730Config>(parsed)) {
        return {IR_POWER_CAP_READ | IR_POWER_CAP_OUTPUT, 1, 1};
    }
    if (const auto *ext = std::get_if<ExternalPowerConfig>(&parsed)) {
        return {ext->inspected.capabilities, ext->inspected.minChannel, ext->inspected.maxChannel};
    }
    throw Error(IR_BAD_ARGUMENT, std::string(path) + " is not a power supply");
}

DeviceLease::DeviceLease(const IRHandle *handles, std::size_t count)
{
    require(handles || count == 0, IR_NULL_POINTER, "Instrument handle list is NULL");
    const auto owner = std::this_thread::get_id();
    std::lock_guard lock(devicesMutex);

    for (std::size_t index = 0; index < count; ++index) {
        if (!handles[index]) continue;
        require(std::find(handles_.begin(), handles_.end(), handles[index]) == handles_.end(),
                IR_BAD_ARGUMENT, "A procedure specifies the same instrument more than once");
        findDevice(handles[index]);
        handles_.push_back(handles[index]);
    }

    std::vector<std::shared_ptr<Session>> sessions;
    sessions.reserve(handles_.size());
    for (const auto handle : handles_)
        sessions.push_back(findDevice(handle)->second);
    std::sort(sessions.begin(), sessions.end(), [](const auto &left, const auto &right) {
        return left->endpointKeys < right->endpointKeys;
    });

    std::vector<std::unique_lock<std::mutex>> sessionLocks;
    sessionLocks.reserve(sessions.size());
    for (const auto &session : sessions) {
        require(session->workflowOwner == std::thread::id{}, IR_BUSY, "Instrument is already reserved");
        sessionLocks.emplace_back(session->mutex, std::try_to_lock);
        require(sessionLocks.back().owns_lock(), IR_BUSY, "An instrument operation is active");
    }
    for (const auto &session : sessions) session->workflowOwner = owner;
}

DeviceLease::~DeviceLease()
{
    const auto owner = std::this_thread::get_id();
    std::lock_guard lock(devicesMutex);
    for (const auto handle : handles_) {
        const auto pos = devices.find(handle);
        if (pos == devices.end()) continue;
        std::lock_guard sessionLock(pos->second->mutex);
        if (pos->second->workflowOwner == owner) pos->second->workflowOwner = {};
    }
}
}

extern "C" int32_t IR_OpenDevice(int32_t kind, const char *config, IRHandle *out, char *err, int32_t bytes)
noexcept{
    if (out) *out = 0;
    return guarded(err, bytes, [&] {
        needed(out, 1, 1);
        auto parsed = parseDeviceConnection(kind, parseConfigJson(config), "connection");
        const auto endpointKeys = std::visit([](auto &options) {
            using Config = std::decay_t<decltype(options)>;
            if constexpr (std::is_same_v<Config, Dmm6500Config>) {
                if (!options.visaResource.empty())
                    return std::vector<std::string>{"visa:" + lowercaseAscii(options.visaResource)};
                return resolveEndpointKeys(options.endpoint, options.timeout);
            } else if constexpr (std::is_same_v<Config, ExternalPowerConfig>) {
                std::error_code ec;
                const auto p = ir::utf8::pathFromUtf8(options.driverPath);
                const auto norm = std::filesystem::weakly_canonical(p, ec);
                auto pathKey = ir::utf8::pathToUtf8(ec ? p.lexically_normal() : norm);
#ifdef _WIN32
                pathKey = lowercaseAscii(std::move(pathKey));
#endif
                return std::vector<std::string>{"external_power:" + pathKey + ":" + options.driverConfigJson};
            } else {
                return resolveEndpointKeys(options.endpoint, options.timeout);
            }
        }, parsed);
        {
            std::lock_guard lock(devicesMutex);
            for (const auto &endpoint : endpointKeys) {
                require(!openingEndpoints.contains(endpoint), IR_BUSY,
                        "Instrument endpoint is being opened");
                for (const auto &[handle, session] : devices)
                    require(std::find(session->endpointKeys.begin(), session->endpointKeys.end(), endpoint) ==
                                session->endpointKeys.end(),
                            IR_BUSY, "Instrument endpoint is already open");
            }
            openingEndpoints.insert(endpointKeys.begin(), endpointKeys.end());
        }

        std::shared_ptr<Session> device;
        try {
            device = std::make_shared<Session>();
            device->endpointKeys = endpointKeys;
            std::visit([&](auto &options) {
                using Config = std::remove_reference_t<decltype(options)>;
                if constexpr (std::is_same_v<Config, BlackbodyConfig>)
                    device->blackbody = std::make_unique<Doc058Blackbody>(std::move(options));
                else if constexpr (std::is_same_v<Config, It6332aConfig>)
                    device->power = std::make_unique<It6332aPower>(std::move(options));
                else if constexpr (std::is_same_v<Config, Udp6730Config>)
                    device->power = std::make_unique<Udp6730Power>(std::move(options));
                else if constexpr (std::is_same_v<Config, ExternalPowerConfig>)
                    device->power = std::make_unique<ExternalPowerSupply>(std::move(options));
                else if constexpr (std::is_same_v<Config, GwsStenConfig>)
                    device->oven = std::make_unique<GwsStenOven>(std::move(options));
                else if constexpr (std::is_same_v<Config, Dmm6500Config>)
                    device->dmm = std::make_unique<Dmm6500>(std::move(options));
                else if constexpr (std::is_same_v<Config, DetectorSerialConfig>)
                    device->detector = std::make_unique<DetectorSerial>(std::move(options));
                else
                    device->cooler = std::make_unique<CoolerModbus>(std::move(options));
            }, parsed);
        } catch (...) {
            std::lock_guard lock(devicesMutex);
            for (const auto &endpoint : endpointKeys) openingEndpoints.erase(endpoint);
            throw;
        }

        std::lock_guard lock(devicesMutex);
        for (const auto &endpoint : endpointKeys) openingEndpoints.erase(endpoint);
        const auto handle = nextHandle++;
        devices.emplace(handle, std::move(device));
        *out = handle;
    });
}

extern "C" int32_t IR_CloseDevice(IRHandle handle, char *err, int32_t bytes)
noexcept{
    return guarded(err, bytes, [&] {
        std::shared_ptr<Session> device;
        std::unique_ptr<Doc058Blackbody> closingBlackbody;
        std::unique_ptr<PowerSupply> closingPower;
        std::unique_ptr<GwsStenOven> closingOven;
        std::unique_ptr<Dmm6500> closingDmm;
        std::unique_ptr<DetectorSerial> closingDetector;
        std::unique_ptr<CoolerModbus> closingCooler;
        std::unique_lock<std::mutex> deviceLock;
        std::vector<std::string> endpointKeys;
        {
            std::lock_guard registryLock(devicesMutex);
            const auto pos = findDevice(handle);
            device = pos->second;
            require(device->workflowOwner == std::thread::id{}, IR_BUSY, "Instrument is in use");
            deviceLock = std::unique_lock<std::mutex>(device->mutex, std::try_to_lock);
            require(deviceLock.owns_lock(), IR_BUSY, "Instrument operation is active");
            endpointKeys = device->endpointKeys;
            openingEndpoints.insert(endpointKeys.begin(), endpointKeys.end());
            closingBlackbody = std::move(device->blackbody);
            closingPower = std::move(device->power);
            closingOven = std::move(device->oven);
            closingDmm = std::move(device->dmm);
            closingDetector = std::move(device->detector);
            closingCooler = std::move(device->cooler);
            devices.erase(pos);
        }
        closingBlackbody.reset();
        try {
            if (closingPower) closingPower->close();
            closingPower.reset();
            closingOven.reset();
            closingDmm.reset();
            closingDetector.reset();
            closingCooler.reset();
        } catch (...) {
            closingPower.reset();
            closingOven.reset();
            closingDmm.reset();
            closingDetector.reset();
            closingCooler.reset();
            deviceLock.unlock();
            std::lock_guard registryLock(devicesMutex);
            for (const auto &endpoint : endpointKeys) openingEndpoints.erase(endpoint);
            throw;
        }
        deviceLock.unlock();
        {
            std::lock_guard registryLock(devicesMutex);
            for (const auto &endpoint : endpointKeys) openingEndpoints.erase(endpoint);
        }
    });
}

extern "C" int32_t IR_SetBlackbody(IRHandle handle, double target, double *readback, char *err, int32_t bytes)
noexcept{
    return guarded(err, bytes, [&] {
        needed(readback, 1, 1);
        useBlackbody(handle, [&](auto &blackbody) { *readback = blackbody.set(target); });
    });
}

extern "C" int32_t IR_ReadBlackbody(IRHandle handle, IRBlackbodyReading *reading, char *err, int32_t bytes)
noexcept{
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useBlackbody(handle, [&](auto &blackbody) { *reading = blackbody.read(); });
    });
}

extern "C" int32_t IR_SetPowerChannel(IRHandle handle, int32_t channel,
    double voltage, double currentLimit, IRPowerReading *readback, char *err, int32_t bytes)
noexcept{
    if (readback) *readback = {};
    return guarded(err, bytes, [&] {
        needed(readback, 1, 1);
        usePower(handle, [&](PowerSupply &power) {
            *readback = power.setPowerChannel(channel, voltage, currentLimit);
        });
    });
}

extern "C" int32_t IR_SetPowerOutput(IRHandle handle, int32_t channel, int32_t enabled,
    IRPowerReading *readback, char *err, int32_t bytes)
noexcept{
    if (readback) *readback = {};
    return guarded(err, bytes, [&] {
        needed(readback, 1, 1);
        require(enabled == 0 || enabled == 1, IR_BAD_ARGUMENT, "enabled must be 0 or 1");
        usePower(handle, [&](PowerSupply &power) {
            *readback = power.setPowerOutput(channel, enabled != 0);
        });
    });
}

extern "C" int32_t IR_ReadPower(IRHandle handle, int32_t channel,
    IRPowerReading *reading, char *err, int32_t bytes)
noexcept{
    if (reading) *reading = {};
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        usePower(handle, [&](PowerSupply &power) {
            *reading = power.readPower(channel);
        });
    });
}

extern "C" int32_t IR_SetOven(IRHandle handle, double target_c, double *readback_c, char *err, int32_t bytes)
noexcept{
    return guarded(err, bytes, [&] {
        needed(readback_c, 1, 1);
        useOven(handle, [&](GwsStenOven &oven) { *readback_c = oven.set(target_c); });
    });
}

extern "C" int32_t IR_ReadOven(IRHandle handle, IROvenReading *reading, char *err, int32_t bytes)
noexcept{
    if (reading) *reading = {};
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useOven(handle, [&](GwsStenOven &oven) { *reading = oven.read(); });
    });
}

extern "C" int32_t IR_StopOven(IRHandle handle, char *err, int32_t bytes)
noexcept{
    return guarded(err, bytes, [&] {
        useOven(handle, [&](GwsStenOven &oven) { oven.stop(); });
    });
}

extern "C" int32_t IR_SetDmmMode(IRHandle handle, int32_t mode, char *err, int32_t bytes)
noexcept{
    return guarded(err, bytes, [&] {
        useDmm(handle, [&](Dmm6500 &dmm) { dmm.setMode(mode); });
    });
}

extern "C" int32_t IR_ReadDmm(IRHandle handle, int32_t expected_mode, IRDmmReading *reading, char *err, int32_t bytes)
noexcept{
    if (reading) *reading = {};
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useDmm(handle, [&](Dmm6500 &dmm) { *reading = dmm.read(expected_mode); });
    });
}

extern "C" int32_t IR_SetDetectorTiming(IRHandle handle, int32_t integration_us,
    int32_t frame_hz, int32_t empty_us, char *err, int32_t bytes)
noexcept{
    return guarded(err, bytes, [&] {
        useDetector(handle, [&](DetectorSerial &det) {
            det.setTiming(integration_us, frame_hz, empty_us);
        });
    });
}

extern "C" int32_t IR_SetDetectorGpol(IRHandle handle, double gpol_v, char *err, int32_t bytes)
noexcept{
    return guarded(err, bytes, [&] {
        useDetector(handle, [&](DetectorSerial &det) { det.setGpol(gpol_v); });
    });
}

extern "C" int32_t IR_SetDetectorRegister(IRHandle handle, uint8_t command_code,
    const uint8_t *payload, int32_t payload_bytes, char *err, int32_t bytes)
noexcept{
    return guarded(err, bytes, [&] {
        needed(payload, payload_bytes, 1);
        useDetector(handle, [&](DetectorSerial &det) {
            det.setRegister(command_code, payload, payload_bytes);
        });
    });
}

extern "C" int32_t IR_MeasureDetectorCurrent(IRHandle handle, int32_t gain_code,
    uint16_t channel_code, IRDetectorCurrentReading *reading, char *err, int32_t bytes)
noexcept{
    if (reading) *reading = {};
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useDetector(handle, [&](DetectorSerial &det) {
            *reading = det.measureCurrent(gain_code, channel_code);
        });
    });
}

extern "C" int32_t IR_ReadCooler(IRHandle handle, IRCoolerReading *reading, char *err, int32_t bytes)
noexcept{
    if (reading) *reading = {};
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useCooler(handle, [&](CoolerModbus &cooler) {
            *reading = cooler.read();
        });
    });
}
