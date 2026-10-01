// The tirage CLI against a real tiraged (plan § 5): what it writes, what it
// prints, and its exit codes, for a success, a refusal, busy, a failure and
// status.

#include <doctest/doctest.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "harness.h"

using namespace tirage;
using namespace tirage::test;
namespace fs = std::filesystem;

namespace tirage::test {

// What the CLI prints for encode and probe (tirage::cli::Report), the parts
// the tests look at. Not in the anonymous namespace: glaze reflection needs
// linkage.
struct Printed {
    std::string error;
    int code = 0;
    std::optional<Source> source;
    std::vector<Rect> masks;
    struct File {
        std::string variant;
        int width = 0;
        std::size_t size = 0;
        std::string path;
    };
    std::vector<File> outputs;
};

}  // namespace tirage::test

namespace {

struct Run {
    int code = -1;
    std::string out;
    std::string err;
};

// Runs the CLI (or another `program`) in `dir` with `args`, TIRAGE_SOCKET set
// to `socket` and TIRAGE_DIRECT unset.
Run cli(const fs::path& dir, const std::string& socket, std::vector<std::string> args,
        const char* program = TIRAGE_CLI_PATH) {
    std::vector<std::string> env_strings;
    for (char** e = environ; *e; ++e) {
        const std::string_view v = *e;
        if (!v.starts_with("TIRAGE_DIRECT=") && !v.starts_with("TIRAGE_SOCKET=")) env_strings.emplace_back(v);
    }
    env_strings.push_back("TIRAGE_SOCKET=" + socket);
    std::vector<char*> envp;
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);

    args.insert(args.begin(), program);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    const std::string out = (dir / "cli.out").string(), err = (dir / "cli.err").string();
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addchdir_np(&actions, dir.c_str());
    pid_t pid = 0;
    REQUIRE(::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), envp.data()) == 0);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return {.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1, .out = read_file(out), .err = read_file(err)};
}

// The harness's request, as the files the CLI takes.
void write_inputs(const fs::path& dir, const Request& r = request()) {
    write_file(dir / "profile.json", glz::write_json(r.profile).value_or(""));
    write_file(dir / "in.png", r.input);
}

Printed printed(const Run& run) {
    Printed p;
    constexpr glz::opts lenient{.error_on_unknown_keys = false};
    const auto ec = glz::read<lenient>(p, run.out);
    REQUIRE_MESSAGE(!ec, run.out);
    return p;
}

}  // namespace

TEST_CASE("cli: encode and probe through the daemon") {
    Daemon d;
    write_inputs(d.dir());

    const Run enc = cli(d.dir(), d.socket(), {"encode", "--profile", "profile.json", "out", "in.png"});
    REQUIRE_MESSAGE(enc.code == 0, enc.err);
    const Printed e = printed(enc);
    CHECK(e.error.empty());
    REQUIRE(e.outputs.size() == 1);
    CHECK(e.outputs[0].path == "out/small-32.webp");
    const std::string webp = read_file(d.dir() / e.outputs[0].path);
    CHECK(webp.size() == e.outputs[0].size);
    CHECK(webp.substr(8, 4) == "WEBP");

    const Run probe = cli(d.dir(), d.socket(), {"probe", "--profile", "profile.json", "in.png"});
    REQUIRE_MESSAGE(probe.code == 0, probe.err);
    const Printed p = printed(probe);
    REQUIRE(p.source.has_value());
    CHECK(p.source->width == 64);
    CHECK(d.log().find("op=probe") != std::string::npos);
}

TEST_CASE("cli: masks, and their options checked") {
    Daemon d;
    write_inputs(d.dir());

    const Run run = cli(d.dir(), d.socket(),
                        {"encode", "--profile", "profile.json", "--mask", "0,0,10,10", "--mask", "500,0,100,1000",
                         "--mask-unit", "permille", "--mask-style", "pixelate", "--mask-edge", "soft", "out", "in.png"});
    REQUIRE_MESSAGE(run.code == 0, run.err);
    const Printed p = printed(run);
    CHECK(p.error.empty());
    REQUIRE(p.masks.size() == 2);
    CHECK((p.masks[1].x == 32 && p.masks[1].width == 6 && p.masks[1].height == 48));  // of 64x48
    CHECK(fs::exists(d.dir() / "out/small-32.webp"));

    // A fifth number tilts the mask, in degrees: it comes back as the box around it.
    const Run tilted = cli(d.dir(), d.socket(),
                           {"encode", "--profile", "profile.json", "--mask", "12,19,40,10,90", "--mask", "0,0,10,10",
                            "--mask-unit", "px", "tilted", "in.png"});
    REQUIRE_MESSAGE(tilted.code == 0, tilted.err);
    const Printed t = printed(tilted);
    CHECK(t.error.empty());
    REQUIRE(t.masks.size() == 2);
    CHECK((t.masks[0].x == 27 && t.masks[0].y == 4 && t.masks[0].width == 10 && t.masks[0].height == 40));
    CHECK((t.masks[1].x == 0 && t.masks[1].width == 10));
    // Past half a turn, the daemon refuses it: a refusal, exit code 0.
    const Run steep = cli(d.dir(), d.socket(),
                          {"encode", "--profile", "profile.json", "--mask", "0,0,10,10,181", "--mask-unit", "px", "steep",
                           "in.png"});
    REQUIRE(steep.code == 0);
    CHECK(printed(steep).error == "masks[0].angle: degrees from -180 to 180");

    const auto usage = [&](std::vector<std::string> args) {
        args.insert(args.begin(), {"encode", "--profile", "profile.json"});
        args.insert(args.end(), {"out", "in.png"});
        return cli(d.dir(), d.socket(), args).code;
    };
    CHECK(usage({"--mask", "0,0,10,10"}) == 2);                                   // no unit
    CHECK(usage({"--mask-unit", "px"}) == 2);                                     // no mask
    CHECK(usage({"--mask", "0,0,10", "--mask-unit", "px"}) == 2);                 // three numbers
    CHECK(usage({"--mask", "0,0,10,10,x", "--mask-unit", "px"}) == 2);            // not an angle
    CHECK(usage({"--mask", "0,0,10,10,1,2", "--mask-unit", "px"}) == 2);          // six numbers
    CHECK(usage({"--mask", "0,0,10,10", "--mask-unit", "px", "--mask-style", "erase"}) == 2);
    CHECK(usage({"--mask", "0,0,10,10", "--mask-unit", "px", "--mask-edge", "round"}) == 2);
    CHECK(usage({"--mask-edge", "soft"}) == 2);                                   // no mask
    CHECK(cli(d.dir(), d.socket(), {"probe", "--profile", "profile.json", "--mask", "0,0,1,1", "--mask-unit", "px",
                                    "in.png"})
              .code == 2);
}

TEST_CASE("cli: a refusal is printed with exit code 0, busy exits 75, no daemon exits 1") {
    Daemon d(R"("queue": {"interactive": 1, "background": 1})");

    // The profile is checked by the daemon, against its bounds.
    Request bad = request();
    bad.profile.variants["small"].quality[0].webp = 0;
    write_inputs(d.dir(), bad);
    const Run refused = cli(d.dir(), d.socket(), {"encode", "--profile", "profile.json", "out", "in.png"});
    CHECK_MESSAGE(refused.code == 0, refused.err);
    const Printed r = printed(refused);
    CHECK(r.code == static_cast<int>(Code::invalid_profile));
    CHECK(r.error.starts_with("profile.variants.small"));
    CHECK_FALSE(fs::exists(d.dir() / "out"));

    write_inputs(d.dir());
    d.delay(2);
    Connection a(d.socket()), b(d.socket());
    start_running(a, request(Priority::background));
    b.send(request(Priority::background));
    expect<Queued>(b);
    const Run busy = cli(d.dir(), d.socket(),
                         {"encode", "--profile", "profile.json", "--priority", "background", "out", "in.png"});
    CHECK(busy.code == 75);
    CHECK(busy.err.find("busy (queue_full)") != std::string::npos);
    CHECK(busy.out.empty());

    const Run none = cli(d.dir(), (d.dir() / "nothing.sock").string(),
                         {"encode", "--profile", "profile.json", "out", "in.png"});
    CHECK(none.code == 1);
    CHECK(none.err.find("cannot reach tiraged") != std::string::npos);
}

TEST_CASE("cli: status, for a person and in JSON") {
    Daemon d(R"("threads": 3)");
    d.delay(2);
    Connection a(d.socket()), b(d.socket());
    start_running(a, request(Priority::background));
    b.send(request(Priority::interactive));
    expect<Queued>(b);

    const Run text = cli(d.dir(), d.socket(), {"status"});
    REQUIRE_MESSAGE(text.code == 0, text.err);
    CHECK(text.out.find("3 threads per encode, 3 taken") != std::string::npos);
    CHECK(text.out.find("running: 1") != std::string::npos);
    CHECK(text.out.find("encode background") != std::string::npos);
    CHECK(text.out.find("queued interactive: 1 of 32") != std::string::npos);
    CHECK(text.out.find("queued background: 0 of 256") != std::string::npos);

    const Run json = cli(d.dir(), d.socket(), {"status", "--json"});
    REQUIRE_MESSAGE(json.code == 0, json.err);
    Status s;
    REQUIRE_FALSE(glz::read_json(s, json.out));
    CHECK(s.threads == 3);
    CHECK(s.queued.size() == 1);

    CHECK(cli(d.dir(), d.socket(), {"status", "--verbose"}).code == 2);
}

TEST_CASE("examples: the documented profile and caller work through the daemon") {
    Daemon d;
    write_file(d.dir() / "in.png", png(640, 400));
    const std::string profile = std::string(TIRAGE_EXAMPLES_DIR) + "/profile.json";

    // Every variant of examples/profile.json, none wider than the image: web at
    // 480 and 640 (960 capped), in AVIF and WebP, thumb at 320, archive at 640.
    const Run run = cli(d.dir(), d.socket(), {"encode", "--profile", profile, "out", "in.png"});
    REQUIRE_MESSAGE(run.code == 0, run.err);
    const Printed p = printed(run);
    CHECK_MESSAGE(p.error.empty(), p.error);
    std::vector<std::string> paths;
    for (const auto& o : p.outputs) paths.push_back(o.path);
    CHECK(paths == std::vector<std::string>{"out/archive-640.avif", "out/thumb-320.jpg", "out/web-480.avif",
                                            "out/web-480.webp", "out/web-640.avif", "out/web-640.webp"});

    const Run example = cli(d.dir(), d.socket(), {profile, "in.png", "example"}, TIRAGE_EXAMPLE_PATH);
    REQUIRE_MESSAGE(example.code == 0, example.err);
    CHECK(example.err.find("queued, 0 encode(s) ahead") != std::string::npos);
    CHECK(read_file(d.dir() / "example/thumb-320.jpg").starts_with("\xFF\xD8\xFF"));
    CHECK(read_file(d.dir() / "example/web-640.webp").substr(8, 4) == "WEBP");
    CHECK(fs::exists(d.dir() / "example/archive-640.avif"));
}
