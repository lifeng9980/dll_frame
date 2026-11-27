#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ir::transport {
using Deadline = std::chrono::steady_clock::time_point;

// The caller supplies a stop predicate for synchronous instrument calls on this thread.
class CancellationScope {
public:
    using Predicate = bool (*)(const void *) noexcept;
    CancellationScope(const void *context, Predicate predicate) noexcept;
    ~CancellationScope();
    CancellationScope(const CancellationScope &) = delete;
    CancellationScope &operator=(const CancellationScope &) = delete;

    bool stopRequested() const noexcept { return predicate_(context_); }

private:
    const void *context_;
    Predicate predicate_;
    const CancellationScope *previous_;
};

void waitUntil(Deadline deadline);
void waitFor(std::chrono::milliseconds duration);
bool isRetryableError(int32_t status) noexcept;
inline constexpr int ReconnectRetries = 3;
inline constexpr std::chrono::milliseconds ReconnectDelay{500};

struct TcpEndpoint {
    std::string host;
    uint16_t port;
    std::vector<std::string> resolvedAddresses;
};
struct SerialEndpoint {
    std::string port;
    unsigned baud;
    unsigned dataBits{8};
    int parity{0};    // 0=none, 1=odd, 2=even
    int stopBits{1};  // 1 or 2
};
using Endpoint = std::variant<TcpEndpoint, SerialEndpoint>;

// Resolve once with the instrument timeout so endpoint reservation and connect
// use the same numeric addresses (for example, localhost and 127.0.0.1).
std::vector<std::string> resolveTcpAddresses(const TcpEndpoint &endpoint, Deadline deadline);

// One caller owns a link at a time. A request and all its response fragments
// share the same deadline. Delimited and exact reads retain extra bytes for the next read.
class ScpiLink {
public:
    virtual ~ScpiLink() = default;
    virtual void writeAll(std::string_view bytes, Deadline deadline) = 0;
    virtual std::string readDelimited(char delimiter, Deadline deadline) = 0;
    virtual void discardSerialInput() = 0;
};

std::unique_ptr<ScpiLink> openVisaLink(const std::string &resource, Deadline deadline);

class AsioLink final : public ScpiLink {
public:
    AsioLink(const TcpEndpoint &endpoint, Deadline deadline);
    explicit AsioLink(const SerialEndpoint &endpoint);
    ~AsioLink() override;
    AsioLink(const AsioLink &) = delete;
    AsioLink &operator=(const AsioLink &) = delete;

    void writeAll(std::string_view bytes, Deadline deadline) override;
    std::string readDelimited(char delimiter, Deadline deadline) override;
    std::string readDelimited(std::string_view delimiter, Deadline deadline);
    std::string readExact(std::size_t count, Deadline deadline);
    void discardSerialInput() override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

std::unique_ptr<AsioLink> openLink(const Endpoint &endpoint, Deadline deadline);
}
