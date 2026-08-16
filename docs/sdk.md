# MediaPlayer C SDK

## Quick start

1. Install/deploy the native MediaPlayer provider with Endstone.
2. Declare and load this consumer after the hard dependency `mediaplayer`.
3. Run API calls only in the same Endstone process and on its main thread.
4. Reacquire the API table and screen handles on every consumer-plugin enable
   or MediaPlayer reload.

The SDK is for an Endstone consumer plugin running in-process. It is not a
standalone external executable: a normal process cannot discover a provider
that is loaded in the server process, and this header does not load one.

`include/endstone_mediaplayer_api.h` is the complete public C23 SDK. It exposes
the stable V1 table through `mp_get_api_v1()` or `mp_try_get_api_v1()`. The
provider export is `endstone_mediaplayer_get_api(uint32_t version)` and
currently supports only `MP_API_V1`.

On Windows the header enumerates loaded process modules and accepts both the
Endstone shadow-copy name `endstone_mediaplayer-<hash>.dll` and
`endstone_mediaplayer.dll`; it validates the export and prefers an active
provider instance. On Linux it resolves the process-global export with a
no-load module lookup fallback. Neither path loads or unloads a DLL. This
module-shadow-copy discovery is intentional and is covered by the SDK and
real-BDS checks.

## Acquire and validate the table

```c
#include "endstone_mediaplayer_api.h"

mp_api_v1 mp = mp_get_api_v1();
mp_screen_handle screen = MP_INVALID_SCREEN_HANDLE;
mp_result result = mp.screen_find("live", &screen);
if (result != MP_OK) {
    // Report mp.result_string(result) in the consumer plugin.
}
```

`mp_try_get_api_v1(&mp)` returns `true` only for a valid already-loaded V1
provider. `mp_get_api_v1()` always returns a callable table; when no provider
is available its stateful functions return `MP_ERR_UNAVAILABLE`, invalidate
output handles, and report an empty screen list. The local table is copied up
to the smaller of the provider and local `struct_size` after ABI/version and
required-function validation. Keep that copy only for the current provider
instance.

Every versioned input/output structure must be zero-initialized, set
`struct_size = sizeof(value)`, and set `flags = 0`. The provider accepts known
prefixes and copies only the returned structure size, so this rule keeps code
forward-compatible.

## Screens: find versus create

Use `screen_find` when Endstone or another plugin already owns the screen. It
works for logical, plugin-managed, and world-backed screens. Use `screen_create`
only for a new logical screen; the current provider rejects API creation of
plugin-managed and world backends even though capabilities report those
backends for discovery/update.

API-created logical screens are sparse surfaces. Width and height are tile
counts, each tile is 128×128 pixels, each axis is 1–1024 tiles, and the
registry holds at most 64 screens. Creating a logical screen does not create a
wall or allocate a full framebuffer. Query `capabilities_get` for the current
ABI, tile size, limits, supported pixel-format bits, and backend bits.

An OP player can run `/mpv materialize <name>` to upgrade an eligible logical
screen of at most 7×4 tiles. The player must stand in the same air-and-backing
layout used by `/mpv create`; the discovered backing must exactly match the
logical dimensions. The feet-level air cell defines the bottom row, backing and
floor blocks below that Y are ignored, and every display cell must be air.
Maps are installed left-to-right and top-to-bottom. Materialization preserves
the screen entry, runtime ID, surface generation, and pixels, so the existing
`mp_screen_handle` remains valid while the backend and physical renderer state
change. A logical screen has no physical output until it is materialized.

## Pixel contract

Region `x`, `y`, `width`, and `height` are pixel coordinates. Tile operations
use tile coordinates, and one tile is always 128×128 pixels. Every supported
format has four bytes per pixel. For a region, `stride >= width * 4` and the
minimum buffer size is:

```text
(height - 1) * stride + width * 4
```

The provider borrows the pointer synchronously and copies pixels during the
call; it never retains the caller's buffer. The supported constants are
`MP_PIXEL_FORMAT_ABGR`, `MP_PIXEL_FORMAT_BGRA`, and `MP_PIXEL_FORMAT_RGBA`.
On the supported little-endian targets, the current provider accepts ABGR's
packed representation as direct `R,G,B,A` bytes, and accepts RGBA input as
direct `R,G,B,A` bytes. BGRA input is `B,G,R,A` and is converted to the
internal `R,G,B,A` bytes. Check `caps.pixel_format_bits` instead of assuming a
future provider supports every format.

For bandwidth and allocation reasons, avoid per-pixel calls. Batch adjacent
pixels into regions or stage several regions in one frame transaction.

```c
const uint32_t width = 16;
const uint32_t height = 16;
const uint64_t stride = (uint64_t)width * 4;
const uint64_t bytes = (uint64_t)(height - 1) * stride + width * 4;

mp_result result = mp.screen_update_region(
    screen, 0, 0, width, height, rgba_pixels, bytes, stride,
    MP_PIXEL_FORMAT_RGBA);
```

`screen_update_tile` is the corresponding one-tile operation. Each direct
update is one atomic frame. For multiple regions that must become visible
together, use `frame_begin`, `frame_update_region` or `frame_update_tile`, and
`frame_commit`; on any update or commit error, call `frame_abort` while the
frame is still active. Only one frame can be active for a screen, so contention
returns `MP_ERR_BUSY`. A successful commit or abort consumes the frame handle;
a failed commit intentionally leaves it active so it can be aborted.

## Lifecycle and ownership

The API table carries `instance_id`. MediaPlayer changes it on every enable
cycle and sets it to zero while inactive. Screen and frame handles are opaque;
do not inspect, serialize, or reuse their numeric values across lifecycle
boundaries. Reacquire the table and all handles in each consumer-plugin
`on_enable`/enable callback, and compare the current `instance_id` before
reusing wrappers.

`screen_clear` stops producers, retires active frames, resets resident pixels,
and preserves the screen entry and screen handle. `screen_delete` removes the
entry and invalidates its handle. A frame handle becomes invalid after a
successful commit or abort, screen clear/deletion, or provider
disable/reload. Direct API writes replace an active video, static image, or
soundtrack; starting `/mpv play` or `/mpv image` replaces an active direct
producer. Do not call clear or delete merely to finish a normal update: clear
erases the visible pixels.

The provider owns screen/frame state. Names, strings, structures supplied to a
call, and pixel buffers are borrowed only for that call. The SDK does not
retain any of them.

## C function cheat sheet

| Purpose | V1 function |
| --- | --- |
| Discover provider | `mp_get_api_v1`, `mp_try_get_api_v1`, `result_string` |
| Capabilities | `capabilities_get(mp_capabilities *)` |
| Create/find/list | `screen_create`, `screen_find`, `screen_list` |
| Inspect | `screen_get_info`, `screen_get_stats` |
| Change/remove | `screen_rename`, `screen_clear`, `screen_delete` |
| Direct pixels | `screen_update_region`, `screen_update_tile` |
| Atomic frame | `frame_begin`, `frame_update_region`, `frame_update_tile`, `frame_commit`, `frame_abort` |
| Lifecycle | reacquire the table and handles after enable/reload; no explicit close function |

`screen_list` supports a size query with `capacity = 0`; it writes the required
count. A short supplied array returns `MP_ERR_BUFFER_TOO_SMALL` and also reports
the required count.

The complete callback signatures are in
[`include/endstone_mediaplayer_api.h`](../include/endstone_mediaplayer_api.h).
[`examples/basic_screen.c`](../examples/basic_screen.c) is a callback helper,
not a standalone `main` program, and demonstrates create-or-find, info,
updates, a two-region frame, stats, and temporary-buffer cleanup.

## Results and errors

`MP_OK` is zero. Check every result; `result_string` returns the stable human-
readable name. The V1 results are:

| Result | Meaning |
| --- | --- |
| `MP_ERR_UNAVAILABLE` | Provider is absent, inactive, or the fallback table is in use |
| `MP_ERR_INVALID_ARGUMENT` | Null/malformed argument, unsupported flags, wrong structure size, range, format, or buffer |
| `MP_ERR_INVALID_HANDLE` | Unknown or stale screen/frame handle |
| `MP_ERR_NOT_FOUND` | Named screen does not exist |
| `MP_ERR_ALREADY_EXISTS` | Screen name is already registered |
| `MP_ERR_CAPACITY` | Screen, handle, or pending-resource capacity is exhausted |
| `MP_ERR_NO_MEMORY` | Allocation failed |
| `MP_ERR_UNSUPPORTED` | Requested operation or feature is not supported |
| `MP_ERR_BUFFER_TOO_SMALL` | Caller-provided list/output capacity is insufficient |
| `MP_ERR_BUSY` | A screen already has an active frame or a commit cannot proceed yet |
| `MP_ERR_IO` | Provider I/O failure |
| `MP_ERR_INTERNAL` | Provider-internal failure |

`MP_ERR_INVALID_ARG` and `MP_ERR_EXISTS` remain compatibility aliases for the
corresponding longer names.

## Limits and troubleshooting

`mp_stats` reports logical dimensions, generation, resident and pending tile
counts, limits, dropped tiles, and allocated/staged pixel bytes. The current
capability table reports 4096 maximum resident tiles and 256 pending tiles;
large sparse updates can still consume substantial memory and bandwidth.

- `MP_ERR_UNAVAILABLE`: the provider is disabled, the consumer is in the wrong
  process, or dependency/load order is wrong. Load the consumer after
  `mediaplayer` and reacquire in its enable callback.
- `MP_ERR_INVALID_HANDLE`: a cached handle is stale after provider reload,
  disable, or delete. `screen_clear` preserves the screen handle but
  invalidates any active frame handle.
- `MP_ERR_BUSY`: another frame is active; finish it or abort it before starting
  a new one.
- Black/no physical output: a logical screen has not been materialized. Use
  `/mpv materialize` for eligible 7×4-or-smaller logical screens.
- Slow or memory-heavy updates: avoid per-pixel calls; batch regions or frames,
  check capabilities, and watch `screen_get_stats`.
