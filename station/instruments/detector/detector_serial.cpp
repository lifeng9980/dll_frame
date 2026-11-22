#include "detector_serial.h"
#include "api_common.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>

namespace ir::instruments {
namespace {
using api::Error;
using api::require;

constexpr uint8_t CmdIntTime = 0x50;
constexpr uint8_t CmdEmptyTime = 0x51;
constexpr uint8_t CmdGpol = 0x21;

std::string formatBytesHex(const std::string &bytes)
{
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) oss << ' ';
        oss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(bytes[i]));
    }
    return oss.str();
}
} // namespace

DetectorSerial::DetectorSerial(DetectorSerialConfig config) : config_(std::move(config))
{
    connect();
}

void DetectorSerial::connect()
{
    link_.reset();
    if (std::holds_alternative<transport::TcpEndpoint>(config_.endpoint)) {
        link_ = std::make_unique<transport::AsioLink>(
            std::get<transport::TcpEndpoint>(config_.endpoint),
            Clock::now() + config_.timeout);
    } else {
        link_ = std::make_unique<transport::AsioLink>(
            std::get<transport::SerialEndpoint>(config_.endpoint));
    }
}

void DetectorSerial::sendCommandAndVerifyAck(uint8_t commandCode, const uint8_t *payload, size_t payloadLen)
{
    require(payload != nullptr, IR_NULL_POINTER, "Detector register payload is NULL");
    require(payloadLen >= 1 && payloadLen <= 255, IR_BAD_ARGUMENT,
            "Detector command payload length must be in 1..255 bytes");

    if (!link_) connect();
    if (config_.interCommandDelay.count() > 0)
        std::this_thread::sleep_for(config_.interCommandDelay);

    std::string frame;
    frame.reserve(payloadLen + 3);
    frame.push_back(static_cast<char>(0xCC));
    frame.push_back(static_cast<char>(payloadLen));
    frame.push_back(static_cast<char>(commandCode));
    frame.append(reinterpret_cast<const char *>(payload), payloadLen);

    const auto deadline = Clock::now() + config_.timeout;
    std::string ack;
    try {
        link_->discardSerialInput();
        link_->writeAll(frame, deadline);
        ack = link_->readExact(5, deadline);
    } catch (const Error &) {
        link_.reset();
        throw;
    } catch (const std::system_error &e) {
        link_.reset();
        throw Error(IR_IO_ERROR, std::string("Detector serial I/O failed: ") + e.what());
    }

    const std::string expected{
        static_cast<char>(0x55),
        static_cast<char>(0x00),
        static_cast<char>(0x01),
        static_cast<char>(commandCode),
        static_cast<char>(0xFF)
    };
    if (ack != expected) {
        link_.reset();
        std::ostringstream oss;
        oss << "Detector ACK mismatch for command 0x"
            << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(commandCode)
            << ": received " << formatBytesHex(ack)
            << ", expected " << formatBytesHex(expected);
        throw Error(IR_IO_ERROR, oss.str());
    }
}

void DetectorSerial::setTiming(int32_t integrationUs, int32_t frameHz, int32_t emptyUs)
{
    require(integrationUs >= 1 && integrationUs <= 1000000 &&
            frameHz >= 1 && frameHz <= 1000000 &&
            emptyUs >= 0 && emptyUs <= 0xFFFFFF &&
            integrationUs <= 1000000 / frameHz,
            IR_BAD_ARGUMENT, "Invalid detector integration time, frame rate, or empty time");

    const std::array<uint8_t, 3> intPayload{
        static_cast<uint8_t>(integrationUs & 0xFF),
        static_cast<uint8_t>((integrationUs >> 8) & 0xFF),
        static_cast<uint8_t>((integrationUs >> 16) & 0xFF)
    };
    sendCommandAndVerifyAck(CmdIntTime, intPayload.data(), intPayload.size());

    const std::array<uint8_t, 3> emptyPayload{
        static_cast<uint8_t>(emptyUs & 0xFF),
        static_cast<uint8_t>((emptyUs >> 8) & 0xFF),
        static_cast<uint8_t>((emptyUs >> 16) & 0xFF)
    };
    sendCommandAndVerifyAck(CmdEmptyTime, emptyPayload.data(), emptyPayload.size());
}

void DetectorSerial::setGpol(double gpolV)
{
    require(std::isfinite(gpolV) && gpolV >= 0.0 && gpolV <= 5.0,
            IR_BAD_ARGUMENT, "Detector GPOL voltage must be within [0.0, 5.0] V");
    const long code = std::clamp(std::lround((gpolV * 65535.0) / 5.0), 0L, 65535L);
    const std::array<uint8_t, 2> payload{
        static_cast<uint8_t>(code & 0xFF),
        static_cast<uint8_t>((code >> 8) & 0xFF)
    };
    sendCommandAndVerifyAck(CmdGpol, payload.data(), payload.size());
}

void DetectorSerial::setRegister(uint8_t commandCode, const uint8_t *payload, int32_t payloadBytes)
{
    require(payloadBytes >= 1 && payloadBytes <= 255, IR_BAD_ARGUMENT,
            "Detector register payload_bytes must be within 1..255");
    sendCommandAndVerifyAck(commandCode, payload, static_cast<size_t>(payloadBytes));
}

} // namespace ir::instruments
