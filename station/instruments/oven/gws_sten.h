#pragma once
#include "IR_device.h"
#include "station/instruments/transport/asio_link.h"
#include <chrono>
#include <memory>
#include <string>
#include <variant>

namespace ir::instruments {

struct GwsStenConfig {
    std::variant<transport::TcpEndpoint, transport::SerialEndpoint> endpoint;
    int address{1};
    std::string delimiter{"\r\n"};
    double minSetpointC{-45.0};
    double maxSetpointC{70.0};
    std::chrono::milliseconds timeout{2000};
};

class GwsStenOven {
public:
    explicit GwsStenOven(GwsStenConfig config);
    IROvenReading read();
    double set(double targetC);
    void stop();

private:
    using Clock = std::chrono::steady_clock;
    void connect();
    std::string query(std::string_view body);
    IROvenReading readOnce();

    template <class Fn>
    auto withReconnect(Fn fn);

    GwsStenConfig config_;
    std::unique_ptr<transport::AsioLink> link_;
};

} // namespace ir::instruments
