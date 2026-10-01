#pragma once

// Profile validation (docs/profile.md).
//
// Pure logic, no libvips: the daemon runs it before queueing, the worker again
// (direct mode goes around the daemon), and the client may run it before
// sending. The rule that drives most checks: a setting that would be silently
// ignored is refused, because a refusal is better than a surprise.

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tirage/profile.h"

namespace tirage {

// The daemon's safety bounds, from its config. They protect the machine.
struct Bounds {
    std::int64_t max_bytes = 0;
    int max_edge = 0;
    std::int64_t max_pixels = 0;
};

// Compiled defaults: what the daemon uses without a config entry, and what the
// client applies in direct mode.
inline constexpr Bounds kDefaultBounds{
    .max_bytes = 64 * 1024 * 1024,
    .max_edge = 16384,
    .max_pixels = 100'000'000,
};

struct ProfileError {
    std::string path;  // where in the profile, e.g. "variants.tiers.quality[1]"
    std::string message;
};

inline constexpr int kAvifEffortMax = 9;
inline constexpr int kWebpEffortMax = 6;

// Bounds a request is held to: the daemon's, tightened by input.limits. Only
// meaningful for a validated profile.
[[nodiscard]] inline Bounds effective_bounds(const Profile& profile, const Bounds& daemon) {
    Bounds b = daemon;
    if (const auto& l = profile.input.limits) {
        if (l->max_bytes) b.max_bytes = *l->max_bytes;
        if (l->max_edge) b.max_edge = *l->max_edge;
        if (l->max_pixels) b.max_pixels = *l->max_pixels;
    }
    return b;
}

namespace detail {

inline std::string_view format_name(OutputFormat f) {
    switch (f) {
        case OutputFormat::avif: return "avif";
        case OutputFormat::webp: return "webp";
        case OutputFormat::jpeg: return "jpeg";
    }
    return "?";
}

inline const std::optional<int>& quality_of(const QualityStep& q, OutputFormat f) {
    switch (f) {
        case OutputFormat::avif: return q.avif;
        case OutputFormat::webp: return q.webp;
        case OutputFormat::jpeg: return q.jpeg;
    }
    std::unreachable();
}

// Variant names end up in the caller's file names and in logs: keep them tame.
inline bool valid_variant_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

template <class T>
bool has_duplicates(const std::vector<T>& v) {
    return std::set<T>(v.begin(), v.end()).size() != v.size();
}

using Result = std::optional<ProfileError>;

inline Result fail(std::string path, std::string message) {
    return ProfileError{std::move(path), std::move(message)};
}

inline Result check_effort(const std::optional<int>& effort, int max, const std::string& path) {
    if (effort && (*effort < 0 || *effort > max))
        return fail(path + ".effort", std::format("must be between 0 and {}", max));
    return std::nullopt;
}

// Checks one encoding block against the formats it may apply to. `produced`
// says, per format, whether a variant concerned by this block outputs it.
inline Result check_encoding(const Encoding& e, const std::set<OutputFormat>& produced,
                             const std::string& path) {
    const auto unused = [&](OutputFormat f) {
        return fail(std::format("{}.{}", path, format_name(f)),
                    std::format("no variant here produces {}, the block would be ignored",
                                format_name(f)));
    };
    if (e.avif) {
        if (!produced.contains(OutputFormat::avif)) return unused(OutputFormat::avif);
        if (auto r = check_effort(e.avif->effort, kAvifEffortMax, path + ".avif")) return r;
        if (e.avif->bit_depth && *e.avif->bit_depth != 8 && *e.avif->bit_depth != 10)
            return fail(path + ".avif.bit_depth", "must be 8 or 10");
    }
    if (e.webp) {
        if (!produced.contains(OutputFormat::webp)) return unused(OutputFormat::webp);
        if (auto r = check_effort(e.webp->effort, kWebpEffortMax, path + ".webp")) return r;
    }
    if (e.jpeg) {
        if (!produced.contains(OutputFormat::jpeg)) return unused(OutputFormat::jpeg);
        if (const auto& bg = e.jpeg->background) {
            for (int c : *bg)
                if (c < 0 || c > 255)
                    return fail(path + ".jpeg.background", "each channel must be between 0 and 255");
        }
    }
    return std::nullopt;
}

inline Result check_quality(const Variant& v, const std::string& path) {
    if (v.quality.empty()) return fail(path, "must not be empty");
    std::optional<int> previous;
    for (std::size_t i = 0; i < v.quality.size(); ++i) {
        const QualityStep& q = v.quality[i];
        const std::string at = std::format("{}[{}]", path, i);
        const bool last = i + 1 == v.quality.size();

        if (last && q.max_width) return fail(at + ".max_width", "the last step must not have one");
        if (!last && !q.max_width) return fail(at + ".max_width", "required on every step but the last");
        if (q.max_width) {
            if (*q.max_width <= 0) return fail(at + ".max_width", "must be positive");
            if (previous && *q.max_width <= *previous)
                return fail(at + ".max_width", "must be strictly increasing");
            previous = q.max_width;
        }

        for (OutputFormat f : {OutputFormat::avif, OutputFormat::webp, OutputFormat::jpeg}) {
            const auto& value = quality_of(q, f);
            const bool listed = std::ranges::find(v.formats, f) != v.formats.end();
            const std::string key = std::format("{}.{}", at, format_name(f));
            if (listed && !value) return fail(key, "missing: the variant produces this format");
            if (!listed && value)
                return fail(key, "the variant does not produce this format, it would be ignored");
            if (value && (*value < 1 || *value > 100)) return fail(key, "must be between 1 and 100");
        }
    }
    return std::nullopt;
}

inline Result check_variant(const Variant& v, const std::string& path) {
    if (v.widths.empty()) return fail(path + ".widths", "must not be empty");
    for (int w : v.widths)
        if (w <= 0) return fail(path + ".widths", "must be positive");
    if (has_duplicates(v.widths)) return fail(path + ".widths", "duplicate width");

    if (v.formats.empty()) return fail(path + ".formats", "must not be empty");
    if (has_duplicates(v.formats)) return fail(path + ".formats", "duplicate format");

    if (auto r = check_quality(v, path + ".quality")) return r;

    if (v.encoding) {
        const std::set<OutputFormat> produced(v.formats.begin(), v.formats.end());
        if (auto r = check_encoding(*v.encoding, produced, path + ".encoding")) return r;
    }
    return std::nullopt;
}

inline Result check_limits(const Limits& l, const Bounds& daemon) {
    const auto check = [](const auto& value, auto bound, std::string_view key) -> Result {
        if (!value) return std::nullopt;
        const std::string path = std::format("input.limits.{}", key);
        if (*value <= 0) return fail(path, "must be positive");
        if (*value > bound)
            return fail(path, std::format("above the daemon's bound ({}), limits can only tighten",
                                          bound));
        return std::nullopt;
    };
    if (auto r = check(l.max_bytes, daemon.max_bytes, "max_bytes")) return r;
    if (auto r = check(l.max_edge, daemon.max_edge, "max_edge")) return r;
    if (auto r = check(l.max_pixels, daemon.max_pixels, "max_pixels")) return r;
    return std::nullopt;
}

}  // namespace detail

// Checks a profile against the daemon's bounds. Returns the first error found.
[[nodiscard]] inline std::optional<ProfileError> validate(const Profile& p,
                                                          const Bounds& daemon = kDefaultBounds) {
    using detail::fail;

    if (p.version != kProfileVersion)
        return fail("version", std::format("unsupported version {}, expected {}", p.version,
                                           kProfileVersion));

    if (p.input.formats.empty()) return fail("input.formats", "must not be empty");
    if (detail::has_duplicates(p.input.formats)) return fail("input.formats", "duplicate format");

    if (p.input.limits)
        if (auto r = detail::check_limits(*p.input.limits, daemon)) return r;

    if (const auto& m = p.input.min_size) {
        if (m->width <= 0) return fail("input.min_size.width", "must be positive");
        if (m->height <= 0) return fail("input.min_size.height", "must be positive");
        // A floor above the edge bound would refuse every image.
        const Bounds b = effective_bounds(p, daemon);
        if (m->width > b.max_edge || m->height > b.max_edge)
            return fail("input.min_size", std::format("above the edge bound ({})", b.max_edge));
        if (static_cast<std::int64_t>(m->width) * m->height > b.max_pixels)
            return fail("input.min_size", std::format("above the pixel bound ({})", b.max_pixels));
    }

    if (p.variants.empty()) return fail("variants", "must not be empty");
    std::set<OutputFormat> produced;
    for (const auto& [name, v] : p.variants) {
        const std::string path = "variants." + name;
        if (!detail::valid_variant_name(name))
            return fail(path, "a variant name is 1 to 64 characters among a-z, 0-9, _ and -");
        if (auto r = detail::check_variant(v, path)) return r;
        produced.insert(v.formats.begin(), v.formats.end());
    }

    if (p.encoding)
        if (auto r = detail::check_encoding(*p.encoding, produced, "encoding")) return r;

    return std::nullopt;
}

// Reads a JSON profile and validates it. Unknown keys are refused by glaze
// (error_on_unknown_keys is on by default), which is what refuses chroma in WebP
// or effort in JPEG.
[[nodiscard]] inline std::expected<Profile, ProfileError> parse_profile(
    std::string_view json, const Bounds& daemon = kDefaultBounds) {
    Profile p;
    if (const auto ec = glz::read_json(p, json))
        return std::unexpected(ProfileError{"", glz::format_error(ec, json)});
    if (auto error = validate(p, daemon)) return std::unexpected(std::move(*error));
    return p;
}

}  // namespace tirage
