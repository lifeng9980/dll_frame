#include "drivers.h"
#include "api_common.h"
#include "utf8_path.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>

namespace ir::instruments {
namespace {
using api::Error;
using api::require;

std::string upperAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

double parseScpiNumber(const std::string &reply, const std::string &command)
{
    double value{};
    const auto parsed = std::from_chars(reply.data(), reply.data() + reply.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != reply.data() + reply.size() || !std::isfinite(value))
        throw Error(IR_IO_ERROR, command + " returned an invalid number: " + reply);
    return value;
}

bool parseUdpOutput(const std::string &reply)
{
    const auto value = upperAscii(reply);
    if (value == "1" || value == "ON") return true;
    if (value == "0" || value == "OFF") return false;
    throw Error(IR_IO_ERROR, "UDP6730 returned an invalid output state: " + reply);
}

std::string readErrBuf(const char *err, size_t capacity, const char *fallback)
{
    if (!err || capacity == 0) return fallback;
    const size_t len = strnlen(err, capacity - 1);
    if (len == 0) return fallback;
    return std::string(err, len);
}

struct CachedExternalDriver {
#ifdef _WIN32
    HMODULE module{nullptr};
#else
    void *module{nullptr};
#endif
    const IRPowerDriverDescriptor *desc{nullptr};
};

std::mutex externalDriversMutex;
std::map<std::string, CachedExternalDriver> externalDrivers;

std::string normalizeDriverKey(const std::string &driverPath)
{
    std::error_code ec;
    const auto p = ir::utf8::pathFromUtf8(driverPath);
    const auto norm = std::filesystem::weakly_canonical(p, ec);
    const auto chosen = ec ? p.lexically_normal() : norm;
    auto key = ir::utf8::pathToUtf8(chosen);
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return key;
}

const IRPowerDriverDescriptor *loadExternalDriver(const std::string &driverPath,
                                                  const std::string &expectedDriverId,
                                                  std::string_view errorContext)
{
    require(!driverPath.empty(), IR_BAD_ARGUMENT,
            std::string(errorContext) + ".driver_path must be nonempty");
    const std::string key = normalizeDriverKey(driverPath);

    std::lock_guard lock(externalDriversMutex);
    auto it = externalDrivers.find(key);
    if (it == externalDrivers.end()) {
        const auto nativePath = ir::utf8::pathFromUtf8(driverPath);
#ifdef _WIN32
        HMODULE mod = LoadLibraryW(nativePath.c_str());
        require(mod != nullptr, IR_IO_ERROR,
                "Cannot load external power driver '" + driverPath + "' (Win32 error " +
                std::to_string(GetLastError()) + ")");
        auto queryFn = reinterpret_cast<IRPowerDriverQueryFn>(
            GetProcAddress(mod, IR_POWER_DRIVER_QUERY_SYMBOL));
        if (!queryFn) {
            FreeLibrary(mod);
            throw Error(IR_BAD_ARGUMENT,
                        "External power driver '" + driverPath + "' is missing " IR_POWER_DRIVER_QUERY_SYMBOL);
        }
#else
        void *mod = dlopen(nativePath.c_str(), RTLD_NOW | RTLD_LOCAL);
        require(mod != nullptr, IR_IO_ERROR,
                "Cannot load external power driver '" + driverPath + "': " +
                (dlerror() ? dlerror() : "unknown error"));
        auto queryFn = reinterpret_cast<IRPowerDriverQueryFn>(
            dlsym(mod, IR_POWER_DRIVER_QUERY_SYMBOL));
        if (!queryFn) {
            dlclose(mod);
            throw Error(IR_BAD_ARGUMENT,
                        "External power driver '" + driverPath + "' is missing " IR_POWER_DRIVER_QUERY_SYMBOL);
        }
#endif
        const IRPowerDriverDescriptor *desc = nullptr;
        char err[512]{};
        const int32_t rc = queryFn(&desc, err, static_cast<int32_t>(sizeof(err)));
        err[sizeof(err) - 1] = '\0';
        if (rc != IR_OK || !desc) {
#ifdef _WIN32
            FreeLibrary(mod);
#else
            dlclose(mod);
#endif
            throw Error(rc == IR_OK ? IR_BAD_ARGUMENT : static_cast<IRStatus>(rc),
                        readErrBuf(err, sizeof(err), "External power driver query failed"));
        }

        const bool validDesc =
            desc->abi_version == IR_POWER_DRIVER_ABI_VERSION &&
            desc->struct_size >= sizeof(IRPowerDriverDescriptor) &&
            desc->driver_id != nullptr && *desc->driver_id != '\0' &&
            desc->min_channel >= 1 && desc->max_channel >= desc->min_channel &&
            desc->validate_config != nullptr && desc->open != nullptr && desc->close != nullptr &&
            (!(desc->capabilities & IR_POWER_CAP_READ) || desc->read != nullptr) &&
            (!(desc->capabilities & IR_POWER_CAP_OUTPUT) || desc->set_output != nullptr) &&
            (!(desc->capabilities & IR_POWER_CAP_SET) || desc->set_channel != nullptr);

        if (!validDesc) {
#ifdef _WIN32
            FreeLibrary(mod);
#else
            dlclose(mod);
#endif
            throw Error(IR_UNSUPPORTED,
                        "External power driver '" + driverPath + "' returned an invalid descriptor");
        }
        it = externalDrivers.emplace(key, CachedExternalDriver{mod, desc}).first;
    }

    const IRPowerDriverDescriptor *desc = it->second.desc;
    if (!expectedDriverId.empty()) {
        require(expectedDriverId == desc->driver_id, IR_BAD_ARGUMENT,
                std::string(errorContext) + ".driver_id expected '" + expectedDriverId +
                "', got '" + desc->driver_id + "'");
    }
    return desc;
}

} // namespace

// ======================== Udp6730Power ========================

Udp6730Power::Udp6730Power(Udp6730Config config) : config_(std::move(config))
{
    connect();
}

void Udp6730Power::connect()
{
    link_.reset();
    try {
        link_ = transport::openLink(config_.endpoint, Clock::now() + config_.timeout);
        const auto identity = query("*IDN?");
        require(upperAscii(identity).find("UDP6730") != std::string::npos,
                IR_IO_ERROR, "Device is not a UDP6730: " + identity);
    } catch (const std::system_error &error) {
        link_.reset();
        throw Error(IR_IO_ERROR, std::string("Cannot open UDP6730 transport: ") + error.what());
    } catch (...) {
        link_.reset();
        throw;
    }
}

void Udp6730Power::checkChannel(int channel) const
{
    require(channel == 1, IR_BAD_ARGUMENT, "UDP6730 only supports channel 1");
}

void Udp6730Power::write(const std::string &command)
{
    require(bool(link_), IR_IO_ERROR, "UDP6730 is disconnected");
    const auto deadline = Clock::now() + config_.timeout;
    try {
        link_->discardSerialInput();
        link_->writeAll(command + "\n", deadline);
    } catch (const std::system_error &error) {
        throw Error(IR_IO_ERROR, std::string("UDP6730 write failed: ") + error.what());
    }
}

std::string Udp6730Power::query(const std::string &command)
{
    require(bool(link_), IR_IO_ERROR, "UDP6730 is disconnected");
    const auto deadline = Clock::now() + config_.timeout;
    std::string reply;
    try {
        link_->discardSerialInput();
        link_->writeAll(command + "\n", deadline);
        reply = link_->readDelimited('\n', deadline);
    } catch (const std::system_error &error) {
        throw Error(IR_IO_ERROR, std::string("UDP6730 query failed: ") + error.what());
    }
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
    if (reply.empty()) throw Error(IR_IO_ERROR, command + " returned an empty response");
    return reply;
}

double Udp6730Power::queryNumber(const std::string &command)
{
    return parseScpiNumber(query(command), command);
}

IRPowerReading Udp6730Power::readOnce()
{
    IRPowerReading reading{};
    reading.output_enabled = parseUdpOutput(query("OUTPut?")) ? 1 : 0;
    reading.setpoint_v = queryNumber("VOLTage?");
    reading.current_limit_a = queryNumber("CURRent?");
    reading.measured_v = queryNumber("MEASure:VOLTage?");
    reading.measured_a = queryNumber("MEASure:CURRent?");
    reading.reserved = 0;
    return reading;
}

IRPowerReading Udp6730Power::readPower(int channel)
{
    checkChannel(channel);
    for (int attempt = 0; ; ++attempt) {
        try {
            if (!link_) connect();
            return readOnce();
        } catch (const Error &error) {
            link_.reset();
            if (!transport::isRetryableError(error.code) || attempt >= transport::ReconnectRetries) throw;
            transport::waitFor(transport::ReconnectDelay);
        } catch (...) {
            link_.reset();
            throw;
        }
    }
}

IRPowerReading Udp6730Power::setPowerOutput(int channel, bool enabled)
{
    checkChannel(channel);
    try {
        if (!link_) connect();
        write(enabled ? "OUTPut ON" : "OUTPut OFF");
    } catch (const Error &error) {
        link_.reset();
        if (!transport::isRetryableError(error.code)) throw;
    }
    const auto reading = readPower(channel);
    require((reading.output_enabled != 0) == enabled, IR_IO_ERROR,
            "UDP6730 output state readback does not match the requested state");
    return reading;
}

IRPowerReading Udp6730Power::setPowerChannel(int channel, double voltageV, double currentLimitA)
{
    (void)channel;
    (void)voltageV;
    (void)currentLimitA;
    throw Error(IR_UNSUPPORTED, "UDP6730 does not support power_set");
}

// ======================== ExternalPowerSupply ========================

ExternalPowerInspection inspectExternalPowerDriver(const ExternalPowerConfig &config,
                                                   std::string_view path)
{
    const auto *desc = loadExternalDriver(config.driverPath, config.expectedDriverId, path);
    char err[512]{};
    const int32_t rc = desc->validate_config(config.driverConfigJson.c_str(), err,
                                             static_cast<int32_t>(sizeof(err)));
    err[sizeof(err) - 1] = '\0';
    if (rc != IR_OK) {
        throw Error(static_cast<IRStatus>(rc),
                    readErrBuf(err, sizeof(err), "External power driver config validation failed"));
    }
    return {desc->capabilities, desc->min_channel, desc->max_channel};
}

ExternalPowerSupply::ExternalPowerSupply(ExternalPowerConfig config)
    : config_(std::move(config))
{
    desc_ = loadExternalDriver(config_.driverPath, config_.expectedDriverId, "connection");
    capabilities_ = desc_->capabilities;
    minChannel_ = desc_->min_channel;
    maxChannel_ = desc_->max_channel;

    char identity[256]{};
    char err[512]{};
    const int32_t rc = desc_->open(config_.driverConfigJson.c_str(), &handle_,
                                   identity, static_cast<int32_t>(sizeof(identity)),
                                   err, static_cast<int32_t>(sizeof(err)));
    err[sizeof(err) - 1] = '\0';
    if (rc != IR_OK || !handle_) {
        handle_ = nullptr;
        throw Error(rc == IR_OK ? IR_IO_ERROR : static_cast<IRStatus>(rc),
                    readErrBuf(err, sizeof(err), "External power driver open failed"));
    }
}

ExternalPowerSupply::~ExternalPowerSupply()
{
    if (!closed_ && handle_ && desc_ && desc_->close) {
        char err[256]{};
        desc_->close(handle_, err, static_cast<int32_t>(sizeof(err)));
        handle_ = nullptr;
        closed_ = true;
    }
}

void ExternalPowerSupply::checkChannel(int channel) const
{
    require(channel >= minChannel_ && channel <= maxChannel_, IR_BAD_ARGUMENT,
            "Power channel " + std::to_string(channel) + " is outside driver range [" +
            std::to_string(minChannel_) + ", " + std::to_string(maxChannel_) + "]");
}

IRPowerReading ExternalPowerSupply::readPower(int channel)
{
    require(!closed_ && handle_, IR_INVALID_HANDLE, "External power driver is closed");
    require((capabilities_ & IR_POWER_CAP_READ) != 0 && desc_->read != nullptr,
            IR_UNSUPPORTED, "Power driver does not support read");
    checkChannel(channel);

    IRPowerReading reading{};
    char err[512]{};
    const int32_t rc = desc_->read(handle_, channel, &reading, err, static_cast<int32_t>(sizeof(err)));
    err[sizeof(err) - 1] = '\0';
    if (rc != IR_OK) {
        throw Error(static_cast<IRStatus>(rc),
                    readErrBuf(err, sizeof(err), "External power driver read failed"));
    }
    return reading;
}

IRPowerReading ExternalPowerSupply::setPowerOutput(int channel, bool enabled)
{
    require(!closed_ && handle_, IR_INVALID_HANDLE, "External power driver is closed");
    require((capabilities_ & IR_POWER_CAP_OUTPUT) != 0 && desc_->set_output != nullptr,
            IR_UNSUPPORTED, "Power driver does not support power_output");
    checkChannel(channel);

    IRPowerReading reading{};
    char err[512]{};
    const int32_t rc = desc_->set_output(handle_, channel, enabled ? 1 : 0, &reading,
                                         err, static_cast<int32_t>(sizeof(err)));
    err[sizeof(err) - 1] = '\0';
    if (rc != IR_OK) {
        throw Error(static_cast<IRStatus>(rc),
                    readErrBuf(err, sizeof(err), "External power driver set_output failed"));
    }
    return reading;
}

IRPowerReading ExternalPowerSupply::setPowerChannel(int channel, double voltageV, double currentLimitA)
{
    require(!closed_ && handle_, IR_INVALID_HANDLE, "External power driver is closed");
    require((capabilities_ & IR_POWER_CAP_SET) != 0 && desc_->set_channel != nullptr,
            IR_UNSUPPORTED, "Power driver does not support power_set");
    checkChannel(channel);

    IRPowerReading reading{};
    char err[512]{};
    const int32_t rc = desc_->set_channel(handle_, channel, voltageV, currentLimitA, &reading,
                                          err, static_cast<int32_t>(sizeof(err)));
    err[sizeof(err) - 1] = '\0';
    if (rc != IR_OK) {
        throw Error(static_cast<IRStatus>(rc),
                    readErrBuf(err, sizeof(err), "External power driver set_channel failed"));
    }
    return reading;
}

void ExternalPowerSupply::close()
{
    if (closed_ || !handle_) return;
    closed_ = true;
    IRPowerDriverHandle handle = handle_;
    handle_ = nullptr;
    char err[512]{};
    const int32_t rc = desc_->close(handle, err, static_cast<int32_t>(sizeof(err)));
    err[sizeof(err) - 1] = '\0';
    if (rc != IR_OK) {
        throw Error(static_cast<IRStatus>(rc),
                    readErrBuf(err, sizeof(err), "External power driver close failed"));
    }
}

} // namespace ir::instruments
