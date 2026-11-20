#include "IR_device.h"
#include "api_common.h"
#include "station/instruments/blackbody/doc058.h"
#include "station/instruments/device_lease.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {
using ir::instruments::BlackbodyConfig;
using ir::instruments::Doc058Blackbody;
using namespace ir::api;

struct Session {
    std::string endpoint;
    std::mutex mutex;
    std::unique_ptr<Doc058Blackbody> blackbody;
    std::thread::id workflowOwner;
};
std::mutex devicesMutex;
std::map<IRHandle, std::shared_ptr<Session>> devices;
std::set<std::string> openingEndpoints;
IRHandle nextHandle{1};

std::string text(const QJsonObject &json, const char *key, bool required)
{
    const auto value = json.value(QLatin1String(key));
    require((!required && value.isUndefined()) || value.isString(), IR_BAD_ARGUMENT, key);
    const auto bytes = value.toString().trimmed().toUtf8();
    require(!required || !bytes.isEmpty(), IR_BAD_ARGUMENT, key);
    require(!bytes.contains('\0'), IR_BAD_ARGUMENT, key);
    return bytes.toStdString();
}

int integer(const QJsonObject &json, const char *key, int fallback, int low, int high)
{
    const auto value = json.value(QLatin1String(key));
    if (value.isUndefined()) return fallback;
    const auto number = value.toDouble();
    require(value.isDouble() && std::isfinite(number) && number >= low && number <= high &&
            std::trunc(number) == number, IR_BAD_ARGUMENT, key);
    return int(number);
}

std::pair<BlackbodyConfig, std::string> parseConfig(int kind, const char *config)
{
    using namespace ir::transport;
    require(config && *config, IR_BAD_ARGUMENT, "Missing connection JSON");
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(QByteArray(config), &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(), IR_BAD_ARGUMENT, "Invalid connection JSON");
    const auto json = doc.object();

    BlackbodyConfig result;
    result.timeout = std::chrono::milliseconds(integer(json, "timeout_ms", 3000, 1, 60000));
    result.commandInterval = std::chrono::milliseconds(kind == IR_PCN7LT ? 100 : 200);
    result.expectedHead = text(json, "expected_head_serial", false);
    if (kind == IR_PCN7LT) {
        const auto host = text(json, "host", true);
        const auto port = uint16_t(integer(json, "tcp_port", 2101, 1, 65535));
        result.head = integer(json, "head", -1, -1, 1);
        require(result.expectedHead.empty() || result.head >= 0, IR_BAD_ARGUMENT, "Expected head serial requires a head index");
        result.endpoint = TcpEndpoint{host, port};
        return {std::move(result), "tcp:" + QString::fromStdString(host).toCaseFolded().toStdString() + ':' +
            std::to_string(port)};
    }

    const auto name = text(json, "port", true);
    result.endpoint = SerialEndpoint{name, 9600};
#ifdef _WIN32
    auto key = QString::fromStdString(name).toCaseFolded().toStdString();
    if (key.starts_with(R"(\\.\)")) key.erase(0, 4);
#else
    std::error_code errorCode;
    const auto resolved = std::filesystem::canonical(std::filesystem::path(name), errorCode);
    const auto key = errorCode ? name : resolved.string();
#endif
    return {std::move(result), "serial:" + key};
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
        return left->endpoint < right->endpoint;
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
        auto [options, endpoint] = parseConfig(kind, config);
        {
            std::lock_guard lock(devicesMutex);
            for (const auto &[handle, session] : devices)
                require(session->endpoint != endpoint, IR_BUSY, "Instrument endpoint is already open");
            require(openingEndpoints.insert(endpoint).second, IR_BUSY,
                    "Instrument endpoint is being opened");
        }

        std::shared_ptr<Session> device;
        try {
            device = std::make_shared<Session>();
            device->endpoint = endpoint;
            device->blackbody = std::make_unique<Doc058Blackbody>(std::move(options));
        } catch (...) {
            std::lock_guard lock(devicesMutex);
            openingEndpoints.erase(endpoint);
            throw;
        }

        std::lock_guard lock(devicesMutex);
        openingEndpoints.erase(endpoint);
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
        std::string endpoint;
        {
            std::lock_guard registryLock(devicesMutex);
            const auto pos = findDevice(handle);
            device = pos->second;
            deviceLock = std::unique_lock<std::mutex>(device->mutex, std::try_to_lock);
            require(deviceLock.owns_lock(), IR_BUSY, "Instrument operation is active");
            require(device->workflowOwner == std::thread::id{}, IR_BUSY, "Instrument is in use");
            endpoint = device->endpoint;
            openingEndpoints.insert(endpoint);
            closing = std::move(device->blackbody);
            devices.erase(pos);
        }
        closing.reset();
        deviceLock.unlock();
        {
            std::lock_guard registryLock(devicesMutex);
            openingEndpoints.erase(endpoint);
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
