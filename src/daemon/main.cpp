// tiraged: the machine's image encoding daemon (plan § 2 and § 6).
//
//   tiraged [--config <tirage.json>] [--socket <path>]
//   tiraged --version
//
// The socket comes from systemd (socket activation, LISTEN_FDS) when there is
// one, else it is bound at --socket, else at the configuration's `socket`. The
// configuration is /etc/tirage/tirage.json unless --config says otherwise: the
// default file may be missing (every setting has a default), a named one may
// not. SIGHUP reloads it, SIGTERM stops the daemon once its workers are done.

#include <fcntl.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "config.h"
#include "server.h"
#include "tirage/protocol.h"
#include "tirage/profile.h"

namespace {

enum Exit { kOk = 0, kFailure = 1, kUsage = 2 };

int usage(const char* why) {
    std::fprintf(stderr,
                 "tiraged: %s\n"
                 "usage: tiraged [--config <tirage.json>] [--socket <path>]\n"
                 "       tiraged --version\n",
                 why);
    return kUsage;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "tiraged: %s\n", message.c_str());
    return kFailure;
}

// systemd socket activation, without libsystemd: one socket, as fd 3. The
// variables are removed so that no worker inherits them.
int activated_socket() {
    const char* pid = std::getenv("LISTEN_PID");
    const char* fds = std::getenv("LISTEN_FDS");
    const bool ours = pid && fds && std::atol(pid) == static_cast<long>(::getpid()) && std::atoi(fds) == 1;
    ::unsetenv("LISTEN_PID");
    ::unsetenv("LISTEN_FDS");
    ::unsetenv("LISTEN_FDNAMES");
    if (!ours) return -1;
    constexpr int kFirst = 3;
    ::fcntl(kFirst, F_SETFD, FD_CLOEXEC);
    ::fcntl(kFirst, F_SETFL, ::fcntl(kFirst, F_GETFL) | O_NONBLOCK);
    return kFirst;
}

// Binds the socket at `path`. A leftover file from a daemon that died is
// replaced, a daemon still answering there is not.
std::expected<int, std::string> bind_socket(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return std::unexpected("socket path too long: " + path);
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    tirage::detail::Fd probe{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (::connect(probe.fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0)
        return std::unexpected("a daemon already listens on " + path);
    ::unlink(path.c_str());

    tirage::detail::Fd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (fd.fd < 0) return std::unexpected(std::string("socket: ") + std::strerror(errno));
    if (::bind(fd.fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
        return std::unexpected("bind " + path + ": " + std::strerror(errno));
    // The group decides who may encode (plan § 6, SocketGroup=tirage).
    ::chmod(path.c_str(), 0660);
    if (::listen(fd.fd, SOMAXCONN) != 0) return std::unexpected(std::string("listen: ") + std::strerror(errno));
    return fd.release();
}

tirage::daemon::Host host() {
    tirage::daemon::Host h;
    if (const char* env = std::getenv("TIRAGE_THREADS")) {
        char* end = nullptr;
        const long v = std::strtol(env, &end, 10);
        if (end != env && *end == '\0') h.env_threads = static_cast<int>(v);
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof set, &set) == 0) h.affinity = CPU_COUNT(&set);
    h.hardware = std::thread::hardware_concurrency();
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::printf("tiraged %s (protocol %d, profile %d)\n", TIRAGE_VERSION, tirage::kProtocolVersion,
                    tirage::kProfileVersion);
        return kOk;
    }
    std::optional<std::string> config_arg;
    std::optional<std::string> socket_arg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (i + 1 >= argc) return usage("unknown option or missing value");
        if (a == "--config") config_arg = argv[++i];
        else if (a == "--socket") socket_arg = argv[++i];
        else return usage("unknown option");
    }

    // Journal lines as they come, not when a buffer fills.
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    const std::string config_path = config_arg.value_or(std::string(tirage::daemon::kDefaultConfigPath));
    tirage::daemon::Config config;
    if (std::ifstream f(config_path, std::ios::binary); f) {
        const std::string json(std::istreambuf_iterator<char>(f), {});
        auto parsed = tirage::daemon::parse_config(json);
        if (!parsed) return fail(config_path + ": " + parsed.error().message);
        config = std::move(*parsed);
    } else if (config_arg) {
        return fail("cannot read " + config_path);
    } else {
        tirage::daemon::log_line(config_path + " not found, defaults apply");
    }
    if (socket_arg) {
        config.socket = *socket_arg;
        if (auto e = tirage::daemon::check_config(config)) return fail(e->message);
    }

    // Writes to a caller that left must fail with EPIPE, not kill the daemon.
    std::signal(SIGPIPE, SIG_IGN);

    int listener = activated_socket();
    std::string unlink_path;
    if (listener < 0) {
        auto bound = bind_socket(config.socket);
        if (!bound) return fail(bound.error());
        listener = *bound;
        unlink_path = config.socket;
        tirage::daemon::log_line("listening on " + config.socket);
    } else {
        tirage::daemon::log_line("listening on the socket handed over by systemd");
    }

    tirage::daemon::Server server(std::move(config), config_path, host(), listener, std::move(unlink_path));
    return server.run();
}
