#pragma once

// Direct mode (plan § 5): the caller launches tirage-worker itself, without the
// daemon. For the development machine and the tests only: a call that goes
// around the daemon goes around the machine's CPU budget.
//
// The job goes to the worker on its standard input, backed by a memfd (neither
// a temporary file nor a pipe to interleave with the reading of the output),
// and the response comes back on its standard output. Both are BEVE.

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <spawn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glaze/glaze.hpp>

#include "tirage/protocol.h"

extern char** environ;

namespace tirage::direct {

// TIRAGE_WORKER, else "tirage-worker" looked up in the PATH.
[[nodiscard]] inline std::string worker_path() {
    if (const char* env = std::getenv("TIRAGE_WORKER"); env && *env) return env;
    return "tirage-worker";
}

// TIRAGE_THREADS, else one thread. An unreadable or absurd value falls back to
// one rather than letting libvips take the whole machine.
[[nodiscard]] inline int threads_from_env() {
    if (const char* env = std::getenv("TIRAGE_THREADS")) {
        const int v = std::atoi(env);
        if (v > 0 && v <= 64) return v;
    }
    return 1;
}

namespace detail {

inline std::string errno_text(std::string_view what, int err) {
    return std::string(what) + ": " + std::strerror(err);
}

// Closes a descriptor on scope exit.
struct Fd {
    int fd = -1;
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
    int release() { return std::exchange(fd, -1); }
};

}  // namespace detail

// Runs one job in a fresh worker and returns its response. A refusal is a
// response with an error. The unexpected branch is a failure: the worker could
// not be launched, died, or answered something unreadable. The worker's
// standard error is inherited, so libvips messages land in the caller's log.
[[nodiscard]] inline std::expected<Response, std::string> run(const Job& job,
                                                              const std::string& worker = worker_path()) {
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

    std::string arg0 = worker;
    char* argv[] = {arg0.data(), nullptr};
    pid_t pid = 0;
    // A bare name is looked up in the PATH, a path is taken as is.
    const int rc = worker.find('/') == std::string::npos
                       ? ::posix_spawnp(&pid, worker.c_str(), &actions, nullptr, argv, environ)
                       : ::posix_spawn(&pid, worker.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out_write.release());
    if (rc != 0) return std::unexpected(detail::errno_text("worker not launched (" + worker + ")", rc));

    std::string reply;
    char buf[65536];
    for (;;) {
        const auto n = ::read(out_read.fd, buf, sizeof buf);
        if (n > 0) reply.append(buf, static_cast<std::size_t>(n));
        else if (n == 0 || errno != EINTR) break;
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFSIGNALED(status))
        return std::unexpected("worker killed by signal " + std::to_string(WTERMSIG(status)));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return std::unexpected("worker failed (exit code " +
                               std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");

    Response response;
    if (const auto ec = glz::read_beve(response, reply))
        return std::unexpected("worker response unreadable: " + glz::format_error(ec, reply));
    return response;
}

}  // namespace tirage::direct
