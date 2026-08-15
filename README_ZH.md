# MediaPlayer for Endstone

[![CI](https://github.com/ReallocAll/endstone-mediaplayer/actions/workflows/build.yml/badge.svg)](https://github.com/ReallocAll/endstone-mediaplayer/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[English](README.md) | [简体中文](README_ZH.md)

在 Minecraft Bedrock 专用服务器中播放 NBS 音乐，并通过地图播放 `.mcv` 视频的 [Endstone](https://github.com/EndstoneMC/endstone) 插件。

## 功能

- **NBS 播放**：完整支持 `.nbs`（Note Block Studio）格式 v0–v5
- **BossBar 进度条**：实时显示歌曲名和播放进度
- **播放列表**：多首曲目排队播放，支持循环控制
- **多种显示模式**：BossBar、Popup、Tip、隐藏
- **命令控制**：`/mpm` 命令，支持 Tab 补全
- **公共地图屏幕**：自动向附近玩家展示地图视频
- **可复用 API**：单头文件 C23 SDK 与可安装的 Python 绑定

## 安装

1. 将 `endstone_mediaplayer.dll`（Windows）或 `endstone_mediaplayer.so`（Linux）放入 `plugins/` 目录
2. 创建 `plugins/endstone_mediaplayer/nbs/` 和 `plugins/endstone_mediaplayer/video/` 目录
3. 将 `.nbs` 文件放入 `nbs/`，转换后的 `.mcv` 文件放入 `video/`
4. 启动服务器

```
plugins/
├── endstone_mediaplayer.dll / endstone_mediaplayer.so
└── endstone_mediaplayer/
    ├── nbs/
    │   ├── song1.nbs
    │   └── song2.nbs
    └── video/
        └── video1.mcv
        └── video2.mcv
```

## 播放音乐（`/mpm`）

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

### 地图视频（`/mpv`）

控制屏幕的 `/mpv` 命令要求玩家是 OP。所有玩家均可使用 `/mpv help` 和 `/mpv watch [on|off]`；服务器控制台可通过统一权限检查，但创建屏幕、删除托管屏幕等世界操作仍要求执行者是位于相应维度的游戏内玩家。

| 命令                                  | 说明                                                           |
| ------------------------------------- | -------------------------------------------------------------- |
| `/mpv help`                         | 显示简洁的视频帮助                                             |
| `/mpv screens`                      | 列出已注册的公共屏幕                                           |
| `/mpv list [filter]`                | 列出转换后的`.mcv` 视频及其播放序号                          |
| `/mpv create <name>`                | 从玩家位置自动发现背板并创建屏幕                               |
| `/mpv materialize <name>`           | 由 OP 玩家把 API 创建的逻辑屏幕物化到背板                         |
| `/mpv delete <name>`                | 删除插件托管屏幕及仍存在的托管画框                             |
| `/mpv info <name>`                  | 显示屏幕几何和公共访问规则                                     |
| `/mpv play <screen> <index> [loop]` | 按`/mpv list` 序号播放视频；视频的地图格网必须与屏幕完全一致 |
| `/mpv images [filter]`                | 列出有效的 MPS1 静态图像及其序号                             |
| `/mpv image <screen> <image-index>`   | 显示 MPS1 静态图像；图像格网必须与目标屏幕完全一致             |
| `/mpv pause <screen>`               | 暂停播放                                                       |
| `/mpv resume <screen>`              | 继续播放                                                       |
| `/mpv stop <screen>`                | 停止播放                                                       |
| `/mpv status <screen>`              | 显示播放状态                                                   |
| `/mpv watch [on\|off]`              | 查看或设置自己是否接收公共屏幕画面和音乐                       |

屏幕固定为公共模式。任何在线玩家（无需 OP）只要快照有效、与屏幕处于同一维度，并且到屏幕地图格轴对齐包围盒的距离不超过 16 格，就会自动收到视频和屏幕音乐；玩家可使用 `/mpv watch off` 同时关闭两者。

当视频存在主文件名完全相同的 `.nbs` 时，插件会将它作为屏幕配乐播放。视频是主时钟：暂停或停止会同时作用于画面和音乐，每次视频循环都会从头播放 NBS，视频结束时仍未结束的音乐也会停止。没有同名 NBS 或 NBS 无效时，视频仍可静音播放。

创建时，OP 站在一个不超过 7×4 的矩形实体背板正前方空气格内，然后执行 `/mpv create <name>`。玩家脚所在空气格的 Y 坐标定义屏幕最底行；低于该 Y 的背板和地板方块会被忽略，只有实际屏幕格必须是空气。插件从该 Y 开始向上识别背板和朝向、校验所有格、放置空展示框，并把带标签的地图交给创建者。请根据行列标签从左到右、从上到下手动安装地图。

API 创建的逻辑屏幕可以由 OP 玩家执行 `/mpv materialize <name>` 原地物化。玩家脚所在空气格的 Y 坐标定义物理屏幕最底行，发现的背板格网必须与逻辑屏幕的尺寸完全一致；低于该 Y 的背板和地板不参与发现。地图会按照与 `create` 相同的行列标签从左到右、从上到下安装。屏幕条目、运行时身份、Surface、像素和已有 API handle 均保持不变，只将后端升级为插件托管。

### 逻辑屏幕、物理地图与 API

公共 C ABI 和 Python 绑定可以创建每个轴 1～1024 格的逻辑屏幕。逻辑屏幕是用于像素更新的稀疏表面，除非 OP 执行 `/mpv materialize`，否则不会生成墙体或一次性物化百万张物理地图。面向世界的 `/mpv create` 和 `materialize` 均受 7×4 背板和展示框地图屏幕限制；MCV1 继续兼容这一物理路径，格网最大固定为 7×4。逻辑尺寸与物理地图尺寸是两套独立限制；物化是原地升级，因此 API 屏幕 handle 仍然有效。

### 公共 C SDK 与 Python 绑定

单头文件 [`include/endstone_mediaplayer_api.h`](include/endstone_mediaplayer_api.h) 面向 C23；唯一稳定导出为版本化的 `endstone_mediaplayer_get_api`。SDK 在进程中查找 Endstone 已加载的插件（Windows 同时识别 `endstone_mediaplayer-<hash>.dll` 影子副本），不加载第二份模块，也不需要导入库。普通硬依赖使用 `mp_get_api_v1()`，可选依赖使用 `mp_try_get_api_v1()`：

```c
#include "endstone_mediaplayer_api.h"

mp_api_v1 mp = mp_get_api_v1();
mp_screen_handle screen = MP_INVALID_SCREEN_HANDLE;
if (mp.screen_find("live", &screen) == MP_OK) {
    mp.screen_clear(screen);
}
```

`MP_OK` 等于 0；其他操作返回稳定的负值 `mp_result`，并可用 `mp.result_string()` 转为文字。句柄是不透明且带世代检查的整数。MediaPlayer 每次启用都会更换 `instance_id`，禁用时先把 API 标为不可用；消费者必须在自身每次启用时重新取得 API 和屏幕句柄，不能跨重载保存回调或句柄。所有调用都必须位于 Endstone 主线程。完整错误表、结构体演进规则、事务示例和 C23 用法见 [`docs/sdk.md`](docs/sdk.md)。

像素输入支持 `ABGR8888`、`BGRA8888` 和 `RGBA8888`。实时内容可用 `frame_begin`、`frame_update_region`、`frame_commit`/`frame_abort` 原子提交；相同内容会被脏格检测抑制，但大屏或高帧率仍会消耗显著带宽，应查询能力/统计并设置合理的展示预算。

可复用 Python 包位于 [`python/`](python/)：

```python
from endstone_mediaplayer import mediaplayer

with mediaplayer() as mp:
    mp.screen("live").update(pixels, format="bgra8888")
```

它使用同一套 C ABI，隔离全部 `ctypes` 细节，接受连续的 `bytes`、`bytearray`、`memoryview` 及 NumPy 缓冲区，并把 provider 不可用、重载、无效句柄等结果映射为明确异常。详细生命周期、缓冲区和事务说明见 [`python/README.md`](python/README.md)。

### 网络压缩与屏幕尺寸

地图视频会占用服务器上行带宽。请在 `server.properties` 中设置 `compression-algorithm=zlib` 或 `compression-algorithm=snappy`，修改后重启服务器。以下为实测近似值；实际带宽会随视频内容、帧率和观看人数变化，服务器总上行带宽通常随同时观看人数增加。

| 压缩算法   |   1×1 屏幕上传带宽 | 建议实用上限 | 适用场景                           |
| ---------- | ------------------: | -----------: | ---------------------------------- |
| `zlib`   | 每名观看者约 3 Mbps |         3×2 | 压缩率较好，推荐公网服务器的小屏幕 |
| `snappy` | 每名观看者约 5 Mbps |         7×4 | 压缩速度快，推荐本机或局域网使用   |

超过上述建议尺寸容易导致播放卡顿。

### 转换视频

```Shell
python tools/convert_video.py input.mp4 -o output.mcv --tiles 4x2 --fps 20 --mode fit
```

MCV 是插件的视频容器格式（格式版本 1，扩展名 `.mcv`）。文件由带 CRC32 校验的 128 字节头、连续帧数据（默认逐帧独立 zlib 压缩，`--no-compress` 可存原始 ABGR），以及位于文件末尾的 24 字节索引条目组成——每条记录该帧的 64 位偏移、存储大小和 CRC32。C reader 以 O(1) 校验文件头，通过小型顺序窗口按需读取索引条目——无论视频多长内存占用恒定。未压缩帧按索引中的 CRC32 校验；压缩帧则在解压时由 zlib 流自带的完整性校验验证。寻址在 Windows 使用 `_fseeki64`/`_ftelli64`，在 POSIX 使用 `fseeko`/`ftello`，可完整寻址数 GiB 大文件。`MCV_MAX_FRAME_COUNT` 为 6,000,000（20fps 下超过 72 小时）。

转换器只启动一个 FFmpeg 解码进程，请求原始 `rgba` 帧，并把每帧直接写入目标目录中的临时容器，默认以 zlib 等级 6 压缩（`--level` 可调，`--no-compress` 关闭）。内存占用约为一帧原始数据，外加缓存在内存中的帧索引（每帧 24 字节）。FFmpeg 成功退出后，转换器追加索引、写回并 fsync 带 CRC 的文件头，再用 `os.replace` 原子安装结果。出错或中断时会删除临时文件，并保留已有目标文件。

### 静态图像（MPS1）

安装 Pillow 后，将单张图像转换为 MPS1 分块容器：

```Shell
python tools/convert_image.py INPUT OUTPUT --tiles-width W --tiles-height H --mode fit
```

`--mode fit` 是默认的保持比例适配模式。MPS1 支持每个轴最多 1024×1024 格，但 `/mpv image` 仍要求图像格网与目标屏幕完全一致。转换器一次只采样一个输出格；播放时按需校验头部并读取格，只保留有界的索引/缓存和单格状态。对同一屏幕执行 `/mpv image` 会替换正在播放的视频或配乐；执行 `/mpv play` 也会替换静态图像。静态图像不支持暂停/继续，但 `/mpv stop` 可以停止它。

大型逻辑屏幕和高频更新仍可能消耗大量内存与带宽。稀疏表面会限制常驻及待处理格数量，但网络流量取决于变化格、帧率、压缩方式和观看人数；物理地图播放的上行流量也会随观看人数增加。

## 构建

**依赖：** CMake 3.21+、Ninja、Clang（Windows 使用 `clang-cl`；Linux 使用
`clang` 和 `clang++`，并需要 LLD）。

Windows：

```bash
cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo -B build
cmake --build build
```

Linux：

```bash
cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -B build
cmake --build build
```

产物：Windows 为 `endstone_mediaplayer.dll`，Linux 为 `endstone_mediaplayer.so`。

## 技术架构

- **语言**：C23
- **平台结构**：Windows 与 Linux 共用业务源码，通过各自的 ABI 层实现功能
- **第三方库**：`third_party/` — cppcompat、cJSON（MIT，JSON）、miniz（MIT，zlib/CRC32）、nbsparser（NBS 解析）、stb_ds（MIT，动态数组）

## 许可

本项目使用 **GPL-3.0** 许可。详见 [LICENSE](LICENSE)。

`third_party/cjson/` 与 `third_party/miniz/` 使用 MIT 许可。`third_party/stb/` 使用 MIT 许可（公共领域）。
