// A complete caller of tiraged, the one docs/integrating.md walks through: it
// reads a profile and an image, asks for every variant of the profile, retries
// while the daemon is busy, and writes the outputs.
//
//   tirage-example <profile.json> <image> <output-dir>
//
// Built with the tests (the tirage-example target), never installed. It only
// needs tirage::client: no libvips on the caller's side.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include "tirage/client.h"

namespace {

std::optional<std::string> slurp(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f), {});
}

std::string_view extension(tirage::OutputFormat f) {
    switch (f) {
        case tirage::OutputFormat::avif: return "avif";
        case tirage::OutputFormat::webp: return "webp";
        case tirage::OutputFormat::jpeg: return "jpg";
    }
    return "bin";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: tirage-example <profile.json> <image> <output-dir>\n");
        return 2;
    }
    const auto profile_json = slurp(argv[1]);
    auto image = slurp(argv[2]);
    if (!profile_json || !image) {
        std::fprintf(stderr, "cannot read the profile or the image\n");
        return 1;
    }

    // Checked here against the default bounds, so that a mistake in the profile
    // shows up at once. The daemon checks it again against its own bounds.
    auto profile = tirage::parse_profile(*profile_json);
    if (!profile) {
        std::fprintf(stderr, "profile: %s: %s\n", profile.error().path.c_str(), profile.error().message.c_str());
        return 1;
    }

    tirage::Request request{
        .protocol = tirage::kProtocolVersion,
        .operation = tirage::Operation::encode,
        .priority = tirage::Priority::interactive,
        .profile = std::move(*profile),
        .deadline_ms = 30'000,  // give up if still queued after 30 s
        .input = std::move(*image),
    };
    for (const auto& [name, _] : request.profile.variants) request.variants.push_back(name);

    const tirage::client::Options options{
        .on_event =
            [](const tirage::client::Event& e) {
                if (const auto* q = std::get_if<tirage::Queued>(&e))
                    std::fprintf(stderr, "queued, %d encode(s) ahead\n", q->position);
                else if (const auto* s = std::get_if<tirage::Started>(&e))
                    std::fprintf(stderr, "started after %lld ms\n", static_cast<long long>(s->waited_ms));
            },
    };

    // Busy is the daemon saying "not now": the same request may be sent again.
    std::optional<tirage::Response> response;
    for (int attempt = 1; !response; ++attempt) {
        auto reply = tirage::client::call(request, options);
        if (!reply) {
            std::fprintf(stderr, "transport: %s\n", reply.error().c_str());
            return 1;
        }
        if (auto* r = std::get_if<tirage::Response>(&*reply)) {
            response = std::move(*r);
        } else if (const auto* b = std::get_if<tirage::Busy>(&*reply)) {
            if (attempt == 3) {
                std::fprintf(stderr, "busy: %s\n", b->message.c_str());
                return 75;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
        } else {
            std::fprintf(stderr, "failure: %s\n", std::get<tirage::Failure>(*reply).message.c_str());
            return 1;
        }
    }

    // A refusal is the image's or the request's fault: tell the user, by code.
    if (!response->error.empty()) {
        std::fprintf(stderr, "refused (code %d): %s\n", static_cast<int>(response->code), response->error.c_str());
        return 1;
    }
    for (const std::string& w : response->warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());

    const std::filesystem::path dir = argv[3];
    std::filesystem::create_directories(dir);
    for (const tirage::Output& o : response->outputs) {
        const auto path = dir / std::format("{}-{}.{}", o.variant, o.width, extension(o.format));
        std::ofstream(path, std::ios::binary).write(o.bytes.data(), static_cast<std::streamsize>(o.bytes.size()));
        std::printf("%s %dx%d %zu bytes\n", path.c_str(), o.width, o.height, o.bytes.size());
    }
    return 0;
}
