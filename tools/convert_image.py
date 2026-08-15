#!/usr/bin/env python3
"""Convert one image into the streaming MPS1 tiled image container.

The writer is deliberately independent of Pillow.  Its tile source is a
callback taking a row-major tile index and returning exactly one canonical
128x128 RGBA tile.  The Pillow adapter below samples one output tile at a
time, so even a 1024x1024-tile image never becomes a full target framebuffer
or an in-memory index list.
"""

import argparse
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path
from typing import Callable, Iterable, Optional, Tuple, Union


MPS_MAGIC = b"MPS1"
MPS_FORMAT_VERSION = 1
MPS_HEADER_SIZE = 128
MPS_INDEX_ENTRY_SIZE = 32
MPS_TILE_SIZE = 128
MPS_TILE_BYTES = MPS_TILE_SIZE * MPS_TILE_SIZE * 4
MPS_MAX_TILE_DIMENSION = 1024
MPS_PIXEL_FORMAT_ABGR8888 = 0

CODEC_RAW = 0
CODEC_ZLIB = 1
CODEC_ZERO = 2
CODEC_NONE = CODEC_RAW

UINT64_MAX = (1 << 64) - 1
DEFAULT_COMPRESSION_LEVEL = 6
ZERO_TILE = bytes(MPS_TILE_BYTES)

HEADER_STRUCT = struct.Struct("<4sHHII II HHHH QQQQ")
INDEX_STRUCT = struct.Struct("<QIIIIHHI")


class ConversionError(RuntimeError):
    """A deterministic conversion or MPS container-writing failure."""


TileSource = Union[Callable[[int], bytes], Iterable[bytes]]


def checked_u64(value: int, field: str) -> int:
    if value < 0 or value > UINT64_MAX:
        raise ConversionError("{} does not fit uint64".format(field))
    return value


def checked_mul_u64(a: int, b: int, field: str) -> int:
    if a < 0 or b < 0 or (a != 0 and b > UINT64_MAX // a):
        raise ConversionError("{} overflows uint64".format(field))
    return a * b


def validate_tile_dimensions(tile_width: int, tile_height: int) -> Tuple[int, int, int]:
    if (not isinstance(tile_width, int) or not isinstance(tile_height, int) or
            not 1 <= tile_width <= MPS_MAX_TILE_DIMENSION or
            not 1 <= tile_height <= MPS_MAX_TILE_DIMENSION):
        raise ConversionError(
            "tile dimensions must each be between 1 and {}".format(
                MPS_MAX_TILE_DIMENSION))
    tile_count = checked_mul_u64(tile_width, tile_height, "tile count")
    data_offset = checked_u64(
        MPS_HEADER_SIZE + checked_mul_u64(
            tile_count, MPS_INDEX_ENTRY_SIZE, "index size"),
        "data offset")
    return tile_width, tile_height, data_offset


def normalize_compression(value: str) -> str:
    if value not in ("raw", "zlib", "auto"):
        raise ConversionError("compression must be raw, zlib, or auto")
    return value


def serialize_header(tile_width: int, tile_height: int, tile_count: int,
                     file_size: int) -> bytes:
    _, _, data_offset = validate_tile_dimensions(tile_width, tile_height)
    expected_count = tile_width * tile_height
    if tile_count != expected_count or tile_count < 1:
        raise ConversionError("tile count does not match tile dimensions")
    checked_u64(file_size, "file size")

    header = bytearray(MPS_HEADER_SIZE)
    HEADER_STRUCT.pack_into(
        header, 0,
        MPS_MAGIC,
        MPS_FORMAT_VERSION,
        MPS_HEADER_SIZE,
        0,
        0,
        tile_width,
        tile_height,
        MPS_TILE_SIZE,
        MPS_PIXEL_FORMAT_ABGR8888,
        MPS_INDEX_ENTRY_SIZE,
        0,
        tile_count,
        MPS_HEADER_SIZE,
        data_offset,
        file_size,
    )
    struct.pack_into("<I", header, 124, zlib.crc32(header[:124]) & 0xFFFFFFFF)
    return bytes(header)


def serialize_index_entry(data_offset: int, stored_size: int, raw_size: int,
                          decoded_crc32: int, stored_crc32: int, codec: int,
                          flags: int = 0, reserved: int = 0) -> bytes:
    checked_u64(data_offset, "tile data offset")
    for value, field in ((stored_size, "stored tile size"),
                         (raw_size, "raw tile size"),
                         (decoded_crc32, "decoded CRC32"),
                         (stored_crc32, "stored CRC32")):
        if value < 0 or value > 0xFFFFFFFF:
            raise ConversionError("{} does not fit uint32".format(field))
    if codec not in (CODEC_RAW, CODEC_ZLIB, CODEC_ZERO):
        raise ConversionError("unknown codec {}".format(codec))
    if flags != 0 or reserved != 0:
        raise ConversionError("MPS v1 flags and reserved fields must be zero")
    return INDEX_STRUCT.pack(
        data_offset,
        stored_size,
        raw_size,
        decoded_crc32 & 0xFFFFFFFF,
        stored_crc32 & 0xFFFFFFFF,
        codec,
        flags,
        reserved,
    )


def _write_zeros(stream, count: int) -> None:
    zero_block = bytes(64 * 1024)
    while count:
        amount = min(count, len(zero_block))
        stream.write(zero_block[:amount])
        count -= amount


def _get_tile(tile_source: TileSource, iterator, tile_index: int) -> bytes:
    try:
        if callable(tile_source):
            value = tile_source(tile_index)
        else:
            value = next(iterator)
    except StopIteration as error:
        raise ConversionError(
            "tile source ended before tile {}".format(tile_index)) from error
    if value is None:
        raise ConversionError("tile source returned no tile at {}".format(tile_index))
    try:
        tile = bytes(value)
    except (TypeError, ValueError) as error:
        raise ConversionError("tile source returned non-byte data") from error
    if len(tile) != MPS_TILE_BYTES:
        raise ConversionError(
            "tile {} has {} bytes; expected {}".format(
                tile_index, len(tile), MPS_TILE_BYTES))
    return tile


def _encode_tile(raw: bytes, compression: str) -> Tuple[int, bytes]:
    if raw == ZERO_TILE:
        return CODEC_ZERO, b""
    if compression == "raw":
        return CODEC_RAW, raw
    compressed = zlib.compress(raw, DEFAULT_COMPRESSION_LEVEL)
    if compression == "zlib" or (
            compression == "auto" and len(compressed) < len(raw)):
        return CODEC_ZLIB, compressed
    return CODEC_RAW, raw


def write_mps(output_path: Union[str, os.PathLike], tile_width: int,
              tile_height: int, tile_source: TileSource,
              compression: str = "auto") -> Tuple[int, int]:
    """Write an MPS1 image from a callback or one-pass tile iterable.

    A callback is called as ``tile_source(tile_index)`` and must return one
    128x128 RGBA tile.  The returned tuple is ``(tile_count, file_size)``.
    """
    tile_width, tile_height, data_offset = validate_tile_dimensions(
        tile_width, tile_height)
    compression = normalize_compression(compression)
    tile_count = tile_width * tile_height
    destination = Path(output_path)
    temporary_name: Optional[str] = None
    stream = None
    data_cursor = data_offset
    iterator = None if callable(tile_source) else iter(tile_source)

    try:
        # mkstemp in the destination directory makes os.replace atomic on one
        # filesystem and leaves an existing destination untouched on failure.
        fd, temporary_name = tempfile.mkstemp(
            prefix=".{}.".format(destination.name),
            suffix=".tmp",
            dir=str(destination.parent),
        )
        stream = os.fdopen(fd, "w+b")
        _write_zeros(stream, data_offset)
        for tile_index in range(tile_count):
            raw = _get_tile(tile_source, iterator, tile_index)
            decoded_crc32 = zlib.crc32(raw) & 0xFFFFFFFF
            codec, stored = _encode_tile(raw, compression)
            if codec == CODEC_ZERO:
                tile_data_offset = 0
                stored_size = 0
                stored_crc32 = 0
            else:
                tile_data_offset = data_cursor
                stored_size = len(stored)
                if stored_size > 0xFFFFFFFF:
                    raise ConversionError("stored tile is too large")
                stream.seek(data_cursor)
                stream.write(stored)
                data_cursor += stored_size
                checked_u64(data_cursor, "file size")
                stored_crc32 = zlib.crc32(stored) & 0xFFFFFFFF
            entry = serialize_index_entry(
                tile_data_offset,
                stored_size,
                MPS_TILE_BYTES,
                decoded_crc32,
                stored_crc32,
                codec,
            )
            stream.seek(MPS_HEADER_SIZE + tile_index * MPS_INDEX_ENTRY_SIZE)
            stream.write(entry)

        header = serialize_header(tile_width, tile_height, tile_count,
                                  data_cursor)
        stream.seek(0)
        stream.write(header)
        stream.flush()
        os.fsync(stream.fileno())
        stream.close()
        stream = None
        os.replace(temporary_name, destination)
        temporary_name = None
        return tile_count, data_cursor
    except ConversionError:
        raise
    except (OSError, TypeError, ValueError) as error:
        raise ConversionError("unable to write MPS file: {}".format(error)) from error
    finally:
        if stream is not None:
            try:
                stream.close()
            except OSError:
                pass
        if temporary_name is not None:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass
            except OSError:
                pass


def _load_pillow():
    try:
        from PIL import Image
    except ImportError as error:
        raise ConversionError(
            "Pillow is required for image conversion; install it with "
            "'python -m pip install Pillow'") from error
    return Image


def convert_image(input_path: Union[str, os.PathLike],
                  output_path: Union[str, os.PathLike], tile_width: int,
                  tile_height: int, mode: str = "fit",
                  compression: str = "auto") -> Tuple[int, int]:
    """Convert an image using Pillow while sampling one output tile at once."""
    if mode != "fit":
        raise ConversionError("only aspect-fit mode is supported")
    Image = _load_pillow()
    try:
        source_context = Image.open(input_path)
    except (OSError, ValueError) as error:
        raise ConversionError("unable to open input image: {}".format(error)) from error

    try:
        with source_context:
            source = source_context.convert("RGBA")
            try:
                source_width, source_height = source.size
                if source_width < 1 or source_height < 1:
                    raise ConversionError("input image has invalid dimensions")
                _, _, _ = validate_tile_dimensions(tile_width, tile_height)
                target_width = tile_width * MPS_TILE_SIZE
                target_height = tile_height * MPS_TILE_SIZE
                scale = min(target_width / source_width,
                            target_height / source_height)
                fitted_width = max(1, int(round(source_width * scale)))
                fitted_height = max(1, int(round(source_height * scale)))
                offset_x = (target_width - fitted_width) // 2
                offset_y = (target_height - fitted_height) // 2
                scale_x = fitted_width / source_width
                scale_y = fitted_height / source_height

                transform_type = getattr(Image, "Transform", Image)
                affine = getattr(transform_type, "AFFINE", Image.AFFINE)
                resampling = getattr(Image, "Resampling", Image)
                bilinear = getattr(resampling, "BILINEAR", Image.BILINEAR)

                def tile_callback(tile_index: int) -> bytes:
                    tile_x = (tile_index % tile_width) * MPS_TILE_SIZE
                    tile_y = (tile_index // tile_width) * MPS_TILE_SIZE
                    matrix = (
                        1.0 / scale_x,
                        0.0,
                        (tile_x - offset_x) / scale_x,
                        0.0,
                        1.0 / scale_y,
                        (tile_y - offset_y) / scale_y,
                    )
                    tile = source.transform(
                        (MPS_TILE_SIZE, MPS_TILE_SIZE),
                        affine,
                        matrix,
                        resample=bilinear,
                        fillcolor=(0, 0, 0, 255),
                    )
                    try:
                        return tile.tobytes()
                    finally:
                        close = getattr(tile, "close", None)
                        if close is not None:
                            close()

                return write_mps(output_path, tile_width, tile_height,
                                 tile_callback, compression=compression)
            finally:
                close = getattr(source, "close", None)
                if close is not None:
                    close()
    except ConversionError:
        raise
    except (OSError, ValueError) as error:
        raise ConversionError("unable to convert image: {}".format(error)) from error


def parse_tile_dimension(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("tile dimension must be an integer") from error
    if not 1 <= parsed <= MPS_MAX_TILE_DIMENSION:
        raise argparse.ArgumentTypeError(
            "tile dimension must be between 1 and {}".format(
                MPS_MAX_TILE_DIMENSION))
    return parsed


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="input image")
    parser.add_argument("output", type=Path, help="output .mps file")
    parser.add_argument("--tiles-width", type=parse_tile_dimension, default=1)
    parser.add_argument("--tiles-height", type=parse_tile_dimension, default=1)
    parser.add_argument("--mode", choices=("fit",), default="fit")
    parser.add_argument("--compression", choices=("raw", "zlib", "auto"),
                        default="auto")
    return parser


def main(argv=None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    try:
        convert_image(args.input, args.output, args.tiles_width,
                      args.tiles_height, mode=args.mode,
                      compression=args.compression)
    except ConversionError as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
