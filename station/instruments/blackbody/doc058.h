#pragma once
#include "IR_device.h"
#include "station/instruments/transport/asio_link.h"
#include <memory>
#include <string>
#include <variant>

namespace ir::instruments {
struct BlackbodyConfig {
    using Endpoint = std::variant<transport::TcpEndpoint, transport::SerialEndpoint>;
    Endpoint endpoint;
    std::chrono::milliseconds timeout{3000};
    std::chrono::milliseconds commandInterval{};
    int head{-1};
    std::string expectedHead;
};

class Doc058Blackbody {
public:
    explicit Doc058Blackbody(BlackbodyConfig config);
    IRBlackbodyReading read();
    double set(double target);

private:
    void open();
    IRBlackbodyReading readOnce();
    std::string request(uint8_t function, std::string argument = {}, bool write = false);
    double number(uint8_t function);
    bool tcp() const;

    BlackbodyConfig config_;
    std::unique_ptr<transport::AsioLink> link_;
    transport::Deadline nextCommand_{};
};
}
