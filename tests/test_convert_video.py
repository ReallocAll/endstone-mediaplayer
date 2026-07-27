#!/usr/bin/env python3
"""Unit tests for tools/convert_video.py (MCV v1 writer)."""

import importlib.util
import io
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "convert_video.py"
SPEC = importlib.util.spec_from_file_location("convert_video", MODULE_PATH)
assert SPEC and SPEC.loader
cv = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cv)


def make_frame(frame_idx: int, frame_size: int) -> bytes:
    """Deterministic, compressible pattern distinct per frame."""
    return bytes((frame_idx + i // 4) % 256 for i in range(frame_size))


def parse_header(blob: bytes):
    body = struct.Struct("<4sHHII8HQQQQI")
    fields = body.unpack(blob[: body.size])
    (crc,) = struct.unpack("<I", blob[124:128])
    return fields, blob[68:124], crc


class ChunkedStream:
    """Stream that returns data in deliberately small chunks."""

    def __init__(self, data: bytes, chunk: int):
        self._data = data
        self._pos = 0
        self._chunk = chunk

    def read(self, size: int):
        if self._pos >= len(self._data):
            return b""
        take = min(self._chunk, size)
        out = self._data[self._pos:self._pos + take]
        self._pos += len(out)
        return out


class ReadExactFrameTests(unittest.TestCase):
    def test_reassembles_short_reads(self):
        frame = make_frame(0, 4096)
        stream = ChunkedStream(frame, chunk=7)
        got = cv.read_exact_frame(stream, 4096)
        self.assertEqual(bytes(got), frame)

    def test_clean_eof_returns_none(self):
        self.assertIsNone(cv.read_exact_frame(io.BytesIO(b""), 16))

    def test_partial_eof_raises(self):
        stream = io.BytesIO(b"\x00" * 10)
        with self.assertRaises(cv.PartialFrameError):
            cv.read_exact_frame(stream, 16)


class SerializationTests(unittest.TestCase):
    def test_header_bytes(self):
        raw_frame = 128 * 128 * 4
        blob = cv.serialize_header(1, 1, 20, 1, 3, 3 * raw_frame,
                                   cv.CODEC_NONE)
        self.assertEqual(len(blob), cv.MCV_HEADER_SIZE)
        fields, reserved, crc = parse_header(blob)
        (magic, header_size, version, required, optional,
         tile_w, tile_h, pixel_w, pixel_h, pixfmt, codec,
         fps_num, fps_den, frame_count, data_offset, data_size,
         index_offset, entry_size) = fields
        self.assertEqual(magic, b"MCV1")
        self.assertEqual(header_size, 128)
        self.assertEqual(version, 1)
        self.assertEqual((required, optional), (0, 0))
        self.assertEqual((tile_w, tile_h, pixel_w, pixel_h), (1, 1, 128, 128))
        self.assertEqual(pixfmt, cv.PIXFMT_ABGR8888)
        self.assertEqual(codec, cv.CODEC_NONE)
        self.assertEqual((fps_num, fps_den), (20, 1))
        self.assertEqual(frame_count, 3)
        self.assertEqual(data_offset, 128)
        self.assertEqual(data_size, 3 * raw_frame)
        self.assertEqual(index_offset, 128 + 3 * raw_frame)
        self.assertEqual(entry_size, 24)
        self.assertEqual(reserved, b"\x00" * 56)
        self.assertEqual(crc, zlib.crc32(blob[:124]))

    def test_header_rejects_fps_over_ceiling(self):
        with self.assertRaises(cv.ConversionError):
            cv.serialize_header(1, 1, 21, 1, 1, 128 * 128 * 4, cv.CODEC_NONE)

    def test_index_entry_bytes(self):
        blob = cv.serialize_index_entry(5, 9, 0xAABBCCDD)
        self.assertEqual(len(blob), cv.MCV_INDEX_ENTRY_SIZE)
        offset, size, crc, flags, reserved = struct.unpack("<QQIHH", blob)
        self.assertEqual((offset, size, crc), (5, 9, 0xAABBCCDD))
        self.assertEqual(flags, cv.FRAME_FLAG_INDEPENDENT)
        self.assertEqual(reserved, 0)

    def test_validate_fps(self):
        self.assertEqual(cv.validate_fps(20), (20, False))
        self.assertEqual(cv.validate_fps(30), (20, True))
        with self.assertRaises(cv.ConversionError):
            cv.validate_fps(0)


class StreamWriterTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.out = self.dir / "out.mcv"
        self.raw_frame = 128 * 128 * 4

    def tearDown(self):
        self._tmp.cleanup()

    def _frames(self, count):
        return [make_frame(i, self.raw_frame) for i in range(count)]

    def _stream(self, frames):
        return io.BytesIO(b"".join(frames))

    def _no_temp_files_left(self):
        return not [p for p in self.dir.iterdir() if p.suffix == ".tmp"]

    def _read_entries(self, blob, index_offset, frame_count):
        entries = []
        for i in range(frame_count):
            start = index_offset + i * 24
            entries.append(struct.unpack("<QQIHH", blob[start:start + 24]))
        return entries

    def test_compressed_layout_roundtrip(self):
        frames = self._frames(3)
        count, data_size = cv.stream_frames_to_mcv(
            self.out, 1, 1, 20, 1, self._stream(frames),
            codec=cv.CODEC_ZLIB, compress_level=6,
        )
        self.assertEqual(count, 3)
        blob = self.out.read_bytes()
        fields, _, crc = parse_header(blob[:128])
        self.assertEqual(fields[0], b"MCV1")
        self.assertEqual(crc, zlib.crc32(blob[:124]))
        codec, frame_count = fields[10], fields[13]
        index_offset = fields[16]
        self.assertEqual(codec, cv.CODEC_ZLIB)
        self.assertEqual(frame_count, 3)
        self.assertEqual(fields[15], data_size)
        self.assertEqual(index_offset, 128 + data_size)
        self.assertEqual(len(blob), index_offset + 3 * 24)

        expected_offset = 0
        for i, (offset, size, entry_crc, flags, reserved) in enumerate(
            self._read_entries(blob, index_offset, 3)
        ):
            self.assertEqual(offset, expected_offset)
            stored = blob[128 + offset:128 + offset + size]
            self.assertEqual(entry_crc, zlib.crc32(stored))
            self.assertEqual(zlib.decompress(stored), frames[i])
            self.assertLess(size, self.raw_frame)  # pattern compresses
            self.assertEqual(flags, cv.FRAME_FLAG_INDEPENDENT)
            self.assertEqual(reserved, 0)
            expected_offset += size
        self.assertEqual(expected_offset, data_size)

    def test_uncompressed_layout(self):
        frames = self._frames(2)
        count, data_size = cv.stream_frames_to_mcv(
            self.out, 1, 1, 20, 1, self._stream(frames),
            codec=cv.CODEC_NONE,
        )
        self.assertEqual(count, 2)
        self.assertEqual(data_size, 2 * self.raw_frame)
        blob = self.out.read_bytes()
        index_offset = 128 + data_size
        for i, (offset, size, entry_crc, flags, _) in enumerate(
            self._read_entries(blob, index_offset, 2)
        ):
            self.assertEqual(offset, i * self.raw_frame)
            self.assertEqual(size, self.raw_frame)
            stored = blob[128 + offset:128 + offset + size]
            self.assertEqual(stored, frames[i])
            self.assertEqual(entry_crc, zlib.crc32(stored))
            self.assertEqual(flags, cv.FRAME_FLAG_INDEPENDENT)

    def test_zero_frames_error(self):
        with self.assertRaises(cv.ConversionError):
            cv.stream_frames_to_mcv(self.out, 1, 1, 20, 1, io.BytesIO(b""))
        self.assertFalse(self.out.exists())
        self.assertTrue(self._no_temp_files_left())

    def test_partial_frame_preserves_destination(self):
        self.out.write_bytes(b"KEEP")
        bad = b"".join(self._frames(1)) + b"\x00" * 10
        with self.assertRaises(cv.PartialFrameError):
            cv.stream_frames_to_mcv(self.out, 1, 1, 20, 1, io.BytesIO(bad))
        self.assertEqual(self.out.read_bytes(), b"KEEP")
        self.assertTrue(self._no_temp_files_left())

    def test_completion_failure_preserves_destination(self):
        self.out.write_bytes(b"KEEP")

        def failing_check():
            raise cv.ConversionError("ffmpeg exited with status 1")

        with self.assertRaises(cv.ConversionError):
            cv.stream_frames_to_mcv(
                self.out, 1, 1, 20, 1, self._stream(self._frames(1)),
                completion_check=failing_check,
            )
        self.assertEqual(self.out.read_bytes(), b"KEEP")
        self.assertTrue(self._no_temp_files_left())

    def test_frame_limit_enforced(self):
        with self.assertRaises(cv.ConversionError):
            cv.stream_frames_to_mcv(
                self.out, 1, 1, 20, 1, self._stream(self._frames(3)),
                frame_limit=2,
            )
        self.assertFalse(self.out.exists())
        self.assertTrue(self._no_temp_files_left())


@unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg not on PATH")
class FfmpegEndToEndTests(unittest.TestCase):
    def test_real_conversion(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            source = tmp_path / "source.mp4"
            subprocess.run(
                [
                    "ffmpeg", "-loglevel", "error", "-y",
                    "-f", "lavfi", "-i",
                    "testsrc=duration=1:size=64x48:rate=10",
                    str(source),
                ],
                check=True,
            )
            output = tmp_path / "source.mcv"
            frame_count, data_size = cv.convert_video(
                source, output, 1, 1, 10, "fit"
            )
            self.assertGreater(frame_count, 0)
            blob = output.read_bytes()
            fields, _, crc = parse_header(blob[:128])
            self.assertEqual(fields[0], b"MCV1")
            self.assertEqual(crc, zlib.crc32(blob[:124]))
            self.assertEqual(fields[13], frame_count)
            self.assertEqual(fields[15], data_size)
            index_offset = fields[16]
            offset, size, entry_crc, flags, _ = struct.unpack(
                "<QQIHH", blob[index_offset:index_offset + 24]
            )
            stored = blob[128 + offset:128 + offset + size]
            self.assertEqual(entry_crc, zlib.crc32(stored))
            raw = zlib.decompress(stored)
            self.assertEqual(len(raw), 128 * 128 * 4)
            self.assertEqual(flags, cv.FRAME_FLAG_INDEPENDENT)


if __name__ == "__main__":
    unittest.main(verbosity=2)
