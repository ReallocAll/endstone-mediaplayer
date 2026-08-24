# MediaPlayer for Endstone

[![CI](https://github.com/ReallocAll/endstone-mediaplayer/actions/workflows/build.yml/badge.svg)](https://github.com/ReallocAll/endstone-mediaplayer/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[English](README.md) | [简体中文](README_ZH.md)

An [Endstone](https://github.com/EndstoneMC/endstone) plugin that plays NBS music and `.mcv` video through maps in Minecraft Bedrock Dedicated Server.

## Features

- **NBS Playback**: Full support for `.nbs` (Note Block Studio) format v0–v5
- **BossBar Progress**: Real-time progress bar with song title and elapsed/total time
- **Playlist Queue**: Multiple tracks queued with loop control
- **Multiple Display Modes**: BossBar, popup, tip, or hidden
- **Command Control**: `/mpm` command with tab completion
- **Public Map Screens**: automatically show map video to nearby players
- **Reusable API**: a single-header C23 SDK and installable Python binding

## Installation

1. Place `endstone_mediaplayer.dll` (Windows) or `endstone_mediaplayer.so` (Linux) into your `plugins/` directory
2. Create `plugins/endstone_mediaplayer/nbs/` and `plugins/endstone_mediaplayer/video/`
3. Put `.nbs` files in `nbs/` and converted `.mcv` files in `video/`
4. Start the server

```
plugins/
├── endstone_mediaplayer.dll / endstone_mediaplayer.so
└── endstone_mediaplayer/
    ├── nbs/
    │   ├── song1.nbs
    │   └── song2.nbs
    └── video/
        ├── video1.mcv
        └── video2.mcv
```

## Music playback (`/mpm`)

| Command                           | Description                         |
| --------------------------------- | ----------------------------------- |
| `/mpm help`                     | Show help                           |
| `/mpm list [filter]`            | List all songs, optionally filtered |
| `/mpm add <index> [loop] [bar]` | Add song to playlist                |
| `/mpm del <index>`              | Remove song from playlist           |
| `/mpm pause`                    | Pause playback                      |
| `/mpm resume`                   | Resume playback                     |
| `/mpm stop`                     | Stop playback and clear playlist    |
| `/mpm playlist`                 | Show current playlist               |

**Parameters:**

- `loop`: `-1` = infinite, `1` = once (default), `N` = N times
- `bar`: `0` = off, `1` = popup, `2` = tip, `3` = bossbar (default)

### Map video (`/mpv`)

Screen-control `/mpv` commands require operator status. Every player can use `/mpv help` and `/mpv watch [on|off]`; the server console passes the central permission check, although world operations such as screen creation and managed-screen deletion still require an in-game player in the relevant dimension.

| Command | Description |
| --- | --- |
| `/mpv help` | Show concise video help |
| `/mpv screens` | List registered public screens |
| `/mpv list [filter]` | List converted `.mcv` videos with their play indices |
| `/mpv create <name>` | Discover the backing wall at the player and create a screen |
| `/mpv materialize <name>` | Materialize an API-created logical screen at an OP player's backing wall |
| `/mpv delete <name>` | Delete a plugin-managed screen and its remaining managed frames |
| `/mpv info <name>` | Show screen geometry and public access information |
| `/mpv play <screen> <index> [loop]` | Play a video by its `/mpv list` index; its tile grid must match the screen's exactly |
| `/mpv images [filter]` | List valid MPS1 static images with their indices |
| `/mpv image <screen> <image-index>` | Display an MPS1 static image; its tile grid must match the screen's exactly |
| `/mpv pause <screen>` | Pause playback |
| `/mpv resume <screen>` | Resume playback |
| `/mpv stop <screen>` | Stop playback |
| `/mpv status <screen>` | Show playback status |
| `/mpv watch [on\|off]` | Show or change whether this player receives public screen video and music |

Screens use a fixed public viewer model. Any online player—OP or not—with a valid snapshot in the same dimension and within 16 blocks of the screen's axis-aligned tile bounds receives video and screen music automatically, unless they disable both with `/mpv watch off`.

When a video has an exactly matching `.nbs` base name, the plugin plays it as the screen soundtrack. The video is the master clock: pausing or stopping affects both, every video loop restarts the NBS, and music remaining after the video ends is stopped. A missing or invalid matching NBS does not prevent silent video playback.

To create a screen, stand in the air immediately in front of a rectangular solid backing wall no larger than 7×4 and run `/mpv create <name>`. The player's feet-level air cell defines the bottom screen row; backing and floor blocks below that Y are ignored. Every actual display cell must be air. The plugin discovers the wall from that Y upward, validates every tile, places item frames, and inserts the matching filled maps automatically through Endstone's ItemFrame state API.

An API-created logical screen can be materialized in place by an OP player with
`/mpv materialize <name>`. The command requires the discovered backing to have
exactly the logical screen's tile dimensions; it installs maps left-to-right,
top-to-bottom using the same row/column labels as `create`. The screen entry,
runtime identity, surface, pixels, and existing API handle remain valid while
its backend becomes plugin-managed. The player's feet Y likewise defines the
materialized screen's bottom row.

### Logical screens, physical maps, and the API

The public C ABI and Python binding can create logical screens from 1×1 through 1024×1024 tiles. A logical screen is a sparse surface for pixel updates; it does not materialize a wall or a million physical maps until an OP uses `/mpv materialize`. The world-facing `/mpv create` and `materialize` commands remain bounded by a 7×4 backing wall and item-frame map screen, and MCV1 remains compatible with this physical path with a fixed maximum 7×4 tile grid. Treat the logical dimensions and physical map dimensions as separate limits. Materialization is an in-place upgrade, so the API screen handle remains valid.

The API is declared in the single public C23 header [`include/endstone_mediaplayer_api.h`](include/endstone_mediaplayer_api.h). Its reference is [`docs/sdk.md`](docs/sdk.md). It can be used without an import library. The reusable Python binding is documented in [`python/README.md`](python/README.md); its provider must already be loaded by Endstone. Reacquire API tables and handles after a MediaPlayer enable/reload cycle.

### Network compression and screen size

Map video consumes server upload bandwidth. Set either `compression-algorithm=zlib` or `compression-algorithm=snappy` in `server.properties`, then restart the server. The values below are approximate measurements; actual bandwidth varies with video content, frame rate, and viewer count, and total server upload normally increases with the number of simultaneous viewers.

| Algorithm | 1×1 screen upload | Recommended practical limit | Best suited for |
| --- | ---: | ---: | --- |
| `zlib` | About 3 Mbps per viewer | 3×2 | Better compression; small screens on remote servers |
| `snappy` | About 5 Mbps per viewer | 7×4 | Faster compression; local or LAN servers |

Screens beyond these recommended sizes are likely to stutter.

### Converting video

```shell
python tools/convert_video.py input.mp4 -o output.mcv --tiles 4x2 --fps 20 --mode fit
```

MCV is the plugin's video container (format version 1, extension `.mcv`). A file is a 128-byte header protected by a CRC32, continuous frame data (per-frame independent zlib streams by default, or raw ABGR with `--no-compress`), and a trailing index of 24-byte entries carrying each frame's 64-bit offset, stored size, and CRC32. The C reader validates the header in O(1) and fetches index entries on demand through a small sequential window — memory stays constant no matter how long the video is. Uncompressed frames are verified against their index CRC32; compressed frames are validated by the zlib stream's own integrity check during decompression. Seeking uses `_fseeki64`/`_ftelli64` on Windows and `fseeko`/`ftello` on POSIX, so multi-GiB files are fully addressable. `MCV_MAX_FRAME_COUNT` is 6,000,000 (more than 72 hours at 20 fps).

The converter starts one FFmpeg decoding process, requests raw `rgba` frames, and streams each frame directly into a same-directory temporary container, compressing with zlib level 6 by default (`--level` adjusts it, `--no-compress` disables it). Memory holds roughly one raw frame plus the frame index it buffers in RAM (24 bytes per frame). After FFmpeg exits successfully, the converter appends the index, finalizes and fsyncs the CRC-carrying header, then atomically installs the result with `os.replace`. Errors and interruption remove the temporary file and preserve an existing destination.

### Static images (MPS1)

Convert one image into the tiled MPS1 container with Pillow installed:

```shell
python tools/convert_image.py INPUT OUTPUT --tiles-width W --tiles-height H --mode fit
```

`--mode fit` is the default aspect-fit conversion mode. MPS1 supports logical tile dimensions up to 1024×1024, but `/mpv image` still requires an exact match with the target screen. The converter samples one output tile at a time; playback validates the header and reads tiles lazily, retaining only bounded index/cache and tile state. Starting `/mpv image` replaces any active video or soundtrack on that screen; starting `/mpv play` likewise replaces an active static image. Static images do not support pause/resume, but `/mpv stop` does stop them.

Large logical screens and frequent updates can still consume substantial memory and bandwidth. Sparse surfaces bound resident and pending tile state, but network traffic depends on changed tiles, frame rate, compression, and the number of viewers; physical map playback also scales upload traffic with viewers.

## Building

**Requirements:** CMake 3.21+, Ninja, and Clang (`clang-cl` on Windows;
`clang` and `clang++` on Linux, plus LLD).

Windows:

```bash
cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo -B build
cmake --build build
```

Linux:

```bash
cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -B build
cmake --build build
```

Output: `endstone_mediaplayer.dll` on Windows or `endstone_mediaplayer.so` on Linux.

## Architecture

- **Language**: C23
- **Platforms**: Windows and Linux share one business codebase and implement functionality through their respective ABI layers
- **Third-party**: `third_party/` — cppcompat, cJSON (MIT, JSON), miniz (MIT, zlib/CRC32), nbsparser (NBS parsing), stb_ds (MIT, dynamic arrays)

## License

This project is licensed under **GPL-3.0**. See [LICENSE](LICENSE) for details.

`third_party/cjson/` and `third_party/miniz/` are MIT-licensed. `third_party/stb/` is MIT-licensed (public domain).
