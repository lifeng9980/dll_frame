#pragma once
#include "IR_device.h"
#include "station/instruments/transport/asio_link.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <variant>

namespace ir::instruments {

struct DetectorSerialConfig {
    std::variant<transport::TcpEndpoint, transport::SerialEndpoint> endpoint;
    std::chrono::milliseconds timeout{1200};
    std::chrono::milliseconds interCommandDelay{0};
};

class DetectorSerial {
public:
    explicit DetectorSerial(DetectorSerialConfig config);
    void setTiming(int32_t integrationUs, int32_t frameHz, int32_t emptyUs);
    void setGpol(double gpolV);
    void setRegister(uint8_t commandCode, const uint8_t *payload, int32_t payloadBytes);
    IRDetectorCurrentReading measureCurrent(int32_t gainCode, uint16_t channelCode);

private:
    using Clock = std::chrono::steady_clock;
    void connect();
    void sendCommandAndVerifyAck(uint8_t commandCode, const uint8_t *payload, size_t payloadLen);

    DetectorSerialConfig config_;
    std::unique_ptr<transport::AsioLink> link_;
};

} // namespace ir::instruments
