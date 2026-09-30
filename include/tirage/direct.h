#pragma once

// Direct mode (plan § 5): the caller launches tirage-worker itself, without the
// daemon. For the development machine and the tests only: a call that goes
// around the daemon goes around the machine's CPU budget.

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <expected>
#include <stop_token>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "tirage/protocol.h"
#include "tirage/spawn.h"

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

// Runs one job in a fresh worker and returns its response. A refusal is a
// response with an error. The unexpected branch is a failure: the worker could
// not be launched, died, or answered something unreadable. A stop request kills
// the worker, like a hang-up does through the daemon.
[[nodiscard]] inline std::expected<Response, std::string> run(const Job& job,
                                                              const std::string& worker = worker_path(),
                                                              std::stop_token stop = {}) {
    auto process = spawn_worker(job, worker);
    if (!process) return std::unexpected(process.error());

    std::string reply;
    {
        // Gone before the worker is reaped: the pid it may kill is still ours.
        const std::stop_callback kill_on_stop(stop, [pid = process->pid] { ::kill(pid, SIGKILL); });
        char buf[65536];
        for (;;) {
            const auto n = ::read(process->output.fd, buf, sizeof buf);
            if (n > 0) reply.append(buf, static_cast<std::size_t>(n));
            else if (n == 0 || errno != EINTR) break;
        }
    }

    int status = 0;
    while (::waitpid(process->pid, &status, 0) < 0 && errno == EINTR) {}
    return worker_outcome(status, reply);
}

}  // namespace tirage::direct
