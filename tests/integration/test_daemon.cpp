// Integration harness, second half: the real tiraged, on a socket of its own,
// with the real worker or a worker script that waits or fails on demand
// (plan § 9). Each test starts its own daemon with the configuration it needs.
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
#include <numeric>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <vips/vips8>

#include "tirage/frame.h"
#include "tirage/protocol.h"

extern char** environ;

using namespace tirage;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

void write_file(const fs::path& p, std::string_view content) {
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

std::string png(int width, int height) {
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

Request request(Priority priority = Priority::interactive, Operation op = Operation::encode) {
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
const T& as(const Message& m) {
    REQUIRE_MESSAGE(std::holds_alternative<T>(m), "message of another kind, index " << m.index());
    return std::get<T>(m);
}

template <class T>
const T& expect(Connection& c) {
    static thread_local Message last;
    auto m = c.next();
    REQUIRE(m.has_value());
    last = std::move(*m);
    return as<T>(last);
}

// Sends a request and reads until Started: the job is running.
void start_running(Connection& c, Request r) {
    c.send(r);
    expect<Queued>(c);
    expect<Started>(c);
}

}  // namespace

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
