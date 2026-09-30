// Request checks and the worker's decisions before decoding (plan § 3 and § 4).

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <variant>

#include "tirage/protocol.h"
#include "tirage/request.h"

using namespace tirage;
using namespace std::literals;

namespace {

constexpr std::string_view kPngHeader = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR"sv;

// Smallest valid encode request, to which each test adds the one thing it checks.
Request minimal() {
    Request r;
    r.protocol = kProtocolVersion;
    r.profile.version = kProfileVersion;
    r.profile.input.formats = {InputFormat::png, InputFormat::jpeg};
    r.profile.variants["v"] = Variant{
        .widths = {400},
        .formats = {OutputFormat::avif},
        .quality = {QualityStep{.avif = 60}},
    };
    r.variants = {"v"};
    r.input = std::string(kPngHeader);
    return r;
}

Refusal refusal(const Request& r) {
    const auto e = validate_request(r);
    REQUIRE(e.has_value());
    return *e;
}

// An ISO BMFF ftyp box with the given brands (major first, then compatible).
std::string ftyp(std::string_view major, std::initializer_list<std::string_view> compatible) {
    const std::size_t size = 16 + 4 * compatible.size();
    std::string box{'\0', '\0', '\0', static_cast<char>(size)};
    box += "ftyp";
    box += major;
    box += std::string(4, '\0');
    for (auto b : compatible) box += b;
    return box;
}

}  // namespace

TEST_CASE("sniff reads magic bytes") {
    CHECK(sniff(kPngHeader) == InputFormat::png);
    CHECK(sniff("\xFF\xD8\xFF\xE0") == InputFormat::jpeg);
    CHECK(sniff("RIFF\x10\0\0\0WEBPVP8 "sv) == InputFormat::webp);
    CHECK(sniff(ftyp("heic", {"mif1", "heic"})) == InputFormat::heic);
    // A generic major brand, HEVC among the compatible ones (some phones).
    CHECK(sniff(ftyp("mif1", {"mif1", "heix"})) == InputFormat::heic);

    CHECK_FALSE(sniff("").has_value());
    CHECK_FALSE(sniff("GIF89a").has_value());
    CHECK_FALSE(sniff("\x89PNG").has_value());  // truncated signature
    // AVIF is ISO BMFF too, but not an accepted input.
    CHECK_FALSE(sniff(ftyp("avif", {"mif1", "avif"})).has_value());
    // A compatible brand past the end of the box does not count.
    std::string lying = ftyp("mif1", {"mif1"});
    lying += "heic";
    CHECK_FALSE(sniff(lying).has_value());
}

TEST_CASE("a minimal request is valid") {
    CHECK_FALSE(validate_request(minimal()).has_value());
}

TEST_CASE("the protocol version must match") {
    Request r = minimal();
    r.protocol = 2;
    CHECK(refusal(r).code == Code::invalid_request);
}

TEST_CASE("an invalid profile is refused with its path") {
    Request r = minimal();
    r.profile.variants["v"].quality[0].avif = 0;
    const Refusal e = refusal(r);
    CHECK(e.code == Code::invalid_profile);
    CHECK(e.message.starts_with("profile.variants.v.quality[0].avif: "));
}

TEST_CASE("encode names existing variants, once each") {
    Request r = minimal();
    r.variants = {};
    CHECK(refusal(r).code == Code::invalid_request);
    r.variants = {"nope"};
    CHECK(refusal(r).message.find("\"nope\"") != std::string::npos);
    r.variants = {"v", "v"};
    CHECK(refusal(r).message.find("twice") != std::string::npos);
}

TEST_CASE("probe takes no variants") {
    Request r = minimal();
    r.operation = Operation::probe;
    CHECK(refusal(r).code == Code::invalid_request);
    r.variants = {};
    CHECK_FALSE(validate_request(r).has_value());
}

TEST_CASE("crop checks") {
    Request r = minimal();
    r.crop = Crop{.x = 0, .y = 0, .width = 10, .height = 10};
    CHECK(refusal(r).message.starts_with("crop.unit"));

    r.crop->unit = CropUnit::px;
    CHECK_FALSE(validate_request(r).has_value());
    r.crop->x = -1;
    CHECK(refusal(r).code == Code::invalid_request);
    r.crop->x = 0;
    r.crop->width = 0;
    CHECK(refusal(r).code == Code::invalid_request);

    // Pixels beyond the image are clamped later, permille beyond 1000 is nonsense.
    r.crop = Crop{.x = 5000, .y = 0, .width = 10, .height = 10, .unit = CropUnit::px};
    CHECK_FALSE(validate_request(r).has_value());
    r.crop->unit = CropUnit::permille;
    CHECK(refusal(r).code == Code::invalid_request);
}

TEST_CASE("an empty input is refused") {
    Request r = minimal();
    r.input.clear();
    CHECK(refusal(r).code == Code::invalid_request);
}

TEST_CASE("check_input: size, then format, then the profile's formats") {
    const Profile p = minimal().profile;
    const Bounds b{.max_bytes = 20, .max_edge = 100, .max_pixels = 1000};

    CHECK_FALSE(check_input(p, kPngHeader, b).has_value());
    CHECK(check_input(p, std::string(21, 'x'), b)->code == Code::file_too_large);
    CHECK(check_input(p, "GIF89a", b)->code == Code::unsupported_format);
    const auto webp = check_input(p, "RIFF\x10\0\0\0WEBPVP8 "sv, b);
    REQUIRE(webp.has_value());
    CHECK(webp->code == Code::unsupported_format);
    CHECK(webp->message.find("input.formats") != std::string::npos);
}

TEST_CASE("check_dimensions: edge and pixel bounds") {
    const Bounds b{.max_bytes = 1, .max_edge = 100, .max_pixels = 5000};
    CHECK_FALSE(check_dimensions(100, 50, b).has_value());
    CHECK(check_dimensions(101, 1, b)->code == Code::image_too_large);
    CHECK(check_dimensions(1, 101, b)->code == Code::image_too_large);
    CHECK(check_dimensions(100, 51, b)->code == Code::image_too_large);
}

TEST_CASE("resolve_crop in pixels clamps into the image") {
    const auto crop = [](int x, int y, int w, int h) {
        return Crop{.x = x, .y = y, .width = w, .height = h, .unit = CropUnit::px};
    };
    const Rect inside = resolve_crop(crop(10, 20, 30, 40), 100, 100);
    CHECK((inside.x == 10 && inside.y == 20 && inside.width == 30 && inside.height == 40));

    const Rect overflow = resolve_crop(crop(90, 95, 50, 50), 100, 100);
    CHECK((overflow.x == 90 && overflow.y == 95 && overflow.width == 10 && overflow.height == 5));

    // Entirely outside: one pixel in the last column, never an empty area.
    const Rect outside = resolve_crop(crop(500, 500, 10, 10), 100, 100);
    CHECK((outside.x == 99 && outside.y == 99 && outside.width == 1 && outside.height == 1));
}

TEST_CASE("resolve_crop in permille rounds to the nearest pixel") {
    const auto crop = [](int x, int y, int w, int h) {
        return Crop{.x = x, .y = y, .width = w, .height = h, .unit = CropUnit::permille};
    };
    const Rect whole = resolve_crop(crop(0, 0, 1000, 1000), 1333, 777);
    CHECK((whole.x == 0 && whole.y == 0 && whole.width == 1333 && whole.height == 777));

    const Rect half = resolve_crop(crop(250, 500, 500, 500), 2000, 1000);
    CHECK((half.x == 500 && half.y == 500 && half.width == 1000 && half.height == 500));

    // Edges are rounded, not the width: adjacent crops share their border.
    const Rect a = resolve_crop(crop(0, 0, 333, 1000), 1000, 10);
    const Rect b = resolve_crop(crop(333, 0, 333, 1000), 1000, 10);
    CHECK(a.x + a.width == b.x);

    // A crop too thin for the image still keeps one pixel.
    const Rect thin = resolve_crop(crop(0, 0, 1, 1), 100, 100);
    CHECK((thin.width == 1 && thin.height == 1));
}

TEST_CASE("check_floor: nothing, a warning or a refusal") {
    CHECK(std::holds_alternative<std::monostate>(check_floor(std::nullopt, 1, 1)));

    MinSize floor{.width = 1280, .height = 720};
    CHECK(std::holds_alternative<std::monostate>(check_floor(floor, 1280, 720)));

    const FloorVerdict refused = check_floor(floor, 1279, 2000);
    REQUIRE(std::holds_alternative<Refusal>(refused));
    CHECK(std::get<Refusal>(refused).code == Code::image_too_small);

    floor.policy = FloorPolicy::warn;
    const FloorVerdict warned = check_floor(floor, 2000, 719);
    REQUIRE(std::holds_alternative<std::string>(warned));
    CHECK(std::get<std::string>(warned) == "below min_size: 2000x719 under 1280x720");
}

TEST_CASE("a job survives BEVE, bytes included") {
    Job job{.request = minimal(), .bounds = {.max_bytes = 7, .max_edge = 8, .max_pixels = 9}, .threads = 3};
    job.request.input = std::string("\0\xFF\x89PNG\0", 7);
    job.request.crop = Crop{.x = 1, .y = 2, .width = 3, .height = 4, .unit = CropUnit::permille};

    std::string frame;
    REQUIRE_FALSE(glz::write_beve(job, frame));
    Job back;
    REQUIRE_FALSE(glz::read_beve(back, frame));
    CHECK(back.request.input == job.request.input);
    CHECK(back.request.crop->unit == CropUnit::permille);
    CHECK(back.request.profile.variants.at("v").quality[0].avif == 60);
    CHECK(back.bounds.max_pixels == 9);
    CHECK(back.threads == 3);
}

TEST_CASE("codes are written as their stable numbers") {
    Response r{.error = "no", .code = Code::image_too_small};
    const auto json = glz::write_json(r);
    REQUIRE(json.has_value());
    CHECK(json->find(R"("code":6)") != std::string::npos);
}

TEST_CASE("a refusal is never mistaken for a success") {
    // libvips sometimes fails with an empty error buffer.
    const Response r = refused({Code::decode_failed, ""});
    CHECK_FALSE(r.error.empty());
    CHECK(r.code == Code::decode_failed);
}
