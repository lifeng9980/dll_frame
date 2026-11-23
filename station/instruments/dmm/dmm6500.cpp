#include "dmm6500.h"
#include "api_common.h"
#include <cctype>
#include <charconv>
#include <cmath>
#include <system_error>
#include <thread>

namespace ir::instruments {
namespace {
using api::Error;
using api::require;
constexpr int ReconnectRetries = 3;
constexpr auto ReconnectDelay = std::chrono::milliseconds(500);

bool transportFailure(const Error &error)
{
    return error.code == IR_IO_ERROR || error.code == IR_TIMEOUT;
}

std::string cleanReply(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    text.erase(0, start);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);
    return text;
}

std::string upper(std::string text)
{
    for (char &ch : text)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return text;
}

int32_t classifyFunction(std::string_view raw)
{
    const auto text = upper(cleanReply(std::string(raw)));
    if (text == "DIOD" || text == "DIODE")
        return IR_DMM_MODE_DIODE;
    if (text == "VOLT" || text == "VOLTAGE" || text == "VOLT:DC" || text == "VOLTAGE:DC")
        return IR_DMM_MODE_DCV;
    if (text == "CURR" || text == "CURRENT" || text == "CURR:DC" || text == "CURRENT:DC")
        return IR_DMM_MODE_DCI;
    if (text == "RES" || text == "RESISTANCE" || text == "FRES" || text == "FRESISTANCE")
        return IR_DMM_MODE_RES;
    return IR_DMM_MODE_ANY;
}

const char *scpiFunctionName(int32_t mode)
{
    switch (mode) {
    case IR_DMM_MODE_DCV: return "VOLT:DC";
    case IR_DMM_MODE_DCI: return "CURR:DC";
    case IR_DMM_MODE_RES: return "RES";
    case IR_DMM_MODE_DIODE: return "DIOD";
    default: return nullptr;
    }
}

double parseNumber(const std::string &reply)
{
    const auto text = cleanReply(reply);
    std::string_view sv = text;
    if (sv.starts_with('+')) sv.remove_prefix(1);
    double val{};
    const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    require(res.ec == std::errc{} && res.ptr == sv.data() + sv.size() && std::isfinite(val),
            IR_IO_ERROR, "DMM6500 :READ? returned invalid number: " + text);
    return val;
}

static_assert(sizeof(IRDmmReading) == 16, "Unexpected IRDmmReading size");
} // namespace

Dmm6500::Dmm6500(Dmm6500Config config) : config_(std::move(config))
{
    connect();
}

void Dmm6500::connect()
{
    link_.reset();
    try {
        if (std::holds_alternative<transport::TcpEndpoint>(config_.endpoint)) {
            link_ = std::make_unique<transport::AsioLink>(
                std::get<transport::TcpEndpoint>(config_.endpoint),
                Clock::now() + config_.timeout);
        } else {
            link_ = std::make_unique<transport::AsioLink>(
                std::get<transport::SerialEndpoint>(config_.endpoint));
        }
        const auto idn = upper(query("*IDN?"));
        require(idn.find("DMM6500") != std::string::npos, IR_IO_ERROR,
                "Device is not a Keithley DMM6500: " + idn);
        const auto lang = upper(query("*LANG?"));
        require(lang == "SCPI", IR_IO_ERROR,
                "DMM6500 command set must be SCPI (got: " + lang + ")");
    } catch (...) {
        link_.reset();
        throw;
    }
}

void Dmm6500::write(std::string_view command)
{
    require(bool(link_), IR_IO_ERROR, "DMM6500 is disconnected");
    const auto deadline = Clock::now() + config_.timeout;
    try {
        link_->discardSerialInput();
        link_->writeAll(std::string(command) + "\n", deadline);
    } catch (const Error &) {
        link_.reset();
        throw;
    } catch (const std::system_error &e) {
        link_.reset();
        throw Error(IR_IO_ERROR, std::string("DMM6500 write failed: ") + e.what());
    }
}

std::string Dmm6500::query(std::string_view command)
{
    require(bool(link_), IR_IO_ERROR, "DMM6500 is disconnected");
    const auto deadline = Clock::now() + config_.timeout;
    std::string reply;
    try {
        link_->discardSerialInput();
        link_->writeAll(std::string(command) + "\n", deadline);
        reply = cleanReply(link_->readDelimited('\n', deadline));
    } catch (const Error &) {
        link_.reset();
        throw;
    } catch (const std::system_error &e) {
        link_.reset();
        throw Error(IR_IO_ERROR, std::string("DMM6500 query failed: ") + e.what());
    }
    require(!reply.empty(), IR_IO_ERROR, std::string("DMM6500 empty response for ") + std::string(command));
    return reply;
}

void Dmm6500::setMode(int32_t mode)
{
    const char *scpi = scpiFunctionName(mode);
    require(scpi != nullptr, IR_BAD_ARGUMENT, "Unsupported DMM6500 measurement mode");
    try {
        if (!link_) connect();
        write(std::string(":SENS:FUNC \"") + scpi + "\"");
        const int32_t actual = classifyFunction(query(":SENS:FUNC?"));
        require(actual == mode, IR_IO_ERROR, "DMM6500 mode readback mismatch after switch");
    } catch (const Error &err) {
        if (transportFailure(err)) {
            link_.reset();
            connect();
            const int32_t actual = classifyFunction(query(":SENS:FUNC?"));
            if (actual == mode) return;
        }
        throw;
    }
}

IRDmmReading Dmm6500::readOnce(int32_t expectedMode)
{
    const int32_t actualMode = classifyFunction(query(":SENS:FUNC?"));
    if (expectedMode != IR_DMM_MODE_ANY && actualMode != expectedMode) {
        throw Error(IR_COMPUTATION_FAILED,
                    "DMM6500 measurement function does not match requested expected_mode");
    }
    const double value = parseNumber(query(":READ?"));
    return {value, actualMode, 1};
}

IRDmmReading Dmm6500::read(int32_t expectedMode)
{
    require(expectedMode >= IR_DMM_MODE_ANY && expectedMode <= IR_DMM_MODE_DIODE,
            IR_BAD_ARGUMENT, "Invalid DMM6500 expected_mode");
    for (int attempt = 0;; ++attempt) {
        try {
            if (!link_) connect();
            return readOnce(expectedMode);
        } catch (const Error &err) {
            if (!transportFailure(err) || attempt >= ReconnectRetries) {
                if (transportFailure(err)) link_.reset();
                throw;
            }
            link_.reset();
            std::this_thread::sleep_for(ReconnectDelay);
        } catch (...) {
            link_.reset();
            throw;
        }
    }
}

} // namespace ir::instruments
