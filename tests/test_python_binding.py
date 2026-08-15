import ctypes
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

SOURCE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(SOURCE_ROOT, "python"))
MOCK_PROVIDER_DIRECTORY = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None

import endstone_mediaplayer as public
from endstone_mediaplayer import _native


class NativeResolveTest(unittest.TestCase):
    def test_resolve_rejects_short_api_table_before_full_copy(self):
        short_table = _native._ApiHeader()
        short_table.abi_version = _native.MP_API_V1
        short_table.struct_size = ctypes.sizeof(short_table)
        short_table.instance_id = 1

        get_api_type = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_uint32)

        def get_api(version):
            if version != _native.MP_API_V1:
                return 0
            return ctypes.addressof(short_table)

        get_api_callback = get_api_type(get_api)
        keepalive = (get_api_callback, short_table)
        address = ctypes.cast(get_api_callback, ctypes.c_void_p).value
        requested_sizes = []
        string_at = ctypes.string_at

        def tracking_string_at(table_address, size):
            requested_sizes.append(size)
            return string_at(table_address, size)

        with mock.patch.object(
            _native,
            "_loaded_symbols",
            return_value=[(address, keepalive)],
        ), mock.patch.object(
            _native.ctypes,
            "string_at",
            side_effect=tracking_string_at,
        ):
            self.assertIsNone(_native.resolve())

        self.assertEqual(ctypes.sizeof(_native._ApiHeader), 16)
        self.assertEqual(requested_sizes, [ctypes.sizeof(_native._ApiHeader)])


class PythonBindingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with unittest.TestCase().assertRaises(public.ProviderUnavailableError):
            public.mediaplayer()
        if MOCK_PROVIDER_DIRECTORY is None:
            raise RuntimeError("mock provider directory argument is required")
        directory = MOCK_PROVIDER_DIRECTORY
        filename = "endstone_mediaplayer.dll" if sys.platform.startswith("win") else "endstone_mediaplayer.so"
        path = os.path.join(directory, filename)
        if sys.platform.startswith("win"):
            cls.provider_directory = tempfile.TemporaryDirectory()
            shadow = os.path.join(cls.provider_directory.name, "endstone_mediaplayer-python-shadow.dll")
            shutil.copy2(path, shadow)
            cls.provider = ctypes.CDLL(shadow)
        else:
            cls.provider = ctypes.CDLL(path, mode=getattr(ctypes, "RTLD_GLOBAL", 0))
        cls.provider.mock_mediaplayer_reset.restype = None
        cls.provider.mock_mediaplayer_reset()
        cls.provider.mock_mediaplayer_set_instance.argtypes = [ctypes.c_uint64]
        cls.provider.mock_mediaplayer_set_instance.restype = None
        cls.provider.mock_mediaplayer_last_buffer_bytes.restype = ctypes.c_uint64
        cls.provider.mock_mediaplayer_last_stride.restype = ctypes.c_uint64
        cls.provider.mock_mediaplayer_last_format.restype = ctypes.c_uint32
        cls.provider.mock_mediaplayer_last_first_byte.restype = ctypes.c_uint32
        cls.provider.mock_mediaplayer_set_unavailable.argtypes = [ctypes.c_uint32]
        cls.provider.mock_mediaplayer_set_unavailable.restype = None
        cls.provider.mock_mediaplayer_invalidate_screen.argtypes = []
        cls.provider.mock_mediaplayer_invalidate_screen.restype = None
        cls.provider.mock_mediaplayer_set_fail_commit.argtypes = [ctypes.c_uint32]
        cls.provider.mock_mediaplayer_set_fail_commit.restype = None
        cls.provider.mock_mediaplayer_abort_count.restype = ctypes.c_uint32

    @classmethod
    def tearDownClass(cls):
        if sys.platform.startswith("win"):
            handle = cls.provider._handle
            del cls.provider
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(handle))
            cls.provider_directory.cleanup()

    def setUp(self):
        self.provider.mock_mediaplayer_reset()
        self.mp = public.mediaplayer()

    def tearDown(self):
        self.mp.close()

    def test_layout_and_acquisition(self):
        self.assertEqual(ctypes.sizeof(_native._ApiHeader), 16)
        self.assertEqual(ctypes.sizeof(_native._ScreenCreateInfo), 32)
        self.assertEqual(ctypes.sizeof(_native._ScreenInfo), 120)
        self.assertEqual(ctypes.sizeof(_native._Stats), 96)
        self.assertEqual(ctypes.sizeof(_native._Capabilities), 44)
        self.assertEqual(ctypes.sizeof(_native._ApiV1), 152)
        caps = self.mp.capabilities()
        self.assertEqual(caps.abi_version, 1)
        self.assertEqual(caps.tile_size, 1)
        screen = self.mp.screen("mock-screen")
        info = screen.info()
        self.assertEqual((info.pixel_width, info.pixel_height), (1, 1))
        self.assertEqual(screen.stats().pixel_width, 1)
        self.assertEqual(len(self.mp.screens()), 1)
        with self.assertRaises(public.NotFoundError):
            self.mp.screen("missing")
        screen.rename("renamed")
        self.assertEqual(screen.info().name, "renamed")
        screen.clear()
        screen.delete()
        with self.assertRaises(public.InvalidHandleError):
            screen.info()
        created = self.mp.create_screen("created", 1, 1)
        self.assertEqual(created.info().name, "created")

    def test_bytes_and_writable_updates(self):
        screen = self.mp.screen("mock-screen")
        screen.update(b"\x01\x02\x03\x04", format="RGBA8888")
        self.assertEqual(self.provider.mock_mediaplayer_last_buffer_bytes(), 4)
        self.assertEqual(self.provider.mock_mediaplayer_last_stride(), 4)
        self.assertEqual(self.provider.mock_mediaplayer_last_format(), _native.MP_PIXEL_FORMAT_RGBA)
        self.assertEqual(self.provider.mock_mediaplayer_last_first_byte(), 1)
        writable = bytearray(4)
        writable[0] = 0xB2
        screen.update_region(0, 0, 1, 1, writable, format="bgra8888")
        self.assertEqual(self.provider.mock_mediaplayer_last_format(), _native.MP_PIXEL_FORMAT_BGRA)
        self.assertEqual(self.provider.mock_mediaplayer_last_first_byte(), 0xB2)
        screen.update_tile(0, 0, memoryview(writable))
        self.assertEqual(self.provider.mock_mediaplayer_last_buffer_bytes(), 4)
        with self.assertRaises(ValueError):
            screen.update(b"\0\0\0", stride=4)
        with self.assertRaises(ValueError):
            screen.update(memoryview(bytearray(8))[::2])

    def test_frame_context_commit_and_abort(self):
        screen = self.mp.screen("mock-screen")
        with screen.frame() as frame:
            frame.update_region(0, 0, 1, 1, bytes(4))
        with self.assertRaises(public.InvalidHandleError):
            frame.commit()
        frame = screen.frame()
        try:
            with frame:
                raise RuntimeError("body")
        except RuntimeError:
            pass
        with self.assertRaises(public.InvalidHandleError):
            frame.abort()

    def test_provider_errors_and_failed_commit_abort(self):
        self.provider.mock_mediaplayer_set_unavailable(1)
        with self.assertRaises(public.ProviderUnavailableError):
            self.mp.screen("mock-screen")
        self.provider.mock_mediaplayer_set_unavailable(0)
        screen = self.mp.screen("mock-screen")
        self.provider.mock_mediaplayer_invalidate_screen()
        with self.assertRaises(public.InvalidHandleError):
            screen.info()
        self.provider.mock_mediaplayer_reset()
        screen = self.mp.screen("mock-screen")
        self.provider.mock_mediaplayer_set_fail_commit(1)
        with self.assertRaises(public.BusyError), screen.frame():
            pass
        self.assertEqual(self.provider.mock_mediaplayer_abort_count(), 1)

    def test_instance_change_is_stale(self):
        screen = self.mp.screen("mock-screen")
        self.provider.mock_mediaplayer_set_instance(0x4343)
        with self.assertRaises(public.ProviderReloadedError):
            screen.info()
        with self.assertRaises(public.ProviderReloadedError):
            self.mp.capabilities()


if __name__ == "__main__":
    unittest.main()
