#include "doc058.h"
#include "api_common.h"
#include <charconv>
#include <cctype>
#include <cmath>
#include <thread>
#include <utility>

namespace ir::instruments {
namespace {
using api::Error;
using api::require;
using Clock = std::chrono::steady_clock;
constexpr auto Escape = '\x1b';
constexpr auto Return = '\r';
constexpr auto NegativeAck = '\x15';
constexpr int ReconnectRetries = 3;
constexpr auto ReconnectDelay = std::chrono::milliseconds(500);
constexpr uint8_t Name = 135, Serial = 136, HeadSerial = 56, Mode = 73, Temperature = 114,
                  Setpoint = 97, LowLimit = 67, HighLimit = 66, Stable = 92;

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string uppercase(std::string_view s)
{
    std::string out(s);
    for (auto &c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool pcnIdentity(std::string_view name)
{
    auto compact = uppercase(name);
    std::erase_if(compact, [](unsigned char c) { return !std::isalnum(c); });
    return compact.ends_with("PCN7LT") || compact.ends_with("PCN7LTDH") ||
           compact.ends_with("PCN7LTHEAD") || compact.ends_with("PCN7LTDHHEAD");
}

double numeric(std::string_view s)
{
    s = trim(s.substr(0, s.find(',')));
    double value{};
    if (s.starts_with('+')) s.remove_prefix(1);
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    require(result.ec == std::errc{} && result.ptr == s.data() + s.size() && std::isfinite(value),
            IR_IO_ERROR, "Invalid numeric DOC058 response");
    return value;
}

bool transportError(const Error &error)
{
    return error.code == IR_IO_ERROR || error.code == IR_TIMEOUT;
}
}

Doc058Blackbody::Doc058Blackbody(BlackbodyConfig config) : config_(std::move(config)) { open(); }

bool Doc058Blackbody::tcp() const { return std::holds_alternative<transport::TcpEndpoint>(config_.endpoint); }

void Doc058Blackbody::open()
{
    try {
        if (tcp())
            link_ = std::make_unique<transport::AsioLink>(std::get<transport::TcpEndpoint>(config_.endpoint),
                                                         Clock::now() + config_.timeout);
        else
            link_ = std::make_unique<transport::AsioLink>(std::get<transport::SerialEndpoint>(config_.endpoint));
    } catch (...) {
        reconnectable_ = true;
        throw;
    }
    try {
        const auto name = request(Name);
        require(tcp() ? pcnIdentity(name) : uppercase(name).find("DCN1000") != std::string::npos,
                IR_IO_ERROR, "Blackbody controller identity mismatch");
        const auto serial = request(Serial);
        require(!trim(serial).empty(), IR_IO_ERROR, "Blackbody controller serial is missing");
        if (!tcp() || config_.head >= 0) {
            const int head = tcp() ? config_.head : 0;
            const auto reply = request(HeadSerial, tcp() ? "" : "0");
            const auto comma = reply.find(',');
            require(comma != std::string::npos && trim(std::string_view(reply).substr(0, comma)) == std::to_string(head),
                    IR_IO_ERROR, "Blackbody head identity mismatch");
            const auto headId = trim(std::string_view(reply).substr(comma + 1));
            require(!headId.empty() && (config_.expectedHead.empty() || uppercase(headId) == uppercase(config_.expectedHead)),
                    IR_IO_ERROR, "Blackbody head serial mismatch");
        }
        reconnectable_ = false;
    } catch (...) {
        link_.reset();
        throw;
    }
}

std::string Doc058Blackbody::request(uint8_t function, std::string argument, bool write)
{
    require(bool(link_), IR_IO_ERROR, "Blackbody is disconnected");
    std::this_thread::sleep_until(nextCommand_);
    if (tcp() && config_.head >= 0) {
        // PCN7-LT prefixes the probe index on the selected read functions.
        // The setpoint write carries it after the value instead.
        const auto head = std::to_string(config_.head);
        if (write && function == Setpoint) argument += "," + head;
        else if (function == LowLimit || function == HighLimit || function == Setpoint ||
                 function == Temperature || function == Stable || function == HeadSerial)
            argument = head + argument;
    }
    const std::string command = std::string{Escape, write ? '1' : '0', char(function)} + argument + Return;
    const auto deadline = Clock::now() + config_.timeout;
    std::string payload;
    try {
        link_->discardSerialInput();
        link_->writeAll(command, deadline);
        nextCommand_ = Clock::now() + config_.commandInterval;
        for (;;) {
            const auto reply = link_->readDelimited(Return, deadline);
            const auto start = reply.find(Escape);
            if (start == std::string::npos || reply.size() - start < 3 ||
                uint8_t(reply[start + 1]) != function)
                continue;
            payload = reply.substr(start + 2, reply.size() - start - 3);
            break;
        }
        reconnectable_ = false;
    } catch (...) {
        nextCommand_ = Clock::now() + config_.commandInterval;
        // A partial or late reply belongs to the old connection.
        link_.reset();
        reconnectable_ = true;
        throw;
    }
    require(payload != std::string(1, NegativeAck), IR_IO_ERROR, "Blackbody rejected the DOC058 command");
    return payload;
}

double Doc058Blackbody::number(uint8_t function) { return numeric(request(function)); }

double Doc058Blackbody::numberWithReconnect(uint8_t function)
{
    int reconnects = 0;
    for (;;) {
        try {
            if (!link_) {
                if (reconnectable_) {
                    if (reconnects == ReconnectRetries)
                        throw Error(IR_IO_ERROR, "Blackbody reconnect limit reached");
                    std::this_thread::sleep_for(ReconnectDelay);
                    ++reconnects;
                }
                open();
            }
            return number(function);
        } catch (...) {
            if (!reconnectable_ || reconnects == ReconnectRetries) throw;
        }
    }
}

IRBlackbodyReading Doc058Blackbody::readOnce()
{
    require(bool(link_), IR_IO_ERROR, "Blackbody is disconnected");
    require(number(Mode) == 0, IR_IO_ERROR, "Blackbody is outside absolute temperature mode");
    IRBlackbodyReading result{};
    result.actual_c = number(Temperature);
    result.setpoint_c = number(Setpoint);
    result.stable = number(Stable) == 1;
    return result;
}

IRBlackbodyReading Doc058Blackbody::read()
{
    int reconnects = 0;
    for (;;) {
        try {
            if (!link_) {
                if (reconnectable_) {
                    if (reconnects == ReconnectRetries)
                        throw Error(IR_IO_ERROR, "Blackbody reconnect limit reached");
                    std::this_thread::sleep_for(ReconnectDelay);
                    ++reconnects;
                }
                open();
            }
            return readOnce();
        } catch (...) {
            if (!reconnectable_ || reconnects == ReconnectRetries) throw;
        }
    }
}

double Doc058Blackbody::set(double target)
{
    const auto minimum = tcp() ? -60.0 : -5.0;
    require(std::isfinite(target) && target >= minimum && target <= 100.0,
            IR_BAD_ARGUMENT, "Blackbody target is outside the software range");
    require(numberWithReconnect(Mode) == 0, IR_IO_ERROR, "Blackbody is outside absolute temperature mode");
    const auto low = numberWithReconnect(LowLimit);
    const auto high = numberWithReconnect(HighLimit);
    require(target >= low && target <= high, IR_BAD_ARGUMENT, "Blackbody target is outside device limits");

    char digits[32];
    const auto converted = std::to_chars(digits, digits + sizeof digits, target, std::chars_format::fixed, 3);
    require(converted.ec == std::errc{}, IR_BAD_ARGUMENT, "Invalid setpoint format");
    try {
        request(Setpoint, std::string(digits, converted.ptr), true);
    } catch (const Error &error) {
        if (!reconnectable_ || !transportError(error)) throw;
        // The controller may have applied the setpoint before the link failed.
        // Reconnect and confirm it without sending the write a second time.
    }
    const auto readback = numberWithReconnect(Setpoint);
    require(std::abs(readback - target) <= 0.01, IR_IO_ERROR, "Blackbody setpoint readback mismatch");
    return readback;
}
}
