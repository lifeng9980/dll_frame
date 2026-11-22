#pragma once
#include "IR_device.h"
#include "station/instruments/transport/asio_link.h"
#include <chrono>
#include <memory>
#include <string>
#include <variant>

namespace ir::instruments {

struct Dmm6500Config {
    std::variant<transport::TcpEndpoint, transport::SerialEndpoint> endpoint;
    std::chrono::milliseconds timeout{2000};
};

class Dmm6500 {
public:
    explicit Dmm6500(Dmm6500Config config);
    void setMode(int32_t mode);
    IRDmmReading read(int32_t expectedMode);

private:
    using Clock = std::chrono::steady_clock;
    void connect();
    std::string query(std::string_view command);
    void write(std::string_view command);
    IRDmmReading readOnce(int32_t expectedMode);

    Dmm6500Config config_;
    std::unique_ptr<transport::AsioLink> link_;
};

} // namespace ir::instruments
