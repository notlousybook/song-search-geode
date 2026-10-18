> This mod was ported to geode using AI. If you don't like using AI ported mods, feel free to not use it.

# Song Search

An in-game search dialog for your downloaded songs, with fuzzy matching and
Jukebox NONG support.

The search dialog replaces the default song browser in the editor's song menu. As you
type, songs are ranked by how well they match — matching across both the song name and
the artist — so short, approximate queries still find the right track.

## Features

- **Fuzzy search** across song name and artist
- **Long titles tamed**: names shrink dynamically to fit their row, so the full
  title stays visible; only absurdly long names fall back to a clean "..." cut
- **Song preview** without leaving the dialog, with the play button updating per row
- **Delete songs** straight from a result row (with a confirmation)
- **Jukebox integration**: each row carries the same pin disc Jukebox draws on its
  own song widgets, pinned on the row's top-left corner; clicking it opens Jukebox's
  nong picker for that song. This is entirely optional — without Jukebox installed
  nothing changes and the mod works the same.
- **Settings** for every part of the above, including fully disabling the mod

## Credits

- **matcool** — the original mod, in [small-gd-mods](https://github.com/matcool/small-gd-mods)
- **Fleym / Flafy** — [Jukebox](https://github.com/Fleeym/jukebox) and AutoNong, for the
  NONG support

## Porting notes

This is a port of the original Windows-only MinHook-based DLL to a proper Geode mod.
The original hooked `CustomSongLayer::onSongBrowser` at a hardcoded offset; the port
uses Geode's `$modify` hook instead, which is what makes it work across GD versions
and platforms.

The NONG integration deserves a note: Jukebox does not expose a query API for other
mods, and (verified against its 3.8.0 source) its pin disc is drawn on *every* song
widget unconditionally — it is the button that opens its nong picker, not a "this song
has a nong" badge. So the port mirrors that look: it bundles a copy of the disc artwork
(MIT), pins it on each row's top-left corner, and — to make it clickable — builds a
throwaway `CustomSongWidget` for the row's song and activates the disc button Jukebox
attaches to it. The picker keeps that widget alive while it is open, so there are no
hooks into Jukebox and no storage guessing.

Contributions and bug reports are welcome — if something looks wrong, it's likely in
this port rather than the original.
