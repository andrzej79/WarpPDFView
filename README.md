# WarpPDFView

A true-colour PDF viewer for AmigaOS 3.x on Picasso96 screens, built on
[MuPDF](https://mupdf.com) and compiled with vbcc.

Pages are rendered by MuPDF in a background process, cached in fast RAM and kept in graphics
card memory once shown, so scrolling, flipping back and window moves are card blits rather
than re-renders. The program was written for CS-Lab's Warp 68060 accelerators, but it uses
nothing board-specific: any 68040/68060 system with an FPU and Picasso96 runs it.

## Requirements

- AmigaOS 3.x.
- A 68040 or 68060 **with an FPU** (`WarpPDFView` for 68060, `WarpPDFView_040` for 68040).
  MuPDF is floating point throughout; there is no softfloat build.
- MUI 3.8 or newer.
- A Picasso96 screen of 15 bits or more. The viewer opens on the Workbench screen and refuses
  a planar one.
- Memory: the binary is ~5 MB. Rendered pages are cached within a budget you choose
  (Settings/Caching, 4-64 MB, 16 MB by default).

## Using it

```
WarpPDFView [FILE] [PAGE n] [DPI n] [AA bits]
```

Without a FILE it opens a file requester. From Workbench it takes a project icon's file, or
asks for one.

- **Toolbar:** Open..., first/previous page, a page number box (type a number and press
  Return), next/last page, Zoom (Fit width, Fit page, 36-200 dpi), anti-aliasing (8/6/4 bit or
  off; 4 is the default: on screen it looks as good as 8 and draws faster) and a status field
  showing the dpi and what the page cost to read and draw, or "from the cache".
- **Bookmarks** (left panel): the PDF's outline, indented by level. Click a bookmark, or move
  through the list with the cursor keys, to go to its page and to the spot on the page it
  points at. The panel highlights the bookmark of the page being read, and its width is
  adjustable with the bar beside it. View/Bookmarks (or `b`) shows or hides it; it stays
  hidden for a document without bookmarks. MUI 3.8 has no tree list, so the outline is flat.
- **Find bar** (bottom): type, then Return or `>` for the next hit, `<` for the previous. Hits
  are outlined on the page, the current one in red. The search runs from the current page to
  the end and wraps round. An empty box clears the marks.
- **Menus:**
  - Project: Open, About, Quit.
  - View: Single page / Continuous, Fit width / Fit page, Bookmarks.
  - Search: Find (Amiga-F), next and previous hit (Amiga-G / H).
  - Settings: Caching, 4 / 8 / 16 / 32 / 64 MB.

  The view mode, the cache size and whether bookmarks show are kept in
  `ENVARC:WarpPDFView.mode`, `.cache` and `.bookmarks`.
- **Scrolling:** the scrollers, dragging the page, the cursor keys (Shift for a screenful),
  keypad 9/3 and the mouse wheel.
- **Keys:** Space/Backspace next/previous page (a screenful in continuous mode), n/p
  next/previous page, keypad 7/1 first/last, + and - zoom, w fit width, f fit page, g type a
  page number, b bookmarks, o open, q quit. Esc leaves a text box, or quits outside one. While
  a text box has the cursor, keys that could mean something in it go to the box.

**View modes.**
- **Single page** shows one page. Scrolling past its end with the wheel or the keys turns the
  page.
- **Continuous** stacks all pages. A page not drawn yet shows at once as a placeholder with its
  number. The pages in view draw first, from the middle out. A page scrolled past before it is
  finished is abandoned half way, so fast scrolling never queues work nobody wants.

**Not supported yet:** password-protected documents, links, text selection and copy, JPEG 2000
images, CJK fonts that are not embedded, printing.

## Building

The build runs on a host (developed on macOS; any Unix-like system with the tools below
should do) and cross-compiles for AmigaOS:

- [vbcc](http://sun.hasenbraten.de/vbcc/) 0.9h with the `m68k-amigaos` target, `vc` on the
  `PATH` and `VBCC` set, as the vbcc distribution describes.
- The AmigaOS **NDK 3.2** C includes.
- The **Picasso96** developer includes (`libraries/Picasso96.h`); vbcc's own target supplies
  the call stubs.
- The **MUI 3.8** developer includes (`libraries/mui.h`, `proto/muimaster.h`).
- `curl`, `patch` and `shasum` for `fetch_mupdf.sh`; Python 3 for `make ftp`.

The include paths are Makefile variables, overridable on the command line:

| variable | default |
|---|---|
| `SDK_DIR` | `/opt/amiga_sdk` |
| `NDK_INC_C` | `$(SDK_DIR)/NDK3.2R4/Include_H` |
| `P96_SDK` | `../p96_gfx_driver/Picasso96Develop/Include` |
| `MUI_SDK` | `../WarpDiag/mui38dev/MUI/Developer/C/Include` |

```
./fetch_mupdf.sh      # once: downloads MuPDF 1.28.5 into thirdparty/ and applies patches/
make -j8 app P96_SDK=/path/to/P96/Include MUI_SDK=/path/to/MUI/Developer/C/Include
make -j8 app CPU=68040 ...            # build/WarpPDFView_040
make clean            # the app only; the MuPDF objects stay
make distclean        # everything under build/
```

`make` (`all`) also copies the binary to `TARGET_DIR` when that directory exists, e.g. an
emulator's shared folder. `make` does not clean first: MuPDF is ~330 files that do not change
between app builds. A full build takes well under a minute with `-j`.

`make ftp` uploads the binary to a running Amiga's FTP server (`FTP_DIR`, default
`SYS:WarpApps`). The login comes from the `AMIGA_FTP_HOST`, `AMIGA_FTP_USER` and
`AMIGA_FTP_PASSWORD` environment variables, or from `tools/ftp_config.local`: copy
`tools/ftp_config.local.example` and fill it in. That file is git-ignored; never commit it.
`tools/amiget.py` goes the other way, fetching files from the Amiga.

`mupdf.mk` holds the MuPDF part of the build (paths, feature set, sources, rules) on its own,
so another program can include it and link against the same objects.

## How it works

- `src/viewer/viewer.c`: the MUI application, the layout, menus and keys, and deciding which
  pages are wanted.
- `src/viewer/worker.c`: the render worker, a DOS process one priority below the UI and the
  only code that touches MuPDF. It owns the job queue and the page cache.
- `src/viewer/pageview.c`: `PageView`, a custom MUI class that shows the pages.
- `src/common/wpdfmem.c`: the heap MuPDF allocates from.
- `compat/`: the POSIX headers MuPDF expects and vbcc's libc lacks, implemented on
  dos.library (see `compat/README.md`).

**Rendering.** The window keeps answering while a page draws, and idle time goes into the pages
ahead:
- **Ahead:** the next two pages and the previous one render after the pages in view.
- **Cache:** rendered pages stay in fast RAM within the Caching budget, least recently used
  first, so going back is instant. The last eight display lists are kept too, so a zoom change
  skips reading the page again.
- **Interruption** is MuPDF's `fz_cookie`, checked while a page is read and while it is drawn.
- **Background jobs:** page sizes for the continuous layout, and search, step aside page by
  page for anything more urgent.

**Display.** `PageView` uploads a rendered page once into a friend bitmap of the window's, in
the screen's own pixel format. After that every scroll, expose or window move is a blit inside
card memory, and pages out of view give their card memory back. If the card has no room it
falls back to uploading the visible part from fast RAM.

### The MuPDF port

- **Feature set.** PDF only: no JavaScript, no HTML engine (HarfBuzz is C++), no ICC colour
  management (lcms2), no JPEG 2000 for now. The CJK and Noto fallback fonts are left out; the
  base-14 fonts stay. See `MUPDF_DEFS` in `mupdf.mk`.
- **`patches/mupdf-1.28.5-vbcc.patch`** covers what vbcc rejects or gets wrong:
  - `offsetof()` with a nested or indexed member (vbcc accepts a plain name only),
  - `INFINITY`/`NAN` in constant initialisers,
  - an initialised flexible array member,
  - a pointer compared with `> 0`,
  - `realpath`/`mkstemp`,
  - **`SEEK_SET/CUR/END`**: vbcc's libc numbers these -1/0/1, while MuPDF uses the POSIX
    0/1/2 both through the macros and as bare literals. Inside MuPDF they are made POSIX, and
    the value is converted at the two places a stdio `FILE` is actually seeked.

  It also makes `fz_atof()` parse a plain decimal (up to 7 significant digits and 10 after the
  point) with a single float division. That is exact - checked against `strtof()` on all 110
  million such strings - and much cheaper on a 68060 than the 64-bit integer arithmetic of
  `fz_strtof()`, on content streams made mostly of numbers.
- **-O1, not -O2.** vbcc 0.9h at -O2 (and -O3) miscompiles `pdf_read_start_xref()`: every
  file's startxref parses as 0, and every open silently falls back to a full repair scan.
  `make OPT=2` builds `WarpPDFView_O2` with that one file still at -O1; it renders identically
  but was measured 5-8% *slower* on a 68060, so -O1 stays.
- **`NDEBUG`**, as in MuPDF's release build. Without it every lock call is debug-checked.

Renders were verified under emulation against a host-built `mutool draw` from the unpatched
1.28.5 sources: bit-identical pages, or differences limited to half-pixel anti-aliasing shifts
on hairlines and one-level image rounding, which follow from the 68k FPU computing in 80-bit
extended precision.

### Testing the worker on the host

```
test/host/build.sh <big.pdf> <other.pdf> [needle [heavy page]]
```

builds MuPDF for the host from the same patched tree, then runs `src/viewer/worker.c`
unchanged over a small Exec emulation on POSIX threads (`test/host/shim/`). It checks that
pages render identically to a direct MuPDF render, the cache (hits, the budget, pinned pages
surviving eviction), aborts, search, the outline, background page sizes, reopening a document
mid-render, and shutdown. On macOS it calls the Command Line Tools' clang directly.

## License

WarpPDFView is free software: you can redistribute it and/or modify it under the terms of the
**GNU Affero General Public License, version 3**, as published by the Free Software Foundation.
It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See `LICENSE`.

Copyright (c) CS-Lab s.c.

WarpPDFView links MuPDF, Copyright (c) Artifex Software, Inc., licensed under the AGPL v3,
which is why the whole program is AGPL. The complete corresponding source is this repository
plus the MuPDF 1.28.5 source release (`fetch_mupdf.sh` downloads it from mupdf.com and checks
its SHA-256) with the changes in `patches/`. The MuPDF release also bundles the third-party
code the binary contains, each part with its license in its own directory there:

- FreeType (FreeType License),
- IJG libjpeg (IJG license),
- zlib (zlib license),
- jbig2dec (AGPL v3, Artifex),
- the URW base-35 fonts (SIL Open Font License 1.1).
