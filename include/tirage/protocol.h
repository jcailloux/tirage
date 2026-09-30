#pragma once

// What travels between a caller, the daemon and a worker (plan § 4).
//
// Plain data, like profile.h: the request a caller sends, the response it gets
// back, and the job a worker reads (the request plus what the daemon adds).
// Checking a request is request.h. On the wire it is BEVE (glaze), which carries
// the input and output bytes as they are.

#include <optional>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

#include "tirage/profile.h"
#include "tirage/validate.h"

namespace tirage {

// Current protocol version. A request with another version is refused.
inline constexpr int kProtocolVersion = 1;

enum class Operation { encode, probe };
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

struct Request {
    int protocol = 0;
    Operation operation = Operation::encode;
    Priority priority = Priority::interactive;
    Profile profile;
    std::vector<std::string> variants;  // encode: the variants to produce, in this order
    std::optional<Crop> crop;
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
    std::vector<Output> outputs;   // encode only: variant order, then width, then format
};

// What a worker reads on its standard input: the request, the daemon's bounds
// (the worker reads no configuration of its own) and its thread count.
struct Job {
    Request request;
    Bounds bounds = kDefaultBounds;
    int threads = 1;
};

}  // namespace tirage

template <>
struct glz::meta<tirage::Operation> {
    using enum tirage::Operation;
    static constexpr auto value = glz::enumerate(encode, probe);
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

// tirage::Code has no meta on purpose: glaze writes it as its number.
