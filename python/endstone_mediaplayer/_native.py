"""Private ctypes ABI definitions and already-loaded provider lookup."""

from __future__ import annotations

import ctypes
import os
import sys
from typing import Any

MP_API_V1 = 1
MP_SCREEN_NAME_SIZE = 64
MP_INVALID_SCREEN_HANDLE = 0
MP_INVALID_FRAME_HANDLE = 0

MP_OK = 0
MP_ERR_UNAVAILABLE = -1
MP_ERR_INVALID_ARGUMENT = -2
MP_ERR_INVALID_HANDLE = -3
MP_ERR_NOT_FOUND = -4
MP_ERR_ALREADY_EXISTS = -5
MP_ERR_CAPACITY = -6
MP_ERR_NO_MEMORY = -7
MP_ERR_UNSUPPORTED = -8
MP_ERR_BUFFER_TOO_SMALL = -9
MP_ERR_BUSY = -10
MP_ERR_IO = -11
MP_ERR_INTERNAL = -12

MP_PIXEL_FORMAT_ABGR = 0
MP_PIXEL_FORMAT_BGRA = 1
MP_PIXEL_FORMAT_RGBA = 2
MP_BACKEND_LOGICAL = 0
MP_BACKEND_PLUGIN_MANAGED = 1
MP_BACKEND_WORLD = 2

_c_uint64 = ctypes.c_uint64
_c_uint32 = ctypes.c_uint32
_c_int32 = ctypes.c_int32
_c_void_p = ctypes.c_void_p

_CALL = ctypes.CFUNCTYPE

class _ScreenCreateInfo(ctypes.Structure):
    pass


class _ScreenInfo(ctypes.Structure):
    pass


class _Stats(ctypes.Structure):
    pass


class _Capabilities(ctypes.Structure):
    pass


_ResultStringFn = _CALL(ctypes.c_char_p, _c_int32)
_CapabilitiesGetFn = _CALL(_c_int32, ctypes.POINTER(_Capabilities))
_ScreenCreateFn = _CALL(
    _c_int32, ctypes.POINTER(_ScreenCreateInfo), ctypes.POINTER(_c_uint64)
)
_ScreenDeleteFn = _CALL(_c_int32, _c_uint64)
_ScreenFindFn = _CALL(_c_int32, ctypes.c_char_p, ctypes.POINTER(_c_uint64))
_ScreenListFn = _CALL(
    _c_int32, ctypes.POINTER(_c_uint64), _c_uint32, ctypes.POINTER(_c_uint32)
)
_ScreenInfoFn = _CALL(_c_int32, _c_uint64, ctypes.POINTER(_ScreenInfo))
_ScreenRenameFn = _CALL(_c_int32, _c_uint64, ctypes.c_char_p)
_ScreenClearFn = _CALL(_c_int32, _c_uint64)
_ScreenStatsFn = _CALL(_c_int32, _c_uint64, ctypes.POINTER(_Stats))
_ScreenUpdateTileFn = _CALL(
    _c_int32,
    _c_uint64,
    _c_uint32,
    _c_uint32,
    _c_void_p,
    _c_uint64,
    _c_uint64,
    _c_uint32,
)
_ScreenUpdateRegionFn = _CALL(
    _c_int32,
    _c_uint64,
    _c_uint32,
    _c_uint32,
    _c_uint32,
    _c_uint32,
    _c_void_p,
    _c_uint64,
    _c_uint64,
    _c_uint32,
)
_FrameBeginFn = _CALL(_c_int32, _c_uint64, ctypes.POINTER(_c_uint64))
_FrameUpdateTileFn = _CALL(
    _c_int32,
    _c_uint64,
    _c_uint32,
    _c_uint32,
    _c_void_p,
    _c_uint64,
    _c_uint64,
    _c_uint32,
)
_FrameUpdateRegionFn = _CALL(
    _c_int32,
    _c_uint64,
    _c_uint32,
    _c_uint32,
    _c_uint32,
    _c_uint32,
    _c_void_p,
    _c_uint64,
    _c_uint64,
    _c_uint32,
)
_FrameFn = _CALL(_c_int32, _c_uint64)


_ScreenCreateInfo._fields_ = [
    ("struct_size", _c_uint32),
    ("flags", _c_uint32),
    ("name", ctypes.c_char_p),
    ("width_tiles", _c_uint32),
    ("height_tiles", _c_uint32),
    ("backend", _c_uint32),
]

_ScreenInfo._fields_ = [
    ("struct_size", _c_uint32),
    ("flags", _c_uint32),
    ("name", ctypes.c_char * MP_SCREEN_NAME_SIZE),
    ("handle", _c_uint64),
    ("instance_id", _c_uint64),
    ("width_tiles", _c_uint32),
    ("height_tiles", _c_uint32),
    ("pixel_width", _c_uint32),
    ("pixel_height", _c_uint32),
    ("backend", _c_uint32),
    ("playing", _c_uint32),
    ("surface_generation", _c_uint64),
]

_Stats._fields_ = [
    ("struct_size", _c_uint32),
    ("flags", _c_uint32),
    ("width_tiles", _c_uint32),
    ("height_tiles", _c_uint32),
    ("pixel_width", _c_uint32),
    ("pixel_height", _c_uint32),
    ("generation", _c_uint64),
    ("resident_tiles", _c_uint64),
    ("pending_tiles", _c_uint64),
    ("max_resident_tiles", _c_uint64),
    ("max_pending_tiles", _c_uint64),
    ("dropped_tiles", _c_uint64),
    ("allocated_pixel_bytes", _c_uint64),
    ("staged_tiles", _c_uint64),
    ("allocated_staging_pixel_bytes", _c_uint64),
]

_Capabilities._fields_ = [
    ("struct_size", _c_uint32),
    ("flags", _c_uint32),
    ("abi_version", _c_uint32),
    ("max_width_tiles", _c_uint32),
    ("max_height_tiles", _c_uint32),
    ("tile_size", _c_uint32),
    ("max_screens", _c_uint32),
    ("max_resident_tiles", _c_uint32),
    ("max_pending_tiles", _c_uint32),
    ("pixel_format_bits", _c_uint32),
    ("backend_bits", _c_uint32),
]


class _ApiV1(ctypes.Structure):
    _fields_ = [
        ("abi_version", _c_uint32),
        ("struct_size", _c_uint32),
        ("instance_id", _c_uint64),
        ("result_string", _ResultStringFn),
        ("capabilities_get", _CapabilitiesGetFn),
        ("screen_create", _ScreenCreateFn),
        ("screen_delete", _ScreenDeleteFn),
        ("screen_find", _ScreenFindFn),
        ("screen_list", _ScreenListFn),
        ("screen_get_info", _ScreenInfoFn),
        ("screen_rename", _ScreenRenameFn),
        ("screen_clear", _ScreenClearFn),
        ("screen_get_stats", _ScreenStatsFn),
        ("screen_update_tile", _ScreenUpdateTileFn),
        ("screen_update_region", _ScreenUpdateRegionFn),
        ("frame_begin", _FrameBeginFn),
        ("frame_update_tile", _FrameUpdateTileFn),
        ("frame_update_region", _FrameUpdateRegionFn),
        ("frame_commit", _FrameFn),
        ("frame_abort", _FrameFn),
    ]


class _ApiHeader(ctypes.Structure):
    _fields_ = [
        ("abi_version", _c_uint32),
        ("struct_size", _c_uint32),
        ("instance_id", _c_uint64),
    ]


class _ResolvedApi:
    __slots__ = ("api", "keepalive")

    def __init__(self, api: _ApiV1, keepalive: Any = None) -> None:
        self.api = api
        self.keepalive = keepalive


def _loaded_symbols() -> list[tuple[int, Any]]:
    """Return candidate exports from already-loaded provider modules."""
    if sys.platform.startswith("win"):
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        get_module_handle = kernel32.GetModuleHandleW
        get_module_handle.argtypes = [ctypes.c_wchar_p]
        get_module_handle.restype = ctypes.c_void_p
        get_proc_address = kernel32.GetProcAddress
        get_proc_address.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        get_proc_address.restype = ctypes.c_void_p
        modules: list[int] = []
        exact = get_module_handle("endstone_mediaplayer.dll")
        if exact:
            modules.append(int(exact))

        try:
            enum_modules = kernel32.K32EnumProcessModules
        except AttributeError:
            enum_modules = None
        if enum_modules is not None:
            enum_modules.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
            enum_modules.restype = ctypes.c_int
            get_current_process = kernel32.GetCurrentProcess
            get_current_process.argtypes = []
            get_current_process.restype = ctypes.c_void_p
            get_module_filename = kernel32.GetModuleFileNameW
            get_module_filename.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32]
            get_module_filename.restype = ctypes.c_uint32
            capacity = 64
            while True:
                handles = (ctypes.c_void_p * capacity)()
                needed = ctypes.c_uint32()
                if not enum_modules(get_current_process(), handles, ctypes.sizeof(handles), ctypes.byref(needed)):
                    break
                count = needed.value // ctypes.sizeof(ctypes.c_void_p)
                if count <= capacity:
                    for index in range(count):
                        module = int(handles[index] or 0)
                        if not module or module in modules:
                            continue
                        path = ctypes.create_unicode_buffer(32768)
                        if not get_module_filename(module, path, len(path)):
                            continue
                        name = os.path.basename(path.value).lower()
                        if name == "endstone_mediaplayer.dll" or (
                            name.startswith("endstone_mediaplayer-") and name.endswith(".dll")
                        ):
                            modules.append(module)
                    break
                capacity = count

        candidates: list[tuple[int, Any]] = []
        for module in modules:
            address = get_proc_address(module, b"endstone_mediaplayer_get_api")
            if address:
                candidates.append((int(address), module))
        return candidates

    process = ctypes.CDLL(None)
    try:
        symbol = process.endstone_mediaplayer_get_api
    except AttributeError:
        symbol = None
    if symbol is not None:
        address = ctypes.cast(symbol, ctypes.c_void_p).value
        return [(int(address), process)] if address else []

    # This mirrors the SDK's RTLD_NOLOAD fallback: it can inspect a module
    # already present in the process without loading a duplicate provider.
    try:
        mode = os.RTLD_NOW | os.RTLD_NOLOAD
        module = ctypes.CDLL("endstone_mediaplayer.so", mode=mode)
    except (AttributeError, OSError):
        return []
    try:
        symbol = module.endstone_mediaplayer_get_api
    except AttributeError:
        return []
    address = ctypes.cast(symbol, ctypes.c_void_p).value
    return [(int(address), module)] if address else []


def resolve() -> _ResolvedApi | None:
    """Resolve and copy the complete V1 table from an already-loaded module."""
    fields = (
        "result_string",
        "capabilities_get",
        "screen_create",
        "screen_delete",
        "screen_find",
        "screen_list",
        "screen_get_info",
        "screen_rename",
        "screen_clear",
        "screen_get_stats",
        "screen_update_tile",
        "screen_update_region",
        "frame_begin",
        "frame_update_tile",
        "frame_update_region",
        "frame_commit",
        "frame_abort",
    )
    for address, keepalive in _loaded_symbols():
        get_api = _CALL(ctypes.c_void_p, _c_uint32)(address)
        table_address = get_api(MP_API_V1)
        if not table_address:
            continue
        try:
            header_raw = ctypes.string_at(table_address, ctypes.sizeof(_ApiHeader))
            header = _ApiHeader.from_buffer_copy(header_raw)
        except (ValueError, OSError):
            continue
        if header.abi_version != MP_API_V1:
            continue
        if header.struct_size < ctypes.sizeof(_ApiV1) or not header.instance_id:
            continue
        try:
            raw = ctypes.string_at(table_address, ctypes.sizeof(_ApiV1))
            api = _ApiV1.from_buffer_copy(raw)
        except (ValueError, OSError):
            continue
        if api.abi_version != MP_API_V1 or api.struct_size < ctypes.sizeof(_ApiV1):
            continue
        if not api.instance_id or any(not getattr(api, field) for field in fields):
            continue
        return _ResolvedApi(api, keepalive)
    return None


def buffer_pointer(value: Any, width: int, height: int, stride: int | None) -> tuple[_c_void_p, int, int, Any]:
    """Validate a pixel buffer and return (pointer, bytes, stride, owner)."""
    try:
        view = memoryview(value)
    except TypeError as exc:
        raise TypeError("pixels must support the buffer protocol") from exc
    if not view.c_contiguous:
        raise ValueError("pixels must be C-contiguous")
    try:
        byte_view = view.cast("B")
    except TypeError as exc:
        raise TypeError("pixels must be byte-compatible") from exc
    row_bytes = width * 4
    actual_stride = row_bytes if stride is None else stride
    if not isinstance(actual_stride, int) or isinstance(actual_stride, bool):
        raise TypeError("stride must be an integer")
    if actual_stride < row_bytes:
        raise ValueError("stride is smaller than one pixel row")
    required = actual_stride * (height - 1) + row_bytes
    if required <= 0 or byte_view.nbytes < required:
        raise ValueError("pixels does not contain enough bytes for the requested image")
    if byte_view.readonly:
        owner = ctypes.create_string_buffer(byte_view[:required].tobytes(), required)
    else:
        owner = (ctypes.c_ubyte * byte_view.nbytes).from_buffer(byte_view)
    return ctypes.cast(owner, _c_void_p), required, actual_stride, (view, byte_view, owner)


def encode_name(name: str) -> bytes:
    if not isinstance(name, str):
        raise TypeError("name must be a string")
    encoded = name.encode("utf-8")
    if not encoded or b"\0" in encoded or len(encoded) >= MP_SCREEN_NAME_SIZE:
        raise ValueError("name must be non-empty and at most 63 UTF-8 bytes")
    return encoded


def format_value(value: str) -> int:
    if not isinstance(value, str):
        raise TypeError("format must be a string")
    formats = {"abgr8888": MP_PIXEL_FORMAT_ABGR, "bgra8888": MP_PIXEL_FORMAT_BGRA, "rgba8888": MP_PIXEL_FORMAT_RGBA}
    try:
        return formats[value.lower()]
    except KeyError as exc:
        raise ValueError("format must be abgr8888, bgra8888, or rgba8888") from exc


def backend_value(value: str) -> int:
    if not isinstance(value, str):
        raise TypeError("backend must be a string")
    backends = {"logical": MP_BACKEND_LOGICAL, "plugin_managed": MP_BACKEND_PLUGIN_MANAGED, "plugin": MP_BACKEND_PLUGIN_MANAGED, "world": MP_BACKEND_WORLD}
    try:
        return backends[value.lower()]
    except KeyError as exc:
        raise ValueError("backend must be logical, plugin_managed, or world") from exc


__all__ = []
