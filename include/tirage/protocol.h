#pragma once

// What travels between a caller, the daemon and a worker (plan § 4).
//
// Plain data, like profile.h: the request a caller sends, the response it gets
// back, and the job a worker reads (the request plus what the daemon adds).
// Checking a request is request.h. On the wire it is BEVE (glaze), which carries
// the input and output bytes as they are.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>

#include "tirage/profile.h"
#include "tirage/validate.h"

namespace tirage {

// Current protocol version. A request with another version is refused.
inline constexpr int kProtocolVersion = 1;

// status asks the daemon what it is doing: it is not a job (plan § 6).
enum class Operation { encode, probe, status };
enum class Priority { interactive, background };
enum class CropUnit { px, permille };

// Crop, on the oriented image (EXIF orientation applied first). The unit has no
// default on purpose: a default would turn a unit mistake into a tiny crop.
struct Crop {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::optional<CropUnit> unit;  // required, checked by validate_request
};

// How a mask hides its zone. Every style reads only the zone's own pixels, and
// is scaled to the zone's size, so that its strength never depends on the
// original's resolution:
// - blur: a Gaussian blur, strong enough to leave shapes and no detail;
// - pixelate: blocks of one colour, 8 along the zone's longer side;
// - fill: one colour, the zone's mean.
enum class MaskStyle { blur, pixelate, fill };

// A zone to hide (a user name, an avatar), on the oriented image like the
// crop, and before it: a zone reaching outside the crop is only partly seen.
// Its unit is required for the same reason as the crop's.
struct Mask {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::optional<CropUnit> unit;  // required, checked by validate_request
    MaskStyle style = MaskStyle::blur;
};

// Bound on the number of masks of one request.
inline constexpr std::size_t kMaxMasks = 32;

struct Request {
    int protocol = 0;
    Operation operation = Operation::encode;
    Priority priority = Priority::interactive;
    Profile profile;
    std::vector<std::string> variants;  // encode: the variants to produce, in this order
    std::optional<Crop> crop;
    std::vector<Mask> masks;  // encode only, applied in this order
    // Encode only: milliseconds from the moment the daemon has read the request.
    // A job still queued past it is dropped (Busy, deadline). Relative, so that
    // the caller's and the daemon's clocks never have to agree. Once started, a
    // job runs to its end or to the daemon's timeout.
    std::optional<std::int64_t> deadline_ms;
    std::string input;  // the original's bytes
};

// Stable numbers: callers map them to their own messages. Never renumber, only
// append. A failure (worker killed, unreadable frame) is not a code: it is a
// transport error.
enum class Code : int {
    ok = 0,
    invalid_request = 1,
    invalid_profile = 2,
    unsupported_format = 3,  // not recognised, or not in input.formats
    file_too_large = 4,      // above max_bytes
    image_too_large = 5,     // above max_edge or max_pixels
    image_too_small = 6,     // below min_size with the reject policy
    decode_failed = 7,       // recognised, but libvips could not read it
};

// The original, after orientation and before any crop.
struct Source {
    int width = 0;
    int height = 0;
    int bands = 0;
    InputFormat format = InputFormat::png;
};

// A rectangle in pixels of the oriented original.
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct Output {
    std::string variant;
    int width = 0;
    int height = 0;
    OutputFormat format = OutputFormat::avif;
    std::string bytes;  // the encoded file
};

// An empty error means success. A non-empty error with its code is a refusal:
// the request was understood and turned down.
struct Response {
    std::string error;
    Code code = Code::ok;
    std::vector<std::string> warnings;
    std::optional<Source> source;  // absent when refused before decoding
    std::optional<Rect> crop;      // the crop actually applied, once clamped
    std::vector<Rect> masks;       // each mask as applied, in pixels, empty when outside the image
    std::vector<Output> outputs;   // encode only: variant order, then width, then format
};

// What a worker reads on its standard input: the request, the daemon's bounds
// (the worker reads no configuration of its own) and its thread count.
struct Job {
    Request request;
    Bounds bounds = kDefaultBounds;
    int threads = 1;
};

// ---------------------------------------------------------------------------
// What the daemon sends back on its socket (plan § 4): zero or more events,
// then exactly one final message (Response, Busy or Failure), then it closes.
// A status request gets a single Status instead.

// Encode only, sent once, as soon as the job is queued. `position` counts every
// encode that starts before this one at that moment: the one running, and the
// queued ones ahead of it, interactive ones included for a background job. It is
// not updated: interactive jobs arriving later still pass a background one.
// 0 means the job starts right away.
struct Queued {
    int position = 0;
};

// Encode only, sent once, when the worker is launched.
struct Started {
    std::int64_t waited_ms = 0;  // time spent in the queue
};

enum class BusyReason {
    queue_full,  // the queue of this priority, the probe limit or the daemon's memory is full
    deadline,    // still queued when deadline_ms ran out
    stopping,    // the daemon is shutting down
};

// Neither a refusal (nothing is wrong with the request) nor a failure (nothing
// broke): the daemon did not run the job, and the same request can be sent
// again later, unchanged.
struct Busy {
    BusyReason reason = BusyReason::queue_full;
    std::string message;
};

// Something broke: worker killed or timed out, unreadable frame, worker not
// launched. The caller logs it, it is never the image's fault.
struct Failure {
    std::string message;
};

// How many encodes may wait, per priority. The running one is not counted.
struct QueueLimits {
    int interactive = 32;
    int background = 256;
};

// One job, as `status` shows it.
struct StatusJob {
    std::string caller;  // the user name behind the connection
    Operation operation = Operation::encode;
    Priority priority = Priority::interactive;
    std::int64_t in_bytes = 0;
    std::int64_t waited_ms = 0;               // in the queue, until now or until it started
    std::optional<std::int64_t> running_ms;  // since it started, running jobs only
};

// The answer to a status request, its only message. Nothing in it is secret
// from the callers: they share the machine and its budget.
struct Status {
    std::string version;       // the daemon's
    int threads = 0;           // the threads of one encode, the budget
    int busy_threads = 0;      // taken now: the running encode's, and one per probe
    QueueLimits queue;
    int probe_limit = 0;
    int timeout_s = 0;
    std::int64_t pending_bytes = 0;      // request bytes the daemon holds
    std::int64_t max_pending_bytes = 0;
    std::vector<StatusJob> running;      // the encode, then the probes
    std::vector<StatusJob> queued;       // in start order
};

// New alternatives go at the end: BEVE numbers them in this order.
using Message = std::variant<Queued, Started, Response, Busy, Failure, Status>;

}  // namespace tirage

template <>
struct glz::meta<tirage::BusyReason> {
    using enum tirage::BusyReason;
    static constexpr auto value = glz::enumerate(queue_full, deadline, stopping);
};

template <>
struct glz::meta<tirage::Operation> {
    using enum tirage::Operation;
    static constexpr auto value = glz::enumerate(encode, probe, status);
};

template <>
struct glz::meta<tirage::Priority> {
    using enum tirage::Priority;
    static constexpr auto value = glz::enumerate(interactive, background);
};

template <>
struct glz::meta<tirage::CropUnit> {
    using enum tirage::CropUnit;
    static constexpr auto value = glz::enumerate(px, permille);
};

template <>
struct glz::meta<tirage::MaskStyle> {
    using enum tirage::MaskStyle;
    static constexpr auto value = glz::enumerate(blur, pixelate, fill);
};

// tirage::Code has no meta on purpose: glaze writes it as its number.
