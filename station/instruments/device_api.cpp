#include "IR_device.h"
#include "api_common.h"
#include "station/instruments/blackbody/doc058.h"
#include "station/instruments/device_lease.h"
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

std::pair<BlackbodyConfig, std::vector<std::string>> parseConfig(int kind, const char *config)
{
    using namespace ir::transport;
    require(config && *config, IR_BAD_ARGUMENT, "Missing connection JSON");
    Json json;
    try {
        json = Json::parse(config);
    } catch (const Json::parse_error &) {
        throw Error(IR_BAD_ARGUMENT, "Invalid connection JSON");
    }
    require(json.is_object(), IR_BAD_ARGUMENT, "Connection JSON must contain an object");

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
#ifdef _WIN32
    auto key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) {
        return character >= 'a' && character <= 'z' ? char(character - 'a' + 'A') : char(character);
    });
    if (key.starts_with(R"(\\.\)")) key.erase(0, 4);
#else
    std::error_code errorCode;
    const auto resolved = std::filesystem::canonical(std::filesystem::path(name), errorCode);
    const auto key = errorCode ? name : resolved.string();
#endif
    return {std::move(result), {"serial:" + key}};
}

auto findDevice(IRHandle handle)
{
    const auto pos = devices.find(handle);
    require(pos != devices.end(), IR_INVALID_HANDLE, "Unknown device handle");
    return pos;
}

template<class Action> void useDevice(IRHandle handle, Action action)
{
    std::shared_ptr<Session> device;
    {
        std::lock_guard lock(devicesMutex);
        device = findDevice(handle)->second;
    }
    std::unique_lock operationLock(device->mutex, std::try_to_lock);
    require(operationLock.owns_lock(), IR_BUSY, "Another instrument operation is active");
    require(device->workflowOwner == std::thread::id{} ||
            device->workflowOwner == std::this_thread::get_id(),
            IR_BUSY, "Instrument is reserved by a measurement procedure");
    require(bool(device->blackbody), IR_INVALID_HANDLE, "Device is closed");
    action(*device->blackbody);
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
        sessionLocks.emplace_back(session->mutex, std::try_to_lock);
        require(sessionLocks.back().owns_lock(), IR_BUSY, "An instrument operation is active");
        require(session->workflowOwner == std::thread::id{}, IR_BUSY, "Instrument is already reserved");
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
        require(kind == IR_PCN7LT || kind == IR_DCN1000, IR_UNSUPPORTED, "Unsupported instrument kind");
        auto [options, endpointKeys] = parseConfig(kind, config);
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
            device->blackbody = std::make_unique<Doc058Blackbody>(std::move(options));
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
        std::unique_ptr<Doc058Blackbody> closing;
        std::unique_lock<std::mutex> deviceLock;
        std::vector<std::string> endpointKeys;
        {
            std::lock_guard registryLock(devicesMutex);
            const auto pos = findDevice(handle);
            device = pos->second;
            deviceLock = std::unique_lock<std::mutex>(device->mutex, std::try_to_lock);
            require(deviceLock.owns_lock(), IR_BUSY, "Instrument operation is active");
            require(device->workflowOwner == std::thread::id{}, IR_BUSY, "Instrument is in use");
            endpointKeys = device->endpointKeys;
            openingEndpoints.insert(endpointKeys.begin(), endpointKeys.end());
            closing = std::move(device->blackbody);
            devices.erase(pos);
        }
        closing.reset();
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
        useDevice(handle, [&](auto &blackbody) { *readback = blackbody.set(target); });
    });
}

extern "C" int32_t IR_ReadBlackbody(IRHandle handle, IRBlackbodyReading *reading, char *err, int32_t bytes)
{
    return guarded(err, bytes, [&] {
        needed(reading, 1, 1);
        useDevice(handle, [&](auto &blackbody) { *reading = blackbody.read(); });
    });
}
