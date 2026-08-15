# Python binding

`endstone-mediaplayer` is an installable `ctypes` package for the stable
`endstone_mediaplayer_api.h` provider ABI. It discovers a provider that is
already loaded into the current process; it never loads a second provider or
unloads one. Install it from this directory with:

```shell
python -m pip install .
```

The native provider is supplied by Endstone and is not a package dependency.
`mediaplayer()` raises `ProviderUnavailableError` until the MediaPlayer
provider is loaded and enabled.

On Windows the resolver enumerates loaded modules and accepts Endstone's
shadow-copy name `endstone_mediaplayer-<hash>.dll` as well as the unsuffixed
name, validates each provider table, and prefers an active instance. On Linux
it uses the process-global export with a no-load lookup fallback. Neither path
loads a native plugin.

## Acquire a screen and update pixels

```python
from endstone_mediaplayer import mediaplayer

mp = mediaplayer()
screen = mp.screen("my-screen")
info = screen.info()

# Four bytes per pixel; a bytearray is writable and C-contiguous.
pixels = bytearray(info.pixel_width * info.pixel_height * 4)
screen.update(pixels, format="bgra8888")

mp.close()
```

`screen.info()` returns the tile and pixel dimensions, backend, playing state,
surface generation, and provider `instance_id`. `screen.update_region()` uses
pixel coordinates; `screen.update_tile()` uses tile coordinates and one
128×128 tile. Supported formats are `abgr8888`, `bgra8888`, and `rgba8888`.

Updates accept `bytes`, `bytearray`, `memoryview`, and other buffer-protocol
objects, including NumPy arrays when NumPy is installed. NumPy is not imported
or required by this package. The buffer must be C-contiguous and byte-
compatible, contain four bytes per pixel, and contain enough rows for the
requested region. The default stride is `width * 4`; pass `stride=` for padded
rows. Read-only buffers are copied for the duration of the native call;
writable buffers are borrowed synchronously and are not retained.

## Errors and provider lifecycle

Native non-zero results become typed exceptions:

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
| Other documented results | `MediaPlayerError` |

Local Python validation raises `TypeError` or `ValueError` before entering the
provider. `ProviderReloadedError` is raised when the provider's `instance_id`
changes after a `MediaPlayer` object or screen/frame wrapper was acquired.
Native handles are opaque and become stale after deletion, clear where
applicable, frame commit/abort, provider disable, or provider reload. Reacquire
with `mediaplayer()` after a provider reload or enable cycle; do not keep old
screen wrappers across that lifecycle boundary.

All provider calls must run on Endstone's main thread. The binding is not
thread-safe and deliberately adds no locks. `mp.close()` invalidates the
session and all wrappers acquired from it; it does not unload the provider.
The session can also be used as a context manager:

```python
from endstone_mediaplayer import mediaplayer

with mediaplayer() as mp:
    screen = mp.screen("my-screen")
    screen.clear()
# The session and its screen wrappers are closed here.
```

## Atomic frame transactions

Use `screen.frame()` when several tile or region updates must be committed
together. Normal context-manager exit commits; an exception aborts. A
successful commit or abort consumes the frame object.

```python
with mediaplayer() as mp:
    screen = mp.screen("my-screen")
    with screen.frame() as frame:
        frame.update_region(0, 0, 128, 128, bytes(128 * 128 * 4),
                            format="abgr8888")
        # Add more frame.update_region(...) or frame.update_tile(...) calls.
    # The frame is committed here; an exception would abort it.
```

Only one frame may be active for a screen at a time. If commit fails, the
binding attempts to abort the transaction before propagating the exception.
