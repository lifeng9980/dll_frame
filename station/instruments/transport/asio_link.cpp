#include "asio_link.h"
#include "api_common.h"
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/serial_port.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>
#include <algorithm>
#include <array>
#include <variant>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <termios.h>
#endif

namespace ir::transport {
namespace {
using Tcp = asio::ip::tcp;
using ir::api::Error;
using ir::api::require;
constexpr size_t MaxReplyBytes = 4096;

template<class Start, class Cancel>
void withDeadline(asio::io_context &io, Deadline deadline, const char *operation, Start start, Cancel cancel)
{
    require(std::chrono::steady_clock::now() < deadline, IR_TIMEOUT, operation);
    io.restart();
    asio::steady_timer timer(io, deadline);
    asio::error_code result;
    bool expired = false;
    start([&](asio::error_code error) {
        result = error;
        timer.cancel();
    });
    timer.async_wait([&](asio::error_code error) {
        if (!error) {
            expired = true;
            cancel();
        }
    });
    // Finish the cancellation handler before caller-owned buffers go out of scope.
    io.run();
    if (expired) throw Error(IR_TIMEOUT, std::string(operation) + " timed out");
    if (result) throw Error(IR_IO_ERROR, std::string(operation) + ": " + result.message());
}
}

std::vector<std::string> resolveTcpAddresses(const TcpEndpoint &endpoint, Deadline deadline)
{
    asio::io_context io;
    Tcp::resolver resolver(io);
    Tcp::resolver::results_type resolved;
    withDeadline(io, deadline, "Resolve blackbody host", [&](auto done) {
        resolver.async_resolve(endpoint.host, std::to_string(endpoint.port),
            [&, done](auto error, auto results) {
                resolved = std::move(results);
                done(error);
            });
    }, [&] { resolver.cancel(); });

    std::vector<std::string> addresses;
    for (const auto &entry : resolved) {
        auto address = entry.endpoint().address();
        if (address.is_v6() && address.to_v6().is_v4_mapped()) {
            const auto bytes = address.to_v6().to_bytes();
            asio::ip::address_v4::bytes_type ipv4{};
            std::copy(bytes.end() - ipv4.size(), bytes.end(), ipv4.begin());
            address = asio::ip::address_v4(ipv4);
        }
        const auto value = address.to_string();
        if (std::find(addresses.begin(), addresses.end(), value) == addresses.end())
            addresses.push_back(value);
    }
    require(!addresses.empty(), IR_IO_ERROR, "Blackbody host resolved to no addresses");
    return addresses;
}

struct AsioLink::State {
    asio::io_context io;
    std::variant<Tcp::socket, asio::serial_port> stream;
    std::string pending;
    explicit State(bool tcp) : stream(std::in_place_type<Tcp::socket>, io)
    {
        if (!tcp) stream.emplace<asio::serial_port>(io);
    }
};

AsioLink::AsioLink(const TcpEndpoint &endpoint, Deadline deadline)
    : state_(std::make_unique<State>(true))
{
    auto &state = *state_;
    auto &socket = std::get<Tcp::socket>(state.stream);
    const auto addressStrings = endpoint.resolvedAddresses.empty() ?
        resolveTcpAddresses(endpoint, deadline) : endpoint.resolvedAddresses;
    std::vector<Tcp::endpoint> addresses;
    addresses.reserve(addressStrings.size());
    for (const auto &address : addressStrings)
        addresses.emplace_back(asio::ip::make_address(address), endpoint.port);
    withDeadline(state.io, deadline, "Connect to blackbody", [&](auto done) {
        asio::async_connect(socket, addresses, [done](auto error, const auto &) { done(error); });
    }, [&] {
        asio::error_code ignored;
        socket.close(ignored);
    });
}

AsioLink::AsioLink(const SerialEndpoint &endpoint) : state_(std::make_unique<State>(false))
{
    auto &port = std::get<asio::serial_port>(state_->stream);
    using Options = asio::serial_port_base;
    port.open(endpoint.port);
    port.set_option(Options::baud_rate(endpoint.baud));
    port.set_option(Options::character_size(8));
    port.set_option(Options::parity(Options::parity::none));
    port.set_option(Options::stop_bits(Options::stop_bits::one));
    port.set_option(Options::flow_control(Options::flow_control::none));
#ifdef _WIN32
    // DOC058 serial uses 8N1 without modem control lines.
    DCB settings{};
    settings.DCBlength = sizeof(settings);
    const auto handle = port.native_handle();
    if (!GetCommState(handle, &settings))
        throw std::system_error(GetLastError(), std::system_category(), "GetCommState");
    settings.fDtrControl = DTR_CONTROL_DISABLE;
    settings.fRtsControl = RTS_CONTROL_DISABLE;
    settings.fOutxDsrFlow = FALSE;
    settings.fDsrSensitivity = FALSE;
    if (!SetCommState(handle, &settings))
        throw std::system_error(GetLastError(), std::system_category(), "SetCommState");
#endif
}

AsioLink::~AsioLink() = default;

void AsioLink::writeAll(std::string_view bytes, Deadline deadline)
{
    std::visit([&](auto &stream) {
        withDeadline(state_->io, deadline, "Instrument write", [&](auto done) {
            asio::async_write(stream, asio::buffer(bytes.data(), bytes.size()),
                [done](auto error, auto) { done(error); });
        }, [&] {
            asio::error_code ignored;
            stream.cancel(ignored);
        });
    }, state_->stream);
}

std::string AsioLink::readDelimited(char delimiter, Deadline deadline)
{
    size_t size = 0;
    std::visit([&](auto &stream) {
        withDeadline(state_->io, deadline, "Instrument reply", [&](auto done) {
            asio::async_read_until(stream, asio::dynamic_buffer(state_->pending, MaxReplyBytes), delimiter,
                [&, done](auto error, auto count) {
                    size = count;
                    done(error);
                });
        }, [&] {
            asio::error_code ignored;
            stream.cancel(ignored);
        });
    }, state_->stream);
    auto reply = state_->pending.substr(0, size);
    state_->pending.erase(0, size);
    return reply;
}

void AsioLink::discardSerialInput()
{
    auto *port = std::get_if<asio::serial_port>(&state_->stream);
    if (!port) return;
    state_->pending.clear();
#ifdef _WIN32
    if (!PurgeComm(port->native_handle(), PURGE_RXABORT | PURGE_RXCLEAR))
        throw std::system_error(GetLastError(), std::system_category(), "PurgeComm");
#else
    if (tcflush(port->native_handle(), TCIFLUSH) != 0)
        throw std::system_error(errno, std::generic_category(), "tcflush");
#endif
}
}
