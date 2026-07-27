# CLAUDE.md — endstone-mediaplayer

## Project

Pure C23 Endstone plugin for NBS music and map video playback in Minecraft
Bedrock Dedicated Server. It constructs the required MSVC and libc++ ABI
objects to interface with Endstone without a C++ compiler.

## Language & Toolchain

- **C standard**: C23 (`-std:c23`).  Backward compatibility with C11/C17/C99 is not required.
- **Primary compiler**: Clang 20+ (`clang-cl` on Windows).
- **Build system**: CMake 3.21+ with Ninja generator.
- **MSVC compatibility**: Only required for ABI interop (STL object layout).  Do not use
  MSVC-specific extensions unless explicitly needed for ABI correctness.
- **Target**: Windows and Linux.

## Coding Principles

- Prefer **standard C23** features when they clearly improve readability, safety,
  maintainability, or correctness.
- Do not introduce modern language features solely because they are available.
- Favor **simple and idiomatic C** over clever or overly abstract code.
- Avoid unnecessary metaprogramming, macro tricks, or compiler extensions.
- Prefer standard C23 over compiler-specific extensions whenever practical.
- Introduce compiler-specific features only when they provide a significant
  engineering benefit (e.g., `__declspec(dllexport)` for the DLL entry point).
- Use C23 features conservatively and intentionally.
- Do not rewrite existing working code merely to use newer language features.
- Keep APIs and implementations easy to understand for experienced C developers.
- **Prioritize consistency with the surrounding codebase** over adopting the newest syntax.

## Code Style

- Use `//` comments, not `/* */`.
- No `typedef` — all structs referenced as `struct tag`.
- Functions: `snake_case`.  Types: `snake_case` with `_t` suffix.
- Constants: `UPPER_SNAKE_CASE`.  Vtable slots: `ES_CLASS_SLOT_METHOD`.
- Offsets: `ES_CLASS_OFF_FIELD`.  Globals: `g_` prefix.
- Use `stdint.h` types (`uint64_t`, `int32_t`) for ABI-critical code.
- Use `static` for all internal functions and variables.
- `calloc` for heap allocations (zero-initialized).  Check for Nnullptr after allocation.
- Use `nullptr` (C23 keyword), never `NULL`.
- Use standard include guards (`#ifndef`/`#define`/`#endif`), never `#pragma once`.

## Memory Management

- Use `cppcompat` library for all MSVC STL object construction/destruction.
- **Shared-heap invariant (Windows).** `cppcompat` uses `malloc`/`free` and
  Endstone uses `operator new`/`delete`, but both bind to the *same* dynamic
  UCRT heap because the plugin is always built with
  `MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"`.  Blocks are therefore
  interchangeable across the boundary: the runtime may `operator delete` a
  block the plugin `malloc`'d, and vice versa.  The managed-screen write path
  (`world_write_abi.c`) depends on this for the fake `BlockStates` nodes and
  every by-value `std::string` parameter a callee destroys.
  CMake enforces the runtime-library choice and fails configuration if it is
  ever changed — **never switch this project to a static CRT.**
- Crossing the boundary is only safe for *raw blocks*, never for typed
  ownership: still never hand Endstone a pointer it will treat as a different
  type, and still match every construction with exactly one destruction.
- For `std::string` pass-by-value to C++ APIs: use `STR_GUARD` macro from
  `abi_helpers.h`.  The C++ callee calls `operator delete` on the heap buffer
  for heap-allocated strings (>15 chars).  `STR_GUARD` saves the pre-call state
  and either destroys normally (SSO) or resets to SSO-empty (heap) to avoid
  double-free.
- `createBossBar` uses `std::move(title)` — the string is moved, not destroyed.
  `cpp_string_destroy` is safe after this call.
- Zero vectors in `plugin_on_disable` to prevent framework cleanup crashes.

## Key ABI Details

- `std::string`: 32 bytes, SSO threshold 15 chars, `_Myres` at offset 24.
- `std::vector`: 24 bytes (three pointers: `_Myfirst`, `_Mylast`, `_Myend`).
- `std::function`: 64 bytes, `_Func_impl` self-pointer at offsets 0x30 and 0x38.
- `std::variant<...>`: 64 bytes, index at offset 32.
- `unique_ptr`: 8 bytes, returned via hidden pointer (non-trivial destructor).
- `shared_ptr`: 16 bytes (pointer + control block).
- Microsoft x64 calling convention: RCX/RDX/R8/R9 + stack.  Hidden pointer for
  return values > 8 bytes and pass-by-value parameters > 8 bytes.

## Project Structure

Every translation unit in this project is C.  There is no C++ source anywhere
outside `third_party/`, and the project's own headers carry no `extern "C"`
guards because nothing C++ ever includes them.

```
include/   — Public headers (abi_helpers.h, endstone_abi.h)
  abi/            — Measured per-platform ABI constants (slots, offsets, sizes)
  mediaplayer/    — bedrock/, map/, screen/, video/ module headers
src/       — Source
  plugin.c        — Plugin lifecycle, command handler, event/scheduler registration
  endstone_api.c  — Endstone API wrappers (Player, BossBar, sendMessage, fopen_utf8)
  sfunc.c         — std::function ABI construction (vtable, trampoline, pool)
  music/          — NBS catalog, cache, sessions and /mpm commands
  bedrock/        — Pure-C world read/write ABI path (Player, Dimension, Block,
                    item frames and filled maps) for Windows and Linux
  map/            — Map ABI adapter and the renderer/canvas pipeline
  screen/         — Screen geometry, registry and JSON persistence
  video/          — .mcv format, playback sessions, catalog, /mpv commands
third_party/ — External libraries (cppcompat, cJSON, miniz, nbsparser, stb)
  cppcompat/      — MSVC STL ABI compatibility (compiled directly, no .lib)
  nbsparser/      — NBS file format parser
  stb/            — stb_ds dynamic array (MIT)
build/     — Build output (compile_commands.json generated here)
```

## Building

```bash
cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo -B build
cmake --build build
```

Output: `build/endstone_mediaplayer.dll`

## Git Conventions

Follow the same format as [spark for Endstone](https://github.com/EndstoneMC/spark):

```
type: description
```

Types: `feat`, `fix`, `refactor`, `build`, `ci`, `docs`, `chore`, `release`.

- All lowercase, no period at end.
- Description is imperative ("add feature" not "added feature").
- Keep the first line ≤72 characters.
