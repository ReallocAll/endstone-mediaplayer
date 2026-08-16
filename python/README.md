# MediaPlayer Python binding

## Quick start

1. Install/deploy the native MediaPlayer provider with Endstone.
2. Declare and load this consumer after the hard dependency `mediaplayer`.
3. Execute binding calls only in the same Endstone process and on its main
   thread.
4. Reacquire `mediaplayer()` and all screen wrappers on every enable/reload.

This package is for an Endstone consumer plugin. It discovers a provider
already loaded in the server process; it does not load a native module or
unload one. A normal external Python interpreter/process cannot discover the
provider. The example in [`../examples/basic_screen.py`](../examples/basic_screen.py)
is a callback body for an Endstone plugin, not a standalone script.

Install the Python package from this directory when packaging the consumer:

```shell
python -m pip install .
```

On Windows the resolver recognizes Endstone's shadow-copy module name
`endstone_mediaplayer-<hash>.dll` as well as `endstone_mediaplayer.dll`; on
Linux it uses the process-global export with a no-load module lookup fallback.
Neither path loads a second provider. The resolver preserves the provider's
`instance_id`, which identifies one enable cycle.

## Endstone plugin example

Use a real Endstone `Plugin` subclass and keep all calls in `on_enable`, event,
or scheduled callbacks that Endstone runs on its main thread. The complete
example includes create-or-find, a direct update, an atomic two-region frame,
stats, and `on_disable` cleanup:

```python
from endstone.plugin import Plugin
from endstone_mediaplayer import AlreadyExistsError, NotFoundError, mediaplayer


def get_or_create_screen(mp, name: str):
    try:
        return mp.screen(name)                 # Find existing logical/physical screen.
    except NotFoundError:
        try:
            return mp.create_screen(name, 1, 1, backend="logical")
        except AlreadyExistsError:            # Another plugin won the race.
            return mp.screen(name)


class BasicScreenPlugin(Plugin):
    depend = ["mediaplayer"]

    def on_enable(self):
        self._mp = mediaplayer()
        self._screen = get_or_create_screen(self._mp, "basic-screen")

        # RGBA8888 is accepted as direct R,G,B,A bytes by the current provider.
        left = bytearray([255, 32, 32, 255] * (8 * 8))
        right = bytearray([32, 32, 255, 255] * (8 * 8))
        self._screen.update_region(0, 0, 8, 8, left, format="rgba8888")

        # Context-manager exit commits; an exception aborts the active frame.
        with self._screen.frame() as frame:
            frame.update_region(0, 0, 8, 8, left, format="rgba8888")
            frame.update_region(8, 0, 8, 8, right, format="rgba8888")

        info = self._screen.info()
        stats = self._screen.stats()
        self.logger.info(
            f"{info.name}: {info.pixel_width}x{info.pixel_height}, "
            f"resident={stats.resident_tiles}, generation={stats.generation}"
        )

    def on_disable(self):
        if getattr(self, "_mp", None) is not None:
            self._mp.close()
            self._screen = None
            self._mp = None
```

Do not clear or delete in `on_disable` by default: `clear()` stops producers,
retires active frames, and resets visible pixels, while `delete()` removes the
screen. Closing the wrapper only invalidates this Python session and its
wrappers; it does not unload the provider. Reacquire after the next enable or
provider reload.

## Find, create, and materialize

`mp.screen(name)` finds an existing screen, including plugin-managed and
world-backed screens. `mp.create_screen(name, width, height,
backend="logical")` creates only a logical screen in the current provider;
passing `plugin_managed`/`plugin` or `world` asks for an unsupported API create
operation. Find those provider- or world-owned screens, then update them.

Logical dimensions are tile counts. Each tile is 128×128 pixels, each axis is
currently 1–1024 tiles, and at most 64 screens are registered. A logical
screen is sparse and has no physical wall by itself. An OP player can run
`/mpv materialize <name>` to upgrade an eligible logical screen of at most 7×4
tiles. The player must use the same air-and-backing layout as `/mpv create`,
with exact logical dimensions. Materialization preserves the screen entry,
runtime ID, generation, and pixels; an existing screen wrapper remains valid.

## Pixels and frames

`Screen.update_region(x, y, width, height, pixels, stride=None,
format="abgr8888")` uses pixel coordinates. `Screen.update_tile(tile_x,
tile_y, pixels, stride=None, format="abgr8888")` uses tile coordinates and one
128×128 tile. `Screen.update(pixels, ...)` updates the whole logical surface.
`Frame.update_region` and `Frame.update_tile` have the same pixel contract.

Every format has four bytes per pixel. The stride must be at least `width * 4`
and the minimum buffer size is `(height - 1) * stride + width * 4`. Buffers
must be C-contiguous and byte-compatible. Read-only buffers are copied for the
native call; writable buffers are borrowed synchronously and no pointer is
retained. The current little-endian provider treats `abgr8888` and `rgba8888`
input as direct `R,G,B,A` bytes. `bgra8888` input is `B,G,R,A` and is
converted. Check `mp.capabilities().pixel_format_bits` for capability bits
before selecting a format.

Each direct update is atomic. Use `with screen.frame() as frame:` for several
regions or tiles that must appear together. Normal exit commits; an exception
aborts. Only one frame may be active per screen (`BusyError`); a successful
commit or abort consumes the frame. Batch regions or frames rather than making
per-pixel calls to limit bandwidth and resident/pending allocations.

## Lifecycle, ownership, and method cheat sheet

| Purpose | Python API |
| --- | --- |
| Acquire/reacquire | `mediaplayer()`; use `instance_id` to detect reloads |
| Capabilities | `mp.capabilities()` |
| Create/find/list | `mp.create_screen(...)`, `mp.screen(name)`, `mp.list_screens()` or `mp.screens()` |
| Inspect | `screen.info()`, `screen.stats()` |
| Rename/clear/delete | `screen.rename(name)`, `screen.clear()`, `screen.delete()` |
| Direct pixels | `screen.update(...)`, `screen.update_region(...)`, `screen.update_tile(...)` |
| Atomic frame | `screen.frame()`, `frame.update_region(...)`, `frame.update_tile(...)`, `frame.commit()`, `frame.abort()` |
| Close/reacquire | `mp.close()` invalidates this session and wrappers; call `mediaplayer()` again after enable/reload |

`ScreenInfo` contains `name`, `handle`, `instance_id`, tile and pixel
dimensions, `backend`, `playing`, and `surface_generation`. `Stats` contains
dimensions, `generation`, resident/pending counts and limits, dropped tiles,
and allocated/staged pixel bytes. `Capabilities` contains ABI version, tile
size, screen/tile limits, pixel-format bits, and backend bits.

`clear()` stops producers, retires active frames, resets resident pixels, and
preserves the screen handle. `delete()` invalidates the screen handle. Frame
handles become stale after commit/abort, clear/delete, or provider
disable/reload. Do not keep a `MediaPlayer`, `Screen`, or `Frame` across a
provider lifecycle boundary.

## Errors

Native non-zero results are raised as follows:

| Native result | Python exception |
| --- | --- |
| `MP_ERR_UNAVAILABLE` | `ProviderUnavailableError` |
| `MP_ERR_INVALID_HANDLE` | `InvalidHandleError` |
| `MP_ERR_NOT_FOUND` | `NotFoundError` |
| `MP_ERR_INVALID_ARGUMENT` | `InvalidArgumentError` |
| `MP_ERR_ALREADY_EXISTS` | `AlreadyExistsError` |
| `MP_ERR_CAPACITY` | `CapacityError` |
| `MP_ERR_BUSY` | `BusyError` |
| `MP_ERR_IO` | `MediaPlayerIOError` |
| `MP_ERR_NO_MEMORY`, `MP_ERR_UNSUPPORTED`, `MP_ERR_BUFFER_TOO_SMALL`, `MP_ERR_INTERNAL` | `MediaPlayerError` |

Every native-result exception has `.result` and `.code` (the integer native
result). `ProviderReloadedError` is also a `MediaPlayerError` raised when the
provider `instance_id` changes. Local validation raises `TypeError` or
`ValueError` before entering the provider, for example for a bad name, format,
coordinate, stride, or buffer.

## Troubleshooting

- `ProviderUnavailableError`: the provider is disabled, the plugin is in the
  wrong process, or dependency/load order is wrong. Declare `depend =
  ["mediaplayer"]`, load after it, and acquire in `on_enable`.
- `InvalidHandleError` or `ProviderReloadedError`: a wrapper is stale after
  disable/reload, delete, or session close. `screen.clear()` preserves its
  screen wrapper but invalidates an active frame wrapper.
- `BusyError`: an active frame must be committed or aborted before another
  frame starts.
- Black/no physical output: logical screens need `/mpv materialize`; eligible
  screens are at most 7×4.
- Slow or memory-heavy output: use larger region updates or frame batches,
  watch `screen.stats()`, and avoid per-pixel calls.
