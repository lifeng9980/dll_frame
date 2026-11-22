#include "IR_device.h"
#include "api_common.h"
#include "station/instruments/blackbody/doc058.h"
#include "station/instruments/cooler/cooler_modbus.h"
#include "station/instruments/detector/detector_serial.h"
#include "station/instruments/device_lease.h"
#include "station/instruments/dmm/dmm6500.h"
#include "station/instruments/oven/gws_sten.h"
#include "station/instruments/power/it6332a.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
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
using ir::instruments::GwsStenConfig;
using ir::instruments::GwsStenOven;
using namespace ir::api;

struct Session {
    std::vector<std::string> endpointKeys;
    std::mutex mutex;
    std::unique_ptr<Doc058Blackbody> blackbody;
    std::unique_ptr<ir::instruments::It6332aPower> power;
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

std::string text(const Json &json, const char *key, bool required)
{
    const auto value = json.find(key);
    if (value == json.end()) {
        require(!required, IR_BAD_ARGUMENT, key);
        return {};
    }
    require(value->is_string(), IR_BAD_ARGUMENT, key);
    auto result = value->get<std::string>();
    const auto whitespace = [](unsigned char character) { return std::isspace(character) != 0; };
    const auto first = std::find_if_not(result.begin(), result.end(), whitespace);
    const auto last = std::find_if_not(result.rbegin(), result.rend(), whitespace).base();
    result = first < last ? std::string(first, last) : std::string{};
    require(!required || !result.empty(), IR_BAD_ARGUMENT, key);
    require(result.find('\0') == std::string::npos, IR_BAD_ARGUMENT, key);
    return result;
}

int integer(const Json &json, const char *key, int fallback, int low, int high)
{
    const auto value = json.find(key);
    if (value == json.end()) return fallback;
    require(value->is_number(), IR_BAD_ARGUMENT, key);
    const auto number = value->get<double>();
    require(std::isfinite(number) && number >= low && number <= high &&
            std::trunc(number) == number, IR_BAD_ARGUMENT, key);
    return int(number);
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
    const auto resolved = std::filesystem::canonical(std::filesystem::path(name), errorCode);
    return "serial:" + (errorCode ? name : resolved.string());
#endif
}

Json parseConfigJson(const char *config)
{
    require(config && *config, IR_BAD_ARGUMENT, "Missing connection JSON");
    Json json;
    try {
        json = Json::parse(config);
    } catch (const Json::parse_error &) {
        throw Error(IR_BAD_ARGUMENT, "Invalid connection JSON");
    }
    require(json.is_object(), IR_BAD_ARGUMENT, "Connection JSON must contain an object");
    return json;
}

std::pair<BlackbodyConfig, std::vector<std::string>> parseBlackbodyConfig(int kind, const char *config)
{
    using namespace ir::transport;
    const auto json = parseConfigJson(config);

    BlackbodyConfig result;
    result.timeout = std::chrono::milliseconds(integer(json, "timeout_ms", 3000, 1, 60000));
    result.commandInterval = std::chrono::milliseconds(kind == IR_PCN7LT ? 100 : 200);
    result.expectedHead = text(json, "expected_head_serial", false);
    if (kind == IR_PCN7LT) {
        const auto host = text(json, "host", true);
        const auto port = uint16_t(integer(json, "tcp_port", 2101, 1, 65535));
        result.head = integer(json, "head", -1, -1, 1);
        require(result.expectedHead.empty() || result.head >= 0, IR_BAD_ARGUMENT, "Expected head serial requires a head index");
        TcpEndpoint endpoint{host, port};
        endpoint.resolvedAddresses = resolveTcpAddresses(endpoint, std::chrono::steady_clock::now() + result.timeout);
        std::vector<std::string> keys;
        keys.reserve(endpoint.resolvedAddresses.size());
        for (const auto &address : endpoint.resolvedAddresses) {
            const auto formattedAddress = address.find(':') == std::string::npos ? address : '[' + address + ']';
            keys.push_back("tcp:" + formattedAddress + ':' + std::to_string(port));
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        result.endpoint = std::move(endpoint);
        return {std::move(result), std::move(keys)};
    }

    const auto name = text(json, "port", true);
    result.endpoint = SerialEndpoint{name, 9600};
    return {std::move(result), {serialEndpointKey(name)}};
}

double floating(const Json &json, const char *key, double fallback)
{
    const auto value = json.find(key);
    if (value == json.end()) return fallback;
    require(value->is_number(), IR_BAD_ARGUMENT, key);
    const auto num = value->get<double>();
    require(std::isfinite(num), IR_BAD_ARGUMENT, key);
    return num;
}

int parseParity(const std::string &parityText)
{
    std::string lower = parityText;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.empty() || lower == "none") return 0;
    if (lower == "odd") return 1;
    if (lower == "even") return 2;
    throw Error(IR_BAD_ARGUMENT, "Invalid serial parity (expected none, odd, or even)");
}

std::pair<std::variant<ir::transport::TcpEndpoint, ir::transport::SerialEndpoint>, std::vector<std::string>>
parseTcpOrSerialEndpoint(const Json &json, uint16_t defaultTcpPort, unsigned defaultBaud,
                         std::chrono::milliseconds resolveTimeout, int defaultStopBits = 1)
{
    using namespace ir::transport;
    const bool hasHost = json.contains("host");
    const bool hasPort = json.contains("port");
    require(hasHost != hasPort, IR_BAD_ARGUMENT, "Connection JSON must specify either 'host' or 'port'");
    if (hasHost) {
        const auto host = text(json, "host", true);
        const auto tcpPort = static_cast<uint16_t>(integer(json, "tcp_port", defaultTcpPort, 1, 65535));
        TcpEndpoint ep{host, tcpPort};
        ep.resolvedAddresses = resolveTcpAddresses(ep, std::chrono::steady_clock::now() + resolveTimeout);
        std::vector<std::string> keys;
        keys.reserve(ep.resolvedAddresses.size());
        for (const auto &address : ep.resolvedAddresses) {
            const auto fmt = address.find(':') == std::string::npos ? address : '[' + address + ']';
            keys.push_back("tcp:" + fmt + ':' + std::to_string(tcpPort));
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        return {std::move(ep), std::move(keys)};
    }
    const auto portName = text(json, "port", true);
    SerialEndpoint sp;
    sp.port = portName;
    sp.baud = static_cast<unsigned>(integer(json, "baud_rate", static_cast<int>(defaultBaud), 1200, 921600));
    sp.dataBits = static_cast<unsigned>(integer(json, "data_bits", 8, 5, 8));
    sp.parity = parseParity(text(json, "parity", false));
    sp.stopBits = integer(json, "stop_bits", defaultStopBits, 1, 2);
    return {std::move(sp), {serialEndpointKey(portName)}};
}

std::pair<ir::instruments::It6332aConfig, std::vector<std::string>> parsePowerConfig(const char *config)
{
    const auto json = parseConfigJson(config);
    ir::instruments::It6332aConfig result;
    result.timeout = std::chrono::milliseconds(integer(json, "timeout_ms", 1200, 1, 60000));
    auto [ep, keys] = parseTcpOrSerialEndpoint(json, 5026, 115200, result.timeout, 1);
    result.endpoint = std::move(ep);
    return {std::move(result), std::move(keys)};
}

std::pair<GwsStenConfig, std::vector<std::string>> parseOvenConfig(const char *config)
{
    const auto json = parseConfigJson(config);
    GwsStenConfig result;
    result.timeout = std::chrono::milliseconds(integer(json, "timeout_ms", 2000, 1, 60000));
    result.address = integer(json, "address", 1, 1, 255);
    result.minSetpointC = floating(json, "min_setpoint_c", -45.0);
    result.maxSetpointC = floating(json, "max_setpoint_c", 70.0);
    require(result.minSetpointC <= result.maxSetpointC, IR_BAD_ARGUMENT,
            "min_setpoint_c must be <= max_setpoint_c");
    auto delim = text(json, "delimiter", false);
    std::transform(delim.begin(), delim.end(), delim.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (delim.empty() || delim == "crlf") result.delimiter = "\r\n";
    else if (delim == "cr") result.delimiter = "\r";
    else if (delim == "lf") result.delimiter = "\n";
    else throw Error(IR_BAD_ARGUMENT, "Unsupported oven delimiter (expected crlf, cr, or lf)");
    auto [ep, keys] = parseTcpOrSerialEndpoint(json, 5000, 9600, result.timeout);
    result.endpoint = std::move(ep);
    return {std::move(result), std::move(keys)};
}

std::pair<Dmm6500Config, std::vector<std::string>> parseDmmConfig(const char *config)
{
    const auto json = parseConfigJson(config);
    Dmm6500Config result;
    result.timeout = std::chrono::milliseconds(integer(json, "timeout_ms", 2000, 1, 60000));
    auto [ep, keys] = parseTcpOrSerialEndpoint(json, 5025, 115200, result.timeout);
    result.endpoint = std::move(ep);
    return {std::move(result), std::move(keys)};
}

std::pair<DetectorSerialConfig, std::vector<std::string>> parseDetectorConfig(const char *config)
{
    const auto json = parseConfigJson(config);
    DetectorSerialConfig result;
    result.timeout = std::chrono::milliseconds(integer(json, "timeout_ms", 1200, 1, 60000));
    result.interCommandDelay = std::chrono::milliseconds(integer(json, "inter_command_ms", 0, 0, 2000));
    auto [ep, keys] = parseTcpOrSerialEndpoint(json, 2102, 115200, result.timeout);
    result.endpoint = std::move(ep);
    return {std::move(result), std::move(keys)};
}

std::pair<CoolerModbusConfig, std::vector<std::string>> parseCoolerConfig(const char *config)
{
    const auto json = parseConfigJson(config);
    CoolerModbusConfig result;
    result.timeout = std::chrono::milliseconds(integer(json, "timeout_ms", 2000, 1, 60000));
    result.slaveAddress = static_cast<uint8_t>(integer(json, "slave_address", 1, 1, 247));
    result.registerAddress = static_cast<uint16_t>(integer(json, "register_address", 0x7A52, 0, 65535));
    auto [ep, keys] = parseTcpOrSerialEndpoint(json, 5020, 9600, result.timeout, 2);
    result.endpoint = std::move(ep);
    return {std::move(result), std::move(keys)};
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
        require(bool(device.power), IR_BAD_ARGUMENT, "Device handle is not an IT6332A power supply");
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
{
    if (out) *out = 0;
    return guarded(err, bytes, [&] {
        needed(out, 1, 1);
        require(kind == IR_PCN7LT || kind == IR_DCN1000 || kind == IR_IT6332A ||
                kind == IR_OVEN_GWS_STEN || kind == IR_DMM6500 || kind == IR_DETECTOR_SERIAL ||
                kind == IR_COOLER_MODBUS,
                IR_UNSUPPORTED, "Unsupported instrument kind");
        BlackbodyConfig blackbodyOptions;
        ir::instruments::It6332aConfig powerOptions;
        GwsStenConfig ovenOptions;
        Dmm6500Config dmmOptions;
        DetectorSerialConfig detectorOptions;
        CoolerModbusConfig coolerOptions;
        std::vector<std::string> endpointKeys;
        if (kind == IR_IT6332A) {
            auto parsed = parsePowerConfig(config);
            powerOptions = std::move(parsed.first);
            endpointKeys = std::move(parsed.second);
        } else if (kind == IR_OVEN_GWS_STEN) {
            auto parsed = parseOvenConfig(config);
            ovenOptions = std::move(parsed.first);
            endpointKeys = std::move(parsed.second);
        } else if (kind == IR_DMM6500) {
            auto parsed = parseDmmConfig(config);
            dmmOptions = std::move(parsed.first);
            endpointKeys = std::move(parsed.second);
        } else if (kind == IR_DETECTOR_SERIAL) {
            auto parsed = parseDetectorConfig(config);
            detectorOptions = std::move(parsed.first);
            endpointKeys = std::move(parsed.second);
        } else if (kind == IR_COOLER_MODBUS) {
            auto parsed = parseCoolerConfig(config);
            coolerOptions = std::move(parsed.first);
            endpointKeys = std::move(parsed.second);
        } else {
            auto parsed = parseBlackbodyConfig(kind, config);
            blackbodyOptions = std::move(parsed.first);
            endpointKeys = std::move(parsed.second);
        }
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
            if (kind == IR_IT6332A)
                device->power = std::make_unique<ir::instruments::It6332aPower>(std::move(powerOptions));
            else if (kind == IR_OVEN_GWS_STEN)
                device->oven = std::make_unique<GwsStenOven>(std::move(ovenOptions));
            else if (kind == IR_DMM6500)
                device->dmm = std::make_unique<Dmm6500>(std::move(dmmOptions));
            else if (kind == IR_DETECTOR_SERIAL)
                device->detector = std::make_unique<DetectorSerial>(std::move(detectorOptions));
            else if (kind == IR_COOLER_MODBUS)
                device->cooler = std::make_unique<CoolerModbus>(std::move(coolerOptions));
            else
                device->blackbody = std::make_unique<Doc058Blackbody>(std::move(blackbodyOptions));
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
{
    return guarded(err, bytes, [&] {
        std::shared_ptr<Session> device;
        std::unique_ptr<Doc058Blackbody> closingBlackbody;
        std::unique_ptr<ir::instruments::It6332aPower> closingPower;
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
        closingPower.reset();
        closingOven.reset();
        closingDmm.reset();
        closingDetector.reset();
        closingCooler.reset();
        deviceLock.unlock();
        {
            std::lock_guard registryLock(devicesMutex);
            for (const auto &endpoint : endpointKeys) openingEndpoints.erase(endpoint);
        }
    });
}

extern "C" int32_t IR_SetBlackbody(IRHandle handle, double target, double *readback, char *err, int32_t bytes)
{
    return guarded(err, bytes, [&] {
        needed(readback, 1, 1);
        useBlackbody(handle, [&](auto &blackbody) { *readback = blackbody.set(target); });
    });
}

extern "C" int32_t IR_ReadBlackbody(IRHandle handle, IRBlackbodyReading *reading, char *err, int32_t bytes)
{
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useBlackbody(handle, [&](auto &blackbody) { *reading = blackbody.read(); });
    });
}

extern "C" int32_t IR_SetPowerChannel(IRHandle handle, int32_t channel,
    double voltage, double currentLimit, IRPowerReading *readback, char *err, int32_t bytes)
{
    if (readback) *readback = {};
    return guarded(err, bytes, [&] {
        needed(readback, 1, 1);
        usePower(handle, [&](auto &power) {
            const auto value = power.setChannel(channel, voltage, currentLimit);
            *readback = {value.setpointVoltageV, value.currentLimitA, value.measuredVoltageV,
                         value.measuredCurrentA, value.outputEnabled ? 1 : 0};
        });
    });
}

extern "C" int32_t IR_SetPowerOutput(IRHandle handle, int32_t channel, int32_t enabled,
    IRPowerReading *readback, char *err, int32_t bytes)
{
    if (readback) *readback = {};
    return guarded(err, bytes, [&] {
        needed(readback, 1, 1);
        require(enabled == 0 || enabled == 1, IR_BAD_ARGUMENT, "enabled must be 0 or 1");
        usePower(handle, [&](auto &power) {
            const auto value = power.setOutput(channel, enabled != 0);
            *readback = {value.setpointVoltageV, value.currentLimitA, value.measuredVoltageV,
                         value.measuredCurrentA, value.outputEnabled ? 1 : 0};
        });
    });
}

extern "C" int32_t IR_ReadPower(IRHandle handle, int32_t channel,
    IRPowerReading *reading, char *err, int32_t bytes)
{
    if (reading) *reading = {};
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        usePower(handle, [&](auto &power) {
            const auto value = power.read(channel);
            *reading = {value.setpointVoltageV, value.currentLimitA, value.measuredVoltageV,
                        value.measuredCurrentA, value.outputEnabled ? 1 : 0};
        });
    });
}

extern "C" int32_t IR_SetOven(IRHandle handle, double target_c, double *readback_c, char *err, int32_t bytes)
{
    return guarded(err, bytes, [&] {
        needed(readback_c, 1, 1);
        useOven(handle, [&](GwsStenOven &oven) { *readback_c = oven.set(target_c); });
    });
}

extern "C" int32_t IR_ReadOven(IRHandle handle, IROvenReading *reading, char *err, int32_t bytes)
{
    if (reading) *reading = {};
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useOven(handle, [&](GwsStenOven &oven) { *reading = oven.read(); });
    });
}

extern "C" int32_t IR_StopOven(IRHandle handle, char *err, int32_t bytes)
{
    return guarded(err, bytes, [&] {
        useOven(handle, [&](GwsStenOven &oven) { oven.stop(); });
    });
}

extern "C" int32_t IR_SetDmmMode(IRHandle handle, int32_t mode, char *err, int32_t bytes)
{
    return guarded(err, bytes, [&] {
        useDmm(handle, [&](Dmm6500 &dmm) { dmm.setMode(mode); });
    });
}

extern "C" int32_t IR_ReadDmm(IRHandle handle, int32_t expected_mode, IRDmmReading *reading, char *err, int32_t bytes)
{
    if (reading) *reading = {};
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useDmm(handle, [&](Dmm6500 &dmm) { *reading = dmm.read(expected_mode); });
    });
}

extern "C" int32_t IR_SetDetectorTiming(IRHandle handle, int32_t integration_us,
    int32_t frame_hz, int32_t empty_us, char *err, int32_t bytes)
{
    return guarded(err, bytes, [&] {
        useDetector(handle, [&](DetectorSerial &det) {
            det.setTiming(integration_us, frame_hz, empty_us);
        });
    });
}

extern "C" int32_t IR_SetDetectorGpol(IRHandle handle, double gpol_v, char *err, int32_t bytes)
{
    return guarded(err, bytes, [&] {
        useDetector(handle, [&](DetectorSerial &det) { det.setGpol(gpol_v); });
    });
}

extern "C" int32_t IR_SetDetectorRegister(IRHandle handle, uint8_t command_code,
    const uint8_t *payload, int32_t payload_bytes, char *err, int32_t bytes)
{
    return guarded(err, bytes, [&] {
        needed(payload, payload_bytes, 1);
        useDetector(handle, [&](DetectorSerial &det) {
            det.setRegister(command_code, payload, payload_bytes);
        });
    });
}

extern "C" int32_t IR_MeasureDetectorCurrent(IRHandle handle, int32_t gain_code,
    uint16_t channel_code, IRDetectorCurrentReading *reading, char *err, int32_t bytes)
{
    if (reading) *reading = {};
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useDetector(handle, [&](DetectorSerial &det) {
            *reading = det.measureCurrent(gain_code, channel_code);
        });
    });
}

extern "C" int32_t IR_ReadCooler(IRHandle handle, IRCoolerReading *reading, char *err, int32_t bytes)
{
    if (reading) *reading = {};
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useCooler(handle, [&](CoolerModbus &cooler) {
            *reading = cooler.read();
        });
    });
}
