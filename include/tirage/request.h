#pragma once

// Request checks and the decisions a worker takes before touching a pixel
// (docs/requests.md): format sniffing, safety bounds, crop, masks and floor.
//
// Pure logic, no libvips, like validate.h: the daemon runs validate_request
// before queueing, the worker runs everything again (direct mode goes around
// the daemon).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

// Format by magic bytes, never by extension or declared type. HEIC and AVIF are
// both ISO BMFF files: their brands tell them apart, HEVC for HEIC, AV1 for AVIF.
[[nodiscard]] inline std::optional<InputFormat> sniff(std::string_view bytes) {
    const auto at = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
    const std::size_t n = bytes.size();

    static constexpr std::string_view kPng = "\x89PNG\r\n\x1a\n";
    if (bytes.starts_with(kPng)) return InputFormat::png;
    if (n >= 3 && at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF) return InputFormat::jpeg;
    if (n >= 12 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP")
        return InputFormat::webp;

    // ftyp box: size (4), "ftyp", major brand (4), minor version (4), compatible brands (4 each).
    // The major brand decides, else the first compatible brand that names a codec.
    if (n >= 16 && bytes.substr(4, 4) == "ftyp") {
        const std::size_t box = (std::size_t{at(0)} << 24) | (std::size_t{at(1)} << 16) |
                                (std::size_t{at(2)} << 8) | std::size_t{at(3)};
        const std::size_t end = std::min(box, n);
        const auto codec = [](std::string_view brand) -> std::optional<InputFormat> {
            if (brand == "heic" || brand == "heix" || brand == "heim" || brand == "heis" ||
                brand == "hevc" || brand == "hevx")
                return InputFormat::heic;
            if (brand == "avif" || brand == "avis") return InputFormat::avif;
            return std::nullopt;
        };
        if (auto f = codec(bytes.substr(8, 4))) return f;
        for (std::size_t i = 16; i + 4 <= end; i += 4)
            if (auto f = codec(bytes.substr(i, 4))) return f;
    }
    return std::nullopt;
}

// Checks a request before it is queued: protocol, profile, variants, crop,
// masks.
[[nodiscard]] inline std::optional<Refusal> validate_request(const Request& r,
                                                             const Bounds& daemon = kDefaultBounds) {
    const auto invalid = [](std::string message) {
        return Refusal{Code::invalid_request, std::move(message)};
    };

    if (r.protocol != kProtocolVersion)
        return invalid(std::format("protocol: unsupported version {}, expected {}", r.protocol,
                                   kProtocolVersion));

    if (r.operation == Operation::status)
        return invalid("operation: status asks the daemon about itself, it is not a job");

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

    // The crop and every mask are rectangles with the same rules, but for one:
    // a mask may start before the image as it may end past it (only its part
    // inside the image is drawn), so that a soft edge keeps its fade around a
    // zone that touches the image's left or top border.
    const auto check_area = [&](const std::string& path, int x, int y, int width, int height,
                                const std::optional<CropUnit>& unit,
                                bool may_start_outside) -> std::optional<Refusal> {
        if (!unit) return invalid(path + ".unit: required (\"px\" or \"permille\")");
        if (!may_start_outside && (x < 0 || y < 0)) return invalid(path + ": x and y must not be negative");
        if (width <= 0 || height <= 0) return invalid(path + ": width and height must be positive");
        if (*unit == CropUnit::permille &&
            (std::abs(x) > 1000 || std::abs(y) > 1000 || width > 1000 || height > 1000))
            return invalid(path + ": permille values are at most 1000");
        return std::nullopt;
    };
    if (const auto& c = r.crop)
        if (auto e = check_area("crop", c->x, c->y, c->width, c->height, c->unit, false)) return e;

    if (!r.masks.empty()) {
        if (r.operation == Operation::probe)
            return invalid("masks: probe encodes nothing, they would be ignored");
        if (r.masks.size() > kMaxMasks)
            return invalid(std::format("masks: {} masks, at most {}", r.masks.size(), kMaxMasks));
        for (std::size_t i = 0; i < r.masks.size(); ++i) {
            const Mask& m = r.masks[i];
            if (auto e = check_area(std::format("masks[{}]", i), m.x, m.y, m.width, m.height, m.unit, true))
                return e;
        }
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

namespace detail {

// A rectangle in pixels, not yet clamped. Permille coordinates are rounded to
// the nearest pixel, edges rather than sizes: adjacent areas share their border.
[[nodiscard]] inline Rect to_pixels(int x, int y, int w, int h, std::optional<CropUnit> unit, int width,
                                    int height) {
    if (unit != CropUnit::permille) return {x, y, w, h};
    const auto scale = [](int v, int size) {
        return static_cast<int>((static_cast<std::int64_t>(v) * size + 500) / 1000);
    };
    const int px = scale(x, width), py = scale(y, height);
    return {px, py, scale(x + w, width) - px, scale(y + h, height) - py};
}

}  // namespace detail

// The crop in pixels, clamped into the image: it always keeps at least one
// pixel.
[[nodiscard]] inline Rect resolve_crop(const Crop& c, int width, int height) {
    const auto [x, y, w, h] = detail::to_pixels(c.x, c.y, c.width, c.height, c.unit, width, height);
    Rect r;
    r.x = std::clamp(x, 0, width - 1);
    r.y = std::clamp(y, 0, height - 1);
    r.width = std::clamp(w, 1, width - r.x);
    r.height = std::clamp(h, 1, height - r.y);
    return r;
}

// A mask in pixels, cut to the image. Unlike a crop it may vanish: a mask
// entirely outside the image, or thinner than a pixel once in permille, hides
// nothing and comes back empty (all zero).
[[nodiscard]] inline Rect resolve_mask(const Mask& m, int width, int height) {
    const Rect p = detail::to_pixels(m.x, m.y, m.width, m.height, m.unit, width, height);
    const std::int64_t x0 = std::max(p.x, 0), y0 = std::max(p.y, 0);
    const std::int64_t x1 = std::min<std::int64_t>(std::int64_t{p.x} + p.width, width);
    const std::int64_t y1 = std::min<std::int64_t>(std::int64_t{p.y} + p.height, height);
    if (x1 <= x0 || y1 <= y0) return {};
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
}

// The soft edge of a mask (MaskEdge::soft), on its whole rectangle in pixels,
// before any cut to the image: a rounded rectangle, faded out towards its
// border. Constants of the contract, like the styles': a caller may preview
// them (soft_coverage).
inline constexpr int kSoftRadiusDivisor = 4;  // corner radius: shorter side / 4
inline constexpr int kSoftFadeDivisor = 10;   // fade: shorter side / 10, 1 pixel at least

// Opacity of a soft mask, from 0 (the image as it was) to 1 (fully hidden), at
// pixel (x, y) of its width x height rectangle, measured at the pixel's centre.
// The fade follows a smoothstep: no visible step where it starts or ends.
[[nodiscard]] inline double soft_coverage(int x, int y, int width, int height) {
    const double shorter = std::min(width, height);
    const double radius = shorter / kSoftRadiusDivisor;
    const double fade = std::max(1.0, shorter / kSoftFadeDivisor);
    // Signed distance to the rounded rectangle, negative inside.
    const double dx = std::abs(x + 0.5 - width / 2.0) - (width / 2.0 - radius);
    const double dy = std::abs(y + 0.5 - height / 2.0) - (height / 2.0 - radius);
    const double outside = std::hypot(std::max(dx, 0.0), std::max(dy, 0.0));
    const double inside = std::min(std::max(dx, dy), 0.0);
    const double depth = radius - (outside + inside);  // how far inside the border
    const double t = std::clamp(depth / fade, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// A rectangle of the oriented image, in the image as stored, before
// orientation: masks are drawn there, on the single copy in memory, and autorot
// then carries them with the pixels. `width` and `height` are the stored
// image's. The EXIF orientations, as libvips autorot applies them: 2 flips
// horizontally, 3 turns 180°, 4 flips vertically, 5 transposes, 6 turns 90°
// clockwise, 7 transverses, 8 turns 90° anticlockwise.
[[nodiscard]] inline Rect to_stored(const Rect& r, int orientation, int width, int height) {
    // Oriented pixel (a, b) to stored pixel.
    const auto point = [&](int a, int b) -> std::pair<int, int> {
        switch (orientation) {
            case 2: return {width - 1 - a, b};
            case 3: return {width - 1 - a, height - 1 - b};
            case 4: return {a, height - 1 - b};
            case 5: return {b, a};
            case 6: return {b, height - 1 - a};
            case 7: return {width - 1 - b, height - 1 - a};
            case 8: return {width - 1 - b, a};
            default: return {a, b};
        }
    };
    const auto [x0, y0] = point(r.x, r.y);
    const auto [x1, y1] = point(r.x + r.width - 1, r.y + r.height - 1);
    return {std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0) + 1, std::abs(y1 - y0) + 1};
}

// The side of a pixelate block: 8 blocks along the zone's longer side. A
// constant of the contract, for a caller's preview.
[[nodiscard]] inline int pixelate_block(int width, int height) {
    return std::max(1, (std::max(width, height) + 7) / 8);
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
