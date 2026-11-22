#pragma once
#include "IR_device.h"
#include "station/instruments/transport/asio_link.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <variant>

namespace ir::instruments {

struct CoolerModbusConfig {
    std::variant<transport::TcpEndpoint, transport::SerialEndpoint> endpoint;
    uint8_t slaveAddress{1};
    uint16_t registerAddress{0x7A52};
    std::chrono::milliseconds timeout{2000};
};

class CoolerModbus {
public:
    explicit CoolerModbus(CoolerModbusConfig config);
    IRCoolerReading read();

private:
    using Clock = std::chrono::steady_clock;
    void connect();
    IRCoolerReading readOnce();
    template <class Fn> auto withReconnect(Fn fn);

    CoolerModbusConfig config_;
    std::unique_ptr<transport::AsioLink> link_;
};

} // namespace ir::instruments
