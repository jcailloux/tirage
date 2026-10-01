#pragma once

// The libvips side of tirage-worker: decode, orient, bound, crop, convert the
// colour, mask, resize and encode (plan § 3, § 4 and § 7).
//
// Ported from codiga (~/Projets/codiga/api/src/media/ingest.h, derive.h and
// fleet/images.h), whose choices are measured there: Lanczos3, autorot before
// anything, bounds on the header before any pixel. Deliberate differences, all
// from the plan:
// - `strip` gives way to `keep`, preceded by the conversion to sRGB;
// - every image with an alpha channel is resized premultiplied (codiga only did
//   it for the fleet renders);
// - the bytes come back to the caller instead of being written to disk;
// - loaders are picked by the sniffed format, so no other libvips loader ever
//   sees an input.

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <vips/vips8>

#include "tirage/protocol.h"
#include "tirage/request.h"
#include "tirage/resolve.h"

namespace tirage::worker {

// An encoder libvips could not run. Not a refusal: the input was fine, the
// machine was not (an AVIF encoder missing, most likely). main() turns it into
// a non-zero exit, which the caller sees as a failure.
struct EncodeFailure {
    std::string message;
};

namespace detail {

using vips::VImage;

// Sequential access: the original is decoded once, top to bottom, straight
// into the single in-memory copy the outputs are made from (run). Random access
// would first decode it into a buffer of the loader's own (a temporary file
// past 100 MB), then copy it: twice the memory for nothing.
inline VImage load(InputFormat format, std::string_view bytes) {
    // The blob borrows the bytes, which outlive every image of the job.
    VipsBlob* blob = vips_blob_new(nullptr, bytes.data(), bytes.size());
    struct Unref {
        VipsBlob* b;
        ~Unref() { vips_area_unref(VIPS_AREA(b)); }
    } unref{blob};
    const auto sequential = [] { return VImage::option()->set("access", VIPS_ACCESS_SEQUENTIAL); };
    switch (format) {
        case InputFormat::png: return VImage::pngload_buffer(blob, sequential());
        case InputFormat::jpeg: return VImage::jpegload_buffer(blob, sequential());
        case InputFormat::webp: return VImage::webpload_buffer(blob, sequential());
        case InputFormat::heic:
        case InputFormat::avif: return VImage::heifload_buffer(blob, sequential());
    }
    throw vips::VError("unknown input format");
}

// A 16-bit source stays 16-bit only for a 10-bit AVIF, the one output that
// uses the precision. Otherwise it is brought to 8 bits before being held in
// memory, which halves what it costs there.
inline bool needs_16bit(const Profile& p, const std::vector<std::string>& variants) {
    for (const std::string& name : variants) {
        const Variant& v = p.variants.at(name);
        if (std::ranges::find(v.formats, OutputFormat::avif) != v.formats.end() &&
            resolve_encoding(p, v).avif.bit_depth > 8)
            return true;
    }
    return false;
}

// color: "srgb" converts through the embedded profile, then the profile is not
// written (keep_flags). "icc" leaves the pixels and the profile alone. CMYK is
// converted to sRGB in both cases: no output format of tirage carries CMYK.
// Without an embedded profile, libvips uses its built-in CMYK profile.
//
// 16-bit sources stay 16-bit here, so that a 10-bit AVIF keeps their precision.
// Each encoder brings them down to what it takes (encode).
inline VImage convert_colour(VImage img, Color color, std::vector<std::string>& warnings) {
    const bool cmyk = img.interpretation() == VIPS_INTERPRETATION_CMYK;
    const bool has_profile = img.get_typeof(VIPS_META_ICC_NAME) != 0;
    if (!cmyk && !(color == Color::srgb && has_profile)) return img;

    const int depth = img.format() == VIPS_FORMAT_USHORT ? 16 : 8;
    try {
        return img.icc_transform("srgb", VImage::option()->set("embedded", true)->set("depth", depth));
    } catch (const vips::VError&) {
        // A profile that does not match the pixels (an RGB profile on a grey
        // image, a truncated one). CMYK has no fallback: it is refused.
        if (cmyk) throw;
        warnings.push_back("unusable ICC profile, the image is taken as sRGB");
        img = img.copy();
        img.remove(VIPS_META_ICC_NAME);
        return img;
    }
}

// Premultiplied when there is an alpha channel: resizing straight alpha bleeds
// the (usually black) colour of transparent pixels into the edges. Measured by
// codiga on the fleet renders (fleet/images.h): 8 % darkening of the contour
// avoided. vips_resize does not premultiply by itself (vips_thumbnail does).
// The cost: premultiplied pixels are floats, and the resize buffers grow with
// them (measured on a 100 Mpx RGBA original: 770 MiB tracked by libvips
// against 480 MiB straight).
inline VImage resize_to(const VImage& src, int width) {
    if (width == src.width()) return src;
    const double scale = static_cast<double>(width) / src.width();
    const auto lanczos = [] { return VImage::option()->set("kernel", VIPS_KERNEL_LANCZOS3); };
    if (!src.has_alpha()) return src.resize(scale, lanczos());
    return src.premultiply().resize(scale, lanczos()).unpremultiply().cast(src.format());
}

// 8 bits per sample, for the encoders that take nothing else.
inline VImage to_8bit(const VImage& img) {
    if (img.format() == VIPS_FORMAT_UCHAR) return img;
    const bool grey = img.bands() <= 2;
    return img.cast(VIPS_FORMAT_UCHAR, VImage::option()->set("shift", true))
        .copy(VImage::option()->set("interpretation",
                                    grey ? VIPS_INTERPRETATION_B_W : VIPS_INTERPRETATION_sRGB));
}

// libvips `keep`: `color` decides the ICC profile, `metadata` the rest.
inline int keep_flags(const Profile& p) {
    int flags = VIPS_FOREIGN_KEEP_NONE;
    if (p.color == Color::icc) flags |= VIPS_FOREIGN_KEEP_ICC;
    if (p.metadata == Metadata::keep)
        flags |= VIPS_FOREIGN_KEEP_EXIF | VIPS_FOREIGN_KEEP_XMP | VIPS_FOREIGN_KEEP_IPTC;
    return flags;
}

// Each cell of bw x bh pixels takes its mean colour. The last row and column
// of cells are completed by repeating the zone's edge.
inline VImage cells(const VImage& zone, int bw, int bh) {
    const int w = zone.width(), h = zone.height();
    const int cw = (w + bw - 1) / bw, ch = (h + bh - 1) / bh;
    return zone.embed(0, 0, cw * bw, ch * bh, VImage::option()->set("extend", VIPS_EXTEND_COPY))
        .shrink(bw, bh)
        .zoom(bw, bh)
        .extract_area(0, 0, w, h);
}

// Blurred at 32 pixels on the zone's longer side, then brought back to its
// size: the same look at any resolution, and a small kernel on a large zone.
// codiga's editor previews it with the same constants (plan § 7).
inline VImage blurred(const VImage& zone) {
    constexpr double kSide = 32;
    const int w = zone.width(), h = zone.height();
    const double scale = std::min(1.0, kSide / std::max(w, h));
    VImage small = scale < 1 ? zone.resize(scale) : zone;
    small = small.gaussblur(std::max(1.0, std::max(small.width(), small.height()) / 8.0));
    if (small.width() == w && small.height() == h) return small;
    return small
        .resize(static_cast<double>(w) / small.width(),
                VImage::option()
                    ->set("vscale", static_cast<double>(h) / small.height())
                    ->set("kernel", VIPS_KERNEL_LINEAR))
        .embed(0, 0, w, h, VImage::option()->set("extend", VIPS_EXTEND_COPY));
}

// What replaces a zone. Premultiplied when there is an alpha channel, like
// resize_to, so that transparent pixels lend no colour.
inline VImage hidden(const VImage& zone, MaskStyle style) {
    const bool alpha = zone.has_alpha();
    const VImage in = alpha ? zone.premultiply() : zone;
    VImage out;
    switch (style) {
        case MaskStyle::blur: out = blurred(in); break;
        case MaskStyle::pixelate: {
            const int block = pixelate_block(in.width(), in.height());
            out = cells(in, block, block);
            break;
        }
        case MaskStyle::fill: out = cells(in, in.width(), in.height()); break;
    }
    if (alpha) out = out.unpremultiply();
    return out.cast(zone.format());
}

// A soft edge: `patch` (the hidden zone) laid over `zone` (the image as it
// was) with soft_coverage as opacity. `whole` is the mask's whole rectangle,
// `z` the part of it inside the image, both as stored. The rounded rectangle
// is the same under every EXIF orientation (flips and quarter turns only swap
// its sides), so its opacity is computed as stored, without turning anything.
inline VImage soften(const VImage& patch, const VImage& zone, const Rect& whole, const Rect& z) {
    std::vector<unsigned char> alpha(static_cast<std::size_t>(z.width) * z.height);
    for (int y = 0; y < z.height; ++y)
        for (int x = 0; x < z.width; ++x)
            alpha[static_cast<std::size_t>(y) * z.width + x] = static_cast<unsigned char>(
                std::lround(255 * soft_coverage(z.x - whole.x + x, z.y - whole.y + y, whole.width, whole.height)));
    const VImage a =
        VImage::new_from_memory_copy(alpha.data(), alpha.size(), z.width, z.height, 1, VIPS_FORMAT_UCHAR);
    return ((patch.cast(VIPS_FORMAT_FLOAT) * a + zone.cast(VIPS_FORMAT_FLOAT) * (255 - a)) / 255)
        .rint()
        .cast(zone.format());
}

// Draws the masks into `img`, the in-memory copy, still as stored. `zones` are
// the masks resolved on the oriented image (`width` x `height`), in their
// order: each is moved to the stored image (to_stored) and replaced in place.
// Only the zone's pixels are read, and nothing is copied but the zone.
inline void draw_masks(VImage& img, const std::vector<Mask>& masks, const std::vector<Rect>& zones, int width,
                       int height) {
    const int orientation = vips_image_get_orientation(img.get_image());
    for (std::size_t i = 0; i < masks.size(); ++i) {
        if (zones[i].width == 0) continue;
        const Mask& m = masks[i];
        const Rect z = to_stored(zones[i], orientation, img.width(), img.height());
        // In memory before drawing: the patch is read from the image it goes into.
        const VImage zone = img.extract_area(z.x, z.y, z.width, z.height).copy_memory();
        VImage patch = hidden(zone, m.style);
        if (m.edge == MaskEdge::soft) {
            const Rect oriented = tirage::detail::to_pixels(m.x, m.y, m.width, m.height, m.unit, width, height);
            const Rect whole = to_stored(oriented, orientation, img.width(), img.height());
            patch = soften(patch, zone, whole, z);
        }
        img.draw_image(patch.copy_memory(), z.x, z.y);
    }
}

inline int subsample(Chroma c) {
    return c == Chroma::s420 ? VIPS_FOREIGN_SUBSAMPLE_ON : VIPS_FOREIGN_SUBSAMPLE_OFF;
}

inline std::string encode(const VImage& img, OutputFormat format, int quality,
                          const ResolvedEncoding& e, int keep) {
    VImage in = img;
    const char* suffix = "";
    vips::VOption* options = VImage::option()->set("Q", quality)->set("keep", keep);

    switch (format) {
        case OutputFormat::avif:
            suffix = ".avif";
            // A 16-bit source goes to a 10-bit AVIF as is: libvips keeps its precision.
            if (e.avif.bit_depth == 8) in = to_8bit(in);
            options->set("compression", VIPS_FOREIGN_HEIF_COMPRESSION_AV1)
                ->set("effort", e.avif.effort)
                ->set("subsample_mode", subsample(e.avif.chroma))
                ->set("bitdepth", e.avif.bit_depth);
            break;
        case OutputFormat::webp:
            suffix = ".webp";
            in = to_8bit(in);
            options->set("effort", e.webp.effort)->set("smart_subsample", e.webp.sharp_yuv);
            break;
        case OutputFormat::jpeg: {
            suffix = ".jpg";
            in = to_8bit(in);
            if (in.has_alpha()) {
                // JPEG has no alpha: flatten on the profile's background.
                if (in.bands() == 2) in = in.colourspace(VIPS_INTERPRETATION_sRGB);
                const auto& bg = e.jpeg.background;
                in = in.flatten(VImage::option()->set(
                    "background", std::vector<double>{static_cast<double>(bg[0]),
                                                      static_cast<double>(bg[1]),
                                                      static_cast<double>(bg[2])}));
            }
            options->set("subsample_mode", subsample(e.jpeg.chroma))
                ->set("interlace", e.jpeg.progressive)
                ->set("optimize_coding", e.jpeg.optimize_coding);
            break;
        }
    }

    void* buf = nullptr;
    std::size_t size = 0;
    in.write_to_buffer(suffix, &buf, &size, options);
    std::string out(static_cast<const char*>(buf), size);
    g_free(buf);
    return out;
}

}  // namespace detail

// Runs a job. A refusal is a response with an error. Throws EncodeFailure when
// an encoder fails on an image that decoded fine.
[[nodiscard]] inline Response run(const Job& job) {
    using vips::VImage;
    const Request& req = job.request;
    const Profile& profile = req.profile;

    if (auto r = validate_request(req, job.bounds)) return refused(std::move(*r));
    const Bounds bounds = effective_bounds(profile, job.bounds);
    if (auto r = check_input(profile, req.input, bounds)) return refused(std::move(*r));
    const InputFormat format = *sniff(req.input);

    Response out;
    VImage work;
    try {
        // Lazy: only the header is read here. The bounds are checked on it,
        // before a single pixel is decoded (rotation keeps both bounds).
        VImage img = detail::load(format, req.input);
        if (auto r = check_dimensions(img.width(), img.height(), bounds))
            return refused(std::move(*r));

        // Sizes of the oriented image, still without decoding: the crop and
        // the floor are expressed on it (EXIF orientation comes first).
        const bool swap = vips_image_get_orientation_swap(img.get_image());
        const int width = swap ? img.height() : img.width();
        const int height = swap ? img.width() : img.height();
        out.source = Source{.width = width, .height = height, .bands = img.bands(), .format = format};

        const Rect area = req.crop ? resolve_crop(*req.crop, width, height) : Rect{0, 0, width, height};
        if (req.crop) out.crop = area;
        const bool extract = area.width != width || area.height != height;

        const FloorVerdict floor = check_floor(profile.input.min_size, area.width, area.height);
        if (const auto* refusal = std::get_if<Refusal>(&floor)) {
            Response r = refused(*refusal);
            r.source = out.source;
            r.crop = out.crop;
            return r;
        }
        if (const auto* warning = std::get_if<std::string>(&floor)) out.warnings.push_back(*warning);

        if (req.operation == Operation::probe) return out;

        // The colour conversion works pixel by pixel, so it runs during the
        // sequential decode, and the only copy held in memory is already
        // converted. Every output is then resized from that copy.
        img = detail::convert_colour(img, profile.color, out.warnings);
        if (!detail::needs_16bit(profile, req.variants)) img = detail::to_8bit(img);
        img = img.copy_memory();

        // Masks, drawn into that copy before anything is resized from it: no
        // output ever holds a zone's pixels.
        if (!req.masks.empty()) {
            for (std::size_t i = 0; i < req.masks.size(); ++i) {
                out.masks.push_back(resolve_mask(req.masks[i], width, height));
                if (out.masks.back().width == 0)
                    out.warnings.push_back(std::format("masks[{}]: no pixel of the image, nothing hidden", i));
            }
            detail::draw_masks(img, req.masks, out.masks, width, height);
        }

        // Orientation and crop, on the copy in memory. autorot also removes the
        // orientation tag, so that no output gets rotated a second time.
        work = img.autorot();
        if (extract) work = work.extract_area(area.x, area.y, area.width, area.height);
    } catch (const vips::VError& e) {
        Response r = refused({Code::decode_failed, e.what()});
        r.source = out.source;
        return r;
    }

    int keep = detail::keep_flags(profile);
    // EXIF may hold a thumbnail of the original, XMP too: a masked image keeps
    // its ICC profile only.
    if (!req.masks.empty() && (keep & ~VIPS_FOREIGN_KEEP_ICC)) {
        keep &= VIPS_FOREIGN_KEEP_ICC;
        out.warnings.push_back("masks: metadata not kept, it may hold a thumbnail of the original");
    }
    try {
        for (const std::string& name : req.variants) {
            const Variant& variant = profile.variants.at(name);
            const ResolvedEncoding encoding = resolve_encoding(profile, variant);
            for (const int width : planned_widths(variant, work.width())) {
                VImage scaled = detail::resize_to(work, width);
                if (variant.formats.size() > 1) scaled = scaled.copy_memory();
                for (const OutputFormat f : variant.formats) {
                    out.outputs.push_back(Output{
                        .variant = name,
                        .width = scaled.width(),
                        .height = scaled.height(),
                        .format = f,
                        .bytes = detail::encode(scaled, f, *quality_at(variant, width, f), encoding,
                                                keep),
                    });
                }
            }
        }
    } catch (const vips::VError& e) {
        throw EncodeFailure{e.what()};
    }
    return out;
}

}  // namespace tirage::worker
