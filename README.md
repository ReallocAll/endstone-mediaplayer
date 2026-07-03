# MediaPlayer for Endstone

[![CI](https://github.com/ReallocAll/endstone-mediaplayer/actions/workflows/build.yml/badge.svg)](https://github.com/ReallocAll/endstone-mediaplayer/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[English](README.md) | [简体中文](README_ZH.md)

An [Endstone](https://github.com/EndstoneMC/endstone) plugin that plays NBS music files on Minecraft Bedrock Dedicated Server.

## Features

- **NBS Playback**: Full support for `.nbs` (Note Block Studio) format v0–v5
- **BossBar Progress**: Real-time progress bar with song title and elapsed/total time
- **Playlist Queue**: Multiple tracks queued with loop control
- **Multiple Display Modes**: BossBar, popup, tip, or hidden
- **Command Control**: `/mpm` command with tab completion

## Installation

1. Place `endstone_mediaplayer.dll` into your `plugins/` directory
2. Create `plugins/endstone_mediaplayer/nbs/`
3. Put your `.nbs` files into the `nbs/` directory
4. Start the server

```
plugins/
├── endstone_mediaplayer.dll
└── endstone_mediaplayer/
    └── nbs/
        ├── song1.nbs
        └── song2.nbs
```

## Commands

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

## Building

**Requirements:** CMake 3.29+, Ninja, Clang 22+ (`clang-cl`)

```bash
cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo -B build
cmake --build build
```

Output: `build/endstone_mediaplayer.dll`

## Architecture

- **Language**: C23
- **Third-party**: `third_party/` — cppcompat (MSVC STL compatibility), nbsparser (NBS parsing), stb_ds (MIT, dynamic arrays)

## License

This project is licensed under **GPL-3.0**. See [LICENSE](LICENSE) for details.

`third_party/stb/` is MIT-licensed (public domain).
