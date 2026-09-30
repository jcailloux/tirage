// tirage-worker: one process per encode, which dies at the end (plan § 2 and § 6).
//
// It exists so that its memory dies with it. libheif >= 1.20 opens one aom
// encoder context per AVIF image and never frees it (about 50 MiB per encode,
// measured by codiga, see ~/Projets/codiga/api/src/media/subprocess.h). No code
// of ours can fix that. A process that exits after one job can.
//
// Contract: a Job in BEVE on the standard input (a memfd from the daemon or
// from direct mode), a Response in BEVE on the standard output, exit code 0.
// A refusal is a response with an error, still with exit code 0. Anything else
// (unreadable job, libvips not starting, an encoder failing) is a failure: a
// message on the standard error and a non-zero exit code.
//
// The worker reads no configuration, needs no HOME and no network: everything
// comes with the job.

#include <cerrno>
#include <cstdio>
#include <string>
#include <string_view>

#include <unistd.h>

#include <glaze/glaze.hpp>
#include <vips/vips8>

#include "pipeline.h"
#include "tirage/protocol.h"

namespace {

enum Exit { kOk = 0, kUsage = 2, kBadJob = 3, kVips = 4, kEncode = 5, kOutput = 6 };

bool read_all(int fd, std::string& out) {
    char buf[65536];
    for (;;) {
        const auto n = ::read(fd, buf, sizeof buf);
        if (n > 0) out.append(buf, static_cast<std::size_t>(n));
        else if (n == 0) return true;
        else if (errno != EINTR) return false;
    }
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const auto n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::printf("tirage-worker %s (libvips %s)\n", TIRAGE_VERSION, vips_version_string());
        return kOk;
    }
    if (argc != 1) {
        std::fprintf(stderr, "usage: tirage-worker < job.beve > response.beve\n");
        return kUsage;
    }

    std::string frame;
    if (!read_all(STDIN_FILENO, frame)) {
        std::perror("tirage-worker: job read");
        return kBadJob;
    }
    tirage::Job job;
    if (const auto ec = glz::read_beve(job, frame)) {
        std::fprintf(stderr, "tirage-worker: unreadable job: %s\n", glz::format_error(ec, frame).c_str());
        return kBadJob;
    }
    frame = {};

    if (VIPS_INIT(argv[0]) != 0) {
        std::fprintf(stderr, "tirage-worker: libvips did not start: %s\n", vips_error_buffer());
        return kVips;
    }
    // vips_concurrency_set is global to the process, which is fine here: one
    // process, one job. The operation cache is useless to a process that runs
    // one pipeline, and it would keep the decoded original alive.
    vips_concurrency_set(job.threads < 1 ? 1 : job.threads);
    vips_cache_set_max(0);
    // Loaders are picked by the sniffed format already. This also refuses the
    // ones libvips itself does not trust with outside input.
    vips_block_untrusted_set(TRUE);

    tirage::Response response;
    try {
        response = tirage::worker::run(job);
    } catch (const tirage::worker::EncodeFailure& e) {
        std::fprintf(stderr, "tirage-worker: encode failed: %s\n", e.message.c_str());
        return kEncode;
    }

    std::string out;
    if (const auto ec = glz::write_beve(response, out)) {
        std::fprintf(stderr, "tirage-worker: response not serialisable\n");
        return kOutput;
    }
    if (!write_all(STDOUT_FILENO, out)) {
        std::perror("tirage-worker: response write");
        return kOutput;
    }
    // No vips_shutdown: the process exits right after, which is the point.
    return kOk;
}
