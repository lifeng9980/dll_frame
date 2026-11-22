#include "cooler_modbus.h"
#include "api_common.h"
#include <array>
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

uint16_t modbusCrc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001)
                            : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

static_assert(sizeof(IRCoolerReading) == 16, "Unexpected IRCoolerReading size");
} // namespace

CoolerModbus::CoolerModbus(CoolerModbusConfig config) : config_(std::move(config))
{
    require(config_.slaveAddress >= 1 && config_.slaveAddress <= 247,
            IR_BAD_ARGUMENT, "Cooler Modbus slave_address must be within 1..247");
    connect();
}

void CoolerModbus::connect()
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
        (void)readOnce();
    } catch (...) {
        link_.reset();
        throw;
    }
}

template <class Fn>
auto CoolerModbus::withReconnect(Fn fn)
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

IRCoolerReading CoolerModbus::readOnce()
{
    require(bool(link_), IR_IO_ERROR, "Cooler Modbus transport is disconnected");

    std::array<uint8_t, 8> request{
        config_.slaveAddress,
        0x03,
        static_cast<uint8_t>((config_.registerAddress >> 8) & 0xFF),
        static_cast<uint8_t>(config_.registerAddress & 0xFF),
        0x00,
        0x02,
        0x00,
        0x00
    };
    const uint16_t reqCrc = modbusCrc16(request.data(), 6);
    request[6] = static_cast<uint8_t>(reqCrc & 0xFF);
    request[7] = static_cast<uint8_t>((reqCrc >> 8) & 0xFF);

    const auto deadline = Clock::now() + config_.timeout;
    std::string header;
    std::string body;
    try {
        link_->discardSerialInput();
        link_->writeAll(std::string_view(reinterpret_cast<const char *>(request.data()), request.size()), deadline);
        header = link_->readExact(3, deadline);
        const uint8_t addr = static_cast<uint8_t>(static_cast<unsigned char>(header[0]));
        const uint8_t fn = static_cast<uint8_t>(static_cast<unsigned char>(header[1]));
        const uint8_t countOrEx = static_cast<uint8_t>(static_cast<unsigned char>(header[2]));
        if ((fn & 0x80) != 0) {
            const std::string tail = link_->readExact(2, deadline);
            std::array<uint8_t, 5> exFrame{
                addr, fn, countOrEx,
                static_cast<uint8_t>(static_cast<unsigned char>(tail[0])),
                static_cast<uint8_t>(static_cast<unsigned char>(tail[1]))
            };
            const uint16_t expected = modbusCrc16(exFrame.data(), 3);
            const uint16_t actual = static_cast<uint16_t>(exFrame[3]) |
                                    (static_cast<uint16_t>(exFrame[4]) << 8);
            require(expected == actual, IR_IO_ERROR, "Cooler Modbus exception response CRC mismatch");
            throw Error(IR_IO_ERROR, "Cooler Modbus returned exception code " + std::to_string(countOrEx));
        }
        require(addr == config_.slaveAddress, IR_IO_ERROR, "Cooler Modbus slave address mismatch");
        require(fn == 0x03 && countOrEx == 0x04, IR_IO_ERROR,
                "Cooler Modbus unexpected function or byte count");
        body = link_->readExact(6, deadline);
    } catch (const Error &) {
        link_.reset();
        throw;
    } catch (const std::system_error &e) {
        link_.reset();
        throw Error(IR_IO_ERROR, std::string("Cooler Modbus I/O error: ") + e.what());
    }

    std::array<uint8_t, 9> frame{};
    for (size_t i = 0; i < 3; ++i)
        frame[i] = static_cast<uint8_t>(static_cast<unsigned char>(header[i]));
    for (size_t i = 0; i < 6; ++i)
        frame[3 + i] = static_cast<uint8_t>(static_cast<unsigned char>(body[i]));

    const uint16_t calculated = modbusCrc16(frame.data(), 7);
    const uint16_t received = static_cast<uint16_t>(frame[7]) |
                              (static_cast<uint16_t>(frame[8]) << 8);
    require(calculated == received, IR_IO_ERROR, "Cooler Modbus CRC-16 mismatch");

    const uint32_t rawValue = (static_cast<uint32_t>(frame[3]) << 24) |
                              (static_cast<uint32_t>(frame[4]) << 16) |
                              (static_cast<uint32_t>(frame[5]) << 8) |
                              static_cast<uint32_t>(frame[6]);
    return {rawValue, 1, static_cast<double>(rawValue) / 100.0};
}

IRCoolerReading CoolerModbus::read()
{
    return withReconnect([&] { return readOnce(); });
}

} // namespace ir::instruments
