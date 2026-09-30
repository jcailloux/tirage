#pragma once

// The daemon's queue (plan § 6, "Ordonnancement"): one encode at a time, two
// queues, interactive first, first come first served in each, bounded lengths,
// deadlines, cancellation.
//
// Pure logic: no clock of its own (every call takes `now`), no I/O. The daemon
// launches the workers, the tests drive it on a simulated clock.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

#include "tirage/protocol.h"

namespace tirage::daemon {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using JobId = std::uint64_t;

// How many encodes may wait, per priority. The running one is not counted.
struct QueueLimits {
    int interactive = 32;
    int background = 256;
};

class Scheduler {
public:
    explicit Scheduler(QueueLimits limits = {}) : limits_(limits) {}

    // New lengths apply to the jobs that arrive from now on. Jobs already
    // queued stay, even above a lowered limit.
    void set_limits(QueueLimits limits) { limits_ = limits; }

    // Queues a job. Returns its position (Queued::position: the running encode
    // and every queued job that starts before it), or nothing when the queue
    // of its priority is full.
    [[nodiscard]] std::optional<int> enqueue(JobId id, Priority priority, std::optional<TimePoint> deadline) {
        auto& queue = of(priority);
        if (std::cmp_greater_equal(queue.size(), limit(priority))) return std::nullopt;
        queue.push_back({id, deadline});
        std::size_t ahead = queue.size() - 1 + (running_ ? 1 : 0);
        if (priority == Priority::background) ahead += interactive_.size();
        return static_cast<int>(ahead);
    }

    // Takes a queued job out (its caller went away). False if it was not
    // queued: already running, finished, or unknown.
    bool cancel(JobId id) {
        for (auto* queue : {&interactive_, &background_}) {
            const auto it = std::ranges::find(*queue, id, &Entry::id);
            if (it != queue->end()) {
                queue->erase(it);
                return true;
            }
        }
        return false;
    }

    // Takes out and returns the queued jobs whose deadline is past at `now`.
    [[nodiscard]] std::vector<JobId> expire(TimePoint now) {
        std::vector<JobId> out;
        for (auto* queue : {&interactive_, &background_}) {
            std::erase_if(*queue, [&](const Entry& e) {
                if (!e.deadline || *e.deadline > now) return false;
                out.push_back(e.id);
                return true;
            });
        }
        return out;
    }

    // The job to launch now: the first interactive one, else the first
    // background one, and only when nothing runs. It becomes the running job.
    [[nodiscard]] std::optional<JobId> start() {
        if (running_) return std::nullopt;
        for (auto* queue : {&interactive_, &background_}) {
            if (queue->empty()) continue;
            running_ = queue->front().id;
            queue->pop_front();
            return running_;
        }
        return std::nullopt;
    }

    // The running job is over (done, failed, killed or cancelled).
    void finish(JobId id) {
        if (running_ == id) running_.reset();
    }

    // The earliest deadline among the queued jobs, for the daemon's wake-up.
    [[nodiscard]] std::optional<TimePoint> next_deadline() const {
        std::optional<TimePoint> next;
        for (const auto* queue : {&interactive_, &background_})
            for (const Entry& e : *queue)
                if (e.deadline && (!next || *e.deadline < *next)) next = e.deadline;
        return next;
    }

    // Everything queued, in start order. Stopping the daemon answers them all.
    [[nodiscard]] std::vector<JobId> drain() {
        std::vector<JobId> out;
        for (auto* queue : {&interactive_, &background_}) {
            for (const Entry& e : *queue) out.push_back(e.id);
            queue->clear();
        }
        return out;
    }

    [[nodiscard]] std::optional<JobId> running() const { return running_; }
    [[nodiscard]] std::size_t queued(Priority p) const {
        return p == Priority::interactive ? interactive_.size() : background_.size();
    }

private:
    struct Entry {
        JobId id;
        std::optional<TimePoint> deadline;
    };

    std::deque<Entry>& of(Priority p) { return p == Priority::interactive ? interactive_ : background_; }
    int limit(Priority p) const {
        return p == Priority::interactive ? limits_.interactive : limits_.background;
    }

    QueueLimits limits_;
    std::deque<Entry> interactive_;
    std::deque<Entry> background_;
    std::optional<JobId> running_;
};

}  // namespace tirage::daemon
