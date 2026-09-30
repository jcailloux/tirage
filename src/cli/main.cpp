// tirage: the command line client (plan § 5).
//
//   tirage encode --profile <profile.json> [--variants a,b] [--crop x,y,w,h --crop-unit px|permille]
//                 [--priority interactive|background] <output-dir> <input-file>
//   tirage probe  --profile <profile.json> [--crop x,y,w,h --crop-unit px|permille] <input-file>
//   tirage --version
//
// Prints one JSON object on the standard output. A refusal is that object with
// a non-empty "error" and exit code 0. A failure (worker missing or killed,
// file unreadable, bad arguments) is a message on the standard error and a
// non-zero exit code. encode writes <variant>-<width>.<ext> into <output-dir>,
// and nowhere else.
//
// Only direct mode for now (TIRAGE_DIRECT=1): the CLI launches tirage-worker
// itself. It goes through tiraged with libtirage-client, in phase 3.

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "tirage/direct.h"
#include "tirage/protocol.h"
#include "tirage/request.h"
#include "tirage/validate.h"

// What the CLI prints: the response, with the files written instead of their
// bytes. Not in the anonymous namespace: glaze reflection needs linkage.
namespace tirage::cli {

struct WrittenOutput {
    std::string variant;
    int width = 0;
    int height = 0;
    tirage::OutputFormat format = tirage::OutputFormat::avif;
    std::size_t size = 0;
    std::string path;
};

struct Report {
    std::string error;
    tirage::Code code = tirage::Code::ok;
    std::vector<std::string> warnings;
    std::optional<tirage::Source> source;
    std::optional<tirage::Rect> crop;
    std::vector<WrittenOutput> outputs;
};

}  // namespace tirage::cli

namespace {

using tirage::cli::Report;

enum Exit { kOk = 0, kFailure = 1, kUsage = 2 };

struct Args {
    std::string command;
    std::string profile;
    std::optional<std::string> variants;
    std::optional<std::string> crop;
    std::optional<std::string> crop_unit;
    std::optional<std::string> priority;
    std::vector<std::string> positional;
};

int usage(const char* why) {
    std::fprintf(stderr,
                 "tirage: %s\n"
                 "usage: tirage encode --profile <profile.json> [--variants a,b] "
                 "[--crop x,y,w,h --crop-unit px|permille] [--priority interactive|background] "
                 "<output-dir> <input-file>\n"
                 "       tirage probe --profile <profile.json> [--crop x,y,w,h --crop-unit px|permille] "
                 "<input-file>\n"
                 "       tirage --version\n",
                 why);
    return kUsage;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "tirage: %s\n", message.c_str());
    return kFailure;
}

std::optional<std::string> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f), {});
}

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    while (true) {
        const auto comma = s.find(',');
        out.emplace_back(s.substr(0, comma));
        if (comma == std::string_view::npos) return out;
        s.remove_prefix(comma + 1);
    }
}

std::optional<tirage::Crop> parse_crop(const std::string& value, const std::string& unit) {
    const auto parts = split(value);
    if (parts.size() != 4) return std::nullopt;
    int v[4];
    for (int i = 0; i < 4; ++i) {
        const auto& p = parts[i];
        const auto [end, ec] = std::from_chars(p.data(), p.data() + p.size(), v[i]);
        if (ec != std::errc{} || end != p.data() + p.size()) return std::nullopt;
    }
    tirage::Crop crop{.x = v[0], .y = v[1], .width = v[2], .height = v[3]};
    if (unit == "px") crop.unit = tirage::CropUnit::px;
    else if (unit == "permille") crop.unit = tirage::CropUnit::permille;
    else return std::nullopt;
    return crop;
}

std::string_view extension(tirage::OutputFormat f) {
    switch (f) {
        case tirage::OutputFormat::avif: return "avif";
        case tirage::OutputFormat::webp: return "webp";
        case tirage::OutputFormat::jpeg: return "jpg";
    }
    return "bin";
}

int print(const Report& report) {
    std::string json;
    if (glz::write_json(report, json)) return fail("report not serialisable");
    std::printf("%s\n", json.c_str());
    return kOk;
}

int print_refusal(tirage::Refusal r) {
    return print(Report{.error = std::move(r.message), .code = r.code});
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::printf("tirage %s (protocol %d, profile %d)\n", TIRAGE_VERSION, tirage::kProtocolVersion,
                    tirage::kProfileVersion);
        return kOk;
    }
    if (argc < 2) return usage("missing command");
    args.command = argv[1];
    if (args.command != "encode" && args.command != "probe") return usage("unknown command");

    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--profile" && (v = value())) args.profile = v;
        else if (a == "--variants" && (v = value())) args.variants = v;
        else if (a == "--crop" && (v = value())) args.crop = v;
        else if (a == "--crop-unit" && (v = value())) args.crop_unit = v;
        else if (a == "--priority" && (v = value())) args.priority = v;
        else if (a.starts_with("--")) return usage("unknown option or missing value");
        else args.positional.emplace_back(a);
    }

    const bool encode = args.command == "encode";
    if (args.positional.size() != (encode ? 2u : 1u)) return usage("wrong number of arguments");
    if (args.profile.empty()) return usage("--profile is required");
    if (args.crop.has_value() != args.crop_unit.has_value())
        return usage("--crop and --crop-unit go together");
    if (!encode && (args.variants || args.priority)) return usage("probe takes no --variants nor --priority");

    const char* direct = std::getenv("TIRAGE_DIRECT");
    if (!direct || std::string_view(direct) != "1")
        return fail("the CLI does not talk to tiraged yet: set TIRAGE_DIRECT=1 to run the worker directly");

    tirage::Request request{.protocol = tirage::kProtocolVersion,
                            .operation = encode ? tirage::Operation::encode : tirage::Operation::probe};
    if (args.priority) {
        if (*args.priority == "interactive") request.priority = tirage::Priority::interactive;
        else if (*args.priority == "background") request.priority = tirage::Priority::background;
        else return usage("--priority is interactive or background");
    }
    if (args.crop) {
        request.crop = parse_crop(*args.crop, *args.crop_unit);
        if (!request.crop) return usage("--crop is x,y,w,h in integers, --crop-unit px or permille");
    }

    const auto profile_json = slurp(args.profile);
    if (!profile_json) return fail("cannot read the profile " + args.profile);
    // Direct mode holds the profile to the daemon's compiled bounds (plan § 3).
    auto profile = tirage::parse_profile(*profile_json, tirage::kDefaultBounds);
    if (!profile) {
        const auto& e = profile.error();
        return print_refusal({tirage::Code::invalid_profile,
                              e.path.empty() ? e.message : "profile." + e.path + ": " + e.message});
    }
    request.profile = std::move(*profile);

    if (encode) {
        if (args.variants) request.variants = split(*args.variants);
        else
            for (const auto& [name, _] : request.profile.variants) request.variants.push_back(name);
    }

    const std::string& input_path = args.positional.back();
    auto input = slurp(input_path);
    if (!input) return fail("cannot read the input " + input_path);
    request.input = std::move(*input);

    // Checked here too, so that a bad request never costs a worker launch.
    if (auto r = tirage::validate_request(request, tirage::kDefaultBounds))
        return print_refusal(std::move(*r));

    const tirage::Job job{.request = std::move(request),
                          .bounds = tirage::kDefaultBounds,
                          .threads = tirage::direct::threads_from_env()};
    auto response = tirage::direct::run(job);
    if (!response) return fail(response.error());

    Report report{.error = std::move(response->error),
                  .code = response->code,
                  .warnings = std::move(response->warnings),
                  .source = response->source,
                  .crop = response->crop};
    if (encode && report.error.empty()) {
        const std::filesystem::path dir = args.positional.front();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return fail("cannot create " + dir.string() + ": " + ec.message());
        for (const tirage::Output& o : response->outputs) {
            const auto path = dir / std::format("{}-{}.{}", o.variant, o.width, extension(o.format));
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f.write(o.bytes.data(), static_cast<std::streamsize>(o.bytes.size()));
            if (!f) return fail("cannot write " + path.string());
            report.outputs.push_back({.variant = o.variant,
                                      .width = o.width,
                                      .height = o.height,
                                      .format = o.format,
                                      .size = o.bytes.size(),
                                      .path = path.string()});
        }
    }
    return print(report);
}
