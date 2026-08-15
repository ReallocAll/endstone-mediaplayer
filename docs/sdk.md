# MediaPlayer C SDK

`include/endstone_mediaplayer_api.h` is the complete public C23 SDK. It is a
single header with a stable C ABI and a versioned V1 function table. The provider exports
`endstone_mediaplayer_get_api(uint32_t version)` and currently returns a table
only for `MP_API_V1`; an unsupported version returns no table.

The SDK looks up a provider that is already loaded in the current process. It
does not load or unload the MediaPlayer module, and consumers do not link an
import library or the plugin binary. All API calls must run on Endstone's main
thread. Names, strings, and pixel buffers are borrowed only for the duration
of a call.

On Windows, Endstone may shadow-copy the plugin to a loaded module named
`endstone_mediaplayer-<hash>.dll`. The header enumerates loaded process modules
and accepts either that form or `endstone_mediaplayer.dll`, validates the
exported table, and prefers an active provider instance. It never loads a DLL.
On Linux it resolves the process-global export, with a no-load module lookup as
a fallback. These lookup paths are covered by the SDK tests and real BDS smoke
tests.

## Acquire the API

Include the repository's `include` directory and use the header from C23:

```c
#include "endstone_mediaplayer_api.h"

mp_api_v1 mp = mp_get_api_v1();
mp_screen_handle screen = MP_INVALID_SCREEN_HANDLE;
mp_result result = mp.screen_find("live", &screen);
if (result != MP_OK) {
    const char *message = mp.result_string(result);
    // Report or handle message.
}
```

`mp_try_get_api_v1(&mp)` returns `true` only when a valid, already-loaded
provider exposes ABI V1. `mp_get_api_v1()` always returns a callable table. On
failure, the header initializes a fallback table whose stateful functions
return `MP_ERR_UNAVAILABLE`; screen/frame output handles are invalidated and a
screen-list count is set to zero. When a provider is found, the SDK copies the
provider table up to the smaller of its `struct_size` and the local table size
after validating the ABI version and required function pointers. The copied
table is safe to keep for the current provider instance, but its handles still
follow the lifecycle rules below.

Every versioned input/output structure must set `struct_size` to the local
structure size and `flags` to zero. The provider accepts known prefixes and
copies only the returned structure size, allowing the table to evolve without
assuming a newer consumer layout.

## Results and errors

`MP_OK` is zero. Check every result, or use `result_string` for the stable
human-readable name. The complete V1 result set is:

| Result | Meaning |
| --- | --- |
| `MP_ERR_UNAVAILABLE` | Provider is absent, inactive, or the fallback table is in use |
| `MP_ERR_INVALID_ARGUMENT` | Null, malformed, unsupported flags, wrong structure size, range, format, or buffer argument |
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

`MP_ERR_INVALID_ARG` and `MP_ERR_EXISTS` are compatibility aliases for the
corresponding longer names.

## Lifecycle, handles, and reacquisition

The API table carries an `instance_id`. MediaPlayer changes it on every enable
cycle and sets it to zero while inactive. Screen and frame handles are opaque;
do not inspect or serialize their numeric values. A screen handle becomes
stale after screen deletion or provider disable/reload. `screen_clear` keeps
the screen handle but stops its producer and retires active frames. A frame
handle becomes invalid after successful `frame_commit`, `frame_abort`, screen
clear/deletion, or provider disable/reload. The provider also invalidates
output handles on failed acquire/create/find/begin calls where applicable.

Reacquire the API table and all screen handles during each consumer-plugin
enable. Do not cache handles or a table across a MediaPlayer lifecycle change;
use the current `instance_id` to detect a provider reload before reusing a
wrapper or handle.

## Screens, capabilities, and limits

An API-created screen is a logical sparse surface. Each axis is 1 through
1024 tiles, each tile is 128×128 pixels, and the provider registry holds at
most 64 screens. The current capability table reports a maximum of 4096
resident tiles and 256 pending tiles. It also reports support bits for
ABGR8888, BGRA8888, and RGBA8888, and for logical, plugin-managed, and world
backends. `screen_create` currently creates logical screens; plugin-managed or
world screens are existing world-backed screens that can be found and updated
when exposed by the provider.

```c
mp_capabilities caps = {
    .struct_size = sizeof(caps),
    .flags = 0,
};
mp_result result = mp.capabilities_get(&caps);
```

Logical dimensions do not materialize a wall or allocate a full framebuffer.
The separate world `/mpv create` path remains limited to a physical 7×4 map
screen, and MCV1 remains compatible with that path with a fixed maximum 7×4
tile grid. MCV video and
MPS1 static-image playback are producers for those screens; direct API writes
replace an active video, static image, or soundtrack on the target screen, and
starting `/mpv play` or `/mpv image` replaces an active direct producer. Large
logical updates can still consume substantial bandwidth and sparse resident or
pending capacity, so query `screen_get_stats` and `capabilities_get` rather
than assuming that logical size equals materialized map count.

An API-created logical screen can be upgraded in place by an OP player with
`/mpv materialize <name>`. The player must stand in the same air-and-backing
layout used by `/mpv create`, and the discovered backing must exactly match the
logical tile dimensions (up to the physical 7×4 limit). The player's feet-level
air cell defines the bottom screen row; backing and floor blocks below that Y
are ignored, while every actual display cell must be air. Maps are installed
left-to-right and top-to-bottom using their row/column labels. Materialization
preserves the screen entry, runtime ID, Surface generation and pixels, and the
existing `mp_screen_handle` remains valid; only the backend and physical map
renderer state change.

The CRUD and inspection functions are:

- `screen_create` and `screen_delete`
- `screen_find` and `screen_list`
- `screen_get_info`, `screen_get_stats`, and `capabilities_get`
- `screen_rename` and `screen_clear`

`screen_list` supports a size-query call with `capacity = 0`; if the supplied
array is short it returns `MP_ERR_BUFFER_TOO_SMALL` and reports the required
count.

## Pixel updates and formats

Region coordinates and dimensions are pixels. Tile coordinates identify one
128×128 tile. All formats use four bytes per pixel: `MP_PIXEL_FORMAT_ABGR`,
`MP_PIXEL_FORMAT_BGRA`, and `MP_PIXEL_FORMAT_RGBA`. Supply the available byte
count and row stride; the stride must cover `width * 4` bytes and the buffer
must cover the requested rows. The provider copies pixels during the call and
retains no caller pointer.

```c
const uint32_t width = 896;
const uint32_t height = 512;
const uint64_t stride = (uint64_t)width * 4;
const uint64_t bytes = stride * height;

mp_result result = mp.screen_update_region(
    screen, 0, 0, width, height, bgra_pixels, bytes, stride,
    MP_PIXEL_FORMAT_BGRA);
```

The `screen_update_tile` operation takes one tile and its tile coordinates.
Every update is an atomic single-frame operation. For several updates that
must become visible together, use an explicit frame transaction:

```c
mp_frame_handle frame = MP_INVALID_FRAME_HANDLE;
mp_result result = mp.frame_begin(screen, &frame);
if (result == MP_OK) {
    result = mp.frame_update_region(
        frame, 0, 0, width, height, bgra_pixels, bytes, stride,
        MP_PIXEL_FORMAT_BGRA);
    if (result == MP_OK)
        result = mp.frame_commit(frame);
    if (result != MP_OK)
        (void)mp.frame_abort(frame);
}
```

Only one frame may be active for a screen at a time; contention returns
`MP_ERR_BUSY`. A successful commit or abort consumes the frame handle. If a
commit fails, the frame remains active so the caller can abort it.
