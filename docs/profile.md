# The encoding profile

A profile says what a caller wants out of an original: which inputs it accepts, and which
**variants** to produce, each at a set of widths and in a set of formats. tirage keeps no profile
of its own: the caller sends its profile with every request, and versions it in its own repository.

From C++ the profile is a `tirage::Profile` (`include/tirage/profile.h`), from the CLI a JSON file.
Both have the same shape. [`examples/profile.json`](../examples/profile.json) is a complete profile,
encoded by the test suite on every build:

```json
{
  "version": 1,
  "input": {
    "formats": ["png", "jpeg", "webp", "heic", "avif"],
    "min_size": { "width": 320, "height": 200, "policy": "warn" },
    "limits": { "max_bytes": 26214400 }
  },
  "color": "srgb",
  "metadata": "strip",
  "encoding": {
    "avif": { "effort": 4, "chroma": "420", "tune": "ssim" },
    "webp": { "effort": 4, "sharp_yuv": true }
  },
  "variants": {
    "web": {
      "widths": [480, 960, 1920],
      "min_step": 1.25,
      "formats": ["avif", "webp"],
      "quality": [
        { "max_width": 960, "avif": 55, "webp": 80 },
        { "avif": 65, "webp": 82 }
      ]
    },
    "thumb": {
      "widths": [320],
      "formats": ["jpeg"],
      "quality": [ { "jpeg": 82 } ],
      "encoding": {
        "jpeg": { "chroma": "444", "progressive": true, "optimize_coding": true, "background": [255, 255, 255] }
      }
    },
    "archive": {
      "widths": [4096],
      "formats": ["avif"],
      "quality": [ { "avif": 80 } ],
      "encoding": { "avif": { "effort": 6, "bit_depth": 10 } }
    }
  }
}
```

## No setting is ignored

The rule behind most checks: a setting that would do nothing is **refused**, never ignored. An
unknown key, `chroma` on WebP, a quality for a format the variant does not produce, or an `encoding`
block for a format no variant produces all make the profile invalid. A refusal at validation is
better than a surprise in the output.

The error names the first problem and where it is, for example
`profile.variants.web.quality[1].webp: missing: the variant produces this format`.

You can validate a profile before sending it, without a daemon: `tirage::parse_profile(json)` reads
and checks a JSON profile, and `tirage::validate(profile)` checks a `Profile` you built in code.
Both check against the compiled default bounds. The daemon checks again against its own.

## Top level

| Key | Required | Values | Meaning |
|---|---|---|---|
| `version` | yes | `1` | Schema version (`tirage::kProfileVersion`). Another value is refused. |
| `input` | yes | object | What the profile accepts, see below. |
| `color` | no | `"srgb"` (default), `"icc"` | See [Colour and metadata](#colour-and-metadata). |
| `metadata` | no | `"strip"` (default), `"keep"` | See [Colour and metadata](#colour-and-metadata). |
| `encoding` | no | object | Encoder settings shared by every variant, see [Encoding](#encoding). |
| `variants` | yes | object, at least one | The variants, by name. |

## `input`

| Key | Required | Meaning |
|---|---|---|
| `formats` | yes | Accepted input formats, among `png`, `jpeg`, `webp`, `heic`, `avif`. Anything else is refused with `unsupported_format`. |
| `min_size` | no | `{ "width", "height", "policy" }`: the floor. Without it, any size is accepted. |
| `limits` | no | `{ "max_bytes", "max_edge", "max_pixels" }`, each optional: tighter safety bounds. |

**Formats are recognised by their bytes**, never by a file name or a declared type. HEIC and AVIF
are told apart by their `ftyp` brands.

**The floor** applies to what is produced: the crop when the request has one, else the whole image,
after EXIF orientation. With `"policy": "reject"` (the default) a smaller image is refused with
`image_too_small`. With `"warn"` it is encoded anyway and the response carries a warning.

**Limits** can only tighten the daemon's bounds, never relax them. The daemon's defaults are 64 MiB,
16384 px on the longer edge and 100 megapixels. A limit above the daemon's bound makes the profile
invalid. The bounds are checked on the header, before any pixel is decoded, so a decompression bomb
costs nothing.

## Variants

A variant is a named set of outputs. A request names the variants it wants, so one profile can
serve several uses (a gallery, an archive, a thumbnail).

Names are 1 to 64 characters among `a-z`, `0-9`, `_` and `-`: they are meant to end up in file
names.

| Key | Required | Meaning |
|---|---|---|
| `widths` | yes | Target widths in pixels, positive, no duplicates, in any order. |
| `min_step` | no | Smallest ratio between two produced widths, a number above 1 (from 0.6.0). See below. |
| `formats` | yes | Output formats among `avif`, `webp`, `jpeg`, no duplicates. Every width is produced in every format. |
| `quality` | yes | Quality steps, see below. |
| `encoding` | no | Encoder settings for this variant only, field by field over the profile's. |

### Widths: never upscaled

Widths are taken in ascending order against the width of what is encoded (the crop, or the whole
oriented image). The first width that reaches or passes it is capped to it, and the wider ones are
not produced. No error, no upscale.

So an image narrower than every width still gets one output, at its own width. With
`"widths": [480, 960, 1920]`:

| Source width | Widths produced |
|---|---|
| 2400 | 480, 960, 1920 |
| 1200 | 480, 960, 1200 |
| 640 | 480, 640 |
| 300 | 300 |

The height follows the aspect ratio. `tirage::planned_widths(variant, width)` (`resolve.h`) gives the
same list, if you need to know it before encoding.

### Widths: `min_step`

A source just past a width gives two nearly equal outputs (800 and 801), which cost an encode and
storage and give a `srcset` nothing. With `min_step`, a width capped to the source that is below
`min_step` times the previous width **replaces** it: the source's full width is kept, the nearly
identical step is dropped. Without `min_step`, nothing changes. With
`"widths": [400, 800, 1600], "min_step": 1.25`:

| Source width | Widths produced |
|---|---|
| 2400 | 400, 800, 1600 |
| 1000 | 400, 800, 1000 |
| 999 | 400, 999 |
| 801 | 400, 801 |
| 500 | 400, 500 |
| 499 | 499 |
| 300 | 300 |

- The comparison is `width < previous × min_step`, in double precision.
- The widths produced are always a subset of those without `min_step`: turning it on only drops
  widths, it never adds one.
- The targets themselves must be at least `min_step` apart, so only a capped width can fall short.
  `[400, 450]` with `min_step: 1.25` is refused, and so is `min_step` on a variant with a single
  width, where it would do nothing.
- A daemon older than 0.6.0 refuses a profile that has `min_step`, like any unknown key.

### Quality steps

`quality` is a list of steps. Each step gives one quality (1 to 100) **per format of the variant**,
no more and no less, and applies up to its `max_width`, inclusive. Every step but the last has a
`max_width`, in strictly increasing order. The last one has none and covers everything wider.

```json
"quality": [
  { "max_width": 960, "avif": 55, "webp": 80 },
  { "avif": 65, "webp": 82 }
]
```

Here a 480 or 960 px output gets AVIF 55 and WebP 80, and anything wider gets 65 and 82. A variant
with one quality for every width has a single step without `max_width`.

## Encoding

Encoder settings come from three layers, field by field: tirage's defaults, then the profile's
`encoding`, then the variant's. A field a layer does not mention keeps the value of the layer
below.

| Format | Key | Values | tirage's default |
|---|---|---|---|
| AVIF | `effort` | 0 to 9 (slower, smaller) | 4 |
| | `chroma` | `"420"`, `"444"` | `"420"` |
| | `bit_depth` | 8, 10 | 8 |
| | `tune` | `"ssim"`, `"iq"` (from 0.5.0) | `"ssim"` |
| WebP | `effort` | 0 to 6 | 4 |
| | `sharp_yuv` | boolean | `false` |
| JPEG | `chroma` | `"420"`, `"444"` | `"420"` |
| | `progressive` | boolean | `false` |
| | `optimize_coding` | boolean (optimised Huffman tables) | `false` |
| | `background` | `[r, g, b]`, 0 to 255 | `[255, 255, 255]` |

Notes:

- `chroma` is applied as given, at every quality. There is no `auto`, which in libvips silently
  switches to 4:4:4 from quality 90. Write `"444"` if you want it.
- WebP has no `chroma`: lossy WebP is always 4:2:0, and `sharp_yuv` is its only chroma setting.
  JPEG has no `effort`.
- A 10-bit AVIF keeps the precision of a 16-bit source and reduces banding in gradients. Any other
  output is made from 8 bits per sample.
- `tune` is the tuning of the aom AV1 encoder:
  - `"ssim"` is the output tirage has always made: SSIM for the colour planes and PSNR for alpha
    (SSIM rings on alpha). libheif decides the latter.
  - `"iq"` is libaom's still-image mode (libaom 3.12 and later), which gives a better perceived
    quality for the same weight. It applies to alpha too: aom has a single tuning for both.
  - **The quality scale shifts with `iq`**: at the same quality, `iq` makes larger files. Measured
    on screenshots in 10 bits 4:2:0, `iq` at 70 weighs about what `ssim` at 80 weighs at 1600 px,
    and `iq` at 40 about `ssim` at 50 at 800 px. Recalibrate your qualities when you switch.
  - **`iq` moves with libaom**: `ssim` gives the same bytes from libaom 3.12 to 3.13, `iq` does not.
    An update of libaom on the server can move the size and quality curve of an `iq` profile.
  - A daemon older than 0.5.0 refuses a profile that has `tune`, like any unknown key.
- An `encoding` block for a format the variant (or, at the top level, any variant) does not produce
  is refused.

## Colour and metadata

`color` decides what happens to the ICC profile:

- `"srgb"`: the pixels are converted to sRGB through the embedded profile, and no profile is
  written. Use this for the web: a Display P3 photo keeps its colours instead of coming out washed
  out.
- `"icc"`: the pixels and the embedded profile are kept as they are.

Whatever `color` says, a CMYK image is converted to sRGB (no output format carries CMYK). A profile
that does not match the pixels is dropped: the image is taken as sRGB, with a warning in the
response. A CMYK image with an unusable profile is refused with `decode_failed`.

`metadata` decides the rest: `"strip"` writes no EXIF, XMP or IPTC, `"keep"` copies them. A request
with masks never keeps them (they may hold a thumbnail of the original).

## Alpha

There is no setting. An image with an alpha channel is resized premultiplied, which avoids dark
halos on its edges. AVIF and WebP keep the alpha. JPEG has none: the image is flattened on the
JPEG `background`.

## Orientation

The EXIF orientation is applied first. Widths, crop, masks, floor and the `source` of the response
are all on the image as it is displayed, never as it is stored.

## Evolution

The schema changes only by adding fields. An existing field never changes meaning. A change that
cannot be made that way would bump `version`.
