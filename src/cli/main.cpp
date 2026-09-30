// tirage: the command line client (plan § 5).
//
//   tirage encode --profile <profile.json> [--variants a,b] [--crop x,y,w,h --crop-unit px|permille]
//                 [--mask x,y,w,h ... --mask-unit px|permille [--mask-style blur|pixelate|fill]]
//                 [--priority interactive|background] <output-dir> <input-file>
//   tirage probe  --profile <profile.json> [--crop x,y,w,h --crop-unit px|permille] <input-file>
//   tirage status [--json]
//   tirage --version
//
// encode and probe print one JSON object on the standard output. A refusal is
// that object with a non-empty "error" and exit code 0. Busy (the daemon did
// not run the job, try again later) is a message on the standard error and
// exit code 75 (EX_TEMPFAIL). A failure (daemon unreachable, worker missing or
// killed, file unreadable) is a message on the standard error and exit code 1,
// bad arguments exit code 2. encode writes <variant>-<width>.<ext> into
// <output-dir>, and nowhere else.
//
// The CLI talks to tiraged through libtirage-client (TIRAGE_SOCKET, else
// /run/tirage/tirage.sock). With TIRAGE_DIRECT=1 it launches tirage-worker
// itself instead.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>

#include "tirage/client.h"
#include "tirage/protocol.h"
#include "tirage/request.h"

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
    std::vector<tirage::Rect> masks;
    std::vector<WrittenOutput> outputs;
};

}  // namespace tirage::cli

namespace {

using tirage::cli::Report;

enum Exit { kOk = 0, kFailure = 1, kUsage = 2, kBusy = 75 };

struct Args {
    std::string command;
    std::string profile;
    std::optional<std::string> variants;
    std::optional<std::string> crop;
    std::optional<std::string> crop_unit;
    std::vector<std::string> masks;
    std::optional<std::string> mask_unit;
    std::optional<std::string> mask_style;
    std::optional<std::string> priority;
    std::vector<std::string> positional;
};

int usage(const char* why) {
    std::fprintf(stderr,
                 "tirage: %s\n"
                 "usage: tirage encode --profile <profile.json> [--variants a,b] "
                 "[--crop x,y,w,h --crop-unit px|permille]\n"
                 "                     [--mask x,y,w,h ... --mask-unit px|permille "
                 "[--mask-style blur|pixelate|fill]]\n"
                 "                     [--priority interactive|background] <output-dir> <input-file>\n"
                 "       tirage probe --profile <profile.json> [--crop x,y,w,h --crop-unit px|permille] "
                 "<input-file>\n"
                 "       tirage status [--json]\n"
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

// x,y,w,h in integers, for --crop and --mask.
std::optional<std::array<int, 4>> parse_area(const std::string& value) {
    const auto parts = split(value);
    if (parts.size() != 4) return std::nullopt;
    std::array<int, 4> v{};
    for (int i = 0; i < 4; ++i) {
        const auto& p = parts[i];
        const auto [end, ec] = std::from_chars(p.data(), p.data() + p.size(), v[i]);
        if (ec != std::errc{} || end != p.data() + p.size()) return std::nullopt;
    }
    return v;
}

std::optional<tirage::CropUnit> parse_unit(const std::string& unit) {
    if (unit == "px") return tirage::CropUnit::px;
    if (unit == "permille") return tirage::CropUnit::permille;
    return std::nullopt;
}

std::optional<tirage::Crop> parse_crop(const std::string& value, const std::string& unit) {
    const auto v = parse_area(value);
    const auto u = parse_unit(unit);
    if (!v || !u) return std::nullopt;
    return tirage::Crop{.x = (*v)[0], .y = (*v)[1], .width = (*v)[2], .height = (*v)[3], .unit = u};
}

std::optional<tirage::MaskStyle> parse_style(const std::string& style) {
    if (style == "blur") return tirage::MaskStyle::blur;
    if (style == "pixelate") return tirage::MaskStyle::pixelate;
    if (style == "fill") return tirage::MaskStyle::fill;
    return std::nullopt;
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

std::string_view name(tirage::Priority p) { return p == tirage::Priority::interactive ? "interactive" : "background"; }

std::string_view name(tirage::BusyReason r) {
    switch (r) {
        case tirage::BusyReason::queue_full: return "queue_full";
        case tirage::BusyReason::deadline: return "deadline";
        case tirage::BusyReason::stopping: return "stopping";
    }
    return "?";
}

std::string size(std::int64_t bytes) {
    if (bytes < (1 << 20)) return std::format("{:.1f} KiB", static_cast<double>(bytes) / (1 << 10));
    return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1 << 20));
}

std::string seconds(std::int64_t ms) { return std::format("{:.1f} s", static_cast<double>(ms) / 1000); }

// The status for a person: the budget, what runs, then each queue by caller.
void print_status(const tirage::Status& s) {
    std::printf("tiraged %s: %d threads per encode, %d taken\n", s.version.c_str(), s.threads, s.busy_threads);
    std::printf("request bytes held: %s of %s\n", size(s.pending_bytes).c_str(), size(s.max_pending_bytes).c_str());
    std::printf("running: %zu\n", s.running.size());
    for (const tirage::StatusJob& j : s.running) {
        const bool probe = j.operation == tirage::Operation::probe;
        std::string line = probe ? std::string("probe") : std::format("encode {}", name(j.priority));
        line += std::format("  {}  {} in, running for {}", j.caller, size(j.in_bytes), seconds(j.running_ms.value_or(0)));
        if (!probe) line += std::format(", queued for {}", seconds(j.waited_ms));
        std::printf("  %s\n", line.c_str());
    }
    for (const auto priority : {tirage::Priority::interactive, tirage::Priority::background}) {
        // Callers in the order of their first job in the queue.
        std::vector<std::pair<std::string, int>> callers;
        int total = 0;
        std::int64_t longest = 0;
        for (const tirage::StatusJob& j : s.queued) {
            if (j.priority != priority) continue;
            ++total;
            longest = std::max(longest, j.waited_ms);
            auto it = std::ranges::find(callers, j.caller, &std::pair<std::string, int>::first);
            if (it == callers.end()) callers.emplace_back(j.caller, 1);
            else ++it->second;
        }
        const int limit = priority == tirage::Priority::interactive ? s.queue.interactive : s.queue.background;
        std::printf("queued %s: %d of %d", std::string(name(priority)).c_str(), total, limit);
        if (total) std::printf(", the longest for %s", seconds(longest).c_str());
        std::printf("\n");
        for (const auto& [caller, count] : callers) std::printf("  %s  %d\n", caller.c_str(), count);
    }
}

int status(bool json) {
    auto s = tirage::client::status();
    if (!s) return fail(s.error());
    if (!json) {
        print_status(*s);
        return kOk;
    }
    std::string out;
    if (glz::write_json(*s, out)) return fail("status not serialisable");
    std::printf("%s\n", out.c_str());
    return kOk;
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
    if (args.command == "status") {
        if (argc == 2) return status(false);
        if (argc == 3 && std::string_view(argv[2]) == "--json") return status(true);
        return usage("status takes --json only");
    }
    if (args.command != "encode" && args.command != "probe") return usage("unknown command");

    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--profile" && (v = value())) args.profile = v;
        else if (a == "--variants" && (v = value())) args.variants = v;
        else if (a == "--crop" && (v = value())) args.crop = v;
        else if (a == "--crop-unit" && (v = value())) args.crop_unit = v;
        else if (a == "--mask" && (v = value())) args.masks.emplace_back(v);
        else if (a == "--mask-unit" && (v = value())) args.mask_unit = v;
        else if (a == "--mask-style" && (v = value())) args.mask_style = v;
        else if (a == "--priority" && (v = value())) args.priority = v;
        else if (a.starts_with("--")) return usage("unknown option or missing value");
        else args.positional.emplace_back(a);
    }

    const bool encode = args.command == "encode";
    if (args.positional.size() != (encode ? 2u : 1u)) return usage("wrong number of arguments");
    if (args.profile.empty()) return usage("--profile is required");
    if (args.crop.has_value() != args.crop_unit.has_value())
        return usage("--crop and --crop-unit go together");
    if (args.masks.empty() == args.mask_unit.has_value()) return usage("--mask and --mask-unit go together");
    if (args.mask_style && args.masks.empty()) return usage("--mask-style needs --mask");
    if (!encode && (args.variants || args.priority || !args.masks.empty()))
        return usage("probe takes no --variants, --priority nor --mask");

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
    if (!args.masks.empty()) {
        const auto unit = parse_unit(*args.mask_unit);
        if (!unit) return usage("--mask-unit is px or permille");
        const auto style = parse_style(args.mask_style.value_or("blur"));
        if (!style) return usage("--mask-style is blur, pixelate or fill");
        for (const std::string& m : args.masks) {
            const auto v = parse_area(m);
            if (!v) return usage("--mask is x,y,w,h in integers");
            request.masks.push_back(
                {.x = (*v)[0], .y = (*v)[1], .width = (*v)[2], .height = (*v)[3], .unit = unit, .style = *style});
        }
    }

    const auto profile_json = slurp(args.profile);
    if (!profile_json) return fail("cannot read the profile " + args.profile);
    // Read only: the daemon checks it against its own bounds, which the CLI
    // does not know (direct mode, against the compiled ones).
    if (const auto ec = glz::read_json(request.profile, *profile_json))
        return print_refusal({tirage::Code::invalid_profile, glz::format_error(ec, *profile_json)});

    if (encode) {
        if (args.variants) request.variants = split(*args.variants);
        else
            for (const auto& [name, _] : request.profile.variants) request.variants.push_back(name);
    }

    const std::string& input_path = args.positional.back();
    auto input = slurp(input_path);
    if (!input) return fail("cannot read the input " + input_path);
    request.input = std::move(*input);

    auto reply = tirage::client::call(request);
    request = {};
    if (!reply) return fail(reply.error());
    if (const auto* busy = std::get_if<tirage::Busy>(&*reply)) {
        std::fprintf(stderr, "tirage: busy (%s): %s\n", std::string(name(busy->reason)).c_str(),
                     busy->message.c_str());
        return kBusy;
    }
    if (const auto* failure = std::get_if<tirage::Failure>(&*reply)) return fail(failure->message);
    tirage::Response& response = std::get<tirage::Response>(*reply);

    Report report{.error = std::move(response.error),
                  .code = response.code,
                  .warnings = std::move(response.warnings),
                  .source = response.source,
                  .crop = response.crop,
                  .masks = std::move(response.masks)};
    if (encode && report.error.empty()) {
        const std::filesystem::path dir = args.positional.front();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return fail("cannot create " + dir.string() + ": " + ec.message());
        for (const tirage::Output& o : response.outputs) {
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
