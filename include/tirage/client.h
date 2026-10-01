#pragma once

// libtirage-client (docs/integrating.md): the caller's side of the daemon's socket.
//
// Header-only, glaze and the protocol headers, never libvips. A call is
// synchronous and blocking, made from a thread of the caller (a job queue,
// never a thread that serves requests): it connects, sends one request, hands each event
// (Queued, Started) to `on_event` as it comes, and returns the final message.
//
// The outcomes are the protocol's three final messages (docs/requests.md), plus the
// transport:
//   - Response: success, or a refusal with its code (the request's fault);
//   - Busy: the daemon did not run the job, send it again later, unchanged;
//   - Failure: something broke on the daemon's side, log it;
//   - the unexpected branch: the daemon could not be reached, cut the
//     connection or answered nonsense, or the call was cancelled (kCancelled).
//
// With TIRAGE_DIRECT=1 the same call launches tirage-worker itself (direct.h):
// for the development machine and the tests, never in production, where it
// would go around the machine's budget.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <glaze/glaze.hpp>

#include "tirage/direct.h"
#include "tirage/frame.h"
#include "tirage/protocol.h"
#include "tirage/request.h"
#include "tirage/spawn.h"
#include "tirage/validate.h"

namespace tirage::client {

using Event = std::variant<Queued, Started>;
using Reply = std::variant<Response, Busy, Failure>;

// The error of a call cancelled through Options::stop.
inline constexpr std::string_view kCancelled = "cancelled";

struct Options {
    // The daemon's socket. Empty: TIRAGE_SOCKET, else /run/tirage/tirage.sock.
    std::string socket;
    // Called on the calling thread, once per event, before the call returns.
    std::function<void(const Event&)> on_event;
    // A stop request hangs up: the daemon takes the job out of its queue or
    // kills its worker, and the call returns kCancelled.
    std::stop_token stop;
};

[[nodiscard]] inline std::string socket_path(const Options& options = {}) {
    if (!options.socket.empty()) return options.socket;
    if (const char* env = std::getenv("TIRAGE_SOCKET"); env && *env) return env;
    return std::string(kDefaultSocketPath);
}

// TIRAGE_DIRECT=1: run the worker here, without the daemon.
[[nodiscard]] inline bool direct_mode() {
    const char* env = std::getenv("TIRAGE_DIRECT");
    return env && std::string_view(env) == "1";
}

namespace detail {

using tirage::detail::errno_text;
using tirage::detail::Fd;

// One blocking connection to the daemon, for one request.
class Connection {
public:
    [[nodiscard]] static std::expected<Connection, std::string> open(const std::string& path) {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof addr.sun_path) return std::unexpected("socket path too long: " + path);
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        Connection c;
        c.fd_ = Fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        if (c.fd_.fd < 0) return std::unexpected(errno_text("socket", errno));
        if (::connect(c.fd_.fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0)
            return std::unexpected(errno_text("cannot reach tiraged at " + path, errno));
        return c;
    }

    [[nodiscard]] int fd() const { return fd_.fd; }

    // Sends the request frame. When the daemon closes first (a refusal on the
    // header, a busy answer), the rest is not sent: its answer is already on
    // the way, next() reads it.
    [[nodiscard]] std::expected<void, std::string> send(const Request& request) {
        std::string payload;
        if (const auto ec = glz::write_beve(request, payload))
            return std::unexpected("request not serialisable: " + glz::format_error(ec));
        if (payload.size() > std::size_t{0xFFFFFFFF})
            return std::unexpected("request of " + std::to_string(payload.size()) + " bytes, above what a frame carries");
        const auto header = frame_header(static_cast<std::uint32_t>(payload.size()));
        if (write_all(std::string_view(header.data(), header.size()))) write_all(payload);
        return {};
    }

    // The next message, nothing when the daemon has closed, or a transport
    // error. A frame above kMaxMessageFrame is refused on its header.
    [[nodiscard]] std::expected<std::optional<Message>, std::string> next() {
        std::array<char, kFrameHeaderSize> header;
        auto got = read_up_to(header.data(), header.size());
        if (!got) return std::unexpected(got.error());
        if (*got == 0) return std::nullopt;
        if (*got < header.size()) return std::unexpected(std::string("connection cut in a frame header"));

        const std::uint32_t length = frame_length(header.data());
        if (length > kMaxMessageFrame)
            return std::unexpected("message frame of " + std::to_string(length) + " bytes, above the bound");
        std::string body(length, '\0');
        got = read_up_to(body.data(), body.size());
        if (!got) return std::unexpected(got.error());
        if (*got < body.size()) return std::unexpected(std::string("connection cut in a frame"));

        Message m;
        if (const auto ec = glz::read<kReadMessage>(m, body))
            return std::unexpected("unreadable message: " + glz::format_error(ec, body));
        return m;
    }

private:
    Connection() = default;

    bool write_all(std::string_view bytes) {
        while (!bytes.empty()) {
            const auto n = ::send(fd_.fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
        return true;
    }

    // Reads `size` bytes, fewer only when the stream ends.
    std::expected<std::size_t, std::string> read_up_to(char* out, std::size_t size) {
        std::size_t got = 0;
        while (got < size) {
            const auto n = ::recv(fd_.fd, out + got, size - got, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return std::unexpected(errno_text("connection to tiraged", errno));
            if (n == 0) break;
            got += static_cast<std::size_t>(n);
        }
        return got;
    }

    Fd fd_;
};

// Direct mode: the checks the daemon would make, with its compiled bounds,
// then the worker. The events are sent too, so that the caller sees the same
// sequence as through the daemon. The request is copied into the job: direct
// mode is for development, the copy does not matter there.
[[nodiscard]] inline std::expected<Reply, std::string> call_direct(const Request& request,
                                                                   const Options& options) {
    std::optional<Refusal> refusal = validate_request(request, kDefaultBounds);
    if (!refusal)
        refusal = check_input(request.profile, request.input, effective_bounds(request.profile, kDefaultBounds));
    if (refusal) return refused(std::move(*refusal));

    const bool encode = request.operation == Operation::encode;
    if (encode && options.on_event) {
        options.on_event(Queued{.position = 0});
        options.on_event(Started{.waited_ms = 0});
    }
    // One thread for a probe, like the daemon.
    const Job job{.request = request, .bounds = kDefaultBounds, .threads = encode ? direct::threads_from_env() : 1};
    auto response = direct::run(job, direct::worker_path(), options.stop);
    if (options.stop.stop_requested()) return std::unexpected(std::string(kCancelled));
    if (!response) return Failure{std::move(response.error())};
    return std::move(*response);
}

}  // namespace detail

// Sends one encode or probe and waits for its final message.
[[nodiscard]] inline std::expected<Reply, std::string> call(const Request& request, const Options& options = {}) {
    if (options.stop.stop_requested()) return std::unexpected(std::string(kCancelled));
    if (direct_mode()) return detail::call_direct(request, options);

    auto connection = detail::Connection::open(socket_path(options));
    if (!connection) return std::unexpected(connection.error());
    // Declared after the connection, so gone before it closes: a late stop
    // never shuts a descriptor number that was reused.
    const std::stop_callback hang_up(options.stop, [fd = connection->fd()] { ::shutdown(fd, SHUT_RDWR); });

    if (auto sent = connection->send(request); !sent) return std::unexpected(sent.error());
    for (;;) {
        auto next = connection->next();
        if (options.stop.stop_requested()) return std::unexpected(std::string(kCancelled));
        if (!next) return std::unexpected(next.error());
        if (!*next) return std::unexpected(std::string("tiraged closed the connection without an answer"));
        Message& m = **next;
        if (auto* e = std::get_if<Queued>(&m)) {
            if (options.on_event) options.on_event(*e);
        } else if (auto* e = std::get_if<Started>(&m)) {
            if (options.on_event) options.on_event(*e);
        } else if (auto* r = std::get_if<Response>(&m)) {
            return std::move(*r);
        } else if (auto* b = std::get_if<Busy>(&m)) {
            return std::move(*b);
        } else if (auto* f = std::get_if<Failure>(&m)) {
            return std::move(*f);
        } else {
            return std::unexpected(std::string("tiraged answered a job with a status"));
        }
    }
}

// Asks the daemon what it is doing (tirage status). There is no daemon to ask
// in direct mode.
[[nodiscard]] inline std::expected<Status, std::string> status(const Options& options = {}) {
    if (direct_mode()) return std::unexpected(std::string("direct mode (TIRAGE_DIRECT=1) has no daemon to ask"));

    auto connection = detail::Connection::open(socket_path(options));
    if (!connection) return std::unexpected(connection.error());
    const std::stop_callback hang_up(options.stop, [fd = connection->fd()] { ::shutdown(fd, SHUT_RDWR); });

    const Request request{.protocol = kProtocolVersion, .operation = Operation::status};
    if (auto sent = connection->send(request); !sent) return std::unexpected(sent.error());
    auto next = connection->next();
    if (options.stop.stop_requested()) return std::unexpected(std::string(kCancelled));
    if (!next) return std::unexpected(next.error());
    if (!*next) return std::unexpected(std::string("tiraged closed the connection without an answer"));
    Message& m = **next;
    if (auto* s = std::get_if<Status>(&m)) return std::move(*s);
    if (auto* r = std::get_if<Response>(&m)) return std::unexpected("tiraged refused: " + r->error);
    if (auto* b = std::get_if<Busy>(&m)) return std::unexpected("tiraged is busy: " + b->message);
    if (auto* f = std::get_if<Failure>(&m)) return std::unexpected("tiraged failed: " + f->message);
    return std::unexpected(std::string("tiraged answered status with an event"));
}

}  // namespace tirage::client
