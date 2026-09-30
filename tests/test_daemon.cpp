// The daemon's pure logic (plan § 6 and § 9): the scheduler on a simulated
// clock, the configuration, the frames and the daemon's messages.

#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <string>
#include <variant>

#include "daemon/config.h"
#include "daemon/scheduler.h"
#include "tirage/frame.h"
#include "tirage/protocol.h"
#include "tirage/request.h"

using namespace tirage;
using namespace tirage::daemon;
using namespace std::chrono_literals;

namespace {

// The simulated clock: any point will do, only differences matter.
const TimePoint t0{};

}  // namespace

TEST_CASE("scheduler: one encode at a time, first come first served") {
    Scheduler s;
    CHECK(s.enqueue(1, Priority::interactive, std::nullopt) == 0);
    CHECK(s.start() == 1u);
    CHECK_FALSE(s.start().has_value());  // 1 runs

    CHECK(s.enqueue(2, Priority::interactive, std::nullopt) == 1);
    CHECK(s.enqueue(3, Priority::interactive, std::nullopt) == 2);
    CHECK_FALSE(s.start().has_value());
    s.finish(1);
    CHECK(s.start() == 2u);
    s.finish(2);
    CHECK(s.start() == 3u);
    s.finish(3);
    CHECK_FALSE(s.start().has_value());
    CHECK_FALSE(s.running().has_value());
}

TEST_CASE("scheduler: interactive passes the background queue") {
    Scheduler s;
    REQUIRE(s.enqueue(1, Priority::background, std::nullopt) == 0);
    REQUIRE(s.start() == 1u);
    for (JobId id = 2; id <= 5; ++id) REQUIRE(s.enqueue(id, Priority::background, std::nullopt));

    // Behind the running one only, not behind the four waiting.
    CHECK(s.enqueue(6, Priority::interactive, std::nullopt) == 1);
    s.finish(1);
    CHECK(s.start() == 6u);
    s.finish(6);
    CHECK(s.start() == 2u);
}

TEST_CASE("scheduler: a background position counts the interactive jobs ahead") {
    Scheduler s;
    REQUIRE(s.enqueue(1, Priority::interactive, std::nullopt));
    REQUIRE(s.start() == 1u);
    REQUIRE(s.enqueue(2, Priority::interactive, std::nullopt) == 1);
    REQUIRE(s.enqueue(3, Priority::background, std::nullopt) == 2);
    // The running one, the interactive one and the background one before it.
    CHECK(s.enqueue(4, Priority::background, std::nullopt) == 3);
    // An interactive job arriving later passes both: the position given to 4
    // was a snapshot, it is not sent again.
    CHECK(s.enqueue(5, Priority::interactive, std::nullopt) == 2);
}

TEST_CASE("scheduler: bounded queues, each on its own") {
    Scheduler s({.interactive = 1, .background = 2});
    REQUIRE(s.enqueue(1, Priority::interactive, std::nullopt));
    REQUIRE(s.start() == 1u);  // the running job is not counted

    CHECK(s.enqueue(2, Priority::interactive, std::nullopt));
    CHECK_FALSE(s.enqueue(3, Priority::interactive, std::nullopt));
    CHECK(s.enqueue(4, Priority::background, std::nullopt));
    CHECK(s.enqueue(5, Priority::background, std::nullopt));
    CHECK_FALSE(s.enqueue(6, Priority::background, std::nullopt));

    // A lowered limit keeps what is queued and refuses what comes.
    s.set_limits({.interactive = 1, .background = 1});
    CHECK(s.queued(Priority::background) == 2);
    CHECK_FALSE(s.enqueue(7, Priority::background, std::nullopt));
}

TEST_CASE("scheduler: a deadline drops a queued job, never a running one") {
    Scheduler s;
    REQUIRE(s.enqueue(1, Priority::interactive, t0 + 100ms));
    REQUIRE(s.start() == 1u);
    REQUIRE(s.enqueue(2, Priority::interactive, t0 + 500ms));
    REQUIRE(s.enqueue(3, Priority::background, t0 + 200ms));
    REQUIRE(s.enqueue(4, Priority::background, std::nullopt));

    CHECK(s.next_deadline() == t0 + 200ms);  // 1 runs, its deadline is over
    CHECK(s.expire(t0 + 199ms).empty());
    CHECK(s.expire(t0 + 200ms) == std::vector<JobId>{3});
    CHECK(s.next_deadline() == t0 + 500ms);
    CHECK(s.expire(t0 + 1h) == std::vector<JobId>{2});
    CHECK(s.running() == 1u);
    CHECK_FALSE(s.next_deadline().has_value());

    s.finish(1);
    CHECK(s.start() == 4u);
}

TEST_CASE("scheduler: cancelling takes a job out of its queue") {
    Scheduler s;
    REQUIRE(s.enqueue(1, Priority::interactive, std::nullopt));
    REQUIRE(s.start() == 1u);
    REQUIRE(s.enqueue(2, Priority::background, std::nullopt));
    REQUIRE(s.enqueue(3, Priority::background, std::nullopt));

    CHECK(s.cancel(2));
    CHECK_FALSE(s.cancel(2));
    CHECK_FALSE(s.cancel(1));  // running: the daemon kills its worker instead
    s.finish(1);
    CHECK(s.start() == 3u);
}

TEST_CASE("scheduler: stopping drains both queues in start order") {
    Scheduler s;
    REQUIRE(s.enqueue(1, Priority::background, std::nullopt));
    REQUIRE(s.start() == 1u);
    REQUIRE(s.enqueue(2, Priority::background, std::nullopt));
    REQUIRE(s.enqueue(3, Priority::interactive, std::nullopt));
    CHECK(s.drain() == std::vector<JobId>{3, 2});
    CHECK(s.queued(Priority::interactive) + s.queued(Priority::background) == 0);
    CHECK(s.running() == 1u);
}

TEST_CASE("config: defaults, partial files, unknown keys") {
    const auto empty = parse_config("{}");
    REQUIRE(empty.has_value());
    CHECK(empty->timeout_s == 120);
    CHECK(empty->bounds.max_bytes == kDefaultBounds.max_bytes);
    CHECK_FALSE(empty->threads.has_value());

    const auto some = parse_config(R"({"threads": 3, "queue": {"background": 8}, "bounds": {"max_edge": 8000}})");
    REQUIRE(some.has_value());
    CHECK(some->threads == 3);
    CHECK(some->queue.interactive == 32);
    CHECK(some->queue.background == 8);
    CHECK(some->bounds.max_edge == 8000);
    CHECK(some->bounds.max_pixels == kDefaultBounds.max_pixels);

    CHECK_FALSE(parse_config(R"({"thread": 3})").has_value());
    CHECK_FALSE(parse_config("not json").has_value());
}

TEST_CASE("config: checks") {
    const auto error = [](std::string_view json) {
        const auto c = parse_config(json);
        REQUIRE_FALSE(c.has_value());
        return c.error().message;
    };
    CHECK(error(R"({"threads": 0})").starts_with("threads"));
    CHECK(error(R"({"socket": "tirage.sock"})").starts_with("socket"));
    CHECK(error(R"({"queue": {"interactive": 0}})").starts_with("queue"));
    CHECK(error(R"({"probe_limit": 0})").starts_with("probe_limit"));
    CHECK(error(R"({"timeout_s": 0})").starts_with("timeout_s"));
    CHECK(error(R"({"bounds": {"max_pixels": 0}})").starts_with("bounds"));
    // Room for at least one request of the largest size.
    CHECK(error(R"({"max_pending_bytes": 1000})").starts_with("max_pending_bytes"));
    CHECK(error(R"({"bounds": {"max_bytes": 5000000000}})").starts_with("bounds.max_bytes"));
}

TEST_CASE("config: the CPU budget, in the plan's order") {
    CHECK(resolve_threads(3, 5, 8, 16) == 3);
    CHECK(resolve_threads(std::nullopt, 5, 8, 16) == 5);
    CHECK(resolve_threads(std::nullopt, 0, 8, 16) == 8);  // an absurd variable is skipped
    CHECK(resolve_threads(std::nullopt, std::nullopt, 8, 16) == 8);
    CHECK(resolve_threads(std::nullopt, std::nullopt, 0, 16) == 16);
    CHECK(resolve_threads(std::nullopt, std::nullopt, 0, 0) == 1);
}

TEST_CASE("frames: little-endian length, then BEVE") {
    const auto h = frame_header(0x01020304);
    CHECK(h[0] == 0x04);
    CHECK(h[3] == 0x01);
    CHECK(frame_length(h.data()) == 0x01020304u);
    const auto big = frame_header(0xFFFFFFFF);
    CHECK(frame_length(big.data()) == 0xFFFFFFFFu);

    CHECK(max_request_frame(kDefaultBounds) == kDefaultBounds.max_bytes + kRequestEnvelope);

    const auto frame = make_frame(Message{Queued{.position = 3}});
    REQUIRE(frame.has_value());
    REQUIRE(frame->size() > kFrameHeaderSize);
    CHECK(frame_length(frame->data()) == frame->size() - kFrameHeaderSize);
    Message back;
    REQUIRE_FALSE(glz::read_beve(back, std::string_view(*frame).substr(kFrameHeaderSize)));
    REQUIRE(std::holds_alternative<Queued>(back));
    CHECK(std::get<Queued>(back).position == 3);
}

TEST_CASE("messages: each kind survives BEVE") {
    const auto round_trip = [](const Message& m) {
        std::string beve;
        REQUIRE_FALSE(glz::write_beve(m, beve));
        Message back;
        REQUIRE_FALSE(glz::read_beve(back, beve));
        return back;
    };
    CHECK(std::get<Started>(round_trip(Started{.waited_ms = 42})).waited_ms == 42);
    const Busy busy = std::get<Busy>(round_trip(Busy{BusyReason::deadline, "late"}));
    CHECK(busy.reason == BusyReason::deadline);
    CHECK(busy.message == "late");
    CHECK(std::get<Failure>(round_trip(Failure{"killed"})).message == "killed");
    const Response r = std::get<Response>(round_trip(Response{.error = "no", .code = Code::image_too_small}));
    CHECK(r.code == Code::image_too_small);
}

TEST_CASE("deadline_ms: positive, encode only") {
    Request r;
    r.protocol = kProtocolVersion;
    r.profile.version = kProfileVersion;
    r.profile.input.formats = {InputFormat::png};
    r.profile.variants["v"] = Variant{
        .widths = {400}, .formats = {OutputFormat::avif}, .quality = {QualityStep{.avif = 60}}};
    r.variants = {"v"};
    r.input = "x";

    r.deadline_ms = 1000;
    CHECK_FALSE(validate_request(r).has_value());
    r.deadline_ms = 0;
    CHECK(validate_request(r)->message.starts_with("deadline_ms"));
    r.deadline_ms = 1000;
    r.operation = Operation::probe;
    r.variants.clear();
    CHECK(validate_request(r)->message.starts_with("deadline_ms"));
}
