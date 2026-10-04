// The AVIF writer: libvips 8.16.1's heifsave (libvips/foreign/heifsave.c),
// ported so that the aom encoder's tune can be set (plan § 11). libvips only
// exposes tune from 8.18, and trixie has 8.16.
//
// It is a subclass of VipsForeignSave, like heifsave: libvips' own save
// machinery makes the image ready (conversion to RGB or RGBA in 8 or 16 bits,
// keep, EXIF rebuilt from the image, an incompatible ICC profile dropped), and
// only the libheif part is ported. With tune ssim, the output is heifsave's,
// byte for byte (checked by the integration tests). Deliberate differences:
// - the aom encoder is picked by name, with no fallback: another AV1 encoder
//   installed one day must not take its place, tune is aom's;
// - only what tirage uses: lossy, AV1, one bit depth always given (8 or 10),
//   no profile argument;
// - tune, the reason for it all, set after the other parameters, and refused
//   loudly if aom does not take it.

#include "avif.h"

#include <cstdint>
#include <cstring>
#include <string>

#include <aom/aom_codec.h>
#include <libheif/heif.h>
#include <vips/vips.h>

namespace tirage::worker {

namespace {

struct AvifSave {
    VipsForeignSave parent_object;

    // Set before build.
    int Q;
    int bitdepth;
    int effort;
    Chroma chroma;
    Tune tune;
    std::string* out;

    heif_context* ctx;
    heif_encoder* encoder;
    heif_image_handle* handle;  // the page being written
    heif_image* img;            // the page in memory, filled as the image is scanned
    std::uint8_t* data;         // its plane
    int stride;
    int page_width;
    int page_height;
};

using AvifSaveClass = VipsForeignSaveClass;

G_DEFINE_TYPE(AvifSave, tirage_avif_save, VIPS_TYPE_FOREIGN_SAVE)

// vips__heif_error, which libvips does not export.
void heif_fail(const heif_error& error) {
    vips_error("heif", "%s (%d.%d)", error.message ? error.message : "(null)", error.code, error.subcode);
}

bool failed(const heif_error& error) {
    if (!error.code) return false;
    heif_fail(error);
    return true;
}

// A parameter the encoder may not have: heifsave tolerates that.
bool failed_unless_unsupported(const heif_error& error) {
    return error.code && error.subcode != heif_suberror_Unsupported_parameter && failed(error);
}

// vips__heif_chroma, not exported either.
heif_chroma interleaved(int bitdepth, bool alpha) {
    if (bitdepth == 8) return alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB;
    return alpha ? heif_chroma_interleaved_RRGGBBAA_BE : heif_chroma_interleaved_RRGGBB_BE;
}

void dispose(GObject* gobject) {
    auto* heif = reinterpret_cast<AvifSave*>(gobject);
    VIPS_FREEF(heif_image_release, heif->img);
    VIPS_FREEF(heif_image_handle_release, heif->handle);
    VIPS_FREEF(heif_encoder_release, heif->encoder);
    VIPS_FREEF(heif_context_free, heif->ctx);
    G_OBJECT_CLASS(tirage_avif_save_parent_class)->dispose(gobject);
}

int write_metadata(AvifSave* heif) {
    using Saver = heif_error (*)(heif_context*, const heif_image_handle*, const void*, int);
    struct Field {
        const char* name;
        Saver saver;
    };
    static constexpr Field fields[] = {
        {VIPS_META_EXIF_NAME, heif_context_add_exif_metadata},
        {VIPS_META_XMP_NAME, heif_context_add_XMP_metadata},
    };
    VipsImage* ready = heif->parent_object.ready;
    for (const Field& f : fields) {
        if (!vips_image_get_typeof(ready, f.name)) continue;
        const void* data;
        size_t length;
        if (vips_image_get_blob(ready, f.name, &data, &length)) return -1;
        if (failed(f.saver(heif->ctx, heif->handle, data, static_cast<int>(length)))) return -1;
    }
    return 0;
}

int write_page(AvifSave* heif, int page) {
    VipsImage* ready = heif->parent_object.ready;

    if (vips_image_get_typeof(ready, VIPS_META_ICC_NAME)) {
        const void* data;
        size_t length;
        if (vips_image_get_blob(ready, VIPS_META_ICC_NAME, &data, &length)) return -1;
        if (failed(heif_image_set_raw_color_profile(heif->img, "rICC", data, length))) return -1;
    }

    heif_encoding_options* options = heif_encoding_options_alloc();
    if (vips_image_hasalpha(ready)) options->save_alpha_channel = 1;
    // EXIF orientation is informational in HEIF: irot and imir carry it.
    options->image_orientation = static_cast<heif_orientation>(vips_image_get_orientation(ready));
    vips_autorot_remove_angle(ready);

    const heif_error error = heif_context_encode_image(heif->ctx, heif->img, heif->encoder, options, &heif->handle);
    heif_encoding_options_free(options);
    if (failed(error)) return -1;

    if (vips_image_get_typeof(ready, "heif-primary")) {
        int primary;
        if (vips_image_get_int(ready, "heif-primary", &primary)) return -1;
        if (page == primary && failed(heif_context_set_primary_image(heif->ctx, heif->handle))) return -1;
    }

    if (write_metadata(heif)) return -1;
    VIPS_FREEF(heif_image_handle_release, heif->handle);
    return 0;
}

int pack(AvifSave* heif, VipsPel* q, VipsPel* p, int ne) {
    VipsImage* ready = heif->parent_object.ready;
    const bool sixteen = ready->Type == VIPS_INTERPRETATION_RGB16 || ready->Type == VIPS_INTERPRETATION_GREY16;

    if (ready->BandFmt == VIPS_FORMAT_UCHAR && heif->bitdepth == 8) {
        std::memcpy(q, p, ne);
    } else if (ready->BandFmt == VIPS_FORMAT_UCHAR && heif->bitdepth > 8) {
        // 8-bit source, a big-endian short shifted up.
        const int shift = heif->bitdepth - 8;
        for (int i = 0; i < ne; i++) {
            const guint16 v = p[i] << shift;
            q[0] = v >> 8;
            q[1] = v;
            q += 2;
        }
    } else if (ready->BandFmt == VIPS_FORMAT_USHORT && heif->bitdepth <= 8) {
        // 16-bit native source, 8-bit write: the high or low bits.
        const int shift = (sixteen ? 16 : 8) - heif->bitdepth;
        for (int i = 0; i < ne; i++) {
            const guint16 v = *reinterpret_cast<gushort*>(p) >> shift;
            q[i] = v;
            p += 2;
        }
    } else if (ready->BandFmt == VIPS_FORMAT_USHORT && heif->bitdepth > 8) {
        // 16-bit native source, 16-bit big-endian write.
        const int shift = (sixteen ? 16 : 8) - heif->bitdepth;
        for (int i = 0; i < ne; i++) {
            const guint16 v = *reinterpret_cast<gushort*>(p) >> shift;
            q[0] = v >> 8;
            q[1] = v;
            p += 2;
            q += 2;
        }
    } else {
        vips_error("tirage_avifsave", "%s", "unimplemented format conversion");
        return -1;
    }
    return 0;
}

// Copies a line at a time into the page, and writes the page once it is full.
int write_block(VipsRegion* region, VipsRect* area, void* a) {
    auto* heif = static_cast<AvifSave*>(a);
    for (int y = 0; y < area->height; y++) {
        const int page = (area->top + y) / heif->page_height;
        const int line = (area->top + y) % heif->page_height;
        VipsPel* p = VIPS_REGION_ADDR(region, 0, area->top + y);
        VipsPel* q = heif->data + static_cast<size_t>(heif->stride) * line;
        if (pack(heif, q, p, VIPS_REGION_N_ELEMENTS(region))) return -1;
        if (line == heif->page_height - 1 && write_page(heif, page)) return -1;
    }
    return 0;
}

heif_error append(heif_context*, const void* data, size_t length, void* userdata) {
    static_cast<AvifSave*>(userdata)->out->append(static_cast<const char*>(data), length);
    return heif_error{heif_error_Ok, heif_suberror_Unspecified, "Success"};
}

// The encoder's tune. ssim leaves libheif's auto alone, as libvips does: SSIM
// for colour, PSNR for alpha (SSIM rings there). From libaom 3.13, auto picks
// IQ for colour: ssim is then set outright, alpha included, rather than give
// IQ to a profile that asked for SSIM.
int set_tune(AvifSave* heif) {
    constexpr int kAom313 = (3 << 16) | (13 << 8);
    const char* tune = nullptr;
    switch (heif->tune) {
        case Tune::ssim:
            if (aom_codec_version() >= kAom313) tune = "ssim";
            break;
        case Tune::iq: tune = "iq"; break;
    }
    // No tolerance here: a tune the encoder does not take would be ignored.
    if (tune && failed(heif_encoder_set_parameter_string(heif->encoder, "tune", tune))) return -1;
    return 0;
}

int build(VipsObject* object) {
    auto* heif = reinterpret_cast<AvifSave*>(object);
    VipsForeignSave* save = &heif->parent_object;

    if (VIPS_OBJECT_CLASS(tirage_avif_save_parent_class)->build(object)) return -1;

    if (heif->bitdepth != 8 && heif->bitdepth != 10) {
        vips_error("tirage_avifsave", "%d-bit colour depth not supported", heif->bitdepth);
        return -1;
    }

    const heif_encoder_descriptor* descriptor;
    if (heif_context_get_encoder_descriptors(heif->ctx, heif_compression_AV1, "aom", &descriptor, 1) < 1) {
        vips_error("tirage_avifsave", "%s", "the aom AV1 encoder of libheif is missing");
        return -1;
    }
    if (failed(heif_context_get_encoder(heif->ctx, descriptor, &heif->encoder))) return -1;

    if (failed(heif_encoder_set_lossy_quality(heif->encoder, heif->Q))) return -1;
    if (failed(heif_encoder_set_lossless(heif->encoder, false))) return -1;
    if (failed_unless_unsupported(heif_encoder_set_parameter_integer(heif->encoder, "speed", 9 - heif->effort)))
        return -1;
    const char* chroma = heif->chroma == Chroma::s444 ? "444" : "420";
    if (failed_unless_unsupported(heif_encoder_set_parameter_string(heif->encoder, "chroma", chroma))) return -1;

    for (const heif_encoder_parameter* const* param = heif_encoder_list_parameters(heif->encoder); *param; param++) {
        if (std::strcmp(heif_encoder_parameter_get_name(*param), "threads") != 0) continue;
        int have_minimum, have_maximum, minimum, maximum;
        if (failed(heif_encoder_parameter_get_valid_integer_values(*param, &have_minimum, &have_maximum, &minimum,
                                                                   &maximum, nullptr, nullptr)))
            return -1;
        if (failed_unless_unsupported(heif_encoder_set_parameter_integer(
                heif->encoder, "threads", VIPS_CLIP(minimum, vips_concurrency_get(), maximum))))
            return -1;
    }

    // Auto tiles: much faster AVIF encodes for a very small increase in size.
    if (failed_unless_unsupported(heif_encoder_set_parameter_boolean(heif->encoder, "auto-tiles", true))) return -1;
    // No intra block copy: more predictable encode times.
    if (failed_unless_unsupported(heif_encoder_set_parameter_boolean(heif->encoder, "enable-intrabc", false)))
        return -1;
    if (set_tune(heif)) return -1;

    heif->page_width = save->ready->Xsize;
    heif->page_height = vips_image_get_page_height(save->ready);
    if (heif->page_width > 16384 || heif->page_height > 16384) {
        vips_error("tirage_avifsave", "%s", "image too large");
        return -1;
    }
    if (save->ready->Type == VIPS_INTERPRETATION_MULTIBAND) {
        vips_error("tirage_avifsave", "unsupported interpretation: %s",
                   vips_enum_nick(VIPS_TYPE_INTERPRETATION, save->ready->Type));
        return -1;
    }

    if (failed(heif_image_create(heif->page_width, heif->page_height, heif_colorspace_RGB,
                                 interleaved(heif->bitdepth, vips_image_hasalpha(save->ready)), &heif->img)))
        return -1;
    if (failed(heif_image_add_plane(heif->img, heif_channel_interleaved, heif->page_width, heif->page_height,
                                    heif->bitdepth)))
        return -1;
    heif->data = heif_image_get_plane(heif->img, heif_channel_interleaved, &heif->stride);

    if (vips_sink_disc(save->ready, write_block, heif)) return -1;

    heif_writer writer{};
    writer.writer_api_version = 1;
    writer.write = append;
    if (failed(heif_context_write(heif->ctx, &writer, heif))) return -1;
    return 0;
}

// Except for 8-bit inputs, everything goes to 16 bits (heifsave's table).
VipsBandFormat band_formats[10] = {
    // UC           C                 US                 S                  UI
    VIPS_FORMAT_UCHAR, VIPS_FORMAT_UCHAR, VIPS_FORMAT_USHORT, VIPS_FORMAT_USHORT, VIPS_FORMAT_USHORT,
    // I            F                  X                  D                  DX
    VIPS_FORMAT_USHORT, VIPS_FORMAT_USHORT, VIPS_FORMAT_USHORT, VIPS_FORMAT_USHORT, VIPS_FORMAT_USHORT,
};

void tirage_avif_save_class_init(AvifSaveClass* klass) {
    GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
    VipsObjectClass* object_class = VIPS_OBJECT_CLASS(klass);
    VipsForeignSaveClass* save_class = VIPS_FOREIGN_SAVE_CLASS(klass);

    gobject_class->dispose = dispose;
    gobject_class->set_property = vips_object_set_property;
    gobject_class->get_property = vips_object_get_property;

    object_class->nickname = "tirage_avifsave";
    object_class->description = "save image in AVIF format with the aom encoder";
    object_class->build = build;

#if VIPS_MAJOR_VERSION > 8 || (VIPS_MAJOR_VERSION == 8 && VIPS_MINOR_VERSION >= 17)
    // libvips 8.17 reworked the saveable flags: heifsave's from then on.
    save_class->saveable = static_cast<VipsForeignSaveable>(VIPS_FOREIGN_SAVEABLE_RGB | VIPS_FOREIGN_SAVEABLE_ALPHA);
#else
    save_class->saveable = VIPS_SAVEABLE_RGBA_ONLY;
#endif
    save_class->format_table = band_formats;
}

void tirage_avif_save_init(AvifSave* heif) {
    heif->ctx = heif_context_alloc();
    heif->Q = 50;
    heif->bitdepth = 8;
    heif->effort = 4;
    heif->chroma = Chroma::s420;
    heif->tune = Tune::ssim;
}

}  // namespace

std::string write_avif(const vips::VImage& in, int quality, const ResolvedAvif& settings, int keep) {
    std::string out;
    auto* heif = static_cast<AvifSave*>(g_object_new(tirage_avif_save_get_type(), nullptr));
    heif->Q = quality;
    heif->bitdepth = settings.bit_depth;
    heif->effort = settings.effort;
    heif->chroma = settings.chroma;
    heif->tune = settings.tune;
    heif->out = &out;

    VipsObject* object = VIPS_OBJECT(heif);
    const bool ok = !vips_object_set(object, "in", in.get_image(), "keep", keep, nullptr) && !vips_object_build(object);
    g_object_unref(heif);
    if (!ok) throw vips::VError();
    return out;
}

}  // namespace tirage::worker
