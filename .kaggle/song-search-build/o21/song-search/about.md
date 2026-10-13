> This mod was ported to geode using AI. If you don't like using AI ported mods, feel free to not use it.

# Song Search

An in-game search dialog for your downloaded songs, with fuzzy matching, sortable
filters, and Jukebox NONG support.

The search dialog replaces the default song browser in the editor's song menu. As you
type, songs are ranked by how well they match — matching across both the song name and
the artist — so short, approximate queries still find the right track.

## Features

- **Fuzzy search** across song name and artist
- **Filter buttons** in the corner of the dialog, to switch what you're searching and
  how results are ordered:
  - **Priority** (default ordering from GD)
  - **Name**
  - **Artist**
  - **Song ID**
  - plus an **ascending/descending** toggle
- **Song preview** without leaving the dialog, with the play button updating per row
- **Delete songs** straight from a result row (with a confirmation)
- **Jukebox integration**: songs that have a NONG configured show a disc symbol, and
  the NONG's name/artist can be shown instead of the original. This is entirely
  optional — without Jukebox installed nothing changes and the mod works the same.
- **Settings** for every part of the above, including fully disabling the mod

## Credits

- **matcool** — the original mod, in [small-gd-mods](https://github.com/matcool/small-gd-mods)
- **Alphalaneous** — [Improved Song Browser](https://github.com/Alphalaneous/Improved-Song-Browser),
  which the filter modes and sort buttons are based on, and whose icons are used
- **Fleym / Flafy** — [Jukebox](https://github.com/Fleeym/jukebox) and AutoNong, for the
  NONG support

## Porting notes

This is a port of the original Windows-only MinHook-based DLL to a proper Geode mod.
The original hooked `CustomSongLayer::onSongBrowser` at a hardcoded offset; the port
uses Geode's `$modify` hook instead, which is what makes it work across GD versions
and platforms.

The NONG integration deserves a note: Jukebox does not expose a public API or bundled
headers, so rather than reimplementing NONG storage, the port reads the disc icon and
metadata that Jukebox itself has already resolved and repaints them in the search rows.

Contributions and bug reports are welcome — if something looks wrong, it's likely in
this port rather than the original.
