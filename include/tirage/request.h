#pragma once

// Request checks and the decisions a worker takes before touching a pixel
// (plan § 3 and § 4): format sniffing, safety bounds, crop and floor.
//
// Pure logic, no libvips, like validate.h: the daemon runs validate_request
// before queueing, the worker runs everything again (direct mode goes around
// the daemon).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>

#include "tirage/profile.h"
#include "tirage/protocol.h"
#include "tirage/validate.h"

namespace tirage {

struct Refusal {
    Code code = Code::invalid_request;
    std::string message;
};

// An empty error means success: a refusal always says something, even when
// libvips did not (it sometimes fails with an empty error buffer).
[[nodiscard]] inline Response refused(Refusal r) {
    Response out;
    out.error = r.message.empty() ? "refused without detail" : std::move(r.message);
    out.code = r.code;
    return out;
}

// Format by magic bytes, never by extension or declared type. HEIC is an ISO
// BMFF file whose brands name HEVC; an AVIF file (AV1 brands) is not HEIC.
[[nodiscard]] inline std::optional<InputFormat> sniff(std::string_view bytes) {
    const auto at = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
    const std::size_t n = bytes.size();

    static constexpr std::string_view kPng = "\x89PNG\r\n\x1a\n";
    if (bytes.starts_with(kPng)) return InputFormat::png;
    if (n >= 3 && at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF) return InputFormat::jpeg;
    if (n >= 12 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP")
        return InputFormat::webp;

    // ftyp box: size (4), "ftyp", major brand (4), minor version (4), compatible brands (4 each).
    if (n >= 16 && bytes.substr(4, 4) == "ftyp") {
        const std::size_t box = (std::size_t{at(0)} << 24) | (std::size_t{at(1)} << 16) |
                                (std::size_t{at(2)} << 8) | std::size_t{at(3)};
        const std::size_t end = std::min(box, n);
        const auto hevc = [](std::string_view brand) {
            return brand == "heic" || brand == "heix" || brand == "heim" || brand == "heis" ||
                   brand == "hevc" || brand == "hevx";
        };
        if (hevc(bytes.substr(8, 4))) return InputFormat::heic;
        for (std::size_t i = 16; i + 4 <= end; i += 4)
            if (hevc(bytes.substr(i, 4))) return InputFormat::heic;
    }
    return std::nullopt;
}

// Checks a request before it is queued: protocol, profile, variants, crop.
[[nodiscard]] inline std::optional<Refusal> validate_request(const Request& r,
                                                             const Bounds& daemon = kDefaultBounds) {
    const auto invalid = [](std::string message) {
        return Refusal{Code::invalid_request, std::move(message)};
    };

    if (r.protocol != kProtocolVersion)
        return invalid(std::format("protocol: unsupported version {}, expected {}", r.protocol,
                                   kProtocolVersion));

    if (auto e = validate(r.profile, daemon))
        return Refusal{Code::invalid_profile, std::format("profile.{}: {}", e->path, e->message)};

    if (r.operation == Operation::encode) {
        if (r.variants.empty()) return invalid("variants: name at least one variant to produce");
        std::set<std::string> seen;
        for (const std::string& v : r.variants) {
            if (!r.profile.variants.contains(v))
                return invalid(std::format("variants: \"{}\" is not a variant of the profile", v));
            if (!seen.insert(v).second)
                return invalid(std::format("variants: \"{}\" is listed twice", v));
        }
    } else if (!r.variants.empty()) {
        return invalid("variants: probe encodes nothing, they would be ignored");
    }

    if (const auto& c = r.crop) {
        if (!c->unit) return invalid("crop.unit: required (\"px\" or \"permille\")");
        if (c->x < 0 || c->y < 0) return invalid("crop: x and y must not be negative");
        if (c->width <= 0 || c->height <= 0) return invalid("crop: width and height must be positive");
        if (*c->unit == CropUnit::permille &&
            (c->x > 1000 || c->y > 1000 || c->width > 1000 || c->height > 1000))
            return invalid("crop: permille values are at most 1000");
    }

    if (r.deadline_ms) {
        if (r.operation == Operation::probe)
            return invalid("deadline_ms: probe is never queued, it would be ignored");
        if (*r.deadline_ms <= 0) return invalid("deadline_ms: must be positive");
    }

    if (r.input.empty()) return invalid("input: empty");
    return std::nullopt;
}

// The format is recognised and accepted by the profile, and the file is within
// the byte bound. `bounds` are the effective ones (effective_bounds).
[[nodiscard]] inline std::optional<Refusal> check_input(const Profile& p, std::string_view bytes,
                                                        const Bounds& bounds) {
    if (static_cast<std::int64_t>(bytes.size()) > bounds.max_bytes)
        return Refusal{Code::file_too_large,
                       std::format("{} bytes, the bound is {}", bytes.size(), bounds.max_bytes)};
    const auto format = sniff(bytes);
    if (!format) return Refusal{Code::unsupported_format, "format not recognised"};
    if (std::ranges::find(p.input.formats, *format) == p.input.formats.end())
        return Refusal{Code::unsupported_format,
                       std::format("{} is not in input.formats", glz::write_json(*format).value_or("?"))};
    return std::nullopt;
}

// Decompression bomb guard: runs on the header's dimensions, before a single
// pixel is decoded.
[[nodiscard]] inline std::optional<Refusal> check_dimensions(int width, int height, const Bounds& b) {
    if (width > b.max_edge || height > b.max_edge)
        return Refusal{Code::image_too_large,
                       std::format("{}x{}, the edge bound is {}", width, height, b.max_edge)};
    if (static_cast<std::int64_t>(width) * height > b.max_pixels)
        return Refusal{Code::image_too_large,
                       std::format("{}x{}, the pixel bound is {}", width, height, b.max_pixels)};
    return std::nullopt;
}

// The crop in pixels, clamped into the image: it always keeps at least one
// pixel, like codiga's worker. Permille coordinates are rounded to the nearest
// pixel.
[[nodiscard]] inline Rect resolve_crop(const Crop& c, int width, int height) {
    int x = c.x, y = c.y, w = c.width, h = c.height;
    if (c.unit == CropUnit::permille) {
        const auto scale = [](int v, int size) {
            return static_cast<int>((static_cast<std::int64_t>(v) * size + 500) / 1000);
        };
        x = scale(c.x, width);
        y = scale(c.y, height);
        w = scale(c.x + c.width, width) - x;
        h = scale(c.y + c.height, height) - y;
    }
    Rect r;
    r.x = std::clamp(x, 0, width - 1);
    r.y = std::clamp(y, 0, height - 1);
    r.width = std::clamp(w, 1, width - r.x);
    r.height = std::clamp(h, 1, height - r.y);
    return r;
}

// The floor, applied to what is produced: the crop when there is one. Returns
// a refusal (reject policy), a warning (warn policy), or nothing.
using FloorVerdict = std::variant<std::monostate, Refusal, std::string>;

[[nodiscard]] inline FloorVerdict check_floor(const std::optional<MinSize>& floor, int width,
                                              int height) {
    if (!floor || (width >= floor->width && height >= floor->height)) return std::monostate{};
    std::string message = std::format("below min_size: {}x{} under {}x{}", width, height,
                                      floor->width, floor->height);
    if (floor->policy == FloorPolicy::warn) return message;
    return Refusal{Code::image_too_small, std::move(message)};
}

}  // namespace tirage
