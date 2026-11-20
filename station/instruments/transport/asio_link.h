#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ir::transport {
using Deadline = std::chrono::steady_clock::time_point;
struct TcpEndpoint {
    std::string host;
    uint16_t port;
};
struct SerialEndpoint {
    std::string port;
    unsigned baud;
};

// One caller owns a link at a time. A request and all its response fragments
// share the same deadline. Delimited reads retain bytes for the next read.
class AsioLink {
public:
    AsioLink(const TcpEndpoint &endpoint, Deadline deadline);
    explicit AsioLink(const SerialEndpoint &endpoint);
    ~AsioLink();
    AsioLink(const AsioLink &) = delete;
    AsioLink &operator=(const AsioLink &) = delete;

    void writeAll(std::string_view bytes, Deadline deadline);
    std::string readDelimited(char delimiter, Deadline deadline);
    void discardSerialInput();

private:
    struct State;
    std::unique_ptr<State> state_;
};
}
