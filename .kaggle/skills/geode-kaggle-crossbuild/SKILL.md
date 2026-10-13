---
name: geode-kaggle-crossbuild
description: Cross-compile a Geode mod (Geometry Dash) for Windows x86_64 on Kaggle using a CPU-only notebook, when no local Windows toolchain (Clang/MSVC) is available. Use when asked to build a Geode mod on Kaggle/CPU, cross-compile a GD mod from Linux, or reproduce the song-search Geode build. Covers clang install, clang-cl symlink, xwin splat, matcool/clang-msvc-sdk toolchain, Geode SDK/CLI/bindings, mod.json version rules, packaging and PE verification.
---

# Geode mod cross-build on Kaggle (CPU-only)

Build a Windows x86_64 Geode mod from a Linux Kaggle notebook when you have no local
Windows toolchain. Every failure below was hit for real; the fixes are documented.

**Success case:** `[28/30] Linking CXX shared library matcool.song-search.dll`,
`geode: build/matcool.song-search.geode (126910 bytes)`,
`ARCH MATCHES GD (x86_64): YES`.

## Why this exists

Geode Windows mods must be built with **Clang 19+** (or MSVC 19.44+) targeting
`x86_64-pc-windows-msvc`, and the output must be **PE32+ x86_64** because GD's
`GeometryDash.exe` and `Geode.dll` are x86_64 (verify: parse the PE header, machine
`0x8664`, win32 platform → `GEODE_TARGET_PLATFORM=Win64`).

A machine with only GCC/MinGW cannot build a Geode mod — ABI-incompatible, and the
docs require Clang/MSVC. Kaggle gives us Linux + fast network instead.

## Links

- https://docs.geode-sdk.org/getting-started/cpp-stuff — **start here**, has the Linux cross-compile commands
- https://docs.geode-sdk.org/getting-started/creating-a-mod
- https://docs.geode-sdk.org/getting-started/geode-cli
- https://docs.geode-sdk.org/classes/ — bindings API docs
- https://docs.geode-sdk.org/mods/configuring/ — mod.json keys, tag list, developers
- https://gist.github.com/matcool/abb65ee59ded3766717c673014c3a2a7 — "linux-win-cross" gist (xwin + matcool toolchain), same author as song-search
- https://github.com/matcool/clang-msvc-sdk — provides `clang-cl-msvc.cmake`
- https://github.com/Jake-Shadle/xwin — downloads/splats MSVC CRT + Windows SDK
- https://apt.llvm.org — clang 19+ (Ubuntu ships 18, which is **too old**)
- https://apt.kitware.com — cmake 3.29+ if needed

## 0. Verify the target architecture first

Never assume. Parse the installed game's PE header:

```python
import struct
d = open('GeometryDash.exe','rb').read()
e = struct.unpack_from('<I', d, 0x3c)[0]
mach = struct.unpack_from('<H', d, e+4)[0]
# 0x8664 -> x86_64, 0x14c -> i386
```

Pick `GEODE_TARGET_PLATFORM=Win64` for x86_64.

## 1. Notebook skeleton

Use `templates/notebook-template.ipynb` in this skill dir — it is a working
python-language notebook. Two rules that cost real runs:

1. **Bash cells need the `%%bash` magic** — a python kernel runs cell bodies as Python.
2. **Embed source files as base64** and write them from one python cell. This avoids
   every escaping bug. Validate generated cells before pushing:
   `compile(src, '<cell>', 'exec')`.

Cells, in order:

1. markdown header
2. `%%bash` — deps (below)
3. `%%bash` — xwin splat, retry loop, layout check
4. `%%bash` — toolchain + Geode CLI + SDK + Geode.lib + profile + bindings + **discover bindings version**
5. python — write base64 sources, align `mod.json`
6. `%%bash` — run the native logic test with `g++` **before** cross-compiling
7. `%%bash` — cmake configure
8. `%%bash` — cmake build
9. `%%bash` — verify `.geode` + DLL PE header

Every cell appends to `/kaggle/working/BUILD-LOG.txt` via
`log(){ echo "== $* ==" | tee -a "$LOG"; }` and checks `$?` / `${PIPESTATUS[0]}`,
exiting with a distinct code per failure.

**Keep `/kaggle/working` small.** Put ALL heavy work in `/tmp/work` (xwin, splat,
toolchain, sdk, bindings, geode-cli). `kaggle kernels output` downloads the whole
`/kaggle/working` tree — with build artifacts in it, the download drops mid-transfer.

## 2. Deps cell

Exactly what the Geode docs list for Linux, plus what we learned:

```bash
apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    git cmake ninja-build clang clang-tools lld llvm libssl3 \
    unzip xz-utils zip ca-certificates curl wget gpg

# Clang 19+ required (21+ recommended). Ubuntu default is 18 = too old.
. /etc/os-release; CODENAME=${VERSION_CODENAME:-noble}
CLANG_VER=$(clang --version | grep -oP 'version [0-9]+' | head -1 | grep -oP '[0-9]+')
if [ "$CLANG_VER" -lt 19 ]; then
  wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg
  echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-19 main" \
    > /etc/apt/sources.list.d/llvm19.list
  apt-get update -qq && apt-get install -y -qq --no-install-recommends clang-19 lld-19 llvm-19
  export PATH=/usr/lib/llvm-19/bin:$PATH; hash -r
fi

# clang-cl is NOT shipped by Debian/Ubuntu at all, and LLVM stopped publishing
# x86_64 linux tarballs after 18.x. clang picks the CL driver from argv[0],
# so a link is enough:
CLANG_DIR=$(dirname "$(command -v clang)")
ln -sf "$CLANG_DIR/clang" "$CLANG_DIR/clang-cl"
command -v clang-cl || { echo "FATAL clang-cl missing"; exit 2; }
command -v lld-link || { echo "FATAL lld-link missing"; exit 2; }
```

Also upgrade cmake from `https://apt.kitware.com/ubuntu ${CODENAME} main` if < 3.29.

## 3. xwin splat

```bash
xwin --accept-license --arch x86_64 --variant desktop --timeout 300 \
     --cache-dir /tmp/work/xwin-cache \
     splat --include-debug-libs --output /tmp/work/splat
```

Three things matter:

- **Global options go BEFORE the subcommand; `--output` belongs to `splat`.** Wrong order
  gives `tip: 'splat --output' exists`.
- **`--include-debug-libs` is required.** Without it CMake's compiler feature test fails
  with `lld-link: error: could not open 'msvcrtd.lib'` (the test links Debug CRT).
- **Wrap in a retry loop** (`for attempt in $(seq 1 60)`). `--cache-dir` makes xwin skip
  already-downloaded payloads, so retries accumulate through flaky connections.

Verify the layout afterwards: `crt/include`, `crt/lib/x86_64`,
`sdk/include/{ucrt,um,shared,winrt}`, `sdk/lib/{ucrt,um}/x86_64`, and
`sdk/include/um/Windows.h` must exist.

## 4. Toolchain, SDK, CLI, loader lib, bindings

```bash
export SPLAT_DIR=/tmp/work/splat
export TOOLCHAIN=/tmp/work/toolchain/clang-cl-msvc.cmake
export HOST_ARCH=x86_64
export PATH=/usr/local/bin:/usr/lib/llvm-19/bin:/tmp/work/geode-cli:$PATH
export GEODE_SDK=/tmp/work/sdk
export GEODE_BINDINGS_REPO_PATH=/tmp/work/bindings

git clone --depth 1 https://github.com/matcool/clang-msvc-sdk.git toolchain
curl -sSL -o geode.zip https://github.com/geode-sdk/cli/releases/download/v3.9.0/geode-cli-v3.9.0-linux.zip
unzip -q geode.zip -d geode-cli && chmod +x geode-cli/geode

# SDK: clone + set-path. `geode sdk install` fails, and `sdk install-binaries`
# fails on this CLI (GitHub 301s it doesn't follow).
git clone --depth 1 --branch main https://github.com/geode-sdk/sdk.git sdk
geode sdk set-path "$PWD/sdk"

# Bindings: clone and export the path so CMake doesn't refetch.
git clone --depth 1 https://github.com/geode-sdk/bindings.git bindings
```

`HOST_ARCH=x86_64` makes the toolchain emit `--target=x86_64-windows-msvc` and link
`user32 kernel32 shell32 ole32 crypt32 advapi32 delayimp gdi32`.

### Geode.lib (skip this and configure fails)

`setup_geode_mod` links `${GEODE_BIN_PATH}/${VERSION}/Geode.lib`, i.e.
`<sdk>/bin/<VER>/Geode.lib`. Missing it errors:

```
No valid loader binary to link to! Install pre-built binaries with `geode sdk install-binaries`, or build Geode from source.
```

Fetch it from the loader release (try `geode sdk install-binaries` first, expect failure):

```bash
SDK_VER=$(cat sdk/VERSION | tr -d '[:space:]')
mkdir -p sdk/bin/${SDK_VER}
curl -L -o geode-win.zip "https://github.com/geode-sdk/geode/releases/download/v${SDK_VER}/geode-v${SDK_VER}-win.zip"
unzip -q geode-win.zip -d geode-win
cp geode-win/Geode.lib sdk/bin/${SDK_VER}/Geode.lib
```

### Geode version + bindings version in mod.json

```json
{
  "geode": "5.11.1",
  "gd": { "win": "<bindings version, DISCOVERED>", "android": "*", "mac": "*", "ios": "*" }
}
```

- `geode` must have **NO `v` prefix**. GeodeFile.cmake reduces both sides with
  `([0-9]+\.[0-9]+)\.[0-9]+ → \1` and `STREQUAL`s them; the SDK's `VERSION` file has no
  `v`, so `"v5.11.1"` becomes `v5.11` vs `5.11` → FATAL_ERROR
  ("Mod X is made for Geode version v5.11.1 but you have 5.11.1 SDK installed").
- Align it at notebook runtime from `sdk/VERSION` rather than trusting the file.
- `gd.win` **must equal the bindings version being compiled against**. Discover it from
  the bindings clone (`ls bindings/bindings/`); do NOT hardcode it. Geode's latest
  bindings change between runs, so derive it every time.

### CLI profile — only needed if you have `dependencies`

`geode project check` (run from `setup_geode_mod`) resolves each dependency and calls
`config.get_current_profile().mods_dir()`. On a bare profile this aborts:

```
| Fail | Unable to read installed mods: No such file or directory (os error 2)
```

(Crash site: `find_dependency(dep, &config.get_current_profile().mods_dir(), true, false)
.nice_unwrap("Unable to read installed mods")` in geode-sdk/cli `src/project.rs`.)

Fix with a profile whose Geode layout exists:

```bash
mkdir -p /tmp/work/gd-scratch/geode/{mods,config,logs,temp}
geode profile add --name kaggle /tmp/work/gd-scratch
geode profile switch kaggle
```

**Otherwise, simply omit `dependencies`** and the code path never runs — prefer that
when the dep is optional (e.g. Jukebox integration), and detect it at runtime with
`Loader::get()->getLoadedMod("fleym.nongd")`.

## 5. Configure and build

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
      -DCMAKE_BUILD_TYPE=Release \
      -DGEODE_TARGET_PLATFORM=Win64 \
      -G Ninja -B build
cmake --build build --config Release
```

CMakeLists:

```cmake
cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
project(my-mod VERSION 1.0.0)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})
if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})
```

`CMAKE_CXX_STANDARD 23` — Geode 5.x expects C++23. Geode auto-enables a PCH whose
generated `cmake_pch.hxx` forces `Geode/DefaultInclude.hpp` then `Geode/Geode.hpp`
**first**, so nothing may be included before `<Geode/Geode.hpp>`.

## 6. Geode v5 porting gotchas

- `CCARRAY_FOREACH(arr, obj)` was **removed** (static_assert). Use
  `for (auto obj : CCArrayExt<T*>(arr))`. This is the most common port failure.
- Recent bindings have **no `GameSoundManager`** (gone after 2.113) — use `FMODAudioEngine`.
- Prefer `CCMenuItemSpriteExtra::setSprite()` over `setNormalImage()`, and
  `FLAlertLayer::init(opacity)` over `CCLayerColor::initWithColor()` (no win binding).
- Verify every binding you use against the bindings `.bro` files or
  `<SDK>/loader/include` before compiling — cross-compile feedback loops are slow.
- `gd::string` is `std::string` on Windows, but `.c_str()` before streaming is safer.

## 7. Package and verify

```bash
geode package new <projdir> --binary <dll> --output out.geode
unzip -l out.geode          # mod.json, <id>.dll, .geode_cache
```

Verify the DLL actually matches GD:

```python
import struct
d = open('matcool.song-search.dll','rb').read()
e = struct.unpack_from('<I', d, 0x3c)[0]
assert d[:2] == b'MZ' and d[e:e+4] == b'PE\x00\x00'
mach  = struct.unpack_from('<H', d, e+4)[0]
chars = struct.unpack_from('<H', d, e+22)[0]
magic = struct.unpack_from('<H', d, e+24)[0]
assert mach == 0x8664                       # x86_64
assert chars & 0x2000                       # DLL
assert magic == 0x20b                       # PE32+
```

Also run pure-logic unit tests natively with `g++ -std=c++20 -I src tests/logic_test.cpp`
before the cross-build — they catch algorithm bugs with a ~2s loop instead of ~7min.

## 8. Kaggle CLI

```bash
kaggle kernels init -p <dir>       # writes kernel-metadata.json
kaggle kernels push -p <dir>       # uploads AND starts execution
kaggle kernels status <user>/<slug>
kaggle kernels output <user>/<slug> --path <dir>   # fetch BUILD-LOG.txt
kaggle kernels pull <user>/<slug>    # source only, NOT outputs
```

`kernel-metadata.json` must set: `code_file` (the .ipynb), `language: python`,
`kernel_type: notebook`, `is_private: true`, `enable_gpu: false`,
`enable_tpu: false`, `enable_internet: true`.

Poll in **short** chunks (~150–230s). Do not block for many minutes at once.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `tip: 'splat --output' exists` | xwin global opts after subcommand | global opts before `splat`, `--output` after it |
| `could not open 'msvcrtd.lib'` | no debug libs | add `--include-debug-libs` |
| `clang-cl: MISSING` | not packaged by Ubuntu | `ln -sf $(dirname $(which clang))/clang .../clang-cl` |
| LLVM tarball returns 9 bytes | asset 404s after 18.x | use apt.llvm.org |
| `No valid loader binary to link to!` | `sdk/bin/<ver>/Geode.lib` missing | copy it from `geode-v<VER>-win.zip` |
| `is made for Geode version v5.11.1 but you have 5.11.1` | `v` prefix in `geode` | drop the `v`, align from `sdk/VERSION` |
| `compiling for Geometry Dash version` FATAL | `gd.win` ≠ bindings version | derive `gd.win` from the bindings clone |
| `Unable to read installed mods: No such file or directory` | bare CLI profile | create profile with `geode/mods`, or drop `dependencies` |
| `CCARRAY_FOREACH` static_assert | removed in Geode v5 | `for (auto x : CCArrayExt<T*>(arr))` |
| `SyntaxError: unterminated string literal` in a generated cell | `\n` escaping in the generator | avoid escapes; validate with `compile()` |
| Cells look like Python errors from bash text | missing `%%bash` | prefix bash cells with `%%bash` |
| `kaggle kernels output` dies mid-transfer | output zip too big | move artifacts out of `/kaggle/working` |

## Reference build in this repo

`T:\Projects\songsearchgeode` — working notebook at
`.kaggle/song-search-build/song-search-build.ipynb`, sources `mod.json`,
`CMakeLists.txt`, `src/main.cpp`, `src/fts_fuzzy_match.h`, `tests/logic_test.cpp`.

Final log excerpt:

```
[27/30] Linking CXX static library bindings/GeodeBindings.lib
[28/30] Linking CXX shared library matcool.song-search.dll
| Done | Successfully packaged matcool.song-search.geode
mod dll: ./matcool.song-search.dll
  machine: x86_64 (64-bit)   is_dll: True   pe_format: PE32+   size: 321024
  ARCH MATCHES GD (x86_64): YES
```
