#pragma once

// tiraged's event loop (plan § 6): one thread, one epoll. It reads requests,
// validates them, queues the encodes, launches one worker at a time (probes
// beside, up to their limit), kills what runs too long or lost its caller, and
// sends the answers back.
//
// Each connection carries one request. The daemon answers with events (Queued,
// Started) then one final message, and closes. The caller keeps the connection
// open until then: a hang-up, even a half-close, cancels the request (out of
// the queue, or its worker killed).
//
// The daemon never links libvips and never parses an image: only the worker
// does, in its own process.

#include <fcntl.h>
#include <pwd.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include <glaze/glaze.hpp>

#include "config.h"
#include "scheduler.h"
#include "tirage/frame.h"
#include "tirage/protocol.h"
#include "tirage/request.h"
#include "tirage/spawn.h"

namespace tirage::daemon {

// Read first, with unknown keys skipped: a request of another protocol version
// gets a refusal that says so, rather than a failure to read it.
struct Envelope {
    int protocol = 0;
};

// Environment the daemon hands its budget from, gathered by main().
struct Host {
    std::optional<int> env_threads;  // TIRAGE_THREADS
    int affinity = 0;                // CPUs in the daemon's affinity mask
    unsigned hardware = 0;           // hardware_concurrency
};

inline void log_line(const std::string& line) { std::fprintf(stderr, "tiraged: %s\n", line.c_str()); }

class Server {
public:
    // `listener` is a listening, non-blocking socket. `unlink_path` is removed
    // at the end when the daemon bound it itself (empty under socket activation).
    Server(Config config, std::string config_path, Host host, int listener, std::string unlink_path)
        : config_(std::move(config)),
          config_path_(std::move(config_path)),
          host_(host),
          scheduler_(config_.queue),
          listener_(listener),
          unlink_path_(std::move(unlink_path)) {
        threads_ = resolve_threads(config_.threads, host_.env_threads, host_.affinity, host_.hardware);
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    ~Server() {
        if (!unlink_path_.empty()) ::unlink(unlink_path_.c_str());
    }

    // Runs until SIGTERM or SIGINT, then until the running workers are done.
    // Returns the process exit code.
    int run() {
        epoll_.fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_.fd < 0) return fatal("epoll_create1");

        sigset_t signals;
        sigemptyset(&signals);
        for (const int sig : {SIGTERM, SIGINT, SIGHUP}) sigaddset(&signals, sig);
        if (::sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) return fatal("sigprocmask");
        signal_.fd = ::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
        if (signal_.fd < 0) return fatal("signalfd");

        watch(listener_.fd, EPOLLIN, key(Kind::listener, 0));
        watch(signal_.fd, EPOLLIN, key(Kind::signal, 0));
        log_line(std::format("ready: {} threads, queues {} interactive and {} background, {} probes, "
                             "timeout {} s",
                             threads_, config_.queue.interactive, config_.queue.background,
                             config_.probe_limit, config_.timeout_s));

        std::array<epoll_event, 64> events;
        while (!(stopping_ && works_.empty() && conns_.empty())) {
            const int n = ::epoll_wait(epoll_.fd, events.data(), events.size(), wake_timeout(Clock::now()));
            if (n < 0 && errno != EINTR) return fatal("epoll_wait");
            for (int i = 0; i < n; ++i) dispatch(events[i]);
            tick(Clock::now());
        }
        log_line("stopped");
        return 0;
    }

private:
    using ConnId = std::uint64_t;
    using Fd = tirage::detail::Fd;

    enum class Kind : std::uint64_t { listener = 1, signal, conn, output, pid };
    static std::uint64_t key(Kind k, std::uint64_t id) { return (static_cast<std::uint64_t>(k) << 56) | id; }

    struct Conn {
        Fd fd;
        std::string caller;
        enum class State { header, body, waiting, closing } state = State::header;
        std::array<char, kFrameHeaderSize> header{};
        std::size_t header_got = 0;
        std::string body;
        std::size_t body_got = 0;
        std::int64_t reserved = 0;  // counted in pending_bytes_ until handed to a Work
        std::string out;
        std::size_t out_sent = 0;
        std::optional<JobId> job;
        std::uint32_t interest = 0;
    };

    // An admitted request: queued, or running in a worker.
    struct Work {
        JobId id = 0;
        ConnId conn = 0;  // 0 once its caller went away
        std::string caller;
        Request request;
        std::int64_t reserved = 0;
        std::size_t in_bytes = 0;
        TimePoint received;
        TimePoint started;
        // Running.
        pid_t pid = -1;
        Fd output;
        Fd pidfd;
        std::string reply;
        bool eof = false;
        bool reaped = false;
        int status = 0;
        long maxrss_kib = 0;
        TimePoint kill_at;
        bool killed = false;
        bool timed_out = false;
        bool too_big = false;
        bool cancelled = false;

        bool running() const { return pid > 0; }
        bool probe() const { return request.operation == Operation::probe; }
    };

    // ------------------------------------------------------------------
    // Plumbing

    int fatal(const char* what) {
        log_line(std::format("{}: {}", what, std::strerror(errno)));
        return 1;
    }

    void watch(int fd, std::uint32_t events, std::uint64_t data) {
        epoll_event ev{.events = events, .data = {.u64 = data}};
        ::epoll_ctl(epoll_.fd, EPOLL_CTL_ADD, fd, &ev);
    }

    void unwatch(int fd) { ::epoll_ctl(epoll_.fd, EPOLL_CTL_DEL, fd, nullptr); }

    void set_interest(ConnId id, Conn& c) {
        std::uint32_t want = 0;
        if (c.state != Conn::State::closing) want |= EPOLLIN | EPOLLRDHUP;
        if (c.out_sent < c.out.size()) want |= EPOLLOUT;
        if (want == c.interest) return;
        epoll_event ev{.events = want, .data = {.u64 = key(Kind::conn, id)}};
        ::epoll_ctl(epoll_.fd, EPOLL_CTL_MOD, c.fd.fd, &ev);
        c.interest = want;
    }

    static std::string ms(std::chrono::nanoseconds d) {
        return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    }

    static std::string_view name(Priority p) { return p == Priority::interactive ? "interactive" : "background"; }
    static std::string_view name(BusyReason r) {
        switch (r) {
            case BusyReason::queue_full: return "queue_full";
            case BusyReason::deadline: return "deadline";
            case BusyReason::stopping: return "stopping";
        }
        return "?";
    }

    // One journal line per request (plan § 6): who, what, how long, how much.
    void log_request(const std::string& caller, const Request* r, const std::string& outcome) {
        std::string line = "request caller=" + caller;
        if (r) {
            line += r->operation == Operation::probe ? " op=probe" : " op=encode";
            if (r->operation == Operation::encode) line += std::format(" priority={}", name(r->priority));
        }
        log_line(line + " " + outcome);
    }

    void log_work(const Work& w, const std::string& outcome) {
        std::string line = std::format("in={} ", w.in_bytes);
        if (w.running()) {
            const auto now = Clock::now();
            if (!w.probe()) line += std::format("waited_ms={} ", ms(w.started - w.received));
            line += std::format("ran_ms={} maxrss_kib={} ", ms(now - w.started), w.maxrss_kib);
        } else if (!w.probe()) {
            line += std::format("waited_ms={} ", ms(Clock::now() - w.received));
        }
        log_request(w.caller, &w.request, line + outcome);
    }

    // ------------------------------------------------------------------
    // Events

    void dispatch(const epoll_event& ev) {
        const auto kind = static_cast<Kind>(ev.data.u64 >> 56);
        const std::uint64_t id = ev.data.u64 & ((std::uint64_t{1} << 56) - 1);
        switch (kind) {
            case Kind::listener: accept_all(); break;
            case Kind::signal: on_signal(); break;
            case Kind::conn: on_conn(id, ev.events); break;
            case Kind::output: on_output(id); break;
            case Kind::pid: on_exit(id); break;
        }
    }

    void on_signal() {
        signalfd_siginfo info;
        while (::read(signal_.fd, &info, sizeof info) == sizeof info) {
            if (info.ssi_signo == SIGHUP) reload();
            else stop();
        }
    }

    void accept_all() {
        for (;;) {
            Fd fd{::accept4(listener_.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
            if (fd.fd < 0) {
                if (errno == EINTR) continue;
                return;  // EAGAIN, or a transient error the next wake-up retries
            }
            Conn c;
            c.caller = peer_name(fd.fd);
            c.fd = std::move(fd);
            c.interest = EPOLLIN | EPOLLRDHUP;
            const ConnId id = ++last_conn_;
            watch(c.fd.fd, c.interest, key(Kind::conn, id));
            conns_.emplace(id, std::move(c));
        }
    }

    // The caller's user name, for the journal only (SO_PEERCRED).
    static std::string peer_name(int fd) {
        ucred cred{};
        socklen_t len = sizeof cred;
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return "?";
        std::array<char, 1024> buf;
        passwd pw{};
        passwd* found = nullptr;
        if (::getpwuid_r(cred.uid, &pw, buf.data(), buf.size(), &found) == 0 && found) return pw.pw_name;
        return std::to_string(cred.uid);
    }

    void on_conn(ConnId id, std::uint32_t events) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        Conn& c = it->second;
        if (events & EPOLLOUT) {
            if (!flush(id)) return;
        }
        if (events & (EPOLLERR | EPOLLHUP)) return drop(id, "hang-up");
        if (!(events & (EPOLLIN | EPOLLRDHUP))) return;
        switch (c.state) {
            case Conn::State::header:
            case Conn::State::body: read_request(id, c); break;
            case Conn::State::waiting: {
                // Nothing more is expected: data or end of file both mean the
                // caller is no longer waiting for this answer.
                char byte;
                const auto n = ::recv(c.fd.fd, &byte, 1, 0);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
                drop(id, n == 0 ? "hang-up" : "unexpected data after the request");
                break;
            }
            case Conn::State::closing: break;
        }
    }

    void read_request(ConnId id, Conn& c) {
        for (;;) {
            if (c.state == Conn::State::header) {
                const auto n = ::recv(c.fd.fd, c.header.data() + c.header_got, c.header.size() - c.header_got, 0);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno == EAGAIN) return;
                if (n <= 0) return drop(id, "hang-up before the request");
                c.header_got += static_cast<std::size_t>(n);
                if (c.header_got < c.header.size()) continue;

                // The length is checked before anything is allocated for it.
                const std::int64_t length = frame_length(c.header.data());
                const std::int64_t bound = max_request_frame(config_.bounds);
                if (length > bound) {
                    log_request(c.caller, nullptr, "result=refused code=4 frame too large");
                    return finish(id, refused({Code::file_too_large,
                                               std::format("request frame of {} bytes, the bound is {}",
                                                           length, bound)}));
                }
                if (pending_bytes_ + length > config_.max_pending_bytes) {
                    log_request(c.caller, nullptr, "result=busy reason=queue_full (memory)");
                    return finish(id, Busy{BusyReason::queue_full,
                                           "the daemon holds as many request bytes as it may, try again"});
                }
                c.reserved = length;
                pending_bytes_ += length;
                c.body.resize(static_cast<std::size_t>(length));
                c.state = Conn::State::body;
            }
            if (c.body_got < c.body.size()) {
                const auto n = ::recv(c.fd.fd, c.body.data() + c.body_got, c.body.size() - c.body_got, 0);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno == EAGAIN) return;
                if (n <= 0) return drop(id, "hang-up during the request");
                c.body_got += static_cast<std::size_t>(n);
                if (c.body_got < c.body.size()) continue;
            }
            return admit(id, c);
        }
    }

    // A whole request frame has arrived: check it, then queue or launch it.
    void admit(ConnId id, Conn& c) {
        const TimePoint now = Clock::now();
        std::string body = std::exchange(c.body, {});

        Envelope envelope;
        constexpr glz::opts lenient{.format = glz::BEVE, .error_on_unknown_keys = false};
        if (glz::read<lenient>(envelope, body)) {
            log_request(c.caller, nullptr, "result=failure unreadable request");
            return finish(id, Failure{"unreadable request frame"});
        }
        if (envelope.protocol != kProtocolVersion) {
            log_request(c.caller, nullptr, std::format("result=refused code=1 protocol {}", envelope.protocol));
            return finish(id, refused({Code::invalid_request,
                                       std::format("protocol: unsupported version {}, expected {}",
                                                   envelope.protocol, kProtocolVersion)}));
        }

        Request request;
        if (const auto ec = glz::read_beve(request, body)) {
            log_request(c.caller, nullptr, "result=failure unreadable request");
            return finish(id, Failure{"unreadable request: " + glz::format_error(ec)});
        }
        body = {};

        // Everything that can be decided without an image is decided here,
        // before the caller waits in the queue (plan § 6).
        std::optional<Refusal> refusal = validate_request(request, config_.bounds);
        if (!refusal)
            refusal = check_input(request.profile, request.input,
                                  effective_bounds(request.profile, config_.bounds));
        if (refusal) {
            log_request(c.caller, &request,
                        std::format("in={} result=refused code={}", request.input.size(),
                                    static_cast<int>(refusal->code)));
            return finish(id, refused(std::move(*refusal)));
        }

        const bool probe = request.operation == Operation::probe;
        if (probe && probes_running_ >= config_.probe_limit) {
            log_request(c.caller, &request, "result=busy reason=queue_full (probes)");
            return finish(id, Busy{BusyReason::queue_full, "as many probes run as the daemon allows, try again"});
        }

        const JobId job = ++last_job_;
        Work w;
        w.id = job;
        w.conn = id;
        w.caller = c.caller;
        w.in_bytes = request.input.size();
        w.received = now;
        w.reserved = std::exchange(c.reserved, 0);
        const std::optional<std::int64_t> deadline_ms = request.deadline_ms;
        const Priority priority = request.priority;
        w.request = std::move(request);

        if (probe) {
            auto [pos, _] = works_.emplace(job, std::move(w));
            c.job = job;
            c.state = Conn::State::waiting;
            launch(pos->second, 1);
            return;
        }

        std::optional<TimePoint> deadline;
        if (deadline_ms) deadline = now + std::chrono::milliseconds(*deadline_ms);
        const auto position = scheduler_.enqueue(job, priority, deadline);
        if (!position) {
            pending_bytes_ -= w.reserved;
            log_work(w, "result=busy reason=queue_full");
            return finish(id, Busy{BusyReason::queue_full,
                                   std::format("the {} queue is full, try again", name(priority))});
        }
        works_.emplace(job, std::move(w));
        c.job = job;
        c.state = Conn::State::waiting;
        send(id, Queued{*position});
    }

    // Launches the worker of a job. On failure, answers it and ends it.
    void launch(Work& w, int threads) {
        const TimePoint now = Clock::now();
        const std::int64_t waited_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - w.received).count();
        Job job{.request = std::move(w.request), .bounds = config_.bounds, .threads = threads};
        auto process = spawn_worker(job, config_.worker);
        // The worker has its own copy of the input, in its memfd: the daemon's
        // goes. The rest of the request stays, for the journal.
        w.request = std::move(job.request);
        w.request.input = {};
        if (!process) {
            w.started = now;
            deliver(w, Failure{process.error()});
            log_work(w, "result=failure " + process.error());
            return end(w.id);
        }
        w.pid = process->pid;
        w.output = std::move(process->output);
        ::fcntl(w.output.fd, F_SETFL, ::fcntl(w.output.fd, F_GETFL) | O_NONBLOCK);
        // Through syscall(): the glibc wrapper is not declared extern "C" everywhere.
        w.pidfd = Fd{static_cast<int>(::syscall(SYS_pidfd_open, w.pid, 0))};
        w.started = now;
        if (w.pidfd.fd < 0) {
            // Without a pidfd its end would go unseen: better a failure now.
            const std::string why = std::string("pidfd_open: ") + std::strerror(errno);
            ::kill(w.pid, SIGKILL);
            while (::waitpid(w.pid, nullptr, 0) < 0 && errno == EINTR) {}
            w.pid = -1;
            deliver(w, Failure{why});
            log_work(w, "result=failure " + why);
            return end(w.id);
        }
        w.kill_at = now + std::chrono::seconds(config_.timeout_s);
        watch(w.output.fd, EPOLLIN, key(Kind::output, w.id));
        watch(w.pidfd.fd, EPOLLIN, key(Kind::pid, w.id));
        if (w.probe()) ++probes_running_;
        else deliver_event(w, Started{waited_ms});
    }

    void on_output(JobId id) {
        auto it = works_.find(id);
        if (it == works_.end()) return;
        read_output(it->second);
        complete_if_done(it->second);
    }

    // Reads what the worker wrote, up to the end of its output or to what the
    // pipe holds for now.
    void read_output(Work& w) {
        if (w.eof) return;
        char buf[65536];
        for (;;) {
            const auto n = ::read(w.output.fd, buf, sizeof buf);
            if (n > 0) {
                w.reply.append(buf, static_cast<std::size_t>(n));
                if (std::cmp_greater(w.reply.size(), kMaxMessageFrame)) {
                    w.too_big = true;
                    kill(w);
                    break;
                }
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN && !w.reaped) return;
            // End of file, an error, or a reaped worker whose pipe is empty.
            // Once the worker is reaped, all it wrote is in the pipe: a
            // process it left behind holding the pipe open changes nothing.
            break;
        }
        unwatch(w.output.fd);
        w.output.reset();
        w.eof = true;
    }

    void on_exit(JobId id) {
        auto it = works_.find(id);
        if (it == works_.end()) return;
        Work& w = it->second;
        rusage usage{};
        int status = 0;
        const pid_t r = ::wait4(w.pid, &status, WNOHANG, &usage);
        if (r == 0) return;
        w.status = status;
        w.maxrss_kib = usage.ru_maxrss;
        w.reaped = true;
        unwatch(w.pidfd.fd);
        w.pidfd.reset();
        read_output(w);
        complete_if_done(w);
    }

    void kill(Work& w) {
        if (w.running() && !w.reaped && !w.killed) ::kill(w.pid, SIGKILL);
        w.killed = true;
    }

    // A worker's end: its output is read to the end and it is reaped.
    void complete_if_done(Work& w) {
        if (!w.eof || !w.reaped) return;
        if (w.cancelled) {
            log_work(w, "result=cancelled");
        } else if (w.timed_out) {
            const std::string m = std::format("worker killed after {} s", config_.timeout_s);
            deliver(w, Failure{m});
            log_work(w, "result=failure timeout");
        } else if (w.too_big) {
            deliver(w, Failure{"worker response above the frame bound"});
            log_work(w, "result=failure response too large");
        } else {
            auto outcome = worker_outcome(w.status, w.reply);
            w.reply = {};
            if (!outcome) {
                log_work(w, "result=failure " + outcome.error());
                deliver(w, Failure{outcome.error()});
            } else {
                std::size_t out_bytes = 0;
                for (const Output& o : outcome->outputs) out_bytes += o.bytes.size();
                log_work(w, outcome->error.empty()
                                ? std::format("out={} result=ok", out_bytes)
                                : std::format("result=refused code={}", static_cast<int>(outcome->code)));
                deliver(w, std::move(*outcome));
            }
        }
        end(w.id);
    }

    // Forgets a job, and frees its place: the running slot, or a probe slot.
    void end(JobId id) {
        auto it = works_.find(id);
        if (it == works_.end()) return;
        Work& w = it->second;
        if (w.probe()) {
            if (w.running()) --probes_running_;
        } else {
            scheduler_.finish(id);
        }
        pending_bytes_ -= w.reserved;
        if (auto c = conns_.find(w.conn); c != conns_.end()) c->second.job.reset();
        works_.erase(it);
    }

    // Timers: workers past their time, queued jobs past their deadline, and
    // the next encode to launch.
    void tick(TimePoint now) {
        for (auto& [id, w] : works_) {
            if (w.running() && !w.killed && now >= w.kill_at) {
                w.timed_out = true;
                kill(w);
            }
        }
        for (const JobId id : scheduler_.expire(now)) {
            auto it = works_.find(id);
            if (it == works_.end()) continue;
            log_work(it->second, "result=busy reason=deadline");
            deliver(it->second, Busy{BusyReason::deadline, "still queued when deadline_ms ran out"});
            end(id);
        }
        if (stopping_) return;
        while (const auto id = scheduler_.start()) {
            auto it = works_.find(*id);
            if (it == works_.end()) {
                scheduler_.finish(*id);
                continue;
            }
            launch(it->second, threads_);
        }
    }

    int wake_timeout(TimePoint now) const {
        std::optional<TimePoint> next = scheduler_.next_deadline();
        for (const auto& [id, w] : works_)
            if (w.running() && !w.killed && (!next || w.kill_at < *next)) next = w.kill_at;
        if (!next) return -1;
        if (*next <= now) return 0;
        const auto wait = std::chrono::ceil<std::chrono::milliseconds>(*next - now).count();
        return static_cast<int>(std::min<std::int64_t>(wait, 60'000));
    }

    // ------------------------------------------------------------------
    // Answers

    // An event, the connection stays open.
    void send(ConnId id, const Message& m) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        auto frame = make_frame(m);
        if (!frame) return finish(id, Failure{frame.error()});
        it->second.out += *frame;
        flush(id);
    }

    // The final message: the connection closes once it is sent.
    void finish(ConnId id, const Message& m) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        Conn& c = it->second;
        auto frame = make_frame(m);
        if (!frame) frame = make_frame(Message{Failure{frame.error()}});
        c.out += *frame;
        c.state = Conn::State::closing;
        pending_bytes_ -= std::exchange(c.reserved, 0);
        c.body = {};
        flush(id);
    }

    void deliver_event(Work& w, const Message& m) {
        if (w.conn) send(w.conn, m);
    }

    // The job's final message. The connection forgets the job first: whatever
    // happens to the connection from here on no longer concerns the job.
    void deliver(Work& w, const Message& m) {
        if (!w.conn) return;
        const ConnId id = std::exchange(w.conn, 0);
        if (auto it = conns_.find(id); it != conns_.end()) it->second.job.reset();
        finish(id, m);
    }

    // Sends what is pending. False when the connection is gone.
    bool flush(ConnId id) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return false;
        Conn& c = it->second;
        while (c.out_sent < c.out.size()) {
            const auto n = ::send(c.fd.fd, c.out.data() + c.out_sent, c.out.size() - c.out_sent,
                                  MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) {
                set_interest(id, c);
                return true;
            }
            if (n < 0) {
                drop(id, "hang-up");
                return false;
            }
            c.out_sent += static_cast<std::size_t>(n);
        }
        c.out = {};
        c.out_sent = 0;
        if (c.state == Conn::State::closing) {
            close_conn(it);
            return false;
        }
        set_interest(id, c);
        return true;
    }

    void close_conn(std::map<ConnId, Conn>::iterator it) {
        Conn& c = it->second;
        pending_bytes_ -= c.reserved;
        unwatch(c.fd.fd);
        conns_.erase(it);
    }

    // The caller went away: its job leaves the queue, or its worker is killed.
    void drop(ConnId id, std::string_view why) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        if (const auto job = it->second.job) {
            if (auto w = works_.find(*job); w != works_.end()) {
                Work& work = w->second;
                work.conn = 0;
                if (work.running()) {
                    work.cancelled = true;
                    kill(work);
                } else {
                    scheduler_.cancel(*job);
                    log_work(work, std::format("result=cancelled ({})", why));
                    end(*job);
                }
            }
        }
        close_conn(conns_.find(id));
    }

    // ------------------------------------------------------------------
    // Signals

    void stop() {
        if (stopping_) return;
        stopping_ = true;
        log_line("stopping: queued jobs are answered busy, running ones finish");
        unwatch(listener_.fd);
        listener_.reset();
        if (!unlink_path_.empty()) ::unlink(std::exchange(unlink_path_, {}).c_str());
        for (const JobId id : scheduler_.drain()) {
            auto it = works_.find(id);
            if (it == works_.end()) continue;
            log_work(it->second, "result=busy reason=stopping");
            deliver(it->second, Busy{BusyReason::stopping, "the daemon is stopping, try again"});
            end(id);
        }
        std::vector<ConnId> reading;
        for (const auto& [id, c] : conns_)
            if (c.state == Conn::State::header || c.state == Conn::State::body) reading.push_back(id);
        for (const ConnId id : reading) finish(id, Busy{BusyReason::stopping, "the daemon is stopping, try again"});
    }

    // Reload (SIGHUP): the new budget, queue lengths, timeout and bounds apply
    // to what starts from now on. Running workers keep theirs.
    void reload() {
        std::ifstream f(config_path_, std::ios::binary);
        if (!f) return log_line("reload: cannot read " + config_path_ + ", configuration kept");
        const std::string json(std::istreambuf_iterator<char>(f), {});
        auto next = parse_config(json);
        if (!next) return log_line("reload: " + next.error().message + ", configuration kept");
        if (next->socket != config_.socket) log_line("reload: the socket moves at the next restart only");
        next->socket = config_.socket;
        config_ = std::move(*next);
        scheduler_.set_limits(config_.queue);
        threads_ = resolve_threads(config_.threads, host_.env_threads, host_.affinity, host_.hardware);
        log_line(std::format("reloaded: {} threads, queues {} interactive and {} background, {} probes, "
                             "timeout {} s",
                             threads_, config_.queue.interactive, config_.queue.background,
                             config_.probe_limit, config_.timeout_s));
    }

    Config config_;
    std::string config_path_;
    Host host_;
    int threads_ = 1;
    Scheduler scheduler_;
    Fd listener_;
    std::string unlink_path_;
    Fd epoll_;
    Fd signal_;
    bool stopping_ = false;

    std::map<ConnId, Conn> conns_;
    std::map<JobId, Work> works_;
    ConnId last_conn_ = 0;
    JobId last_job_ = 0;
    int probes_running_ = 0;
    std::int64_t pending_bytes_ = 0;
};

}  // namespace tirage::daemon
