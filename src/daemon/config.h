#pragma once

// The daemon's configuration (plan § 6): /etc/tirage/tirage.json, read at
// start and at each reload (SIGHUP). Only the daemon reads it.
//
// Pure logic: parsing, checking, and the CPU budget worked out from what the
// daemon found around it.

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

#include "scheduler.h"
#include "tirage/frame.h"
#include "tirage/validate.h"

namespace tirage::daemon {

inline constexpr std::string_view kDefaultConfigPath = "/etc/tirage/tirage.json";

struct Config {
    // Threads of each encode. Absent: TIRAGE_THREADS, then the daemon's CPU
    // affinity, then hardware_concurrency (resolve_threads).
    std::optional<int> threads;
    // Where to listen when systemd does not hand over the socket. Read at start
    // only: a reload does not move the socket.
    std::string socket = std::string(kDefaultSocketPath);
    // A path, or a bare name looked up in the PATH.
    std::string worker = "tirage-worker";
    QueueLimits queue;
    // Probes run outside the queue, at most this many at once.
    int probe_limit = 2;
    // A worker still running after this is killed, and its caller gets a failure.
    int timeout_s = 120;
    // Bytes of requests the daemon holds at once: being read, queued or
    // running. Above it, new requests are busy. Also bounds what slow or
    // hostile callers can make the daemon allocate.
    std::int64_t max_pending_bytes = std::int64_t{512} << 20;
    Bounds bounds = kDefaultBounds;
};

struct ConfigError {
    std::string message;
};

// The first problem found, or nothing.
[[nodiscard]] inline std::optional<ConfigError> check_config(const Config& c) {
    const auto fail = [](std::string m) { return ConfigError{std::move(m)}; };
    if (c.threads && (*c.threads < 1 || *c.threads > 1024)) return fail("threads: from 1 to 1024");
    if (c.socket.empty() || c.socket.front() != '/') return fail("socket: an absolute path");
    if (c.worker.empty()) return fail("worker: empty");
    if (c.queue.interactive < 1 || c.queue.background < 1)
        return fail("queue: each length is at least 1");
    if (c.probe_limit < 1) return fail("probe_limit: at least 1");
    if (c.timeout_s < 1 || c.timeout_s > 3600) return fail("timeout_s: from 1 to 3600");
    if (c.bounds.max_bytes < 1 || c.bounds.max_edge < 1 || c.bounds.max_pixels < 1)
        return fail("bounds: each bound is positive");
    // A frame is at most 4 GiB (its length is 32 bits).
    if (max_request_frame(c.bounds) > std::int64_t{0xFFFFFFFF})
        return fail("bounds.max_bytes: at most 4 GiB minus the request envelope");
    if (c.max_pending_bytes < max_request_frame(c.bounds))
        return fail(std::format("max_pending_bytes: at least one request of the largest size ({} bytes)",
                                max_request_frame(c.bounds)));
    return std::nullopt;
}

// Every key is optional, an unknown key is refused (a misspelt setting would
// otherwise be ignored in silence).
[[nodiscard]] inline std::expected<Config, ConfigError> parse_config(std::string_view json) {
    Config c;
    if (const auto ec = glz::read_json(c, json))
        return std::unexpected(ConfigError{glz::format_error(ec, json)});
    if (auto e = check_config(c)) return std::unexpected(std::move(*e));
    return c;
}

// The CPU budget, in the plan's order: the configuration (or TIRAGE_THREADS),
// then the CPUs the daemon may run on, then the machine's count. Never
// hardware_concurrency first: under systemd the affinity is what we are given.
[[nodiscard]] inline int resolve_threads(std::optional<int> configured, std::optional<int> env,
                                         int affinity, unsigned hardware) {
    if (configured) return *configured;
    if (env && *env >= 1 && *env <= 1024) return *env;
    if (affinity >= 1) return affinity;
    return hardware >= 1 ? static_cast<int>(hardware) : 1;
}

}  // namespace tirage::daemon
