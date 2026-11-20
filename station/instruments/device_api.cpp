#include "IR_device.h"
#include "api_common.h"
#include "station/instruments/blackbody/doc058.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <filesystem>
#include <map>
#include <mutex>
#include <utility>

namespace {
using ir::instruments::BlackbodyConfig;
using ir::instruments::Doc058Blackbody;
using namespace ir::api;

struct Session {
    std::string endpoint;
    std::mutex mutex;
    std::unique_ptr<Doc058Blackbody> blackbody;
};
std::mutex devicesMutex;
std::map<IRHandle, std::shared_ptr<Session>> devices;
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
    std::lock_guard lock(device->mutex);
    require(bool(device->blackbody), IR_INVALID_HANDLE, "Device is closed");
    action(*device->blackbody);
}
}

extern "C" int32_t IR_OpenDevice(int32_t kind, const char *config, IRHandle *out, char *err, int32_t bytes)
{
    if (out) *out = 0;
    return guarded(err, bytes, [&] {
        needed(out, 1, 1);
        require(kind == IR_PCN7LT || kind == IR_DCN1000, IR_UNSUPPORTED, "Unsupported instrument kind");
        auto [options, endpoint] = parseConfig(kind, config);
        std::lock_guard lock(devicesMutex);
        for (const auto &[handle, session] : devices)
            require(session->endpoint != endpoint, IR_BUSY, "Instrument endpoint is already open");
        auto device = std::make_shared<Session>();
        device->endpoint = std::move(endpoint);
        device->blackbody = std::make_unique<Doc058Blackbody>(std::move(options));
        const auto handle = nextHandle++;
        devices.emplace(handle, std::move(device));
        *out = handle;
    });
}

extern "C" int32_t IR_CloseDevice(IRHandle handle, char *err, int32_t bytes)
{
    return guarded(err, bytes, [&] {
        std::lock_guard registryLock(devicesMutex);
        const auto pos = findDevice(handle);
        const auto device = pos->second;
        std::lock_guard deviceLock(device->mutex);
        device->blackbody.reset();
        devices.erase(pos);
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
