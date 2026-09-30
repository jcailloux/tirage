// Integration harness, second half: the real tiraged, with the real worker or a
// worker script that waits or fails on demand (plan § 9). Each test starts its
// own daemon with the configuration it needs (harness.h).

#include <doctest/doctest.h>

#include <signal.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <numeric>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "harness.h"

using namespace tirage;
using namespace tirage::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------

TEST_CASE("daemon: an encode is queued, started, then answered") {
    Daemon d;
    Connection c(d.socket());
    c.send(request());
    const auto messages = c.all();
    REQUIRE(messages.size() == 3);
    CHECK(as<Queued>(messages[0]).position == 0);
    as<Started>(messages[1]);
    const Response& r = as<Response>(messages[2]);
    CHECK_MESSAGE(r.error.empty(), r.error);
    REQUIRE(r.outputs.size() == 1);
    CHECK(r.outputs[0].width == 32);
    CHECK(r.outputs[0].height == 24);
    CHECK(r.outputs[0].bytes.substr(8, 4) == "WEBP");
    CHECK(d.log().find("result=ok") != std::string::npos);
}

TEST_CASE("daemon: a probe is answered without events") {
    Daemon d;
    Connection c(d.socket());
    c.send(request(Priority::interactive, Operation::probe));
    const auto messages = c.all();
    REQUIRE(messages.size() == 1);
    const Response& r = as<Response>(messages[0]);
    CHECK_MESSAGE(r.error.empty(), r.error);
    REQUIRE(r.source.has_value());
    CHECK(r.source->width == 64);
}

TEST_CASE("daemon: refusals come before the queue") {
    Daemon d;
    const auto refusal = [&](const Request& r) {
        Connection c(d.socket());
        c.send(r);
        const auto messages = c.all();
        REQUIRE(messages.size() == 1);  // no Queued: refused before
        const Response& resp = as<Response>(messages[0]);
        CHECK_FALSE(resp.error.empty());
        return resp.code;
    };

    Request bad_profile = request();
    bad_profile.profile.variants["small"].quality[0].webp = 0;
    CHECK(refusal(bad_profile) == Code::invalid_profile);

    Request gif = request();
    gif.input = "GIF89a";
    CHECK(refusal(gif) == Code::unsupported_format);

    Request future = request();
    future.protocol = kProtocolVersion + 1;
    CHECK(refusal(future) == Code::invalid_request);

    // The profile's own limits are checked by the daemon too.
    Request limited = request();
    limited.profile.input.limits = Limits{.max_bytes = 10};
    CHECK(refusal(limited) == Code::file_too_large);
}

TEST_CASE("daemon: a frame above the bound is refused on its header") {
    Daemon d(R"("bounds": {"max_bytes": 1000})");
    Connection c(d.socket());
    const auto header = frame_header(static_cast<std::uint32_t>(1000 + kRequestEnvelope + 1));
    c.send_raw(std::string_view(header.data(), header.size()));  // and nothing after
    const Response& r = expect<Response>(c);
    CHECK(r.code == Code::file_too_large);
    CHECK_FALSE(c.next().has_value());
}

TEST_CASE("daemon: an unreadable frame is a failure, not a refusal") {
    Daemon d;
    Connection c(d.socket());
    const std::string garbage = "not beve at all";
    const auto header = frame_header(static_cast<std::uint32_t>(garbage.size()));
    c.send_raw(std::string_view(header.data(), header.size()));
    c.send_raw(garbage);
    CHECK_FALSE(expect<Failure>(c).message.empty());
}

TEST_CASE("daemon: interactive passes the background queue, positions count what is ahead") {
    Daemon d;
    d.delay(0.4);
    Connection a(d.socket()), b(d.socket()), c(d.socket()), i(d.socket());
    start_running(a, request(Priority::background));

    b.send(request(Priority::background));
    CHECK(expect<Queued>(b).position == 1);
    c.send(request(Priority::background));
    CHECK(expect<Queued>(c).position == 2);
    i.send(request(Priority::interactive));
    CHECK(expect<Queued>(i).position == 1);  // behind the running one only

    // Each Started says how long its job waited. The interactive one, queued
    // last, must have waited the least.
    std::int64_t waited_b = 0, waited_c = 0, waited_i = 0;
    std::thread tb([&] { waited_b = expect<Started>(b).waited_ms; });
    std::thread tc([&] { waited_c = expect<Started>(c).waited_ms; });
    std::thread ti([&] { waited_i = expect<Started>(i).waited_ms; });
    tb.join();
    tc.join();
    ti.join();
    CHECK(waited_i < waited_b);
    CHECK(waited_b < waited_c);
    for (Connection* conn : {&a, &b, &c, &i}) CHECK(expect<Response>(*conn).error.empty());
}

TEST_CASE("daemon: a full queue answers busy at once") {
    Daemon d(R"("queue": {"interactive": 1, "background": 1})");
    d.delay(2);
    Connection a(d.socket()), b(d.socket()), c(d.socket());
    start_running(a, request(Priority::background));
    b.send(request(Priority::background));
    expect<Queued>(b);

    const auto before = Clock::now();
    c.send(request(Priority::background));
    const Busy& busy = expect<Busy>(c);
    CHECK(busy.reason == BusyReason::queue_full);
    CHECK(Clock::now() - before < 1s);
    CHECK_FALSE(c.next().has_value());

    // The other queue is not full.
    Connection e(d.socket());
    e.send(request(Priority::interactive));
    CHECK(expect<Queued>(e).position == 1);
}

TEST_CASE("daemon: a full memory budget answers busy on the header") {
    // Room for exactly one request of the largest size.
    Daemon d(std::format(R"("bounds": {{"max_bytes": 1000}}, "max_pending_bytes": {})", 1000 + kRequestEnvelope));
    Connection hog(d.socket());
    const auto header = frame_header(static_cast<std::uint32_t>(1000 + kRequestEnvelope));
    hog.send_raw(std::string_view(header.data(), header.size()));  // announced, never sent

    // The daemon handles connections in turn: give it the hog's header first.
    std::this_thread::sleep_for(100ms);
    Connection c(d.socket());
    c.send(request());
    CHECK(expect<Busy>(c).reason == BusyReason::queue_full);

    // The hog leaves, its reservation goes with it.
    hog.close();
    std::this_thread::sleep_for(100ms);
    Connection e(d.socket());
    e.send(request());
    expect<Queued>(e);
}

TEST_CASE("daemon: a deadline drops a job still queued") {
    Daemon d;
    d.delay(1.5);
    Connection a(d.socket()), b(d.socket());
    start_running(a, request());

    Request late = request();
    late.deadline_ms = 200;
    const auto before = Clock::now();
    b.send(late);
    expect<Queued>(b);
    CHECK(expect<Busy>(b).reason == BusyReason::deadline);
    const auto waited = Clock::now() - before;
    CHECK(waited >= 200ms);
    CHECK(waited < 1s);  // not at the end of the running job

    // A deadline only concerns the queue: a started job runs to its end.
    CHECK(expect<Response>(a).error.empty());
}

TEST_CASE("daemon: a caller that hangs up cancels its job") {
    Daemon d;
    d.delay(30);
    Connection a(d.socket()), b(d.socket());
    start_running(a, request());
    b.send(request());
    CHECK(expect<Queued>(b).position == 1);

    // Queued: it leaves the queue, the next one does not count it.
    b.close();
    std::this_thread::sleep_for(100ms);
    Connection c(d.socket());
    c.send(request());
    CHECK(expect<Queued>(c).position == 1);

    // Running: its worker is killed at once, not in 30 s.
    d.delay(0);
    const auto before = Clock::now();
    a.close();
    expect<Started>(c);
    CHECK(expect<Response>(c).error.empty());
    CHECK(Clock::now() - before < 10s);
    CHECK(d.log().find("result=cancelled") != std::string::npos);
}

TEST_CASE("daemon: a worker past its time is killed, and its caller gets a failure") {
    Daemon d(R"("timeout_s": 1)");
    d.delay(10);
    Connection a(d.socket());
    const auto before = Clock::now();
    start_running(a, request());
    const Failure& f = expect<Failure>(a);
    CHECK(f.message.find("1 s") != std::string::npos);
    CHECK(Clock::now() - before < 5s);

    // The slot is free again.
    d.delay(0);
    Connection b(d.socket());
    b.send(request());
    CHECK(expect<Queued>(b).position == 0);
}

TEST_CASE("daemon: a worker that fails is a failure") {
    Daemon d("", "#!/bin/sh\nexit 3\n");
    Connection a(d.socket());
    start_running(a, request());
    CHECK(expect<Failure>(a).message.find("exit code 3") != std::string::npos);

    Daemon missing(R"("worker": "/nonexistent/tirage-worker")");
    Connection b(missing.socket());
    b.send(request());
    expect<Queued>(b);
    CHECK(expect<Failure>(b).message.find("not launched") != std::string::npos);
}

TEST_CASE("daemon: a reload applies to the requests that follow") {
    Daemon d(R"("queue": {"interactive": 1, "background": 1})");
    d.delay(2);
    Connection a(d.socket()), b(d.socket());
    start_running(a, request(Priority::background));
    b.send(request(Priority::background));
    expect<Queued>(b);

    d.write_config(R"("queue": {"interactive": 1, "background": 4})");
    d.signal(SIGHUP);
    std::this_thread::sleep_for(200ms);
    Connection c(d.socket());
    c.send(request(Priority::background));
    CHECK(expect<Queued>(c).position == 2);
    CHECK(d.log().find("reloaded") != std::string::npos);

    // A broken configuration is not applied.
    d.write_config(R"("queue": {"interactive": 0})");
    d.signal(SIGHUP);
    std::this_thread::sleep_for(200ms);
    CHECK(d.log().find("configuration kept") != std::string::npos);
}

TEST_CASE("daemon: stopping answers the queue busy and lets the running job finish") {
    Daemon d;
    d.delay(1);
    Connection a(d.socket()), b(d.socket());
    start_running(a, request());
    b.send(request());
    expect<Queued>(b);

    d.signal(SIGTERM);
    CHECK(expect<Busy>(b).reason == BusyReason::stopping);
    CHECK(expect<Response>(a).error.empty());
    CHECK(d.wait() == 0);
    CHECK_FALSE(fs::exists(d.socket()));
}

// Plan § 9, leak non-regression: long, so skipped by default. Run it in
// debian:trixie-slim, whose libheif is in the leaking range:
//   tirage-integration -tc='*memory stays flat*' --no-skip
TEST_CASE("daemon: memory stays flat over many AVIF encodes" * doctest::skip()) {
    const int count = std::getenv("TIRAGE_LEAK_COUNT") ? std::atoi(std::getenv("TIRAGE_LEAK_COUNT")) : 500;
    Daemon d(R"("threads": 2)");
    const auto rss_kib = [&] {
        const std::string status = read_file(std::format("/proc/{}/status", d.pid()));
        const auto at = status.find("VmRSS:");
        return std::atol(status.c_str() + at + 6);
    };

    Request r = request();
    r.profile.variants["small"] = Variant{.widths = {320, 640},
                                          .formats = {OutputFormat::avif},
                                          .quality = {QualityStep{.avif = 60}}};
    r.input = png(1024, 768);

    long rss_early = 0;
    for (int i = 0; i < count; ++i) {
        Connection c(d.socket());
        c.send(r);
        const auto messages = c.all();
        REQUIRE(messages.size() == 3);
        REQUIRE(as<Response>(messages[2]).error.empty());
        if (i == 19) rss_early = rss_kib();
    }
    const long rss_late = rss_kib();

    // Each worker's peak, from the journal, in order.
    std::vector<long> peaks;
    const std::string log = d.log();
    const std::regex re("maxrss_kib=([0-9]+)");
    for (auto it = std::sregex_iterator(log.begin(), log.end(), re); it != std::sregex_iterator(); ++it)
        peaks.push_back(std::stol((*it)[1]));
    REQUIRE(peaks.size() == static_cast<std::size_t>(count));
    const auto mean = [](auto first, auto last) {
        return std::accumulate(first, last, 0.0) / static_cast<double>(std::distance(first, last));
    };
    const double first = mean(peaks.begin(), peaks.begin() + 20);
    const double last = mean(peaks.end() - 20, peaks.end());
    MESSAGE(std::format("daemon RSS {} KiB after 20, {} KiB after {}. Worker peak {:.0f} KiB over the "
                        "first 20, {:.0f} KiB over the last 20",
                        rss_early, rss_late, count, first, last));
    CHECK(rss_late - rss_early < 2048);
    CHECK(last < first * 1.1);
}
