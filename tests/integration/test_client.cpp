// libtirage-client against a real tiraged (plan § 5 and § 9): the three final
// messages and the transport errors, events, cancellation, status, and direct
// mode.

#include <doctest/doctest.h>

#include <pwd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "harness.h"
#include "tirage/client.h"

using namespace tirage;
using namespace tirage::test;
using namespace std::chrono_literals;

namespace {

// The events a call handed over, from its thread.
struct Recorder {
    std::mutex mutex;
    std::vector<client::Event> events;
    std::atomic<bool> queued = false;
    std::atomic<bool> started = false;

    std::function<void(const client::Event&)> callback() {
        return [this](const client::Event& e) {
            const std::lock_guard lock(mutex);
            events.push_back(e);
            if (std::holds_alternative<Queued>(e)) queued = true;
            else started = true;
        };
    }
};

template <class T>
const T& reply_as(const std::expected<client::Reply, std::string>& r) {
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error()));
    REQUIRE_MESSAGE(std::holds_alternative<T>(*r), "reply of another kind, index " << r->index());
    return std::get<T>(*r);
}

void wait_for(const std::atomic<bool>& flag) {
    for (int i = 0; i < 1000 && !flag; ++i) std::this_thread::sleep_for(10ms);
    REQUIRE(flag);
}

// Sets environment variables for the test's duration.
class Env {
public:
    Env& set(const char* name, const char* value) {
        const char* old = std::getenv(name);
        saved_.push_back({name, old ? std::optional<std::string>(old) : std::nullopt});
        ::setenv(name, value, 1);
        return *this;
    }
    ~Env() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            if (it->second) ::setenv(it->first.c_str(), it->second->c_str(), 1);
            else ::unsetenv(it->first.c_str());
        }
    }

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

std::string user_name() {
    const passwd* pw = ::getpwuid(::getuid());
    return pw ? pw->pw_name : std::to_string(::getuid());
}

}  // namespace

TEST_CASE("client: an encode through the daemon, its events in order") {
    Daemon d;
    Recorder rec;
    const auto r = client::call(request(), {.socket = d.socket(), .on_event = rec.callback()});
    const Response& response = reply_as<Response>(r);
    CHECK_MESSAGE(response.error.empty(), response.error);
    REQUIRE(response.outputs.size() == 1);
    CHECK(response.outputs[0].bytes.substr(8, 4) == "WEBP");

    REQUIRE(rec.events.size() == 2);
    REQUIRE(std::holds_alternative<Queued>(rec.events[0]));
    CHECK(std::get<Queued>(rec.events[0]).position == 0);
    CHECK(std::holds_alternative<Started>(rec.events[1]));
}

TEST_CASE("client: a probe has no events, TIRAGE_SOCKET finds the daemon") {
    Daemon d;
    Env env;
    env.set("TIRAGE_SOCKET", d.socket().c_str());
    Recorder rec;
    const auto r = client::call(request(Priority::interactive, Operation::probe), {.on_event = rec.callback()});
    const Response& response = reply_as<Response>(r);
    CHECK_MESSAGE(response.error.empty(), response.error);
    REQUIRE(response.source.has_value());
    CHECK(response.source->width == 64);
    CHECK(rec.events.empty());
}

TEST_CASE("client: refusals and busy come back as they are") {
    Daemon d(R"("queue": {"interactive": 1, "background": 1})");
    const client::Options options{.socket = d.socket()};

    Request gif = request();
    gif.input = "GIF89a";
    CHECK(reply_as<Response>(client::call(gif, options)).code == Code::unsupported_format);

    d.delay(2);
    Connection a(d.socket()), b(d.socket());
    start_running(a, request(Priority::background));
    b.send(request(Priority::background));
    expect<Queued>(b);
    CHECK(reply_as<Busy>(client::call(request(Priority::background), options)).reason == BusyReason::queue_full);
}

TEST_CASE("client: a request above the frame bound gets its refusal, though the daemon stops reading") {
    Daemon d(R"("bounds": {"max_bytes": 1000})");
    Request big = request();
    big.input.resize(std::size_t{3} << 20, 'x');  // more than the socket buffers hold
    const auto r = client::call(big, {.socket = d.socket()});
    CHECK(reply_as<Response>(r).code == Code::file_too_large);
}

TEST_CASE("client: no daemon is a transport error") {
    const auto r = client::call(request(), {.socket = "/nonexistent/tirage.sock"});
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("cannot reach tiraged") != std::string::npos);
    CHECK_FALSE(client::status({.socket = "/nonexistent/tirage.sock"}).has_value());
}

TEST_CASE("client: a stop request cancels the job, queued or running") {
    Daemon d;
    d.delay(30);

    // Queued behind a job that holds the slot.
    Connection a(d.socket());
    start_running(a, request());
    {
        std::stop_source stop;
        Recorder rec;
        std::expected<client::Reply, std::string> r;
        std::jthread t([&] {
            r = client::call(request(), {.socket = d.socket(), .on_event = rec.callback(), .stop = stop.get_token()});
        });
        wait_for(rec.queued);
        stop.request_stop();
        t.join();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == client::kCancelled);
    }
    std::this_thread::sleep_for(100ms);
    CHECK(d.log().find("result=cancelled") != std::string::npos);

    // Running: its worker is killed, not left for 30 s.
    a.close();
    std::stop_source stop;
    Recorder rec;
    std::expected<client::Reply, std::string> r;
    std::jthread t([&] {
        r = client::call(request(), {.socket = d.socket(), .on_event = rec.callback(), .stop = stop.get_token()});
    });
    wait_for(rec.started);
    const auto before = Clock::now();
    stop.request_stop();
    t.join();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == client::kCancelled);

    // The slot frees at once: the next job starts without waiting.
    d.delay(0);
    Connection c(d.socket());
    c.send(request());
    CHECK(expect<Queued>(c).position <= 1);
    expect<Started>(c);
    CHECK(expect<Response>(c).error.empty());
    CHECK(Clock::now() - before < 10s);

    // A stop requested before the call sends nothing.
    std::stop_source early;
    early.request_stop();
    const auto never = client::call(request(), {.socket = d.socket(), .stop = early.get_token()});
    REQUIRE_FALSE(never.has_value());
    CHECK(never.error() == client::kCancelled);
}

TEST_CASE("client: status shows the budget, what runs and what waits, in start order") {
    Daemon d(R"("threads": 2, "queue": {"interactive": 4, "background": 8})");
    d.delay(2);
    Connection a(d.socket()), b(d.socket()), c(d.socket());
    start_running(a, request(Priority::background));
    b.send(request(Priority::background));
    expect<Queued>(b);
    c.send(request(Priority::interactive));
    expect<Queued>(c);

    const auto s = client::status({.socket = d.socket()});
    REQUIRE_MESSAGE(s.has_value(), (s ? "" : s.error()));
    CHECK(s->version == TIRAGE_VERSION);
    CHECK(s->threads == 2);
    CHECK(s->busy_threads == 2);
    CHECK(s->queue.interactive == 4);
    CHECK(s->queue.background == 8);
    CHECK(s->pending_bytes > 0);

    REQUIRE(s->running.size() == 1);
    CHECK(s->running[0].caller == user_name());
    CHECK(s->running[0].operation == Operation::encode);
    CHECK(s->running[0].priority == Priority::background);
    CHECK(s->running[0].running_ms.has_value());
    CHECK(s->running[0].in_bytes == static_cast<std::int64_t>(request().input.size()));

    // The interactive job, sent last, starts first.
    REQUIRE(s->queued.size() == 2);
    CHECK(s->queued[0].priority == Priority::interactive);
    CHECK(s->queued[1].priority == Priority::background);
    CHECK_FALSE(s->queued[0].running_ms.has_value());
    CHECK(s->queued[1].waited_ms >= s->queued[0].waited_ms);

    // Status is not a job: nothing in the journal.
    CHECK(d.log().find("op=status") == std::string::npos);
}

TEST_CASE("client: direct mode runs the worker itself, and has no status") {
    Env env;
    env.set("TIRAGE_DIRECT", "1").set("TIRAGE_WORKER", TIRAGE_WORKER_PATH).set("TIRAGE_SOCKET", "/nonexistent");

    Recorder rec;
    const auto r = client::call(request(), {.on_event = rec.callback()});
    const Response& response = reply_as<Response>(r);
    CHECK_MESSAGE(response.error.empty(), response.error);
    CHECK(response.outputs.size() == 1);
    CHECK(rec.events.size() == 2);  // the same sequence as through the daemon

    Request gif = request();
    gif.input = "GIF89a";
    CHECK(reply_as<Response>(client::call(gif)).code == Code::unsupported_format);

    // A worker that cannot be launched is a failure, not a transport error.
    env.set("TIRAGE_WORKER", "/nonexistent/tirage-worker");
    CHECK(reply_as<Failure>(client::call(request())).message.find("not launched") != std::string::npos);

    const auto s = client::status();
    REQUIRE_FALSE(s.has_value());
    CHECK(s.error().find("direct mode") != std::string::npos);
}
