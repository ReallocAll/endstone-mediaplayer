#!/usr/bin/env python3
"""Convert any FFmpeg-decodable video into the MCV v1 screen container.

MCV v1 layout (little-endian):
    [header: 128 bytes] [frame data] [frame index: frame_count * 24 bytes]

Frames are raw ABGR (pixel_width * pixel_height * 4 bytes) or, by
default, per-frame independent zlib streams.  Every frame carries a
CRC32 of its stored bytes in the index; the header carries a CRC32 of
its own first 124 bytes.  The writer streams frames from a single
FFmpeg process into a same-directory temporary file and atomically
replaces the destination only on success.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from typing import IO, Callable, Optional, Sequence, Tuple

MCV_MAGIC = b"MCV1"
MCV_HEADER_SIZE = 128
MCV_FORMAT_VERSION = 1
MCV_INDEX_ENTRY_SIZE = 24
MCV_MAX_FRAME_COUNT = 6_000_000  # > 72h at 20fps
TILE_SIZE = 128
MAX_WIDTH = 7
MAX_HEIGHT = 4
MAX_FPS = 20
UINT64_MAX = (1 << 64) - 1

PIXFMT_ABGR8888 = 0
CODEC_NONE = 0
CODEC_ZLIB = 1
FRAME_FLAG_INDEPENDENT = 0x0001
DEFAULT_COMPRESS_LEVEL = 6

# Header bytes 0..67; reserved[56] and the trailing CRC32 are appended
# separately so the CRC can cover bytes 0..123.
HEADER_BODY_STRUCT = struct.Struct("<4sHHII8HQQQQI")
HEADER_CRC_STRUCT = struct.Struct("<I")
INDEX_STRUCT = struct.Struct("<QQIHH")


class ConversionError(RuntimeError):
    """A deterministic conversion or container-writing failure."""


class PartialFrameError(ConversionError):
    """FFmpeg ended after emitting only part of a raw frame."""


def checked_u64(value: int, field: str) -> int:
    if value < 0 or value > UINT64_MAX:
        raise ConversionError("{} does not fit uint64".format(field))
    return value


def checked_mul_u64(a: int, b: int, field: str) -> int:
    if a < 0 or b < 0 or (a != 0 and b > UINT64_MAX // a):
        raise ConversionError("{} overflows uint64".format(field))
    return a * b


def parse_tiles(value: str) -> Tuple[int, int]:
    try:
        parts = value.lower().split("x")
        if len(parts) != 2:
            raise ValueError
        tile_width, tile_height = int(parts[0]), int(parts[1])
    except ValueError as error:
        raise ConversionError("--tiles must be WxH (for example 4x2)") from error
    if not 1 <= tile_width <= MAX_WIDTH or not 1 <= tile_height <= MAX_HEIGHT:
        raise ConversionError(
            "tiles must be 1-{} x 1-{}".format(MAX_WIDTH, MAX_HEIGHT)
        )
    return tile_width, tile_height


def validate_fps(value: int) -> Tuple[int, bool]:
    """Return (effective_fps, was_clamped); reject non-positive values."""
    if value < 1:
        raise ConversionError("fps must be at least 1")
    if value > MAX_FPS:
        return MAX_FPS, True
    return value, False


def validate_compress_level(value: int) -> int:
    if not 1 <= value <= 9:
        raise ConversionError("compression level must be 1-9")
    return value


def dimensions_for_tiles(tile_width: int, tile_height: int) -> Tuple[int, int, int]:
    if not 1 <= tile_width <= MAX_WIDTH or not 1 <= tile_height <= MAX_HEIGHT:
        raise ConversionError("invalid tile dimensions")
    pixel_width = tile_width * TILE_SIZE
    pixel_height = tile_height * TILE_SIZE
    frame_size = checked_mul_u64(pixel_width, pixel_height, "pixel count")
    frame_size = checked_mul_u64(frame_size, 4, "raw frame size")
    return pixel_width, pixel_height, frame_size


def build_filter(pixel_width: int, pixel_height: int, fps: int, mode: str) -> str:
    if mode == "fit":
        scale = (
            "scale={}:{}:force_original_aspect_ratio=decrease,"
            "pad={}:{}:(ow-iw)/2:(oh-ih)/2:color=black"
        ).format(pixel_width, pixel_height, pixel_width, pixel_height)
    elif mode == "fill":
        scale = (
            "scale={}:{}:force_original_aspect_ratio=increase,"
            "crop={}:{}"
        ).format(pixel_width, pixel_height, pixel_width, pixel_height)
    elif mode == "stretch":
        scale = "scale={}:{}".format(pixel_width, pixel_height)
    else:
        raise ConversionError("unknown scaling mode: {}".format(mode))
    return "fps={},{},format=rgba".format(fps, scale)


def discover_ffmpeg() -> str:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise ConversionError("ffmpeg not found in PATH")
    return ffmpeg


def build_ffmpeg_command(ffmpeg: str, input_path: str, pixel_width: int,
                         pixel_height: int, fps: int, mode: str) -> Sequence[str]:
    return [
        ffmpeg,
        "-nostdin",
        "-i", input_path,
        "-vf", build_filter(pixel_width, pixel_height, fps, mode),
        "-an", "-sn", "-dn",
        "-f", "rawvideo",
        "-pix_fmt", "rgba",
        "pipe:1",
    ]


def read_exact_frame(stream: IO[bytes], frame_size: int) -> Optional[bytearray]:
    """Return one complete frame, None at clean EOF, or reject partial EOF."""
    if frame_size <= 0:
        raise ConversionError("frame size must be positive")
    data = bytearray()
    while len(data) < frame_size:
        chunk = stream.read(frame_size - len(data))
        if chunk is None:
            raise ConversionError("raw frame stream returned no data")
        if not chunk:
            if not data:
                return None
            raise PartialFrameError(
                "partial final frame: {} of {} bytes".format(len(data), frame_size)
            )
        data.extend(chunk)
    return data


def encode_stored_frame(raw: bytes, codec: int, compress_level: int) -> bytes:
    if codec == CODEC_NONE:
        return bytes(raw)
    if codec == CODEC_ZLIB:
        return zlib.compress(bytes(raw), compress_level)
    raise ConversionError("unknown codec {}".format(codec))


def serialize_index_entry(offset: int, stored_size: int, crc: int) -> bytes:
    return INDEX_STRUCT.pack(
        checked_u64(offset, "frame offset"),
        checked_u64(stored_size, "stored frame size"),
        crc & 0xFFFFFFFF,
        FRAME_FLAG_INDEPENDENT,
        0,
    )


def serialize_header(tile_width: int, tile_height: int, fps_num: int,
                     fps_den: int, frame_count: int, frame_data_size: int,
                     codec: int) -> bytes:
    pixel_width, pixel_height, _ = dimensions_for_tiles(tile_width, tile_height)
    if fps_num < 1 or fps_num > 0xFFFF or fps_den < 1 or fps_den > 0xFFFF:
        raise ConversionError("FPS numerator and denominator must fit uint16")
    if fps_num > MAX_FPS * fps_den:
        raise ConversionError("FPS may not exceed {}".format(MAX_FPS))
    if frame_count < 1 or frame_count > MCV_MAX_FRAME_COUNT:
        raise ConversionError("frame count is outside the MCV safety limit")
    if codec not in (CODEC_NONE, CODEC_ZLIB):
        raise ConversionError("unknown codec {}".format(codec))
    frame_data_offset = MCV_HEADER_SIZE
    frame_index_offset = checked_u64(
        frame_data_offset + frame_data_size, "frame index offset"
    )
    body = HEADER_BODY_STRUCT.pack(
        MCV_MAGIC,
        MCV_HEADER_SIZE,
        MCV_FORMAT_VERSION,
        0,  # required_flags
        0,  # optional_flags
        tile_width,
        tile_height,
        pixel_width,
        pixel_height,
        PIXFMT_ABGR8888,
        codec,
        fps_num,
        fps_den,
        checked_u64(frame_count, "frame count"),
        frame_data_offset,
        checked_u64(frame_data_size, "frame data size"),
        frame_index_offset,
        MCV_INDEX_ENTRY_SIZE,
    )
    encoded = body + b"\x00" * 56
    if len(encoded) != MCV_HEADER_SIZE - 4:
        raise AssertionError("MCV header body is not 124 bytes")
    return encoded + HEADER_CRC_STRUCT.pack(zlib.crc32(encoded))


def stream_frames_to_mcv(
    output_path: Path,
    tile_width: int,
    tile_height: int,
    fps_num: int,
    fps_den: int,
    frame_stream: IO[bytes],
    codec: int = CODEC_ZLIB,
    compress_level: int = DEFAULT_COMPRESS_LEVEL,
    completion_check: Optional[Callable[[], None]] = None,
    frame_limit: int = MCV_MAX_FRAME_COUNT,
    progress: Optional[Callable[[int], None]] = None,
) -> Tuple[int, int]:
    """Stream raw RGBA frames into a temporary .mcv, then atomically replace.

    Returns (frame_count, frame_data_size).
    """
    output_path = Path(output_path)
    parent = output_path.parent
    if not parent.exists() or not parent.is_dir():
        raise ConversionError("output directory does not exist: {}".format(parent))
    if frame_limit < 1 or frame_limit > MCV_MAX_FRAME_COUNT:
        raise ConversionError("invalid frame-count limit")
    _, _, raw_frame_size = dimensions_for_tiles(tile_width, tile_height)

    temp_file = tempfile.NamedTemporaryFile(
        mode="w+b",
        prefix=".{}-".format(output_path.name),
        suffix=".tmp",
        dir=str(parent),
        delete=False,
    )
    temp_path = Path(temp_file.name)
    replaced = False
    try:
        temp_file.write(b"\x00" * MCV_HEADER_SIZE)
        # 24 bytes per frame; even at the 6M-frame cap this stays at
        # 144 MB, so the index is buffered rather than spooled to disk.
        index = bytearray()
        frame_count = 0
        data_offset = 0
        while True:
            frame = read_exact_frame(frame_stream, raw_frame_size)
            if frame is None:
                break
            if frame_count >= frame_limit:
                raise ConversionError(
                    "frame count exceeds limit {}".format(frame_limit)
                )
            stored = encode_stored_frame(frame, codec, compress_level)
            # RGBA bytes are already 0xAABBGGRR on little-endian canvases.
            temp_file.write(stored)
            index += serialize_index_entry(
                data_offset, len(stored), zlib.crc32(stored)
            )
            data_offset = checked_u64(
                data_offset + len(stored), "frame data size"
            )
            frame_count += 1
            if progress:
                progress(frame_count)

        if completion_check:
            completion_check()
        if frame_count == 0:
            raise ConversionError("FFmpeg produced no complete frames")

        frame_data_size = data_offset
        expected_index_offset = checked_u64(
            MCV_HEADER_SIZE + frame_data_size, "frame index offset"
        )
        if temp_file.tell() != expected_index_offset:
            raise ConversionError("streamed frame-data position mismatch")

        temp_file.write(index)
        temp_file.seek(0)
        temp_file.write(
            serialize_header(
                tile_width, tile_height, fps_num, fps_den,
                frame_count, frame_data_size, codec,
            )
        )
        temp_file.flush()
        os.fsync(temp_file.fileno())
        temp_file.close()
        os.replace(str(temp_path), str(output_path))
        replaced = True
        return frame_count, frame_data_size
    except BaseException:
        if not temp_file.closed:
            temp_file.close()
        if not replaced:
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass
        raise


def convert_video(input_path: Path, output_path: Path, tile_width: int,
                  tile_height: int, fps: int, mode: str,
                  codec: int = CODEC_ZLIB,
                  compress_level: int = DEFAULT_COMPRESS_LEVEL) -> Tuple[int, int]:
    input_path = Path(input_path)
    output_path = Path(output_path)
    if not input_path.is_file():
        raise ConversionError("input file not found: {}".format(input_path))
    fps, _ = validate_fps(fps)
    pixel_width, pixel_height, _ = dimensions_for_tiles(tile_width, tile_height)
    ffmpeg = discover_ffmpeg()
    command = build_ffmpeg_command(
        ffmpeg, str(input_path), pixel_width, pixel_height, fps, mode
    )

    # stderr is inherited so FFmpeg can never block on an undrained pipe.
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=None)
    if process.stdout is None:
        process.terminate()
        process.wait()
        raise ConversionError("unable to open FFmpeg output pipe")

    def check_completion() -> None:
        return_code = process.wait()
        if return_code != 0:
            raise ConversionError(
                "FFmpeg exited with status {}".format(return_code)
            )

    def report_progress(frame_count: int) -> None:
        if frame_count % 100 == 0:
            print("  streamed {} frames...".format(frame_count))

    try:
        return stream_frames_to_mcv(
            output_path,
            tile_width,
            tile_height,
            fps,
            1,
            process.stdout,
            codec=codec,
            compress_level=compress_level,
            completion_check=check_completion,
            progress=report_progress,
        )
    finally:
        process.stdout.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Convert video to MCV v1")
    parser.add_argument("input", help="Input video file")
    parser.add_argument(
        "-o", "--output",
        help="Output .mcv file (default: input name with .mcv suffix)",
    )
    parser.add_argument(
        "--tiles", default="1x1",
        help="Screen size in tiles WxH (for example 4x2, maximum 7x4)",
    )
    parser.add_argument(
        "--fps", type=int, default=20,
        help="Output FPS (maximum 20, default 20)",
    )
    parser.add_argument(
        "--mode", choices=["fit", "fill", "stretch"], default="fit",
        help="Scaling mode (default: fit with black bars)",
    )
    parser.add_argument(
        "--no-compress", action="store_true",
        help="Store raw ABGR frames instead of per-frame zlib streams",
    )
    parser.add_argument(
        "--level", type=int, default=DEFAULT_COMPRESS_LEVEL,
        help="zlib compression level 1-9 (default {})".format(
            DEFAULT_COMPRESS_LEVEL
        ),
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    try:
        tile_width, tile_height = parse_tiles(args.tiles)
        fps, clamped = validate_fps(args.fps)
        if clamped:
            print(
                "Warning: {} fps exceeds the map update ceiling; "
                "clamping to {} fps".format(args.fps, MAX_FPS),
                file=sys.stderr,
            )
        codec = CODEC_NONE if args.no_compress else CODEC_ZLIB
        compress_level = validate_compress_level(args.level)
        input_path = Path(args.input)
        output_path = (
            Path(args.output) if args.output else input_path.with_suffix(".mcv")
        )
        pixel_width, pixel_height, raw_frame_size = dimensions_for_tiles(
            tile_width, tile_height
        )
        print("Input: {}".format(input_path))
        print("Output: {}".format(output_path))
        print(
            "Tiles: {}x{} ({}x{} pixels)".format(
                tile_width, tile_height, pixel_width, pixel_height
            )
        )
        print(
            "FPS: {}, Mode: {}, Codec: {}".format(
                fps, args.mode,
                "zlib level {}".format(compress_level)
                if codec == CODEC_ZLIB else "none",
            )
        )
        frame_count, frame_data_size = convert_video(
            input_path, output_path, tile_width, tile_height, fps, args.mode,
            codec=codec, compress_level=compress_level,
        )
        size = output_path.stat().st_size
        raw_total = frame_count * raw_frame_size
        ratio = (frame_data_size / raw_total) if raw_total else 1.0
        print(
            "Written: {} ({:.1f} MB, {} frames, {:.0%} of raw)".format(
                output_path, size / 1024 / 1024, frame_count, ratio
            )
        )
        return 0
    except KeyboardInterrupt:
        print("Conversion interrupted", file=sys.stderr)
        return 130
    except (ConversionError, OSError, subprocess.SubprocessError) as error:
        print("Error: {}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
