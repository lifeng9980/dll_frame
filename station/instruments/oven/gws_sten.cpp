#include "gws_sten.h"
#include "api_common.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <system_error>
#include <thread>
#include <vector>

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

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string upper(std::string_view s)
{
    std::string out(s);
    for (char &ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return out;
}

std::vector<std::string_view> splitComma(std::string_view text)
{
    std::vector<std::string_view> fields;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t next = text.find(',', pos);
        if (next == std::string_view::npos) {
            const auto part = trim(text.substr(pos));
            if (!part.empty()) fields.push_back(part);
            break;
        }
        const auto part = trim(text.substr(pos, next - pos));
        if (!part.empty()) fields.push_back(part);
        pos = next + 1;
    }
    return fields;
}

double parseNumber(std::string_view text, const char *label)
{
    text = trim(text);
    if (text.starts_with('+')) text.remove_prefix(1);
    double val{};
    const auto res = std::from_chars(text.data(), text.data() + text.size(), val);
    require(res.ec == std::errc{} && res.ptr == text.data() + text.size() && std::isfinite(val),
            IR_IO_ERROR, std::string("GWS STEN invalid number in ") + label + ": " + std::string(text));
    return val;
}

int32_t parseMode(std::string_view reply)
{
    const auto mode = upper(trim(reply));
    require(!mode.empty() && !mode.starts_with("NA"), IR_IO_ERROR,
            "GWS STEN MODE? failed: " + mode);
    if (mode == "STANDBY") return IR_OVEN_MODE_STANDBY;
    if (mode == "CONSTANT") return IR_OVEN_MODE_CONSTANT;
    if (mode == "PROGRAM") return IR_OVEN_MODE_PROGRAM;
    return IR_OVEN_MODE_OTHER;
}

int32_t parseAlarm(std::string_view reply)
{
    const auto alarm = upper(trim(reply));
    require(!alarm.starts_with("NA"), IR_IO_ERROR, "GWS STEN ALARM? failed: " + alarm);
    if (alarm.empty() || alarm == "0" || alarm == "NONE" || alarm == "NO" ||
        alarm == "OK" || alarm == "NORMAL") {
        return 0;
    }
    return 1;
}

std::string formatFixed1(double value)
{
    if (value == 0.0) value = 0.0;
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof buf, value, std::chars_format::fixed, 1);
    require(res.ec == std::errc{}, IR_BAD_ARGUMENT, "GWS STEN setpoint format error");
    return std::string(buf, res.ptr);
}

static_assert(sizeof(IROvenReading) == 24, "Unexpected IROvenReading size");
} // namespace

GwsStenOven::GwsStenOven(GwsStenConfig config) : config_(std::move(config))
{
    require(std::isfinite(config_.minSetpointC) && std::isfinite(config_.maxSetpointC) &&
            config_.minSetpointC <= config_.maxSetpointC,
            IR_BAD_ARGUMENT, "Invalid oven setpoint range");
    require(!config_.delimiter.empty(), IR_BAD_ARGUMENT, "Oven delimiter must not be empty");
    connect();
}

void GwsStenOven::connect()
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
        const auto modeReply = query("MODE?");
        (void)parseMode(modeReply);
    } catch (...) {
        link_.reset();
        throw;
    }
}

std::string GwsStenOven::query(std::string_view body)
{
    require(bool(link_), IR_IO_ERROR, "GWS STEN oven is disconnected");
    const auto deadline = Clock::now() + config_.timeout;
    const std::string cmd = std::to_string(config_.address) + "," + std::string(body) + config_.delimiter;
    std::string reply;
    try {
        link_->discardSerialInput();
        link_->writeAll(cmd, deadline);
        reply = link_->readDelimited(config_.delimiter, deadline);
    } catch (const Error &) {
        link_.reset();
        throw;
    } catch (const std::system_error &e) {
        link_.reset();
        throw Error(IR_IO_ERROR, std::string("GWS STEN I/O error: ") + e.what());
    }
    const auto trimmed = trim(reply);
    require(!trimmed.empty(), IR_IO_ERROR, std::string("GWS STEN empty reply for ") + std::string(body));
    return std::string(trimmed);
}

template <class Fn>
auto GwsStenOven::withReconnect(Fn fn)
{
    for (int attempt = 0;; ++attempt) {
        try {
            if (!link_) connect();
            return fn();
        } catch (const Error &err) {
            link_.reset();
            if (!transportFailure(err) || attempt >= ReconnectRetries) throw;
            std::this_thread::sleep_for(ReconnectDelay);
        } catch (...) {
            link_.reset();
            throw;
        }
    }
}

IROvenReading GwsStenOven::readOnce()
{
    const auto tempReply = query("TEMP?");
    require(!upper(tempReply).starts_with("NA"), IR_IO_ERROR, "GWS STEN TEMP? failed: " + tempReply);
    const auto fields = splitComma(tempReply);
    require(fields.size() >= 4, IR_IO_ERROR, "GWS STEN TEMP? returned too few fields: " + tempReply);

    IROvenReading out{};
    out.actual_c = parseNumber(fields[0], "TEMP?[0]");
    out.setpoint_c = parseNumber(fields[1], "TEMP?[1]");
    out.mode = parseMode(query("MODE?"));
    out.alarm_active = parseAlarm(query("ALARM?"));
    return out;
}

IROvenReading GwsStenOven::read()
{
    return withReconnect([&] { return readOnce(); });
}

double GwsStenOven::set(double targetC)
{
    require(std::isfinite(targetC) && targetC >= config_.minSetpointC && targetC <= config_.maxSetpointC,
            IR_BAD_ARGUMENT, "GWS STEN target_c is outside configured safe limits");
    const std::string cmd = "TEMP, S" + formatFixed1(targetC);
    try {
        if (!link_) connect();
        const auto r1 = upper(query(cmd));
        require(r1.starts_with("OK"), IR_IO_ERROR, "GWS STEN rejected TEMP setpoint: " + r1);
        const auto r2 = upper(query("MODE, CONSTANT"));
        require(r2.starts_with("OK"), IR_IO_ERROR, "GWS STEN rejected MODE, CONSTANT: " + r2);
    } catch (const Error &err) {
        link_.reset();
        if (!transportFailure(err)) throw;
    }
    const auto readback = read();
    require(std::abs(readback.setpoint_c - targetC) <= 0.11, IR_IO_ERROR,
            "GWS STEN setpoint readback mismatch");
    return readback.setpoint_c;
}

void GwsStenOven::stop()
{
    try {
        if (!link_) connect();
        const auto r1 = upper(query("MODE, STANDBY"));
        require(r1.starts_with("OK"), IR_IO_ERROR, "GWS STEN rejected MODE, STANDBY: " + r1);
    } catch (const Error &err) {
        link_.reset();
        if (!transportFailure(err)) throw;
    }
    const auto verified = withReconnect([&] { return parseMode(query("MODE?")); });
    require(verified == IR_OVEN_MODE_STANDBY, IR_IO_ERROR,
            "GWS STEN mode readback after STANDBY did not match STANDBY");
}

} // namespace ir::instruments
