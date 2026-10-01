#pragma once

// Launching one tirage-worker, shared by direct mode (direct.h) and
// the daemon. POSIX only, no libvips.
//
// The job goes to the worker on its standard input, backed by a memfd (neither
// a temporary file nor a pipe to interleave with the reading of the output),
// and the response comes back on its standard output. Both are BEVE.

#include <cerrno>
#include <csignal>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glaze/glaze.hpp>

#include "tirage/protocol.h"

extern char** environ;

namespace tirage {

namespace detail {

inline std::string errno_text(std::string_view what, int err) {
    return std::string(what) + ": " + std::strerror(err);
}

// Closes a descriptor on scope exit.
struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int f) : fd(f) {}
    Fd(Fd&& o) noexcept : fd(o.release()) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd = o.release();
        }
        return *this;
    }
    ~Fd() { reset(); }
    void reset() {
        if (fd >= 0) ::close(std::exchange(fd, -1));
    }
    int release() { return std::exchange(fd, -1); }
};

}  // namespace detail

// A launched worker: its pid, to reap, and the read end of its standard output.
struct WorkerProcess {
    pid_t pid = -1;
    detail::Fd output;
};

// Launches a worker on `job`. `worker` is a path, or a bare name looked up in
// the PATH. The worker's standard error is inherited, so libvips messages land
// in the caller's log (the journal, for the daemon).
[[nodiscard]] inline std::expected<WorkerProcess, std::string> spawn_worker(const Job& job,
                                                                           const std::string& worker) {
    std::string frame;
    if (const auto ec = glz::write_beve(job, frame))
        return std::unexpected("job not serialisable: " + glz::format_error(ec));

    detail::Fd input{::memfd_create("tirage-job", MFD_CLOEXEC)};
    if (input.fd < 0) return std::unexpected(detail::errno_text("memfd_create", errno));
    for (std::size_t off = 0; off < frame.size();) {
        const auto n = ::write(input.fd, frame.data() + off, frame.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return std::unexpected(detail::errno_text("job write", errno));
        off += static_cast<std::size_t>(n);
    }
    frame = {};
    if (::lseek(input.fd, 0, SEEK_SET) != 0) return std::unexpected(detail::errno_text("lseek", errno));

    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return std::unexpected(detail::errno_text("pipe", errno));
    detail::Fd out_read{fds[0]};
    detail::Fd out_write{fds[1]};

    // dup2 clears close-on-exec on the target, so the child gets exactly its
    // standard input and output, and never the read end of the pipe (a failed
    // exec would otherwise leave us waiting for a close that never comes).
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input.fd, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_write.fd, STDOUT_FILENO);

    // The daemon blocks the signals it reads through a signalfd and ignores
    // SIGPIPE. Both would be inherited through exec: the worker starts with an
    // empty mask and default dispositions instead.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (const int sig : {SIGPIPE, SIGHUP, SIGINT, SIGTERM, SIGQUIT, SIGCHLD}) sigaddset(&defaults, sig);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::string arg0 = worker;
    char* argv[] = {arg0.data(), nullptr};
    pid_t pid = 0;
    const int rc = worker.find('/') == std::string::npos
                       ? ::posix_spawnp(&pid, worker.c_str(), &actions, &attr, argv, environ)
                       : ::posix_spawn(&pid, worker.c_str(), &actions, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return std::unexpected(detail::errno_text("worker not launched (" + worker + ")", rc));
    return WorkerProcess{.pid = pid, .output = std::move(out_read)};
}

// What a worker's exit status and output amount to: its response, or the
// failure they tell.
[[nodiscard]] inline std::expected<Response, std::string> worker_outcome(int status, std::string_view output) {
    if (WIFSIGNALED(status))
        return std::unexpected("worker killed by signal " + std::to_string(WTERMSIG(status)));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return std::unexpected("worker failed (exit code " +
                               std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
    Response response;
    if (const auto ec = glz::read_beve(response, output))
        return std::unexpected("worker response unreadable: " + glz::format_error(ec, output));
    return response;
}

}  // namespace tirage
