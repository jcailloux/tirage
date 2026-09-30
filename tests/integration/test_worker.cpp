// Integration harness: the real tirage-worker, launched through direct mode
// (tirage/direct.h), on images built here with libvips. Every output is decoded
// again to check what a caller would get (plan § 9).
//
// Fixtures are made in code rather than stored: each one says what it is and
// why it is there, and none needs a binary in the repository.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <vips/vips8>

#include "tirage/direct.h"
#include "tirage/protocol.h"

using namespace tirage;
using vips::VImage;

int main(int argc, char** argv) {
    if (VIPS_INIT(argv[0]) != 0) return 1;
    doctest::Context context(argc, argv);
    return context.run();
}

namespace {

// ---------------------------------------------------------------------------
// Building fixtures

VImage solid(int width, int height, std::vector<double> rgb) {
    return (VImage::black(width, height, VImage::option()->set("bands", 3)) + rgb)
        .cast(VIPS_FORMAT_UCHAR)
        .copy(VImage::option()->set("interpretation", VIPS_INTERPRETATION_sRGB));
}

// Horizontal ramp from black to white, one band.
VImage grey_ramp(int width, int height) {
    return (VImage::xyz(width, height)[0] * (255.0 / width))
        .cast(VIPS_FORMAT_UCHAR)
        .copy(VImage::option()->set("interpretation", VIPS_INTERPRETATION_B_W));
}

// Alpha band: 255 on the right half, 0 on the left.
VImage right_half_alpha(int width, int height) {
    return (VImage::xyz(width, height)[0] >= width / 2).cast(VIPS_FORMAT_UCHAR);
}

std::string save(const VImage& img, const char* suffix, vips::VOption* options = nullptr) {
    void* buf = nullptr;
    std::size_t size = 0;
    img.write_to_buffer(suffix, &buf, &size, options);
    std::string out(static_cast<const char*>(buf), size);
    g_free(buf);
    return out;
}

VImage decode(const std::string& bytes) {
    // copy_memory: new_from_buffer borrows the bytes, the image must not.
    return VImage::new_from_buffer(bytes.data(), bytes.size(), "").copy_memory();
}

bool has_icc(const VImage& img) { return img.get_typeof(VIPS_META_ICC_NAME) != 0; }
bool has_exif(const VImage& img) { return img.get_typeof(VIPS_META_EXIF_NAME) != 0; }

// Every channel of the pixel within `tolerance` of `expected`.
bool near(const VImage& img, int x, int y, std::vector<double> expected, double tolerance) {
    const std::vector<double> px = img.getpoint(x, y);
    if (px.size() < expected.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i)
        if (std::abs(px[i] - expected[i]) > tolerance) return false;
    return true;
}

std::string pixel_text(const VImage& img, int x, int y) {
    std::string s;
    for (double v : img.getpoint(x, y)) s += std::to_string(static_cast<int>(v)) + " ";
    return s;
}

// PNG chunks by hand, for the one image libvips must never build: a bomb.
std::uint32_t crc32(std::string_view data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : data) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string be32(std::uint32_t v) {
    return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8),
            static_cast<char>(v)};
}

std::string png_chunk(std::string_view type, std::string_view data) {
    std::string body = std::string(type) + std::string(data);
    return be32(static_cast<std::uint32_t>(data.size())) + body + be32(crc32(body));
}

// ---------------------------------------------------------------------------
// Profiles and runs

Variant variant(std::vector<int> widths, std::vector<OutputFormat> formats, int quality = 90) {
    QualityStep step;
    for (OutputFormat f : formats) {
        if (f == OutputFormat::avif) step.avif = quality;
        if (f == OutputFormat::webp) step.webp = quality;
        if (f == OutputFormat::jpeg) step.jpeg = quality;
    }
    return Variant{.widths = std::move(widths), .formats = std::move(formats), .quality = {step}};
}

constexpr std::initializer_list<OutputFormat> kAll = {OutputFormat::avif, OutputFormat::webp,
                                                      OutputFormat::jpeg};

Profile profile(Variant v) {
    Profile p;
    p.version = kProfileVersion;
    p.input.formats = {InputFormat::png, InputFormat::jpeg, InputFormat::webp, InputFormat::heic};
    p.variants["v"] = std::move(v);
    return p;
}

Response run(const Profile& p, std::string input, Operation op = Operation::encode,
             std::optional<Crop> crop = std::nullopt) {
    Job job;
    job.request = Request{.protocol = kProtocolVersion,
                          .operation = op,
                          .profile = p,
                          .crop = crop,
                          .input = std::move(input)};
    if (op == Operation::encode)
        for (const auto& [name, _] : p.variants) job.request.variants.push_back(name);
    job.threads = 2;
    const auto r = direct::run(job, TIRAGE_WORKER_PATH);
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error()));
    return *r;
}

Response encoded(const Profile& p, std::string input) {
    Response r = run(p, std::move(input));
    REQUIRE_MESSAGE(r.error.empty(), r.error);
    return r;
}

const Output& output(const Response& r, OutputFormat f, int width = 0) {
    for (const Output& o : r.outputs)
        if (o.format == f && (width == 0 || o.width == width)) return o;
    FAIL("no such output");
    return r.outputs.front();
}

}  // namespace

// ---------------------------------------------------------------------------

TEST_CASE("widths are capped to the source, in every requested format") {
    const Response r = encoded(profile(variant({400, 800, 1600}, kAll)),
                               save(solid(1000, 500, {10, 120, 200}), ".png"));
    REQUIRE(r.outputs.size() == 9);
    CHECK(r.source->width == 1000);
    CHECK(r.source->format == InputFormat::png);
    for (int w : {400, 800, 1000}) {
        for (OutputFormat f : kAll) {
            const Output& o = output(r, f, w);
            const VImage img = decode(o.bytes);
            CHECK(img.width() == w);
            CHECK(img.height() == w / 2);
            CHECK(o.height == w / 2);
        }
    }
    CHECK(decode(output(r, OutputFormat::avif).bytes).get_string("vips-loader") ==
          std::string("heifload_buffer"));
    CHECK(decode(output(r, OutputFormat::jpeg).bytes).get_string("vips-loader") ==
          std::string("jpegload_buffer"));
}

TEST_CASE("RGBA: alpha kept in AVIF and WebP, resized premultiplied, flattened in JPEG") {
    // Red, transparent on the left half, and the transparent pixels are black:
    // resizing straight alpha would bleed that black into the red edge.
    const VImage red = solid(800, 400, {255, 0, 0});
    const VImage transparent_black = (red * right_half_alpha(800, 400) / 255).cast(VIPS_FORMAT_UCHAR);
    const VImage rgba = transparent_black.bandjoin(right_half_alpha(800, 400))
                            .copy(VImage::option()->set("interpretation", VIPS_INTERPRETATION_sRGB));

    Variant v = variant({333}, kAll, 95);
    v.encoding = Encoding{.jpeg = JpegEncoding{.background = std::array{0, 0, 255}}};
    const Response r = encoded(profile(v), save(rgba, ".png"));

    for (OutputFormat f : {OutputFormat::avif, OutputFormat::webp}) {
        const VImage img = decode(output(r, f).bytes);
        CHECK(img.bands() == 4);
        // Every partly transparent pixel of the edge is still red. Resized
        // straight, their red would drop to about their alpha.
        int edge = 0;
        for (int x = 100; x < 230; ++x) {
            const auto px = img.getpoint(x, 75);
            if (px[3] > 5 && px[3] < 250) {
                ++edge;
                CHECK_MESSAGE(px[0] > 200, "darkened edge at x=", x, ": ", pixel_text(img, x, 75));
            }
        }
        CHECK(edge > 0);
    }

    const VImage jpeg = decode(output(r, OutputFormat::jpeg).bytes);
    CHECK(jpeg.bands() == 3);
    CHECK_MESSAGE(near(jpeg, 20, 75, {0, 0, 255}, 12), pixel_text(jpeg, 20, 75));
    CHECK_MESSAGE(near(jpeg, 310, 75, {255, 0, 0}, 12), pixel_text(jpeg, 310, 75));
}

TEST_CASE("EXIF orientation is applied, then EXIF is stripped or kept") {
    // Stored landscape, to be shown portrait (orientation 6: rotate 90° clockwise).
    VImage landscape = solid(600, 400, {90, 90, 90}).copy();
    landscape.set("orientation", 6);
    const std::string jpeg = save(landscape, ".jpg");
    REQUIRE(decode(jpeg).get_int("orientation") == 6);

    Profile p = profile(variant({300}, {OutputFormat::webp}));
    Response r = encoded(p, jpeg);
    CHECK(r.source->width == 400);
    CHECK(r.source->height == 600);
    const VImage stripped = decode(r.outputs.at(0).bytes);
    CHECK(stripped.width() == 300);
    CHECK(stripped.height() == 450);
    CHECK_FALSE(has_exif(stripped));

    p.metadata = Metadata::keep;
    r = encoded(p, jpeg);
    const VImage kept = decode(r.outputs.at(0).bytes);
    CHECK(kept.height() == 450);
    REQUIRE(has_exif(kept));
    // The pixels are upright now: an orientation left in the EXIF would have
    // browsers rotate them a second time.
    CHECK((kept.get_typeof("orientation") == 0 || kept.get_int("orientation") == 1));
}

TEST_CASE("CMYK JPEG is converted to sRGB, never taken for RGBA") {
    const VImage cmyk = solid(64, 64, {220, 40, 40}).icc_transform("cmyk");
    REQUIRE(cmyk.interpretation() == VIPS_INTERPRETATION_CMYK);
    const std::string with_profile = save(cmyk, ".jpg");
    const std::string without_profile =
        save(cmyk, ".jpg", VImage::option()->set("keep", VIPS_FOREIGN_KEEP_NONE));
    REQUIRE(decode(with_profile).bands() == 4);
    REQUIRE(has_icc(decode(with_profile)));
    REQUIRE_FALSE(has_icc(decode(without_profile)));

    for (const std::string* input : {&with_profile, &without_profile}) {
        for (Color color : {Color::srgb, Color::icc}) {
            CAPTURE(input == &with_profile);
            CAPTURE(static_cast<int>(color));
            Profile p = profile(variant({64}, kAll));
            p.color = color;
            const Response r = encoded(p, *input);
            for (OutputFormat f : kAll) {
                const VImage img = decode(output(r, f).bytes);
                CHECK(img.bands() == 3);
                CHECK(img.interpretation() == VIPS_INTERPRETATION_sRGB);
                // CMYK has a smaller gamut: the red comes back close, not exact.
                CHECK_MESSAGE(near(img, 32, 32, {220, 40, 40}, 30), pixel_text(img, 32, 32));
                CHECK(has_icc(img) == (color == Color::icc));
            }
        }
    }
}

TEST_CASE("16-bit PNG in Display P3: converted to 8-bit sRGB, or kept as is with icc") {
    const std::vector<double> orange{200, 60, 30};
    const VImage p3 = solid(64, 64, orange).icc_transform("p3", VImage::option()->set("depth", 16));
    const std::string png = save(p3, ".png", VImage::option()->set("bitdepth", 16));
    REQUIRE(decode(png).format() == VIPS_FORMAT_USHORT);
    REQUIRE(has_icc(decode(png)));

    Profile p = profile(variant({64}, kAll));
    p.variants["deep"] = variant({64}, {OutputFormat::avif});
    p.variants["deep"].encoding = Encoding{.avif = AvifEncoding{.bit_depth = 10}};

    const Response srgb = encoded(p, png);
    for (const Output& o : srgb.outputs) {
        CAPTURE(o.variant);
        CAPTURE(static_cast<int>(o.format));
        const VImage img = decode(o.bytes);
        CHECK_FALSE(has_icc(img));
        if (o.variant == "deep") {
            // A 10-bit AVIF decodes as 16 bits: the source's precision went through.
            REQUIRE(img.format() == VIPS_FORMAT_USHORT);
            CHECK_MESSAGE(near(img / 257, 32, 32, orange, 6), pixel_text(img / 257, 32, 32));
        } else {
            CHECK(img.format() == VIPS_FORMAT_UCHAR);
            CHECK_MESSAGE(near(img, 32, 32, orange, 6), pixel_text(img, 32, 32));
        }
    }

    // icc: pixels untouched, profile kept. The P3 values are not the sRGB ones.
    p.color = Color::icc;
    const Response icc = encoded(p, png);
    const VImage webp = decode(output(icc, OutputFormat::webp).bytes);
    CHECK(has_icc(webp));
    CHECK_FALSE(near(webp, 32, 32, orange, 6));
    CHECK(near(webp.icc_transform("srgb"), 32, 32, orange, 6));
}

TEST_CASE("greyscale goes through 4:2:0 in every format, odd sizes included") {
    const VImage grey = grey_ramp(501, 301);
    const VImage grey_alpha = grey.bandjoin(right_half_alpha(501, 301));
    const VImage grey16 = (grey.cast(VIPS_FORMAT_USHORT) * 257)
                              .cast(VIPS_FORMAT_USHORT)
                              .copy(VImage::option()->set("interpretation", VIPS_INTERPRETATION_GREY16));

    Variant v = variant({333, 501}, kAll);
    v.encoding = Encoding{.avif = AvifEncoding{.chroma = Chroma::s420},
                          .jpeg = JpegEncoding{.chroma = Chroma::s420}};
    Profile p = profile(v);
    p.variants["deep"] = variant({333}, {OutputFormat::avif});
    p.variants["deep"].encoding = Encoding{.avif = AvifEncoding{.chroma = Chroma::s420, .bit_depth = 10}};

    for (const VImage* img : {&grey, &grey_alpha, &grey16}) {
        CAPTURE(img->bands());
        CAPTURE(img->format());
        const Response r = encoded(p, save(*img, ".png"));
        CHECK(r.outputs.size() == 7);
        for (const Output& o : r.outputs) {
            const VImage out = decode(o.bytes);
            CHECK(out.width() == o.width);
            // The ramp survives: light on the right, and dark on the left
            // where it is opaque.
            const double scale = out.format() == VIPS_FORMAT_USHORT ? 257 : 1;
            CHECK(out.getpoint(o.width - 1, 0)[0] / scale > 180);
            if (img != &grey_alpha) CHECK(out.getpoint(0, 0)[0] / scale < 60);
        }
    }
}

TEST_CASE("the floor refuses or warns, on the crop when there is one") {
    Profile p = profile(variant({400}, {OutputFormat::webp}));
    p.input.min_size = MinSize{.width = 1280, .height = 720};
    const std::string small = save(solid(800, 600, {0, 0, 0}), ".png");

    Response r = run(p, small);
    CHECK(r.code == Code::image_too_small);
    CHECK(r.outputs.empty());
    CHECK(r.source->width == 800);

    p.input.min_size->policy = FloorPolicy::warn;
    r = encoded(p, small);
    CHECK(r.outputs.size() == 1);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0] == "below min_size: 800x600 under 1280x720");

    // A large enough image, cropped under the floor.
    p.input.min_size->policy = FloorPolicy::reject;
    r = run(p, save(solid(2000, 1000, {0, 0, 0}), ".png"), Operation::encode,
            Crop{.x = 0, .y = 0, .width = 500, .height = 500, .unit = CropUnit::permille});
    CHECK(r.code == Code::image_too_small);
    CHECK(r.crop->width == 1000);
}

TEST_CASE("a crop in permille, applied to the oriented image") {
    const Response r = run(profile(variant({400, 800}, {OutputFormat::jpeg})),
                           save(solid(1000, 500, {0, 0, 0}), ".png"), Operation::encode,
                           Crop{.x = 250, .y = 0, .width = 500, .height = 1000, .unit = CropUnit::permille});
    REQUIRE(r.error.empty());
    CHECK((r.crop->x == 250 && r.crop->y == 0 && r.crop->width == 500 && r.crop->height == 500));
    REQUIRE(r.outputs.size() == 2);
    CHECK(r.outputs[0].width == 400);
    CHECK(r.outputs[1].width == 500);  // 800 capped to the crop
    CHECK(decode(r.outputs[1].bytes).height() == 500);
}

TEST_CASE("probe answers without encoding") {
    Profile p = profile(variant({400}, {OutputFormat::avif}));
    p.input.min_size = MinSize{.width = 1280, .height = 720, .policy = FloorPolicy::warn};
    const Response r = run(p, save(solid(640, 480, {0, 0, 0}), ".jpg"), Operation::probe);
    REQUIRE(r.error.empty());
    CHECK(r.outputs.empty());
    CHECK(r.source->format == InputFormat::jpeg);
    CHECK(r.source->width == 640);
    CHECK(r.source->bands == 3);
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("a decompression bomb is refused on its header, before decoding") {
    // 30000 x 30000 RGBA announced, no pixel data: a decoder that went past the
    // header would fail, the refusal proves it did not.
    std::string ihdr = be32(30000) + be32(30000);
    ihdr += std::string{8, 6, 0, 0, 0};  // 8 bits, RGBA, deflate, adaptive, not interlaced
    const std::string bomb = "\x89PNG\r\n\x1a\n" + png_chunk("IHDR", ihdr) +
                             png_chunk("IDAT", "\x78\x9c") + png_chunk("IEND", "");

    const auto start = std::chrono::steady_clock::now();
    const Response r = run(profile(variant({400}, {OutputFormat::avif})), bomb);
    CHECK(r.code == Code::image_too_large);
    CHECK_MESSAGE(r.error.find("30000x30000") != std::string::npos, r.error);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

TEST_CASE("refusals before and at decoding") {
    const Profile p = profile(variant({400}, {OutputFormat::avif}));
    CHECK(run(p, "GIF89a\x01\x00\x01\x00").code == Code::unsupported_format);

    Profile png_only = p;
    // Not `= {InputFormat::png}`: GCC 14 at -O2 sees a bogus out-of-bounds copy
    // when a one-element list replaces the four of `p` (-Warray-bounds).
    png_only.input.formats.assign(1, InputFormat::png);
    CHECK(run(png_only, save(solid(8, 8, {0, 0, 0}), ".webp")).code == Code::unsupported_format);

    Profile tight = p;
    tight.input.limits = Limits{.max_bytes = 100};
    CHECK(run(tight, save(grey_ramp(300, 300), ".png")).code == Code::file_too_large);

    std::string corrupt = save(grey_ramp(300, 300), ".png");
    corrupt.resize(60);
    const Response r = run(p, corrupt);
    CHECK(r.code == Code::decode_failed);
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("HEIC from a phone: decoded, EXIF stripped") {
    VImage photo = solid(640, 480, {30, 140, 60}).copy();
    photo.set("exif-ifd0-Make", "tirage test");
    std::string heic;
    try {
        heic = save(photo, ".heic", VImage::option()->set("compression", VIPS_FOREIGN_HEIF_COMPRESSION_HEVC));
    } catch (const vips::VError&) {
        MESSAGE("no HEVC encoder in this libheif, HEIC test skipped");
        return;
    }
    REQUIRE(has_exif(decode(heic)));
    const Response r = encoded(profile(variant({320}, kAll)), heic);
    CHECK(r.source->format == InputFormat::heic);
    for (OutputFormat f : kAll) {
        const VImage img = decode(output(r, f).bytes);
        CHECK(img.width() == 320);
        CHECK_FALSE(has_exif(img));
        CHECK(near(img, 100, 100, {30, 140, 60}, 12));
    }
}
