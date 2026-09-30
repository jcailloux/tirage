#pragma once

// The integration harness around a real tiraged (plan § 9): a daemon of its
// own per test, on a socket of its own, and a raw connection to it. Shared by
// the daemon's, the client's and the CLI's tests.
//
// The worker script sleeps for the number of seconds written in a file of the
// test's directory, then runs the real worker: the tests steer each job's
// length without touching the daemon.

#include <doctest/doctest.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <vips/vips8>

#include "tirage/frame.h"
#include "tirage/protocol.h"

extern char** environ;

namespace tirage::test {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

using Clock = std::chrono::steady_clock;

inline std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

inline void write_file(const fs::path& p, std::string_view content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}

// A daemon of its own, in a directory of its own, stopped at the end.
class Daemon {
public:
    // `config` is the JSON body without its braces, socket and worker added here.
    explicit Daemon(std::string_view config = "", std::string_view worker_script = "") {
        char tmpl[] = "/tmp/tirage-test-XXXXXX";
        dir_ = ::mkdtemp(tmpl);
        socket_ = (dir_ / "tirage.sock").string();
        delay(0);

        fs::path worker = TIRAGE_WORKER_PATH;
        const std::string script = worker_script.empty()
                                       ? std::format("#!/bin/sh\nsleep \"$(cat '{}')\" </dev/null >/dev/null\n"
                                                     "exec '{}' \"$@\"\n",
                                                     (dir_ / "delay").string(), worker.string())
                                       : std::string(worker_script);
        worker = dir_ / "worker.sh";
        write_file(worker, script);
        fs::permissions(worker, fs::perms::owner_all);

        config_path_ = dir_ / "tirage.json";
        write_config(config);
        start();
    }

    ~Daemon() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    const std::string& socket() const { return socket_; }
    const fs::path& dir() const { return dir_; }
    pid_t pid() const { return pid_; }

    // Seconds the next workers wait before working.
    void delay(double seconds) { write_file(dir_ / "delay", std::format("{}", seconds)); }

    void write_config(std::string_view config) {
        write_file(config_path_, std::format(R"({{"socket": "{}", "worker": "{}"{}{}}})", socket_,
                                             (dir_ / "worker.sh").string(), config.empty() ? "" : ", ",
                                             config));
    }

    void signal(int sig) { ::kill(pid_, sig); }

    // Waits for the daemon to exit and returns its exit code, -1 if killed.
    int wait() {
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    std::string log() const { return read_file(dir_ / "tiraged.log"); }

private:
    void start() {
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        const std::string log = (dir_ / "tiraged.log").string();
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                         0644);
        std::string path = TIRAGE_DAEMON_PATH, flag = "--config", config = config_path_.string();
        char* argv[] = {path.data(), flag.data(), config.data(), nullptr};
        REQUIRE(::posix_spawn(&pid_, path.c_str(), &actions, nullptr, argv, environ) == 0);
        posix_spawn_file_actions_destroy(&actions);

        // Ready when the socket answers.
        for (int i = 0; i < 500; ++i) {
            if (fs::exists(socket_)) return;
            std::this_thread::sleep_for(10ms);
        }
        FAIL("tiraged did not start: " << this->log());
    }

    fs::path dir_;
    fs::path config_path_;
    std::string socket_;
    pid_t pid_ = -1;
};

// One connection, as a caller would hold it.
class Connection {
public:
    explicit Connection(const std::string& path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
        REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    }
    ~Connection() { close(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    // Best effort: the daemon may answer and close before reading it all.
    void send_raw(std::string_view bytes) {
        while (!bytes.empty()) {
            const auto n = ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (n <= 0) return;
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
    }

    void send(const Request& r) {
        auto frame = make_frame(r);
        REQUIRE(frame.has_value());
        send_raw(*frame);
    }

    // The next message, or nothing once the daemon has closed.
    std::optional<Message> next(std::chrono::milliseconds timeout = 30s) {
        std::string header(kFrameHeaderSize, '\0');
        if (!read_exact(header, timeout)) return std::nullopt;
        std::string body(frame_length(header.data()), '\0');
        REQUIRE(read_exact(body, timeout));
        Message m;
        REQUIRE_FALSE(glz::read_beve(m, body));
        return m;
    }

    // Every message until the daemon closes.
    std::vector<Message> all() {
        std::vector<Message> out;
        while (auto m = next()) out.push_back(std::move(*m));
        return out;
    }

private:
    bool read_exact(std::string& buf, std::chrono::milliseconds timeout) {
        std::size_t got = 0;
        while (got < buf.size()) {
            pollfd p{.fd = fd_, .events = POLLIN, .revents = 0};
            REQUIRE_MESSAGE(::poll(&p, 1, static_cast<int>(timeout.count())) == 1, "no answer in time");
            const auto n = ::recv(fd_, buf.data() + got, buf.size() - got, 0);
            if (n <= 0) {
                REQUIRE_MESSAGE(got == 0, "frame cut short");
                return false;
            }
            got += static_cast<std::size_t>(n);
        }
        return true;
    }

    int fd_ = -1;
};

inline std::string png(int width, int height) {
    const vips::VImage img = (vips::VImage::black(width, height, vips::VImage::option()->set("bands", 3)) +
                              std::vector<double>{200, 40, 40})
                                 .cast(VIPS_FORMAT_UCHAR);
    void* buf = nullptr;
    std::size_t size = 0;
    img.write_to_buffer(".png", &buf, &size);
    std::string out(static_cast<const char*>(buf), size);
    g_free(buf);
    return out;
}

inline Request request(Priority priority = Priority::interactive, Operation op = Operation::encode) {
    Request r;
    r.protocol = kProtocolVersion;
    r.operation = op;
    r.priority = priority;
    r.profile.version = kProfileVersion;
    r.profile.input.formats = {InputFormat::png};
    r.profile.variants["small"] =
        Variant{.widths = {32}, .formats = {OutputFormat::webp}, .quality = {QualityStep{.webp = 80}}};
    if (op == Operation::encode) r.variants = {"small"};
    r.input = png(64, 48);
    return r;
}

template <class T>
inline const T& as(const Message& m) {
    REQUIRE_MESSAGE(std::holds_alternative<T>(m), "message of another kind, index " << m.index());
    return std::get<T>(m);
}

template <class T>
inline const T& expect(Connection& c) {
    static thread_local Message last;
    auto m = c.next();
    REQUIRE(m.has_value());
    last = std::move(*m);
    return as<T>(last);
}

// Sends a request and reads until Started: the job is running.
inline void start_running(Connection& c, Request r) {
    c.send(r);
    expect<Queued>(c);
    expect<Started>(c);
}

}  // namespace tirage::test
