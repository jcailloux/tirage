# Requests and responses

What a caller sends to `tiraged` and what it gets back. The C++ types are in
`include/tirage/protocol.h`. The [client](integrating.md) does the transport for you: this page is
about what goes in a request and what comes out.

## The request

```cpp
tirage::Request request{
    .protocol = tirage::kProtocolVersion,
    .operation = tirage::Operation::encode,
    .priority = tirage::Priority::interactive,
    .profile = profile,
    .variants = {"web", "thumb"},
    .crop = tirage::Crop{.x = 0, .y = 100, .width = 1000, .height = 600, .unit = tirage::CropUnit::permille},
    .masks = {tirage::Mask{.x = 40, .y = 12, .width = 220, .height = 30, .unit = tirage::CropUnit::px,
                           .style = tirage::MaskStyle::blur, .edge = tirage::MaskEdge::soft, .angle = -8}},
    .deadline_ms = 30'000,
    .input = std::move(bytes),
};
```

| Field | Meaning |
|---|---|
| `protocol` | Always `tirage::kProtocolVersion`. Another value is refused. |
| `operation` | `encode`, or `probe` to read the image's size and format without encoding anything. |
| `priority` | `interactive` (default) or `background`, see [Priority](#priority-and-the-queue). |
| `profile` | The [profile](profile.md), sent with every request. |
| `variants` | Encode: the variants of the profile to produce, at least one, each once. Probe: empty. |
| `crop` | Optional, see [Crop](#crop). |
| `masks` | Optional, encode only, at most 32 (`tirage::kMaxMasks`), see [Masks](#masks). |
| `deadline_ms` | Optional, encode only: give up if the job is still queued after this many milliseconds. |
| `input` | The original's bytes. Not empty. |

Everything that can be checked without decoding is checked **before the job is queued**: the
protocol, the profile, the variants, the crop and masks, the size of the file and its format. A
wrong request is refused at once instead of after a wait. `tirage::validate_request(request)`
(`request.h`) runs the same checks on your side if you want them earlier.

## Crop

A crop keeps one rectangle of the image. It is expressed on the **oriented** image (EXIF
orientation applied), in one of two units, which you must always give:

- `px`: pixels of the oriented original.
- `permille`: thousandths of its width and height, from 0 to 1000. Handy when the caller only knows
  proportions, from an editor showing a reduced preview for example.

`x` and `y` must not be negative and the size must be positive. A crop reaching past the image is
cut to it, and a crop always keeps at least one pixel. The response says what was applied, in
pixels (`crop`).

Widths, the floor and the outputs are all taken from the cropped area.

## Masks

A mask hides a rectangle of the image before anything is resized from it: a user name, a face, an
avatar, a licence plate. No output ever holds the hidden pixels.

| Field | Values | Meaning |
|---|---|---|
| `x`, `y`, `width`, `height` | integers | The rectangle, on the oriented original, like the crop. |
| `unit` | `px`, `permille` | Required, like the crop's. |
| `style` | `blur` (default), `pixelate`, `fill` | How the zone is hidden. |
| `edge` | `sharp` (default), `soft` | How the zone meets the image around it. |
| `angle` | degrees, -180 to 180, 0 by default | Tilts the zone, see [Tilted masks](#tilted-masks). |

Styles, all scaled to the zone's size so that their strength does not depend on the resolution:

- `blur`: a Gaussian blur strong enough to leave shapes and no detail.
- `pixelate`: blocks of one colour, 8 along the zone's longer side.
- `fill`: one colour, the zone's mean.

Edges:

- `sharp`: the whole rectangle, square corners.
- `soft`: rounded corners, with a radius of a quarter of the shorter side, faded into the image over
  a tenth of the shorter side (at least one pixel). Only the inside of the fade is fully hidden.
  To hide a whole rectangle with a soft edge, send a bigger mask around it. A margin of a third of
  the shorter side, rounded up and 4 px at least, on every side is enough at every size.
  `tirage::soft_coverage` (`request.h`) gives the exact opacity at each pixel, for a preview.

Placement rules:

- Masks are drawn **in the order given**: a mask overlapping another one reads the first one's
  result.
- They are placed on the whole original, **before the crop**. A mask partly outside the crop is
  only partly seen.
- A mask may reach past the image on any side, and may start before it (negative `x` or `y`, and in
  permille down to -1000). Only its part inside the image is drawn. This lets a soft edge keep its
  fade around a zone that touches the border.
- A mask entirely outside the image hides nothing. The response then has an empty rectangle for it,
  and a warning.

A masked image never keeps its EXIF, XMP or IPTC, even with `"metadata": "keep"`: they may hold a
thumbnail of the original. The response warns when that happens.

### Tilted masks

`angle` turns the rectangle **clockwise** around its centre, by that many degrees (a negative angle
turns it anticlockwise), as CSS `rotate()` does. Text written along a slope, a tilted licence
plate: the zone follows it instead of hiding a whole box around it.

- The rectangle is the one the mask would hide without its angle: give it straight, then turn it.
  A permille rectangle is converted to pixels first, then turned: on an image that is not square,
  a permille square is not a pixel square, and it is the pixel rectangle that turns.
- The style is computed **in the rectangle's own frame**, as if the image were turned to make it
  straight: the blocks of `pixelate` are tilted with it, and `blur` and `fill` have the strength
  of the rectangle, not of the box around it. Pixels are resampled (bilinear) for that.
- `sharp` hides the turned rectangle, its edge anti-aliased over one pixel. `soft` turns the
  rounded rectangle and its fade with it, the same shape as without an angle. The margin advised
  for a soft edge is still measured on the rectangle before it is turned: add it to `width` and
  `height`, and keep the same centre.
- The response gives the box around the turned rectangle, cut to the image: every pixel the mask
  may have touched. Its corners keep the image as it was. A tilted mask near a corner of the image
  can have a box that reaches the image while the mask itself does not: the response then has a
  non-empty box, no warning, and nothing is hidden.

`tirage::soft_coverage` and `tirage::sharp_coverage` (`request.h`) take the angle too, for a
preview.

## Priority and the queue

The daemon runs **one encode at a time**, with all the threads of the machine's budget. The others
wait in two queues:

- `interactive`: someone is waiting for the result, an upload for example.
- `background`: batch work, a catalogue rebuild for example.

An interactive job always starts before a background one, and within each queue the first to
arrive starts first. A running encode is never interrupted, so an interactive job waits at most for
the end of the current encode, plus the interactive jobs ahead of it.

Each queue has a length limit, 32 and 256 by default. A request arriving at a full queue gets
`Busy` at once. A probe never waits in the queue: up to two run beside the encode, else `Busy`.

## What comes back

The daemon answers with zero or more **events**, then exactly one **final message**, then closes
the connection.

Events, encode only:

- `Queued{position}`, once, as soon as the job is queued: the number of encodes that start before
  this one at that moment. 0 means it starts now. It is not updated afterwards, and interactive
  jobs arriving later still pass a background one.
- `Started{waited_ms}`, once, when the encode starts: the time spent in the queue.

Final messages:

| Message | Meaning | What to do |
|---|---|---|
| `Response` with an empty `error` | Success. | Use the outputs. |
| `Response` with an `error` | A **refusal**: the request or the image is at fault. | Tell the user, by `code`. Sending it again gives the same refusal. |
| `Busy` | The daemon did **not** run the job: queue full, deadline passed, or daemon stopping. | Send the same request again later. |
| `Failure` | Something broke on the daemon's side: worker killed or timed out, unreadable frame. | Log it. It is never the image's fault. |

Hanging up before the final message **cancels** the job: it leaves the queue, or its worker is
killed. With the client, that is a stop request on `Options::stop`.

A worker running longer than the daemon's `timeout_s` (120 s by default) is killed, and the caller
gets a `Failure`.

### `Response`

| Field | Meaning |
|---|---|
| `error` | Empty on success, else what was refused, for logs and developers. |
| `code` | `tirage::Code`, see below. |
| `warnings` | Things worth knowing that did not stop the encode: a floor with the `warn` policy, an unusable ICC profile, a mask outside the image, metadata dropped because of masks. |
| `source` | The original after orientation: `width`, `height`, `bands`, input `format`. Absent when refused before decoding. |
| `crop` | The crop actually applied, in pixels, when the request had one. |
| `masks` | Each mask as applied, in pixels and cut to the image, in request order: a tilted mask's box. All zero for a mask outside the image. |
| `outputs` | Encode only: one per variant, width and format, in the request's variant order, then by width, then in the variant's format order. |

Each `Output` has its `variant`, `width`, `height`, `format` and `bytes`, the encoded file. tirage
writes nothing to disk: naming and storing the files is the caller's business.

A probe's response has `source`, `crop` and `warnings`, and no outputs. It also tells whether the
image would pass the floor.

### Refusal codes

Stable numbers: callers map them to their own messages. Codes are only ever appended.

| Code | Number | When |
|---|---|---|
| `ok` | 0 | Success. |
| `invalid_request` | 1 | Wrong protocol version, unknown variant, bad crop or mask (an angle past 180° included), empty input… |
| `invalid_profile` | 2 | The profile does not validate. `error` starts with `profile.` and the path. |
| `unsupported_format` | 3 | Not recognised, or not in `input.formats`. |
| `file_too_large` | 4 | Above `max_bytes`. |
| `image_too_large` | 5 | Above `max_edge` or `max_pixels`, read from the header before decoding. |
| `image_too_small` | 6 | Below `min_size`, with the `reject` policy. |
| `decode_failed` | 7 | Recognised, but the file could not be read. |

`Busy` carries a `reason`: `queue_full` (the queue of this priority, the probe limit or the
daemon's memory), `deadline` or `stopping`.

## On the wire

You only need this to write a client in another language. The C++ client does it for you.

- A Unix stream socket, `/run/tirage/tirage.sock` by default.
- One request per connection.
- Each frame is a 4-byte little-endian length, then that many bytes of
  [BEVE](https://github.com/beve-org/beve), as glaze writes the C++ types of `protocol.h`. The
  daemon sends `tirage::Message`, a variant of `Queued`, `Started`, `Response`, `Busy`, `Failure`
  and `Status`, in that order.
- A request frame is at most the daemon's `max_bytes` plus 1 MiB. A bigger one is refused on its
  header, before its body is read. A caller should refuse a message frame above 1 GiB.
- The daemon reads a request strictly: a field it does not know makes the request unreadable
  (`Failure`), since dropping a mask in silence would publish what it should hide. A caller should
  skip unknown fields in the daemon's messages, which is what lets an older client talk to a newer
  daemon.
