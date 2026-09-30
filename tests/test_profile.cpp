// Profile reading and validation (plan § 3 and § 6).
//
// Every refusal here guards the rule "no setting is silently ignored": a caller
// who writes a setting that would do nothing must hear about it at validation,
// not discover it later in the output.

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "tirage/resolve.h"
#include "tirage/validate.h"

using namespace tirage;

namespace {

// The plan's example profile (§ 3), without its comments.
constexpr std::string_view kExample = R"({
  "version": 1,
  "input": {
    "formats": ["png", "jpeg", "webp", "heic"],
    "min_size": { "width": 1280, "height": 720, "policy": "reject" },
    "limits": { "max_bytes": 26214400, "max_edge": 10000, "max_pixels": 50000000 }
  },
  "color": "srgb",
  "metadata": "strip",
  "encoding": {
    "avif": { "effort": 4, "chroma": "420", "bit_depth": 8 },
    "webp": { "effort": 4, "sharp_yuv": false }
  },
  "variants": {
    "tiers": {
      "widths": [400, 800, 1600],
      "formats": ["avif", "webp"],
      "quality": [ { "max_width": 800, "avif": 50, "webp": 80 }, { "avif": 80, "webp": 85 } ]
    },
    "master": {
      "widths": [2560],
      "formats": ["avif"],
      "quality": [ { "avif": 80 } ],
      "encoding": { "avif": { "bit_depth": 10 } }
    }
  }
})";

// Smallest valid profile, to which each test adds the one thing it checks.
Profile minimal() {
    Profile p;
    p.version = kProfileVersion;
    p.input.formats = {InputFormat::png};
    p.variants["v"] = Variant{
        .widths = {400},
        .formats = {OutputFormat::avif},
        .quality = {QualityStep{.avif = 60}},
    };
    return p;
}

std::string error_path(const Profile& p, const Bounds& b = kDefaultBounds) {
    const auto e = validate(p, b);
    REQUIRE(e.has_value());
    return e->path;
}

// Parses `json` and returns the error, which must come from reading, not from
// validation (validation errors always carry a path).
ProfileError parse_error(std::string_view json) {
    const auto r = parse_profile(json);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().path.empty());
    return r.error();
}

}  // namespace

TEST_CASE("the plan's example profile is valid") {
    const auto r = parse_profile(kExample);
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().path + ": " + r.error().message));
    const Profile& p = *r;
    CHECK(p.variants.size() == 2);
    CHECK(p.input.min_size->policy == FloorPolicy::reject);
    CHECK(p.encoding->avif->chroma == Chroma::s420);
    CHECK(p.variants.at("master").encoding->avif->bit_depth == 10);
}

TEST_CASE("the minimal profile is valid and a profile round-trips through JSON") {
    const Profile p = minimal();
    CHECK_FALSE(validate(p).has_value());

    std::string json;
    REQUIRE_FALSE(glz::write_json(p, json));
    const auto back = parse_profile(json);
    REQUIRE(back.has_value());
    CHECK(back->variants.at("v").quality[0].avif == 60);
}

TEST_CASE("defaults: color srgb, metadata strip, no floor, no limits") {
    const auto r = parse_profile(R"({"version":1,"input":{"formats":["png"]},
        "variants":{"v":{"widths":[400],"formats":["webp"],"quality":[{"webp":80}]}}})");
    REQUIRE(r.has_value());
    CHECK(r->color == Color::srgb);
    CHECK(r->metadata == Metadata::strip);
    CHECK_FALSE(r->input.min_size.has_value());
    CHECK_FALSE(r->input.limits.has_value());
}

TEST_CASE("unknown keys are refused, whatever their depth") {
    SUBCASE("top level") {
        parse_error(R"({"version":1,"colour":"srgb"})");
    }
    SUBCASE("chroma does not exist in WebP") {
        const auto e = parse_error(R"({"version":1,"input":{"formats":["png"]},
            "variants":{"v":{"widths":[400],"formats":["webp"],"quality":[{"webp":80}],
            "encoding":{"webp":{"chroma":"444"}}}}})");
        CHECK(e.message.find("chroma") != std::string::npos);
    }
    SUBCASE("effort does not exist in JPEG") {
        parse_error(R"({"version":1,"input":{"formats":["png"]},
            "variants":{"v":{"widths":[400],"formats":["jpeg"],"quality":[{"jpeg":80}],
            "encoding":{"jpeg":{"effort":4}}}}})");
    }
    SUBCASE("alpha is no longer an option") {
        parse_error(R"({"version":1,"input":{"formats":["png"],"alpha":"ignore"}})");
    }
}

TEST_CASE("chroma has no auto") {
    parse_error(R"({"version":1,"input":{"formats":["png"]},
        "encoding":{"avif":{"chroma":"auto"}},
        "variants":{"v":{"widths":[400],"formats":["avif"],"quality":[{"avif":60}]}}})");
}

TEST_CASE("version") {
    Profile p = minimal();
    p.version = 2;
    CHECK(error_path(p) == "version");
}

TEST_CASE("input formats") {
    Profile p = minimal();
    SUBCASE("empty") {
        p.input.formats.clear();
        CHECK(error_path(p) == "input.formats");
    }
    SUBCASE("duplicate") {
        p.input.formats = {InputFormat::png, InputFormat::png};
        CHECK(error_path(p) == "input.formats");
    }
}

TEST_CASE("min_size") {
    Profile p = minimal();
    SUBCASE("zero width") {
        p.input.min_size = MinSize{.width = 0, .height = 720};
        CHECK(error_path(p) == "input.min_size.width");
    }
    SUBCASE("floor above the edge bound refuses every image") {
        p.input.min_size = MinSize{.width = 1280, .height = 720};
        p.input.limits = Limits{.max_edge = 1000};
        CHECK(error_path(p) == "input.min_size");
    }
    SUBCASE("policy warn") {
        p.input.min_size = MinSize{.width = 1280, .height = 720, .policy = FloorPolicy::warn};
        CHECK_FALSE(validate(p).has_value());
    }
}

TEST_CASE("limits only tighten the daemon's bounds") {
    Profile p = minimal();
    const Bounds daemon{.max_bytes = 100, .max_edge = 100, .max_pixels = 100};

    SUBCASE("each field is optional") {
        p.input.limits = Limits{.max_edge = 50};
        CHECK_FALSE(validate(p, daemon).has_value());
        const Bounds b = effective_bounds(p, daemon);
        CHECK(b.max_edge == 50);
        CHECK(b.max_bytes == 100);
        CHECK(b.max_pixels == 100);
    }
    SUBCASE("equal to the bound is allowed") {
        p.input.limits = Limits{.max_bytes = 100, .max_edge = 100, .max_pixels = 100};
        CHECK_FALSE(validate(p, daemon).has_value());
    }
    SUBCASE("above the bound is refused") {
        p.input.limits = Limits{.max_bytes = 101};
        CHECK(error_path(p, daemon) == "input.limits.max_bytes");
        p.input.limits = Limits{.max_edge = 101};
        CHECK(error_path(p, daemon) == "input.limits.max_edge");
        p.input.limits = Limits{.max_pixels = 101};
        CHECK(error_path(p, daemon) == "input.limits.max_pixels");
    }
    SUBCASE("zero is refused") {
        p.input.limits = Limits{.max_bytes = 0};
        CHECK(error_path(p, daemon) == "input.limits.max_bytes");
    }
    SUBCASE("codiga's 25 MiB fits the compiled defaults") {
        p.input.limits = Limits{.max_bytes = 25 * 1024 * 1024, .max_edge = 10000,
                                .max_pixels = 50'000'000};
        CHECK_FALSE(validate(p).has_value());
    }
}

TEST_CASE("variants") {
    Profile p = minimal();
    SUBCASE("at least one") {
        p.variants.clear();
        CHECK(error_path(p) == "variants");
    }
    SUBCASE("names are tame") {
        p.variants["Tiers"] = p.variants.at("v");
        CHECK(error_path(p) == "variants.Tiers");
        p.variants.erase("Tiers");
        p.variants["../x"] = p.variants.at("v");
        CHECK(error_path(p) == "variants.../x");
    }
    SUBCASE("widths") {
        p.variants.at("v").widths = {};
        CHECK(error_path(p) == "variants.v.widths");
        p.variants.at("v").widths = {0};
        CHECK(error_path(p) == "variants.v.widths");
        p.variants.at("v").widths = {400, 400};
        CHECK(error_path(p) == "variants.v.widths");
    }
    SUBCASE("formats") {
        p.variants.at("v").formats = {};
        CHECK(error_path(p) == "variants.v.formats");
        p.variants.at("v").formats = {OutputFormat::avif, OutputFormat::avif};
        CHECK(error_path(p) == "variants.v.formats");
    }
}

TEST_CASE("quality steps") {
    Profile p = minimal();
    Variant& v = p.variants.at("v");
    v.formats = {OutputFormat::avif, OutputFormat::webp};

    SUBCASE("every listed format in every step") {
        v.quality = {QualityStep{.max_width = 800, .avif = 50, .webp = 80},
                     QualityStep{.avif = 80}};
        CHECK(error_path(p) == "variants.v.quality[1].webp");
    }
    SUBCASE("a quality for a format the variant does not produce is refused") {
        v.quality = {QualityStep{.avif = 50, .webp = 80, .jpeg = 85}};
        CHECK(error_path(p) == "variants.v.quality[0].jpeg");
    }
    SUBCASE("range 1 to 100") {
        v.quality = {QualityStep{.avif = 0, .webp = 80}};
        CHECK(error_path(p) == "variants.v.quality[0].avif");
        v.quality = {QualityStep{.avif = 50, .webp = 101}};
        CHECK(error_path(p) == "variants.v.quality[0].webp");
    }
    SUBCASE("only the last step has no max_width") {
        v.quality = {QualityStep{.avif = 50, .webp = 80}, QualityStep{.avif = 80, .webp = 85}};
        CHECK(error_path(p) == "variants.v.quality[0].max_width");
        v.quality = {QualityStep{.max_width = 800, .avif = 50, .webp = 80}};
        CHECK(error_path(p) == "variants.v.quality[0].max_width");
    }
    SUBCASE("max_width strictly increasing") {
        v.quality = {QualityStep{.max_width = 800, .avif = 50, .webp = 80},
                     QualityStep{.max_width = 800, .avif = 60, .webp = 82},
                     QualityStep{.avif = 80, .webp = 85}};
        CHECK(error_path(p) == "variants.v.quality[1].max_width");
    }
    SUBCASE("empty") {
        v.quality.clear();
        CHECK(error_path(p) == "variants.v.quality");
    }
}

TEST_CASE("effort range depends on the format") {
    Profile p = minimal();
    Variant& v = p.variants.at("v");
    v.formats = {OutputFormat::avif, OutputFormat::webp};
    v.quality = {QualityStep{.avif = 50, .webp = 80}};

    SUBCASE("AVIF 0 to 9") {
        p.encoding = Encoding{.avif = AvifEncoding{.effort = 9}};
        CHECK_FALSE(validate(p).has_value());
        p.encoding = Encoding{.avif = AvifEncoding{.effort = 10}};
        CHECK(error_path(p) == "encoding.avif.effort");
        p.encoding = Encoding{.avif = AvifEncoding{.effort = -1}};
        CHECK(error_path(p) == "encoding.avif.effort");
    }
    SUBCASE("WebP 0 to 6") {
        p.encoding = Encoding{.webp = WebpEncoding{.effort = 6}};
        CHECK_FALSE(validate(p).has_value());
        p.encoding = Encoding{.webp = WebpEncoding{.effort = 7}};
        CHECK(error_path(p) == "encoding.webp.effort");
    }
    SUBCASE("checked in a variant's override too") {
        v.encoding = Encoding{.webp = WebpEncoding{.effort = 9}};
        CHECK(error_path(p) == "variants.v.encoding.webp.effort");
    }
}

TEST_CASE("AVIF bit depth is 8 or 10") {
    Profile p = minimal();
    p.encoding = Encoding{.avif = AvifEncoding{.bit_depth = 12}};
    CHECK(error_path(p) == "encoding.avif.bit_depth");
    p.encoding = Encoding{.avif = AvifEncoding{.bit_depth = 10}};
    CHECK_FALSE(validate(p).has_value());
}

TEST_CASE("JPEG background channels are bytes") {
    Profile p = minimal();
    Variant& v = p.variants.at("v");
    v.formats = {OutputFormat::jpeg};
    v.quality = {QualityStep{.jpeg = 85}};
    p.encoding = Encoding{.jpeg = JpegEncoding{.background = std::array{0, 0, 256}}};
    CHECK(error_path(p) == "encoding.jpeg.background");
    p.encoding = Encoding{.jpeg = JpegEncoding{.background = std::array{0, 0, 0}}};
    CHECK_FALSE(validate(p).has_value());
}

TEST_CASE("an encoding block for a format nobody produces is refused") {
    Profile p = minimal();  // the only variant produces AVIF
    SUBCASE("at the profile level") {
        p.encoding = Encoding{.webp = WebpEncoding{.effort = 4}};
        CHECK(error_path(p) == "encoding.webp");
    }
    SUBCASE("at the variant level") {
        p.variants.at("v").encoding = Encoding{.jpeg = JpegEncoding{.progressive = true}};
        CHECK(error_path(p) == "variants.v.encoding.jpeg");
    }
    SUBCASE("a profile block used by one variant only is fine") {
        p.variants["w"] = Variant{.widths = {400},
                                  .formats = {OutputFormat::webp},
                                  .quality = {QualityStep{.webp = 80}}};
        p.encoding = Encoding{.webp = WebpEncoding{.effort = 4}};
        CHECK_FALSE(validate(p).has_value());
    }
}

TEST_CASE("encoding resolves field by field: defaults, then profile, then variant") {
    const auto p = parse_profile(kExample);
    REQUIRE(p.has_value());

    const ResolvedEncoding tiers = resolve_encoding(*p, p->variants.at("tiers"));
    CHECK(tiers.avif.bit_depth == 8);
    CHECK(tiers.avif.effort == 4);

    const ResolvedEncoding master = resolve_encoding(*p, p->variants.at("master"));
    CHECK(master.avif.bit_depth == 10);                // variant override
    CHECK(master.avif.chroma == Chroma::s420);         // kept from the profile
    CHECK(master.jpeg.background == std::array{255, 255, 255});  // tirage default
}

TEST_CASE("quality at a width") {
    const auto p = parse_profile(kExample);
    REQUIRE(p.has_value());
    const Variant& tiers = p->variants.at("tiers");
    CHECK(quality_at(tiers, 400, OutputFormat::avif) == 50);
    CHECK(quality_at(tiers, 800, OutputFormat::avif) == 50);  // max_width is inclusive
    CHECK(quality_at(tiers, 801, OutputFormat::avif) == 80);
    CHECK(quality_at(tiers, 1600, OutputFormat::webp) == 85);
    CHECK_FALSE(quality_at(tiers, 400, OutputFormat::jpeg).has_value());
}

TEST_CASE("planned widths never upscale") {
    Variant v{.widths = {1600, 400, 800}};
    CHECK(planned_widths(v, 4000) == std::vector{400, 800, 1600});
    CHECK(planned_widths(v, 1600) == std::vector{400, 800, 1600});
    CHECK(planned_widths(v, 1000) == std::vector{400, 800, 1000});  // capped, not dropped
    CHECK(planned_widths(v, 800) == std::vector{400, 800});
    CHECK(planned_widths(v, 300) == std::vector{300});  // narrower than every width
    Variant master{.widths = {2560}};
    CHECK(planned_widths(master, 1000) == std::vector{1000});  // codiga still gets its master
}
