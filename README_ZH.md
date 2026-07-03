# MediaPlayer for Endstone

[![CI](https://github.com/ReallocAll/endstone-mediaplayer/actions/workflows/build.yml/badge.svg)](https://github.com/ReallocAll/endstone-mediaplayer/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[English](README.md) | [简体中文](README_ZH.md)

在 Minecraft Bedrock 专用服务器中播放 NBS 音乐文件的 [Endstone](https://github.com/EndstoneMC/endstone) 插件。

## 功能

- **NBS 播放**：完整支持 `.nbs`（Note Block Studio）格式 v0–v5
- **BossBar 进度条**：实时显示歌曲名和播放进度
- **播放列表**：多首曲目排队播放，支持循环控制
- **多种显示模式**：BossBar、Popup、Tip、隐藏
- **命令控制**：`/mpm` 命令，支持 Tab 补全

## 安装

1. 将 `endstone_mediaplayer.dll` 放入 `plugins/` 目录
2. 创建 `plugins/endstone_mediaplayer/nbs/` 目录
3. 将 `.nbs` 文件放入 `nbs/` 目录
4. 启动服务器

```
plugins/
├── endstone_mediaplayer.dll
└── endstone_mediaplayer/
    └── nbs/
        ├── song1.nbs
        └── song2.nbs
```

## 命令

| 命令                              | 说明                   |
| --------------------------------- | ---------------------- |
| `/mpm help`                     | 显示帮助               |
| `/mpm list [filter]`            | 列出所有歌曲，支持过滤 |
| `/mpm add <index> [loop] [bar]` | 添加歌曲到播放列表     |
| `/mpm del <index>`              | 从播放列表删除         |
| `/mpm pause`                    | 暂停播放               |
| `/mpm resume`                   | 继续播放               |
| `/mpm stop`                     | 停止播放并清空列表     |
| `/mpm playlist`                 | 查看当前播放列表       |

**参数说明：**

- `loop`：`-1` = 无限循环，`1` = 一次（默认），`N` = N 次
- `bar`：`0` = 不显示，`1` = 弹窗，`2` = Tip，`3` = bossbar（默认）

## 构建

**依赖：** CMake 3.29+、Ninja、Clang 22+（`clang-cl`）

```bash
cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo -B build
cmake --build build
```

产物：`build/endstone_mediaplayer.dll`

## 技术架构

- **语言**：C23
- **第三方库**：`third_party/` — cppcompat（MSVC STL 兼容）、nbsparser（NBS 解析）、stb_ds（MIT，动态数组）

## 许可

本项目使用 **GPL-3.0** 许可。详见 [LICENSE](LICENSE)。

`third_party/stb/` 使用 MIT 许可（公共领域）。
