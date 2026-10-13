# Song Search (Geode port)

> This mod was ported to geode using AI. If you don't like using AI ported mods, feel free to not use it.

A Geode port of matcool's **song-search** from
[small-gd-mods](https://github.com/matcool/small-gd-mods).

The original is a Windows-only DLL that hooks `CustomSongLayer::onSongBrowser` at a
hardcoded offset using MinHook + `gd.h` + `cocos-headers`. This port replaces that with
Geode's `$modify` hooking, so it works across GD versions and platforms instead of
breaking on every game update.

Replaces the editor's song browser with a searchable, scrollable list of your
downloaded songs.

## Features

| Feature | Notes |
|---|---|
| Fuzzy search | Ranks results across **song name + artist**; ported from matcool's vendored `fts_fuzzy_match.h` |
| Filter buttons | 4 modes — Priority, Name, Artist, Song ID — plus an ascending/descending toggle. Based on Improved-Song Browser's sorting, but drawn in our own UI in the dialog corner |
| Preview / select / delete | Per-row play button, select-to-use, and delete with confirmation |
| Jukebox NONG support | Shows a disc on songs with a NONG set, and can display the NONG's name/artist |
| Settings | Toggle filters, toggle NONG symbols, or disable the mod entirely |
| Scrolling | Mouse wheel + drag, with a recycled widget pool to keep scrolling smooth |

Everything except the NONG symbols works with no other mods installed.

## Jukebox / AutoNong integration

Jukebox (`fleym.nongd`) ships **no public API** — no `api` section in its `mod.json`,
and its headers are private. So the port does not reimplement NONG storage. Instead:

1. A spy hook runs after `CustomSongWidget::init`, after Jukebox has already laid out
   its own UI.
2. It looks for Jukebox's disc button, which Jukebox tags with the ID `"nong-button"`.
3. From that it copies the disc's sprite frame (which comes from a spritesheet Jukebox
   registers globally, including the gold "verified" variant) and the resolved song
   name / artist.
4. Our rows then repaint that icon and metadata.

This is a hard **no-op when Jukebox is not installed** — checked via
`Loader::get()->getLoadedMod("fleym.nongd")` — so the feature cannot break the mod for
people without Jukebox.

## Building

Geode Windows mods need Clang 19+ (or MSVC 19.44+) targeting `x86_64-pc-windows-msvc`.
GCC/MinGW will not work — the ABI is incompatible.

The reference build here was done **cross-compiling on Kaggle's CPU-only notebooks**,
because the machine this was ported on had no Windows toolchain. The full recipe lives
in [`skills/geode-kaggle-crossbuild/SKILL.md`](skills/geode-kaggle-crossbuild/SKILL.md)
— a reusable skill covering clang + clang-cl, `xwin`, the
[matcool/clang-msvc-sdk](https://github.com/matcool/clang-msvc-sdk) toolchain, the Geode
SDK/CLI/bindings, `Geode.lib`, the `mod.json` version rules, packaging, and PE
verification.

A working notebook template ships with the skill at
`skills/geode-kaggle-crossbuild/templates/notebook-template.ipynb`.

Short version, if you already have the toolchain:

```bash
export SPLAT_DIR=/path/to/splat
export TOOLCHAIN=/path/to/clang-cl-msvc.cmake
export HOST_ARCH=x86_64
export GEODE_SDK=/path/to/sdk
export GEODE_BINDINGS_REPO_PATH=/path/to/bindings

cmake -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN -DCMAKE_BUILD_TYPE=Release \
      -DGEODE_TARGET_PLATFORM=Win64 -G Ninja -B build
cmake --build build --config Release
```

Run the native logic tests before a cross-build (fast feedback, no Windows headers
needed):

```bash
g++ -std=c++20 -I src -o /tmp/logic_test tests/logic_test.cpp && /tmp/logic_test
```

## Layout

```
mod.json              mod metadata, tags, settings
CMakeLists.txt        standard setup_geode_mod build
about.md              in-game description (disclaimer + credits + port notes)
src/
  main.cpp            search layer, rows, filter buttons, hooks
  fts_fuzzy_match.h   vendored fuzzy matcher (public domain, Forrest Smith)
resources/            filter button icons (from Improved-Song Browser)
tests/
  logic_test.cpp      pure-C++ checks for search/sort/scroll-clamp logic
.kaggle/              recipe used to build on Kaggle
skills/               the Geode-on-Kaggle build skill
```

## Credits

- **matcool** — [small-gd-mods](https://github.com/matcool/small-gd-mods), the original mod
- **Alphalaneous** — [Improved Song Browser](https://github.com/Alphalaneous/Improved-Song-Browser), source of the filter/sort design and its icons
- **Fleym & Flafy** — [Jukebox](https://github.com/Fleeym/jukebox) and [AutoNong](https://github.com/FlafyDev/auto-nong), for the NONG support
- **Forrest Smith** — `fts_fuzzy_match.h`, the public-domain fuzzy matcher
