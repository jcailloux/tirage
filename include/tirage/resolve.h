#pragma once

// What a validated profile means for one variant: the encoding settings once
// the variant's and the profile's overrides are applied, the quality at a
// given width, and the widths actually produced (docs/profile.md).

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include "tirage/profile.h"

namespace tirage {

struct ResolvedAvif {
    int effort = 4;
    Chroma chroma = Chroma::s420;
    int bit_depth = 8;
};

struct ResolvedWebp {
    int effort = 4;
    bool sharp_yuv = false;
};

struct ResolvedJpeg {
    Chroma chroma = Chroma::s420;
    bool progressive = false;
    bool optimize_coding = false;
    std::array<int, 3> background{255, 255, 255};
};

// Default-constructed members are tirage's defaults, used when neither the
// profile nor the variant says otherwise.
struct ResolvedEncoding {
    ResolvedAvif avif;
    ResolvedWebp webp;
    ResolvedJpeg jpeg;
};

namespace detail {

template <class T>
void apply(T& target, const std::optional<T>& value) {
    if (value) target = *value;
}

inline void apply_encoding(ResolvedEncoding& r, const Encoding& e) {
    if (e.avif) {
        apply(r.avif.effort, e.avif->effort);
        apply(r.avif.chroma, e.avif->chroma);
        apply(r.avif.bit_depth, e.avif->bit_depth);
    }
    if (e.webp) {
        apply(r.webp.effort, e.webp->effort);
        apply(r.webp.sharp_yuv, e.webp->sharp_yuv);
    }
    if (e.jpeg) {
        apply(r.jpeg.chroma, e.jpeg->chroma);
        apply(r.jpeg.progressive, e.jpeg->progressive);
        apply(r.jpeg.optimize_coding, e.jpeg->optimize_coding);
        apply(r.jpeg.background, e.jpeg->background);
    }
}

}  // namespace detail

// tirage's defaults, then the profile's encoding, then the variant's, field by field.
[[nodiscard]] inline ResolvedEncoding resolve_encoding(const Profile& profile, const Variant& variant) {
    ResolvedEncoding r;
    if (profile.encoding) detail::apply_encoding(r, *profile.encoding);
    if (variant.encoding) detail::apply_encoding(r, *variant.encoding);
    return r;
}

// Quality of `format` at `width`: the first step whose max_width is at least
// `width`, else the last step. Empty when the variant does not produce `format`.
[[nodiscard]] inline std::optional<int> quality_at(const Variant& variant, int width,
                                                   OutputFormat format) {
    for (const QualityStep& q : variant.quality) {
        if (!q.max_width || width <= *q.max_width) {
            switch (format) {
                case OutputFormat::avif: return q.avif;
                case OutputFormat::webp: return q.webp;
                case OutputFormat::jpeg: return q.jpeg;
            }
        }
    }
    return std::nullopt;
}

// Widths produced from a source (or crop) `source_width` wide, ascending. Never
// upscale: the first width at or above the source is capped to the source, and
// the wider ones are dropped. A source narrower than every width still gets one
// output, at its own width.
[[nodiscard]] inline std::vector<int> planned_widths(const Variant& variant, int source_width) {
    std::vector<int> targets = variant.widths;
    std::ranges::sort(targets);
    std::vector<int> out;
    for (int t : targets) {
        const int w = std::min(t, source_width);
        out.push_back(w);
        if (w == source_width) break;
    }
    return out;
}

}  // namespace tirage
