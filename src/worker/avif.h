#pragma once

// The AVIF writer (plan § 3 and § 11): libvips 8.16's heifsave, ported so that
// the aom encoder's tune can be set, which libvips only exposes from 8.18.

#include <string>

#include <vips/vips8>

#include "tirage/resolve.h"

namespace tirage::worker {

// Encodes `in` to AVIF with the aom encoder, at quality `quality` (1 to 100),
// keeping the metadata `keep` says (VipsForeignKeep). The image is made ready
// for saving by libvips itself, as for any of its savers. Throws vips::VError
// on failure, the aom encoder missing included.
[[nodiscard]] std::string write_avif(const vips::VImage& in, int quality, const ResolvedAvif& settings,
                                     int keep);

}  // namespace tirage::worker
