#!/usr/bin/env python3
"""Tests for the Pillow-independent MPS1 writer and Pillow adapter."""

import gc
import importlib.util
import struct
import tempfile
import tracemalloc
import unittest
import zlib
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "convert_image.py"
SPEC = importlib.util.spec_from_file_location("convert_image", MODULE_PATH)
assert SPEC and SPEC.loader
cv = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cv)


def pattern_tile(seed):
    return bytes((seed + (index * 17)) & 0xFF for index in range(cv.MPS_TILE_BYTES))


def parse_container(path):
    blob = Path(path).read_bytes()
    fields = cv.HEADER_STRUCT.unpack(blob[:cv.HEADER_STRUCT.size])
    header_crc, = struct.unpack_from("<I", blob, 124)
    (magic, version, header_size, required_flags, optional_flags,
     tile_width, tile_height, tile_pixel_size, pixel_format,
     index_entry_size, reserved, tile_count, index_offset, data_offset,
     file_size) = fields
    return {
        "blob": blob,
        "magic": magic,
        "version": version,
        "header_size": header_size,
        "required_flags": required_flags,
        "optional_flags": optional_flags,
        "tile_width": tile_width,
        "tile_height": tile_height,
        "tile_pixel_size": tile_pixel_size,
        "pixel_format": pixel_format,
        "index_entry_size": index_entry_size,
        "reserved": reserved,
        "tile_count": tile_count,
        "index_offset": index_offset,
        "data_offset": data_offset,
        "file_size": file_size,
        "header_crc": header_crc,
    }


def parse_entries(parsed):
    blob = parsed["blob"]
    entries = []
    for index in range(parsed["tile_count"]):
        offset = parsed["index_offset"] + index * cv.MPS_INDEX_ENTRY_SIZE
        entries.append(cv.INDEX_STRUCT.unpack_from(blob, offset))
    return entries


def decode_entry(parsed, entry):
    data_offset, stored_size, raw_size, decoded_crc, stored_crc, codec, flags, reserved = entry
    if codec == cv.CODEC_ZERO:
        decoded = cv.ZERO_TILE
        stored = b""
    else:
        stored = parsed["blob"][data_offset:data_offset + stored_size]
        decoded = zlib.decompress(stored) if codec == cv.CODEC_ZLIB else stored
    if (len(decoded) != raw_size or flags != 0 or reserved != 0 or
            zlib.crc32(stored) & 0xFFFFFFFF != stored_crc or
            zlib.crc32(decoded) & 0xFFFFFFFF != decoded_crc):
        raise AssertionError("invalid generated MPS entry")
    return decoded


class WriterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.output = self.directory / "image.mps"

    def tearDown(self):
        self.temp.cleanup()

    def assert_no_temporary_files(self):
        self.assertEqual(list(self.directory.glob(".*.tmp")), [])

    def test_raw_zlib_and_zero_structural_roundtrip(self):
        raw = pattern_tile(3)
        compressible = bytes((index % 5) for index in range(cv.MPS_TILE_BYTES))
        tiles = [raw, compressible, cv.ZERO_TILE, cv.ZERO_TILE]
        count, file_size = cv.write_mps(
            self.output, 2, 2, tiles, compression="raw")
        self.assertEqual((count, file_size), (4, self.output.stat().st_size))
        parsed = parse_container(self.output)
        self.assertEqual(parsed["magic"], b"MPS1")
        self.assertEqual(parsed["version"], 1)
        self.assertEqual(parsed["header_size"], 128)
        self.assertEqual(parsed["required_flags"], 0)
        self.assertEqual(parsed["optional_flags"], 0)
        self.assertEqual((parsed["tile_width"], parsed["tile_height"]), (2, 2))
        self.assertEqual(parsed["tile_pixel_size"], 128)
        self.assertEqual(parsed["pixel_format"], 0)
        self.assertEqual(parsed["index_entry_size"], 32)
        self.assertEqual(parsed["reserved"], 0)
        self.assertEqual(parsed["tile_count"], 4)
        self.assertEqual(parsed["index_offset"], 128)
        self.assertEqual(parsed["data_offset"], 128 + 4 * 32)
        self.assertEqual(parsed["file_size"], len(parsed["blob"]))
        self.assertEqual(parsed["blob"][64:124], b"\0" * 60)
        self.assertEqual(parsed["header_crc"], zlib.crc32(parsed["blob"][:124]) & 0xFFFFFFFF)

        entries = parse_entries(parsed)
        self.assertEqual([entry[5] for entry in entries], [0, 0, 2, 2])
        for entry in entries:
            self.assertEqual(entry[2], cv.MPS_TILE_BYTES)
            self.assertEqual(entry[6:], (0, 0))
        self.assertEqual(entries[2][:2], (0, 0))
        self.assertEqual(entries[3][:2], (0, 0))
        for entry, expected in zip(entries[:2], tiles[:2]):
            data_offset, stored_size, _, decoded_crc, stored_crc, codec, _, _ = entry
            stored = parsed["blob"][data_offset:data_offset + stored_size]
            self.assertEqual(codec, 0)
            self.assertEqual(stored, expected)
            self.assertEqual(decoded_crc, zlib.crc32(expected) & 0xFFFFFFFF)
            self.assertEqual(stored_crc, zlib.crc32(stored) & 0xFFFFFFFF)

        zlib_output = self.directory / "zlib.mps"
        cv.write_mps(zlib_output, 1, 1, [compressible], compression="zlib")
        zlib_parsed = parse_container(zlib_output)
        zlib_entry, = parse_entries(zlib_parsed)
        self.assertEqual(zlib_entry[5], cv.CODEC_ZLIB)
        stored = zlib_parsed["blob"][zlib_entry[0]:zlib_entry[0] + zlib_entry[1]]
        self.assertEqual(zlib.decompress(stored), compressible)
        self.assertEqual(zlib_entry[4], zlib.crc32(stored) & 0xFFFFFFFF)

    def test_auto_uses_raw_for_incompressible_and_zero_codec(self):
        state = 0x12345678
        random_bytes = bytearray()
        for _ in range(cv.MPS_TILE_BYTES):
            state ^= (state << 13) & 0xFFFFFFFF
            state ^= state >> 17
            state ^= (state << 5) & 0xFFFFFFFF
            random_bytes.append(state & 0xFF)
        incompressible = bytes(random_bytes)
        cv.write_mps(self.output, 1, 2, [incompressible, cv.ZERO_TILE],
                     compression="auto")
        entries = parse_entries(parse_container(self.output))
        self.assertEqual(entries[0][5], cv.CODEC_RAW)
        self.assertEqual(entries[1][5], cv.CODEC_ZERO)

    def test_callback_is_one_tile_at_a_time(self):
        class LiveTile:
            live = 0
            maximum = 0

            def __init__(self, data):
                self.data = data
                LiveTile.live += 1
                LiveTile.maximum = max(LiveTile.maximum, LiveTile.live)

            def __bytes__(self):
                return self.data

            def __del__(self):
                LiveTile.live -= 1

        calls = []

        def callback(tile_index):
            gc.collect()
            self.assertEqual(LiveTile.live, 0)
            calls.append(tile_index)
            return LiveTile(pattern_tile(tile_index))

        cv.write_mps(self.output, 1, 3, callback, compression="raw")
        gc.collect()
        self.assertEqual(calls, [0, 1, 2])
        self.assertLessEqual(LiveTile.maximum, 1)
        self.assertEqual(LiveTile.live, 0)

    def test_atomic_overwrite_and_failure_cleanup(self):
        self.output.write_bytes(b"KEEP")

        def failing_callback(tile_index):
            raise cv.ConversionError("deliberate callback failure")

        with self.assertRaises(cv.ConversionError):
            cv.write_mps(self.output, 1, 1, failing_callback)
        self.assertEqual(self.output.read_bytes(), b"KEEP")
        self.assert_no_temporary_files()

        cv.write_mps(self.output, 1, 1, [cv.ZERO_TILE], compression="raw")
        self.assertNotEqual(self.output.read_bytes(), b"KEEP")
        self.assert_no_temporary_files()

    def test_invalid_dimensions_and_tile_bytes(self):
        for dimensions in ((0, 1), (1, 0), (1025, 1), (1, 1025)):
            with self.subTest(dimensions=dimensions):
                with self.assertRaises(cv.ConversionError):
                    cv.write_mps(self.output, *dimensions, [cv.ZERO_TILE])
                self.assertFalse(self.output.exists())
                self.assert_no_temporary_files()

        with self.assertRaises(cv.ConversionError):
            cv.write_mps(self.output, 1, 1, [b"short"], compression="raw")
        self.assertFalse(self.output.exists())
        self.assert_no_temporary_files()

    def test_large_sparse_strategy_does_not_retain_index_or_framebuffer(self):
        calls = []

        def aborting_callback(tile_index):
            calls.append(tile_index)
            if len(calls) > 3:
                raise cv.ConversionError("bounded synthetic abort")
            return cv.ZERO_TILE

        tracemalloc.start()
        with self.assertRaises(cv.ConversionError):
            cv.write_mps(self.output, 1024, 1024, aborting_callback,
                         compression="auto")
        _, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        self.assertEqual(calls, [0, 1, 2, 3])
        self.assertLess(peak, 8 * 1024 * 1024)
        self.assertFalse(self.output.exists())
        self.assert_no_temporary_files()


class PillowAdapterTests(unittest.TestCase):
    def test_fit_transform_when_pillow_is_available(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow not installed; core writer tests remain Pillow-free")

        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            source_path = directory / "source.png"
            output_path = directory / "image.mps"
            image = Image.new("RGBA", (32, 16), (20, 40, 60, 255))
            image.save(source_path)
            count, _ = cv.convert_image(source_path, output_path, 2, 2,
                                        mode="fit", compression="auto")
            self.assertEqual(count, 4)
            parsed = parse_container(output_path)
            self.assertEqual(parsed["file_size"], len(parsed["blob"]))
            entries = parse_entries(parsed)
            self.assertEqual(len(entries), 4)

            tiles = [decode_entry(parsed, entry) for entry in entries]
            top_left = tiles[0]
            bottom_left = tiles[2]
            self.assertEqual(top_left[0:4], bytes((0, 0, 0, 255)))
            self.assertEqual(top_left[127 * 128 * 4:127 * 128 * 4 + 4],
                             bytes((20, 40, 60, 255)))
            self.assertEqual(bottom_left[0:4], bytes((20, 40, 60, 255)))
            self.assertEqual(bottom_left[127 * 128 * 4:127 * 128 * 4 + 4],
                             bytes((0, 0, 0, 255)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
