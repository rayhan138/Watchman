# Watchman's embedded fonts

The font files referenced by `watchman_resources.rc` are compiled into
**Watchman.exe**. Normal builds and users do not need Python, FontTools, font
installation, a fonts folder next to the executable, or a writable font cache.

| Use | Family | Embedded weights |
| --- | --- | --- |
| Main UI | Inter | 400, 500, 600, 700, 800 |
| Large values | Nunito Sans | 800 (static ExtraBold) |
| Monospace/taskbar | JetBrains Mono | 400, 500, 600, 700 |

`bundled_fonts.cpp` provides a resource-backed DirectWrite file loader and an
explicit font collection for both the main renderer and taskbar compositor.
Requested families and weights are checked before creating a format. The GDI
layered fallback uses `AddFontMemResourceEx` on the same embedded bytes. Neither
path relies on global registration or `%TEMP%\Watchman\fonts`.

The floating monitor intentionally uses Windows' built-in Segoe UI Bold, not a
user-selected family. Its preview has its own system-font format rather than
asking the private Inter collection for Segoe UI.

## Nunito Sans static ExtraBold

`NunitoSans-Variable.ttf` remains the original source. Its default weight is 200
and its legacy family name is `Nunito Sans 12pt ExtraLight`, which is ambiguous
with older weight/stretch/style DirectWrite collections. Watchman only needs
weight 800 for this family, so `NunitoSans-ExtraBold.ttf` is a checked-in static
instance with these axes:

- `wght=800`
- `wdth=100`
- `opsz=12`
- `YTLC=500`

Its typographic/WWS family metadata is explicitly `Nunito Sans` / `ExtraBold`,
weight 800. The legacy four-style name is `Nunito Sans ExtraBold` / `Regular`,
matching Inter's static ExtraBold naming model. Variable-axis `STAT` naming and
the variable PostScript prefix are removed, so DirectWrite cannot derive an
unintended family from the source's non-elidable YTLC axis.
Only this static instance is embedded; the variable source is not shipped in the
EXE. This also avoids adding a dependency on newer variable-font APIs.

Maintainer-only regeneration, from the repository root:

```sh
python -m venv .venv
# Activate that environment, then:
python -m pip install fonttools==4.68.0
python fonts/build_nunito_static.py
```

The source timestamps are retained for reproducible output. The normal CMake
build consumes the existing TTF and does not execute this script.

## Licenses and verification

The font software remains under its included licenses:

- `Inter-License.txt`
- `NunitoSans-OFL.txt` (also covers the derived static instance)
- `JetBrainsMono-OFL.txt`

The source's copyright and license metadata are preserved in the generated
Nunito font. No reserved font name is declared after that source's copyright.

`BundledFontsTest` embeds the production resources, shapes text with the
production loader, and checks actual glyph-run resource IDs, font bytes and
weights (including unsimulated Nunito 800). It tests simultaneous main/taskbar
collections, cleanup/recreation, invalid streams and GDI's exact embedded
JetBrains Mono Bold bytes. Installed copies cannot satisfy these assertions.
`SettingsTextLayoutTest` and `TaskbarWidgetLayoutTest` use the same production
collection for their native DirectWrite measurements.
