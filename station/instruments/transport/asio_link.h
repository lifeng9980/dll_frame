#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ir::transport {
using Deadline = std::chrono::steady_clock::time_point;
struct TcpEndpoint {
    std::string host;
    uint16_t port;
    std::vector<std::string> resolvedAddresses;
};
struct SerialEndpoint {
    std::string port;
    unsigned baud;
};

// Resolve once with the instrument timeout so endpoint reservation and connect
// use the same numeric addresses (for example, localhost and 127.0.0.1).
std::vector<std::string> resolveTcpAddresses(const TcpEndpoint &endpoint, Deadline deadline);

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
