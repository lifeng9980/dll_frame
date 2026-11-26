#include "drivers.h"
#include "api_common.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <system_error>

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
    link_ = transport::openLink(config_.endpoint, Clock::now() + config_.timeout);
}

void DetectorSerial::sendCommandAndVerifyAck(uint8_t commandCode, const uint8_t *payload, size_t payloadLen)
{
    require(payload != nullptr, IR_NULL_POINTER, "Detector register payload is NULL");
    require(payloadLen >= 1 && payloadLen <= 255, IR_BAD_ARGUMENT,
            "Detector command payload length must be in 1..255 bytes");

    if (!link_) connect();
    if (config_.interCommandDelay.count() > 0)
        transport::waitFor(config_.interCommandDelay);

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

IRDetectorCurrentReading DetectorSerial::measureCurrent(int32_t gainCode, uint16_t channelCode)
{
    static_assert(sizeof(IRDetectorCurrentReading) == 24, "Unexpected IRDetectorCurrentReading size");
    require(gainCode >= 0 && gainCode <= 7, IR_BAD_ARGUMENT,
            "Detector current gain_code must be within 0..7");
    const int gainMultiplier = 1 << gainCode;

    const std::array<uint8_t, 1> gainPayload{static_cast<uint8_t>(gainCode)};
    sendCommandAndVerifyAck(0x40, gainPayload.data(), gainPayload.size());

    const std::array<uint8_t, 2> channelPayload{
        static_cast<uint8_t>((channelCode >> 8) & 0xFF),
        static_cast<uint8_t>(channelCode & 0xFF)
    };
    sendCommandAndVerifyAck(0x30, channelPayload.data(), channelPayload.size());

    transport::waitFor(std::chrono::milliseconds(2));

    if (!link_) connect();
    if (config_.interCommandDelay.count() > 0)
        transport::waitFor(config_.interCommandDelay);

    const std::string queryFrame{
        static_cast<char>(0xCC),
        static_cast<char>(0x01),
        static_cast<char>(0x41),
        static_cast<char>(0x03)
    };
    const auto deadline = Clock::now() + config_.timeout;
    std::string response;
    try {
        link_->discardSerialInput();
        link_->writeAll(queryFrame, deadline);
        response = link_->readExact(7, deadline);
    } catch (const Error &) {
        link_.reset();
        throw;
    } catch (const std::system_error &e) {
        link_.reset();
        throw Error(IR_IO_ERROR, std::string("Detector current read I/O failed: ") + e.what());
    }

    const auto byteAt = [&](size_t idx) -> uint8_t {
        return static_cast<uint8_t>(static_cast<unsigned char>(response[idx]));
    };
    if (response.size() != 7 || byteAt(0) != 0x55 || byteAt(1) != 0x00 ||
        byteAt(2) != 0x03 || byteAt(3) != 0x41) {
        link_.reset();
        throw Error(IR_IO_ERROR,
                    "Detector current response mismatch: received " + formatBytesHex(response) +
                    ", expected 55 00 03 41 <ADC_H> <ADC_M> <ADC_L>");
    }

    const uint32_t rawCode = (static_cast<uint32_t>(byteAt(4)) << 16) |
                             (static_cast<uint32_t>(byteAt(5)) << 8) |
                             static_cast<uint32_t>(byteAt(6));
    const double adcVoltageV = (static_cast<double>(rawCode) / 8388608.0 - 1.0) * 2.5;
    const double senseVoltageV = adcVoltageV / static_cast<double>(gainMultiplier);
    const double currentMa = std::abs(senseVoltageV) * 1000.0;
    return {rawCode, 0, senseVoltageV, currentMa};
}

} // namespace ir::instruments
