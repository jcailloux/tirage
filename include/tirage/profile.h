#pragma once

// The encoding profile: what a caller wants out of an original (plan § 3).
//
// The profile travels with every request. It is plain data: this header only
// declares its shape and how glaze reads and writes it. Checking that a profile
// makes sense is validate.h.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

namespace tirage {

// Current profile schema version. A profile with another version is refused.
inline constexpr int kProfileVersion = 1;

// A new format goes at the end: BEVE may number them in this order.
enum class InputFormat { png, jpeg, webp, heic, avif };
enum class OutputFormat { avif, webp, jpeg };
enum class FloorPolicy { reject, warn };
enum class Color { srgb, icc };
enum class Metadata { strip, keep };
// libvips subsample_mode, without "auto" (plan § 3: applied as is at any quality).
enum class Chroma { s420, s444 };

struct MinSize {
    int width = 0;
    int height = 0;
    FloorPolicy policy = FloorPolicy::reject;
};

// Safety bounds. The daemon owns them (config); a profile can only tighten them
// through input.limits, never relax them.
struct Limits {
    std::optional<std::int64_t> max_bytes;
    std::optional<int> max_edge;
    std::optional<std::int64_t> max_pixels;
};

struct Input {
    std::vector<InputFormat> formats;
    std::optional<MinSize> min_size;  // absent: no floor
    std::optional<Limits> limits;
};

// Encoding settings are all optional: a variant overrides the profile field by
// field, and the profile overrides tirage's defaults (resolve.h).
struct AvifEncoding {
    std::optional<int> effort;  // 0 to 9
    std::optional<Chroma> chroma;
    std::optional<int> bit_depth;  // 8 or 10
};

// No chroma: libwebp's lossy mode is always 4:2:0, sharp_yuv is its only knob.
struct WebpEncoding {
    std::optional<int> effort;  // 0 to 6
    std::optional<bool> sharp_yuv;
};

// No effort: JPEG has none.
struct JpegEncoding {
    std::optional<Chroma> chroma;
    std::optional<bool> progressive;
    std::optional<bool> optimize_coding;
    std::optional<std::array<int, 3>> background;  // RGB the alpha is flattened on
};

struct Encoding {
    std::optional<AvifEncoding> avif;
    std::optional<WebpEncoding> webp;
    std::optional<JpegEncoding> jpeg;
};

// One quality step: applies up to max_width (inclusive), the last step has none.
struct QualityStep {
    std::optional<int> max_width;
    std::optional<int> avif;
    std::optional<int> webp;
    std::optional<int> jpeg;
};

struct Variant {
    std::vector<int> widths;
    std::vector<OutputFormat> formats;
    std::vector<QualityStep> quality;
    std::optional<Encoding> encoding;
};

struct Profile {
    int version = 0;
    Input input;
    Color color = Color::srgb;
    Metadata metadata = Metadata::strip;
    std::optional<Encoding> encoding;
    std::map<std::string, Variant> variants;
};

}  // namespace tirage

template <>
struct glz::meta<tirage::Chroma> {
    static constexpr std::array keys{"420", "444"};
    static constexpr std::array value{tirage::Chroma::s420, tirage::Chroma::s444};
};

template <>
struct glz::meta<tirage::InputFormat> {
    using enum tirage::InputFormat;
    static constexpr auto value = glz::enumerate(png, jpeg, webp, heic, avif);
};

template <>
struct glz::meta<tirage::OutputFormat> {
    using enum tirage::OutputFormat;
    static constexpr auto value = glz::enumerate(avif, webp, jpeg);
};

template <>
struct glz::meta<tirage::FloorPolicy> {
    using enum tirage::FloorPolicy;
    static constexpr auto value = glz::enumerate(reject, warn);
};

template <>
struct glz::meta<tirage::Color> {
    using enum tirage::Color;
    static constexpr auto value = glz::enumerate(srgb, icc);
};

template <>
struct glz::meta<tirage::Metadata> {
    using enum tirage::Metadata;
    static constexpr auto value = glz::enumerate(strip, keep);
};
