#include "drivers.h"
#include "api_common.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <system_error>

namespace ir::instruments {
namespace {
using api::Error;
using api::require;
constexpr double VoltageToleranceV = 0.05;
constexpr double CurrentToleranceA = 0.02;

std::string upper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

double parseNumber(const std::string &reply, const std::string &command)
{
    double value{};
    const auto parsed = std::from_chars(reply.data(), reply.data() + reply.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != reply.data() + reply.size() || !std::isfinite(value))
        throw Error(IR_IO_ERROR, command + " returned an invalid number: " + reply);
    return value;
}

bool parseOutput(const std::string &reply)
{
    const auto value = upper(reply);
    if (value == "1" || value == "ON") return true;
    if (value == "0" || value == "OFF") return false;
    throw Error(IR_IO_ERROR, "IT6332A returned an invalid output state: " + reply);
}

std::string formatFixed(double value)
{
    if (value == 0.0) value = 0.0;  // Avoid emitting "-0.000000" to SCPI power supplies.
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof buffer, value,
                                         std::chars_format::fixed, 6);
    require(converted.ec == std::errc{}, IR_BAD_ARGUMENT,
            "IT6332A setpoint cannot be formatted");
    return std::string(buffer, converted.ptr);
}
}

It6332aPower::It6332aPower(It6332aConfig config) : config_(std::move(config))
{
    connect();
}

void It6332aPower::connect()
{
    link_.reset();
    try {
        const auto deadline = Clock::now() + config_.timeout;
        if (config_.visaResource.empty())
            link_ = transport::openLink(config_.endpoint, deadline);
        else
            link_ = transport::openVisaLink(config_.visaResource, deadline);
        const auto identity = query("*IDN?");
        require(upper(identity).find("IT6332A") != std::string::npos,
                IR_IO_ERROR, "Device is not an IT6332A: " + identity);
        remote_ = false;
    } catch (const std::system_error &error) {
        link_.reset();
        throw Error(IR_IO_ERROR, std::string("Cannot open IT6332A transport: ") + error.what());
    } catch (...) {
        link_.reset();
        throw;
    }
}

void It6332aPower::ensureRemote()
{
    if (!remote_) {
        write("SYST:REM");
        remote_ = true;
    }
}

void It6332aPower::checkChannel(int channel) const
{
    require(channel >= 1 && channel <= 3, IR_BAD_ARGUMENT, "IT6332A channel must be from 1 to 3");
}

void It6332aPower::write(const std::string &command)
{
    require(bool(link_), IR_IO_ERROR, "IT6332A is disconnected");
    const auto deadline = Clock::now() + config_.timeout;
    try {
        link_->discardSerialInput();
        link_->writeAll(command + "\n", deadline);
    } catch (const std::system_error &error) {
        throw Error(IR_IO_ERROR, std::string("IT6332A serial write failed: ") + error.what());
    }
}

std::string It6332aPower::query(const std::string &command)
{
    require(bool(link_), IR_IO_ERROR, "IT6332A is disconnected");
    const auto deadline = Clock::now() + config_.timeout;
    std::string reply;
    try {
        link_->discardSerialInput();
        link_->writeAll(command + "\n", deadline);
        reply = link_->readDelimited('\n', deadline);
    } catch (const std::system_error &error) {
        throw Error(IR_IO_ERROR, std::string("IT6332A serial query failed: ") + error.what());
    }
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
    if (reply.empty()) throw Error(IR_IO_ERROR, command + " returned an empty response");
    return reply;
}

double It6332aPower::queryNumber(const std::string &command)
{
    return parseNumber(query(command), command);
}

void It6332aPower::selectChannel(int channel)
{
    checkChannel(channel);
    write("INST:NSEL " + std::to_string(channel));
    const auto reply = upper(query("INST:NSEL?"));
    const auto number = std::to_string(channel);
    const auto named = channel == 1 ? "CH1" : channel == 2 ? "CH2" : "CH3";
    require(reply == number || reply == named, IR_IO_ERROR,
            "IT6332A channel selection did not match the requested channel");
}

It6332aReading It6332aPower::readOnce(int channel)
{
    ensureRemote();
    selectChannel(channel);
    It6332aReading reading{};
    reading.setpointVoltageV = queryNumber("VOLT?");
    reading.currentLimitA = queryNumber("CURR?");
    reading.measuredVoltageV = queryNumber("MEAS:VOLT?");
    reading.measuredCurrentA = queryNumber("MEAS:CURR?");
    reading.outputEnabled = parseOutput(query("CHAN:OUTP?"));
    return reading;
}

It6332aReading It6332aPower::read(int channel)
{
    checkChannel(channel);
    for (int attempt = 0; ; ++attempt) {
        try {
            if (!link_) connect();
            return readOnce(channel);
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

It6332aReading It6332aPower::setChannel(int channel, double voltageV, double currentLimitA)
{
    checkChannel(channel);
    require(std::isfinite(voltageV) && voltageV >= 0.0 &&
            std::isfinite(currentLimitA) && currentLimitA > 0.0,
            IR_BAD_ARGUMENT, "IT6332A voltage must be nonnegative and current limit must be positive");
    const auto voltageText = formatFixed(voltageV);
    const auto currentText = formatFixed(currentLimitA);

    try {
        if (!link_) connect();
        ensureRemote();
        selectChannel(channel);
        write("VOLT " + voltageText);
        write("CURR " + currentText);
    } catch (const Error &error) {
        link_.reset();
        if (!transport::isRetryableError(error.code)) throw;
        // A serial write can have reached the instrument before its link failed.
        // Reconnect and verify the state; never resend an uncertain write.
    }

    const auto reading = read(channel);
    require(std::abs(reading.setpointVoltageV - voltageV) <= VoltageToleranceV &&
            std::abs(reading.currentLimitA - currentLimitA) <= CurrentToleranceA,
            IR_IO_ERROR, "IT6332A setpoint readback does not match the requested values");
    return reading;
}

It6332aReading It6332aPower::setOutput(int channel, bool enabled)
{
    checkChannel(channel);
    try {
        if (!link_) connect();
        ensureRemote();
        selectChannel(channel);
        write(enabled ? "CHAN:OUTP ON" : "CHAN:OUTP OFF");
    } catch (const Error &error) {
        link_.reset();
        if (!transport::isRetryableError(error.code)) throw;
        // Confirm after reconnect instead of repeating an output command.
    }

    const auto reading = read(channel);
    require(reading.outputEnabled == enabled, IR_IO_ERROR,
            "IT6332A output state readback does not match the requested state");
    return reading;
}
}
