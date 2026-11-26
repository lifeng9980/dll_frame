#include "asio_link.h"
#include "api_common.h"
#include "utf8_path.h"
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/serial_port.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>
#include <algorithm>
#include <array>
#include <thread>
#include <variant>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <termios.h>
#endif

namespace ir::transport {
namespace {
std::string systemErrorText(const std::error_code &code)
{
#ifdef _WIN32
    if (code.category() == asio::system_category())
        return utf8::systemErrorText({code.value(), std::system_category()});
#endif
    return utf8::systemErrorText(code);
}

using Tcp = asio::ip::tcp;
using ir::api::Error;
using ir::api::require;
constexpr size_t MaxReplyBytes = 4096;
constexpr auto CancellationPollInterval = std::chrono::milliseconds(25);
thread_local const CancellationScope *activeCancellation = nullptr;

void checkCancellation()
{
    if (activeCancellation && activeCancellation->stopRequested())
        throw Error(IR_CANCELLED, "Instrument operation cancelled");
}

template<class Stream>
void cancelStream(Stream &stream)
{
    asio::error_code error;
    stream.cancel(error);
    if (error) stream.close(error);
}

template<class Start, class Cancel>
void withDeadline(asio::io_context &io, Deadline deadline, const char *operation, Start start, Cancel cancel)
{
    checkCancellation();
    require(std::chrono::steady_clock::now() < deadline, IR_TIMEOUT, operation);
    io.restart();
    asio::steady_timer timer(io, deadline);
    asio::error_code result;
    enum class Completion { pending, io, timer, cancelled };
    Completion completion = Completion::pending;
    start([&](asio::error_code error) {
        if (completion != Completion::pending)
            return;
        completion = Completion::io;
        result = error;
        timer.cancel();
    });
    timer.async_wait([&](asio::error_code error) {
        // Cancelling the timer cannot change a successful wait already queued for dispatch.
        if (!error && completion == Completion::pending) {
            completion = Completion::timer;
            cancel();
        }
    });
    if (activeCancellation) {
        while (completion == Completion::pending) {
            io.run_one_for(CancellationPollInterval);
            if (completion == Completion::pending && activeCancellation->stopRequested()) {
                completion = Completion::cancelled;
                timer.cancel();
                cancel();
            }
        }
    }
    // Finish the cancellation handler before caller-owned buffers go out of scope.
    io.run();
    if (completion == Completion::timer) throw Error(IR_TIMEOUT, std::string(operation) + " timed out");
    if (completion == Completion::cancelled) throw Error(IR_CANCELLED, std::string(operation) + " cancelled");
    if (result) throw Error(IR_IO_ERROR, std::string(operation) + ": " + systemErrorText(result));
}
}

CancellationScope::CancellationScope(const void *context, Predicate predicate) noexcept
    : context_(context), predicate_(predicate), previous_(activeCancellation)
{
    activeCancellation = this;
}

CancellationScope::~CancellationScope()
{
    activeCancellation = previous_;
}

void waitUntil(Deadline deadline)
{
    if (!activeCancellation) {
        std::this_thread::sleep_until(deadline);
        return;
    }
    for (;;) {
        checkCancellation();
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return;
        std::this_thread::sleep_until(std::min(deadline, now + CancellationPollInterval));
    }
}

void waitFor(std::chrono::milliseconds duration)
{
    waitUntil(std::chrono::steady_clock::now() + duration);
}

bool isRetryableError(int32_t status) noexcept
{
    return status == IR_IO_ERROR || status == IR_TIMEOUT;
}

std::vector<std::string> resolveTcpAddresses(const TcpEndpoint &endpoint, Deadline deadline)
{
    asio::error_code literalEc;
    auto literalAddr = asio::ip::make_address(endpoint.host, literalEc);
    if (!literalEc) {
        if (literalAddr.is_v6() && literalAddr.to_v6().is_v4_mapped()) {
            const auto bytes = literalAddr.to_v6().to_bytes();
            asio::ip::address_v4::bytes_type ipv4{};
            std::copy(bytes.end() - ipv4.size(), bytes.end(), ipv4.begin());
            literalAddr = asio::ip::address_v4(ipv4);
        }
        return {literalAddr.to_string()};
    }

    asio::io_context io;
    Tcp::resolver resolver(io);
    Tcp::resolver::results_type resolved;
    withDeadline(io, deadline, "Resolve instrument host", [&](auto done) {
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
    require(!addresses.empty(), IR_IO_ERROR, "Instrument host resolved to no addresses");
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
    withDeadline(state.io, deadline, "Connect to instrument", [&](auto done) {
        asio::async_connect(socket, addresses, [done](auto error, const auto &) { done(error); });
    }, [&] {
        asio::error_code ignored;
        socket.close(ignored);
    });
}

AsioLink::AsioLink(const SerialEndpoint &endpoint) : state_(std::make_unique<State>(false))
{
    checkCancellation();
    try {
        auto &port = std::get<asio::serial_port>(state_->stream);
        using Options = asio::serial_port_base;
        port.open(endpoint.port);
        port.set_option(Options::baud_rate(endpoint.baud));
        port.set_option(Options::character_size(endpoint.dataBits));
        port.set_option(Options::parity(
            endpoint.parity == 1 ? Options::parity::odd :
            endpoint.parity == 2 ? Options::parity::even : Options::parity::none));
        port.set_option(Options::stop_bits(
            endpoint.stopBits == 2 ? Options::stop_bits::two : Options::stop_bits::one));
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
    } catch (const std::system_error &error) {
        throw Error(IR_IO_ERROR,
                    "Cannot open/configure serial port '" + endpoint.port + "': " + systemErrorText(error.code()));
    }
}

AsioLink::~AsioLink() = default;

std::unique_ptr<AsioLink> openLink(const Endpoint &endpoint, Deadline deadline)
{
    if (const auto *tcp = std::get_if<TcpEndpoint>(&endpoint))
        return std::make_unique<AsioLink>(*tcp, deadline);
    return std::make_unique<AsioLink>(std::get<SerialEndpoint>(endpoint));
}

void AsioLink::writeAll(std::string_view bytes, Deadline deadline)
{
    std::visit([&](auto &stream) {
        withDeadline(state_->io, deadline, "Instrument write", [&](auto done) {
            asio::async_write(stream, asio::buffer(bytes.data(), bytes.size()),
                [done](auto error, auto) { done(error); });
        }, [&] { cancelStream(stream); });
    }, state_->stream);
}

std::string AsioLink::readDelimited(char delimiter, Deadline deadline)
{
    return readDelimited(std::string_view(&delimiter, 1), deadline);
}

std::string AsioLink::readDelimited(std::string_view delimiter, Deadline deadline)
{
    require(!delimiter.empty(), IR_BAD_ARGUMENT, "Delimiter must not be empty");
    size_t size = 0;
    std::visit([&](auto &stream) {
        withDeadline(state_->io, deadline, "Instrument reply", [&](auto done) {
            asio::async_read_until(stream, asio::dynamic_buffer(state_->pending, MaxReplyBytes), delimiter,
                [&, done](auto error, auto count) {
                    size = count;
                    done(error);
                });
        }, [&] { cancelStream(stream); });
    }, state_->stream);
    auto reply = state_->pending.substr(0, size);
    state_->pending.erase(0, size);
    return reply;
}

std::string AsioLink::readExact(std::size_t count, Deadline deadline)
{
    require(count <= MaxReplyBytes, IR_BAD_ARGUMENT, "Requested reply exceeds buffer limit");
    if (count == 0)
        return {};
    if (state_->pending.size() < count) {
        const size_t need = count - state_->pending.size();
        std::visit([&](auto &stream) {
            withDeadline(state_->io, deadline, "Instrument binary reply", [&](auto done) {
                asio::async_read(stream, asio::dynamic_buffer(state_->pending, MaxReplyBytes),
                    asio::transfer_at_least(need),
                    [done](auto error, auto) { done(error); });
            }, [&] { cancelStream(stream); });
        }, state_->stream);
    }
    require(state_->pending.size() >= count, IR_IO_ERROR, "Incomplete binary reply");
    auto reply = state_->pending.substr(0, count);
    state_->pending.erase(0, count);
    return reply;
}

void AsioLink::discardSerialInput()
{
    auto *port = std::get_if<asio::serial_port>(&state_->stream);
    if (!port) return;
    state_->pending.clear();
    try {
#ifdef _WIN32
        if (!PurgeComm(port->native_handle(), PURGE_RXABORT | PURGE_RXCLEAR))
            throw std::system_error(GetLastError(), std::system_category(), "PurgeComm");
#else
        if (tcflush(port->native_handle(), TCIFLUSH) != 0)
            throw std::system_error(errno, std::generic_category(), "tcflush");
#endif
    } catch (const std::system_error &error) {
        throw Error(IR_IO_ERROR, std::string("Serial buffer flush failed: ") + systemErrorText(error.code()));
    }
}
}
