#include "IR_device.h"
#include "api_common.h"
#include "station/instruments/blackbody/doc058.h"
#include "station/instruments/device_lease.h"
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
#include <vector>

namespace {
using Json = nlohmann::json;
using ir::instruments::BlackbodyConfig;
using ir::instruments::Doc058Blackbody;
using namespace ir::api;

struct Session {
    std::vector<std::string> endpointKeys;
    std::mutex mutex;
    std::unique_ptr<Doc058Blackbody> blackbody;
    std::unique_ptr<ir::instruments::It6332aPower> power;
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

std::pair<ir::instruments::It6332aConfig, std::vector<std::string>> parsePowerConfig(const char *config)
{
    const auto json = parseConfigJson(config);
    ir::instruments::It6332aConfig result;
    result.port = text(json, "port", true);
    result.baud = static_cast<unsigned>(integer(json, "baud_rate", 115200, 1200, 921600));
    result.timeout = std::chrono::milliseconds(integer(json, "timeout_ms", 1200, 1, 60000));
    return {std::move(result), {serialEndpointKey(text(json, "port", true))}};
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
        require(kind == IR_PCN7LT || kind == IR_DCN1000 || kind == IR_IT6332A,
                IR_UNSUPPORTED, "Unsupported instrument kind");
        BlackbodyConfig blackbodyOptions;
        ir::instruments::It6332aConfig powerOptions;
        std::vector<std::string> endpointKeys;
        if (kind == IR_IT6332A) {
            auto parsed = parsePowerConfig(config);
            powerOptions = std::move(parsed.first);
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
            devices.erase(pos);
        }
        closingBlackbody.reset();
        closingPower.reset();
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
