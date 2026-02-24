# The Emu Pages

<img src="assets/logo.jpg" alt="The Emu Pages" width="320">

A RetroArch libretro core that displays the [EmuVR wiki](https://www.emuvr.net/wiki/) as an in-game manual, styled like a Commodore 64. All wiki content is fetched at build time and baked into the binary. No internet needed at runtime.

Built for RetroArch 1.7.5 / EmuVR on Windows.

## How it works

A Python script pulls every page from the EmuVR wiki via the MediaWiki API, converts the wikitext to plain text, word-wraps it, and outputs a C header with the content as static arrays. A C compiler turns that into a `.dll` that RetroArch loads like any other core.

The core itself is a three-screen state machine:

1. **Boot** - A fake C64 `LOAD "EMUVR",8,1` sequence with a loading bar. Takes about 10 seconds, skip with any button.
2. **Table of contents** - Scrollable list of all wiki pages.
3. **Page viewer** - Read the selected page, scroll through it, flip between pages.

## Controls

| Input | Action |
|-------|--------|
| D-pad up/down | Move cursor / scroll |
| A | Open page |
| B / Start | Back to contents |
| D-pad left/right | Previous / next page |
| L / R shoulder | Page up / page down |

## Installing

Grab the latest release and copy the files:

- `emu_pages_libretro.dll` goes in `RetroArch/cores/`
- `emu_pages_libretro.info` goes in `RetroArch/info/`
- `emuvr.emupages` goes in your games folder

Load `emuvr.emupages` in RetroArch with The Emu Pages core.

### EmuVR setup

To use this with any media object in EmuVR, add a line to your `custom_media.txt`:

```
Media = "emu_pages_libretro"
```

Replace `Media` with whatever you want to use. For example:

```
Commodore 64 = "emu_pages_libretro"
```

This works with any media object. Just follow the pattern and place the ROM file where that media expects its games.

## Building from source

Builds run automatically via GitHub Actions on push to main. The workflow fetches the wiki, converts the mascot sprite, compiles a Windows DLL, and creates a release.

To build locally you need Python 3, Pillow, a C compiler, Make, and curl.

```
make deps       # fetch libretro.h (once)
make wiki-data  # fetch wiki content from emuvr.net
make sprite     # convert mascot image to C header
make            # compile
```

## Project structure

```
src/
  emu_pages.c    - libretro API, state machine, input handling
  renderer.c     - framebuffer rendering, C64 theme, boot sequence
  renderer.h     - screen geometry and color constants
  font8x8_basic.h - 8x8 bitmap font (vendored, public domain)
tools/
  fetch_wiki.py    - wiki fetcher and wikitext-to-plaintext converter
  convert_sprite.py - mascot image to C header
  emuvr_mascot.webp - mascot source image
```

`src/libretro.h`, `src/wiki_data.h`, and `src/mascot_data.h` are fetched or generated at build time and not checked in.

## License

MIT
