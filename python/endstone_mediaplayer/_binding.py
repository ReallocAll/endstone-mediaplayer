"""Public lifecycle-safe wrappers for the MediaPlayer provider ABI."""

from __future__ import annotations

import ctypes
import weakref
from dataclasses import dataclass
from typing import Any

from . import _native


class MediaPlayerError(RuntimeError):
    """A non-zero result returned by the provider."""

    def __init__(self, result: int, message: str | None = None) -> None:
        self.result = int(result)
        self.code = self.result
        self.message = message or "media player error"
        super().__init__(f"{self.message} (result {self.result})")


class ProviderUnavailableError(MediaPlayerError):
    """No compatible already-loaded provider is available."""

    def __init__(self, message: str = "MediaPlayer provider is unavailable") -> None:
        super().__init__(_native.MP_ERR_UNAVAILABLE, message)


class InvalidHandleError(MediaPlayerError):
    pass


class ProviderReloadedError(MediaPlayerError):
    """The provider instance changed after a wrapper was acquired."""

    def __init__(self, message: str = "MediaPlayer provider was reloaded") -> None:
        super().__init__(_native.MP_ERR_UNAVAILABLE, message)


class NotFoundError(MediaPlayerError):
    pass


class InvalidArgumentError(MediaPlayerError):
    pass


class AlreadyExistsError(MediaPlayerError):
    pass


class CapacityError(MediaPlayerError):
    pass


class BusyError(MediaPlayerError):
    pass


class MediaPlayerIOError(MediaPlayerError):
    pass


_ERROR_TYPES = {
    _native.MP_ERR_UNAVAILABLE: ProviderUnavailableError,
    _native.MP_ERR_INVALID_HANDLE: InvalidHandleError,
    _native.MP_ERR_NOT_FOUND: NotFoundError,
    _native.MP_ERR_INVALID_ARGUMENT: InvalidArgumentError,
    _native.MP_ERR_ALREADY_EXISTS: AlreadyExistsError,
    _native.MP_ERR_CAPACITY: CapacityError,
    _native.MP_ERR_BUSY: BusyError,
    _native.MP_ERR_IO: MediaPlayerIOError,
}


@dataclass(frozen=True)
class Capabilities:
    abi_version: int
    max_width_tiles: int
    max_height_tiles: int
    tile_size: int
    max_screens: int
    max_resident_tiles: int
    max_pending_tiles: int
    pixel_format_bits: int
    backend_bits: int


@dataclass(frozen=True)
class ScreenInfo:
    name: str
    handle: int
    instance_id: int
    width_tiles: int
    height_tiles: int
    pixel_width: int
    pixel_height: int
    backend: int
    playing: bool
    surface_generation: int


@dataclass(frozen=True)
class Stats:
    width_tiles: int
    height_tiles: int
    pixel_width: int
    pixel_height: int
    generation: int
    resident_tiles: int
    pending_tiles: int
    max_resident_tiles: int
    max_pending_tiles: int
    dropped_tiles: int
    allocated_pixel_bytes: int
    staged_tiles: int
    allocated_staging_pixel_bytes: int


def _message(api: _native._ApiV1, result: int) -> str:
    try:
        raw = api.result_string(result)
        if raw:
            return raw.decode("utf-8", "replace")
    except (OSError, UnicodeError):
        pass
    return "media player error"


def _raise_result(api: _native._ApiV1, result: int) -> None:
    if result == _native.MP_OK:
        return
    message = _message(api, result)
    if result == _native.MP_ERR_UNAVAILABLE:
        raise ProviderUnavailableError(message)
    exception = _ERROR_TYPES.get(result, MediaPlayerError)
    raise exception(result, message)


def _as_dimension(value: int, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise TypeError(f"{label} must be an integer")
    if value <= 0 or value > 0xFFFFFFFF:
        raise ValueError(f"{label} must be between 1 and 4294967295")
    return value


def _as_coordinate(value: int, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise TypeError(f"{label} must be an integer")
    if value < 0 or value > 0xFFFFFFFF:
        raise ValueError(f"{label} must be between 0 and 4294967295")
    return value


class MediaPlayer:
    """A provider session. Calls must be made on Endstone's main thread."""

    def __init__(self, instance_id: int, keepalive: Any = None) -> None:
        self._instance_id = int(instance_id)
        self._provider_keepalive = keepalive
        self._active = True
        self._wrappers: weakref.WeakSet[Any] = weakref.WeakSet()

    @property
    def instance_id(self) -> int:
        return self._instance_id

    def _register(self, wrapper: Any) -> None:
        self._wrappers.add(wrapper)

    def _resolve(self, expected: int | None = None) -> _native._ResolvedApi:
        if not self._active:
            raise ProviderUnavailableError("MediaPlayer session is closed")
        resolved = _native.resolve()
        if resolved is None:
            raise ProviderUnavailableError()
        if expected is not None and resolved.api.instance_id != expected:
            raise ProviderReloadedError()
        if resolved.keepalive is not None:
            self._provider_keepalive = resolved.keepalive
        return resolved

    def _invoke(self, expected: int | None, callback: str, *args: Any) -> _native._ResolvedApi:
        resolved = self._resolve(expected)
        result = getattr(resolved.api, callback)(*args)
        _raise_result(resolved.api, int(result))
        return resolved

    def close(self) -> None:
        """Invalidate this session and all wrappers acquired from it."""
        self._active = False
        for wrapper in list(self._wrappers):
            wrapper._handle = 0
        self._wrappers.clear()

    def __enter__(self) -> "MediaPlayer":
        self._resolve(self._instance_id)
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> bool:
        self.close()
        return False

    def capabilities(self, _expected: int | None = None) -> Capabilities:
        out = _native._Capabilities()
        out.struct_size = ctypes.sizeof(out)
        out.flags = 0
        self._invoke(self._instance_id if _expected is None else _expected, "capabilities_get", ctypes.byref(out))
        return Capabilities(
            out.abi_version,
            out.max_width_tiles,
            out.max_height_tiles,
            out.tile_size,
            out.max_screens,
            out.max_resident_tiles,
            out.max_pending_tiles,
            out.pixel_format_bits,
            out.backend_bits,
        )

    def create_screen(
        self, name: str, width: int, height: int, backend: str = "logical"
    ) -> "Screen":
        encoded = _native.encode_name(name)
        width = _as_dimension(width, "width")
        height = _as_dimension(height, "height")
        info = _native._ScreenCreateInfo(
            ctypes.sizeof(_native._ScreenCreateInfo),
            0,
            encoded,
            width,
            height,
            _native.backend_value(backend),
        )
        out = ctypes.c_uint64(_native.MP_INVALID_SCREEN_HANDLE)
        resolved = self._invoke(self._instance_id, "screen_create", ctypes.byref(info), ctypes.byref(out))
        if not out.value:
            raise InvalidHandleError(_native.MP_ERR_INVALID_HANDLE, "provider returned an invalid screen handle")
        return Screen(self, int(out.value), int(resolved.api.instance_id))

    def screen(self, name: str) -> "Screen":
        encoded = _native.encode_name(name)
        out = ctypes.c_uint64(_native.MP_INVALID_SCREEN_HANDLE)
        resolved = self._invoke(self._instance_id, "screen_find", encoded, ctypes.byref(out))
        if not out.value:
            raise InvalidHandleError(_native.MP_ERR_INVALID_HANDLE, "provider returned an invalid screen handle")
        return Screen(self, int(out.value), int(resolved.api.instance_id))

    def list_screens(self) -> list["Screen"]:
        resolved = self._resolve(self._instance_id)
        count = ctypes.c_uint32(0)
        result = int(resolved.api.screen_list(None, 0, ctypes.byref(count)))
        _raise_result(resolved.api, result)
        if not count.value:
            return []
        cap = int(count.value)
        handles = (ctypes.c_uint64 * cap)()
        result = int(resolved.api.screen_list(handles, cap, ctypes.byref(count)))
        _raise_result(resolved.api, result)
        return [Screen(self, int(handles[index]), self._instance_id) for index in range(count.value) if handles[index]]

    def screens(self) -> list["Screen"]:
        return self.list_screens()


class _HandleWrapper:
    def __init__(self, parent: MediaPlayer, handle: int, instance_id: int) -> None:
        self._parent = parent
        self._handle = int(handle)
        self._instance_id = int(instance_id)
        parent._register(self)

    def _require_handle(self) -> None:
        if not self._handle:
            raise InvalidHandleError(_native.MP_ERR_INVALID_HANDLE, "handle is closed or invalid")

    def _invoke(self, callback: str, *args: Any) -> _native._ResolvedApi:
        self._require_handle()
        return self._parent._invoke(self._instance_id, callback, self._handle, *args)

    def _buffer(self, pixels: Any, width: int, height: int, stride: int | None) -> tuple[Any, int, int, Any]:
        width = _as_dimension(width, "width")
        height = _as_dimension(height, "height")
        return _native.buffer_pointer(pixels, width, height, stride)


class Screen(_HandleWrapper):
    """An acquired screen handle; calls must remain on the main thread."""

    def info(self) -> ScreenInfo:
        out = _native._ScreenInfo()
        out.struct_size = ctypes.sizeof(out)
        out.flags = 0
        self._invoke("screen_get_info", ctypes.byref(out))
        name = bytes(out.name).split(b"\0", 1)[0].decode("utf-8", "replace")
        return ScreenInfo(name, out.handle, out.instance_id, out.width_tiles, out.height_tiles, out.pixel_width, out.pixel_height, out.backend, bool(out.playing), out.surface_generation)

    def stats(self) -> Stats:
        out = _native._Stats()
        out.struct_size = ctypes.sizeof(out)
        out.flags = 0
        self._invoke("screen_get_stats", ctypes.byref(out))
        return Stats(out.width_tiles, out.height_tiles, out.pixel_width, out.pixel_height, out.generation, out.resident_tiles, out.pending_tiles, out.max_resident_tiles, out.max_pending_tiles, out.dropped_tiles, out.allocated_pixel_bytes, out.staged_tiles, out.allocated_staging_pixel_bytes)

    def rename(self, name: str) -> None:
        self._invoke("screen_rename", _native.encode_name(name))

    def clear(self) -> None:
        self._invoke("screen_clear")

    def delete(self) -> None:
        self._invoke("screen_delete")
        self._handle = 0

    def update(self, pixels: Any, stride: int | None = None, format: str = "abgr8888") -> None:
        info = self.info()
        self.update_region(0, 0, info.pixel_width, info.pixel_height, pixels, stride, format)

    def update_region(self, x: int, y: int, width: int, height: int, pixels: Any, stride: int | None = None, format: str = "abgr8888") -> None:
        x = _as_coordinate(x, "x")
        y = _as_coordinate(y, "y")
        pointer, nbytes, actual_stride, keepalive = self._buffer(pixels, width, height, stride)
        try:
            self._invoke("screen_update_region", x, y, width, height, pointer, nbytes, actual_stride, _native.format_value(format))
        finally:
            del keepalive

    def update_tile(self, tile_x: int, tile_y: int, pixels: Any, stride: int | None = None, format: str = "abgr8888") -> None:
        tile_x = _as_coordinate(tile_x, "tile_x")
        tile_y = _as_coordinate(tile_y, "tile_y")
        tile_size = self._parent.capabilities(self._instance_id).tile_size
        pointer, nbytes, actual_stride, keepalive = self._buffer(pixels, tile_size, tile_size, stride)
        try:
            self._invoke("screen_update_tile", tile_x, tile_y, pointer, nbytes, actual_stride, _native.format_value(format))
        finally:
            del keepalive

    def frame(self) -> "Frame":
        out = ctypes.c_uint64(_native.MP_INVALID_FRAME_HANDLE)
        resolved = self._invoke("frame_begin", ctypes.byref(out))
        if not out.value:
            raise InvalidHandleError(_native.MP_ERR_INVALID_HANDLE, "provider returned an invalid frame handle")
        return Frame(self._parent, int(out.value), int(resolved.api.instance_id))


class Frame(_HandleWrapper):
    """A staged frame transaction. Successful commit or abort consumes it."""

    def update_region(self, x: int, y: int, width: int, height: int, pixels: Any, stride: int | None = None, format: str = "abgr8888") -> None:
        x = _as_coordinate(x, "x")
        y = _as_coordinate(y, "y")
        pointer, nbytes, actual_stride, keepalive = self._buffer(pixels, width, height, stride)
        try:
            self._invoke("frame_update_region", x, y, width, height, pointer, nbytes, actual_stride, _native.format_value(format))
        finally:
            del keepalive

    def update_tile(self, tile_x: int, tile_y: int, pixels: Any, stride: int | None = None, format: str = "abgr8888") -> None:
        tile_x = _as_coordinate(tile_x, "tile_x")
        tile_y = _as_coordinate(tile_y, "tile_y")
        tile_size = self._parent.capabilities(self._instance_id).tile_size
        pointer, nbytes, actual_stride, keepalive = self._buffer(pixels, tile_size, tile_size, stride)
        try:
            self._invoke("frame_update_tile", tile_x, tile_y, pointer, nbytes, actual_stride, _native.format_value(format))
        finally:
            del keepalive

    def commit(self) -> None:
        self._invoke("frame_commit")
        self._handle = 0

    def abort(self) -> None:
        self._invoke("frame_abort")
        self._handle = 0

    def __enter__(self) -> "Frame":
        self._require_handle()
        self._parent._resolve(self._instance_id)
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> bool:
        if exc_type is None:
            try:
                self.commit()
            except BaseException:
                try:
                    self.abort()
                except BaseException:
                    pass
                raise
        else:
            try:
                self.abort()
            except BaseException:
                pass
        return False


def acquire() -> MediaPlayer:
    resolved = _native.resolve()
    if resolved is None:
        raise ProviderUnavailableError()
    return MediaPlayer(int(resolved.api.instance_id), resolved.keepalive)


__all__ = [
    "AlreadyExistsError",
    "BusyError",
    "Capabilities",
    "CapacityError",
    "Frame",
    "InvalidArgumentError",
    "InvalidHandleError",
    "MediaPlayer",
    "MediaPlayerError",
    "MediaPlayerIOError",
    "NotFoundError",
    "ProviderReloadedError",
    "ProviderUnavailableError",
    "Screen",
    "ScreenInfo",
    "Stats",
    "acquire",
]
