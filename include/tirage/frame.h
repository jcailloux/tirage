#pragma once

// Framing on the daemon's socket (docs/requests.md): each frame is a 4-byte
// little-endian length, then that many bytes of BEVE. A caller sends one
// Request frame per connection, the daemon answers with Message frames.
//
// The length is read before anything is allocated for the body: a frame above
// the reader's bound is refused on its header alone.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

#include "tirage/validate.h"

namespace tirage {

// Where the daemon listens and callers look, unless told otherwise (the
// daemon's configuration, TIRAGE_SOCKET).
inline constexpr std::string_view kDefaultSocketPath = "/run/tirage/tirage.sock";

inline constexpr std::size_t kFrameHeaderSize = 4;

// What a request carries besides its input: the profile, the variants, the
// crop. Generous, a real profile is a few KiB.
inline constexpr std::int64_t kRequestEnvelope = 1 << 20;

// The largest request frame the daemon reads: the largest input its bounds
// accept, plus the envelope.
[[nodiscard]] constexpr std::int64_t max_request_frame(const Bounds& daemon) {
    return daemon.max_bytes + kRequestEnvelope;
}

// How a caller reads the daemon's messages: unknown keys are skipped, so that a
// daemon newer than the caller's headers, whose messages carry the fields added
// since, is still understood. The daemon reads requests strictly instead: a
// request field it does not know (a mask, say) must never be dropped in silence.
inline constexpr glz::opts kReadMessage{.format = glz::BEVE, .error_on_unknown_keys = false};

// The largest message frame a caller should accept from the daemon (the daemon
// holds worker responses to it too). Outputs are compressed images: a whole
// response above 1 GiB means something went wrong.
inline constexpr std::int64_t kMaxMessageFrame = std::int64_t{1} << 30;

[[nodiscard]] constexpr std::array<char, kFrameHeaderSize> frame_header(std::uint32_t length) {
    return {static_cast<char>(length), static_cast<char>(length >> 8), static_cast<char>(length >> 16),
            static_cast<char>(length >> 24)};
}

[[nodiscard]] constexpr std::uint32_t frame_length(const char* header) {
    const auto b = [&](int i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(header[i])); };
    return b(0) | (b(1) << 8) | (b(2) << 16) | (b(3) << 24);
}

// The value in BEVE, behind its header, ready to send.
template <class T>
[[nodiscard]] std::expected<std::string, std::string> make_frame(const T& value) {
    std::string payload;
    if (const auto ec = glz::write_beve(value, payload))
        return std::unexpected("not serialisable: " + glz::format_error(ec));
    if (payload.size() > static_cast<std::size_t>(kMaxMessageFrame))
        return std::unexpected("frame of " + std::to_string(payload.size()) + " bytes, above the frame bound");
    const auto header = frame_header(static_cast<std::uint32_t>(payload.size()));
    std::string frame;
    frame.reserve(kFrameHeaderSize + payload.size());
    frame.append(header.data(), header.size());
    frame.append(payload);
    return frame;
}

}  // namespace tirage
