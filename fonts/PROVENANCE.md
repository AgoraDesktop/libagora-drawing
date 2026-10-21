# Provenance — the bundled default font set

**Agora ships a default font set because a desktop that inherits whatever faces the host happens to have does not
look like one desktop.** This directory is that set: the Latin UI, text, display and symbol faces the stack draws
with when the host has nothing better. It is **data, not code** — `libagora-drawing` compiles none of it — and it
installs as ordinary files under `<prefix>/share/fonts/agora`, which is the first directory `CGFont`'s registry
searches (`CGFont.c`, `AGORA_FONT_DIR`). `CGText`/`CoreText` still finds host fonts after it, so a machine with a
richer or preferred set is not overridden, only preceded.

**NO APPLE FONT IS INCLUDED, AND NONE CAN BE.** macOS's own fonts are licensed for use on Apple systems and are not
redistributable. What is here is a set of **openly licensed substitutes chosen for the same roles**: the system UI
face, the system serif, the system monospace, the document staples, the display and handwriting catalogue, and the
symbols/emoji fallbacks. The mapping from a macOS name to the substitute lives in
`../CGFontAliases.c`; the face keeps its own name and licence, and the alias is only consulted when the requested
name is not installed.

## The set, by macOS role

| macOS role / name | Bundled family | Licence | Files |
|---|---|---|---|
| SF Pro Text, SF Pro Display, `.SF NS`, system UI | **Figtree** (the default UI face) | OFL 1.1 | `Figtree/` |
| the UI fallback, and callers that name it directly | **Inter**, **Inter Display** | OFL 1.1 | `Inter/`, `InterDisplay/` |
| New York, SF Serif | **Source Serif 4** | OFL 1.1 | `SourceSerif4/` |
| SF Mono, Menlo, Monaco | **IBM Plex Mono** | OFL 1.1 (RFN "Plex", unmodified) | `IBMPlexMono/` |
| Helvetica, Helvetica Neue, Arial, Verdana, Tahoma | **Liberation Sans** | OFL 1.1 (RFN "Liberation", unmodified) | `LiberationSans/` |
| Times, Times New Roman | **Liberation Serif** | OFL 1.1 (RFN "Liberation", unmodified) | `LiberationSerif/` |
| Courier, Courier New | **Liberation Mono** | OFL 1.1 (RFN "Liberation", unmodified) | `LiberationMono/` |
| Georgia | **Gelasio** | OFL 1.1 | `Gelasio/` |
| Futura | **Jost** | OFL 1.1 | `Jost/` |
| Avenir, Avenir Next | **Mulish** | OFL 1.1 | `Mulish/` |
| SF Pro Rounded, Arial Rounded | **Nunito** | OFL 1.1 | `Nunito/` |
| Baskerville, Big Caslon | **Libre Baskerville** | OFL 1.1 (RFN, unmodified) | `LibreBaskerville/` |
| Hoefler Text | **EB Garamond** | OFL 1.1 | `EBGaramond/` |
| Charter, Iowan Old Style, Palatino | **Source Serif 4** (alias) | OFL 1.1 | `SourceSerif4/` |
| Copperplate, Herculanum | **Cinzel** | OFL 1.1 | `Cinzel/` |
| Rockwell, Superclarendon | **Zilla Slab** | OFL 1.1 | `ZillaSlab/` |
| Gill Sans, Trebuchet MS, Optima | **Fira Sans** | OFL 1.1 | `FiraSans/` |
| Impact | **Anton** | OFL 1.1 | `Anton/` |
| Comic Sans, Chalkboard | **Comic Neue** | OFL 1.1 | `ComicNeue/` |
| Marker Felt, Bradley Hand | **Patrick Hand** | OFL 1.1 | `PatrickHand/` |
| Brush Script, SignPainter | **Pacifico** | OFL 1.1 | `Pacifico/` |
| Zapfino, Snell Roundhand, Apple Chancery | **Pinyon Script** | OFL 1.1 | `PinyonScript/` |
| American Typewriter | **Special Elite** | Apache 2.0 | `SpecialElite/` |
| Apple Symbols, Symbol, Zapf Dingbats, LastResort | **Noto Sans Symbols 2** | OFL 1.1 | `NotoSansSymbols2/` |
| STIXGeneral / math | **Noto Sans Math** | OFL 1.1 | `NotoSansMath/` |
| Apple Color Emoji | **Noto Emoji** (monochrome) | OFL 1.1 | `NotoEmoji/` |

Every family directory carries the licence text the upstream font ships. `SHA256SUMS` covers every `.ttf` in this
tree; it is regenerated whenever a face changes.

## Where each face came from

| Family | Upstream and version | How it was taken |
|---|---|---|
| Figtree | `google/fonts` commit **47d54ca1ebb14fd864695f1d39dc734f39017c33**, `ofl/figtree/` | **static instances** of `Figtree[wght].ttf` and `Figtree-Italic[wght].ttf` at weights 400/500/600/700 and 400/700; the family has no Reserved Font Name |
| Inter, Inter Display | `rsms/inter` release **v4.1**, archive `Inter-4.1.zip`, SHA-256 `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e` | the `extras/ttf/` static builds, unmodified; `LICENSE.txt` from the archive |
| Source Serif 4 | `adobe-fonts/source-serif` release **4.005R**, archive `source-serif-4.005_Desktop.zip`, SHA-256 `549fdb8f9a682bd06944298621404969f6de77c2e422ff3b8244a1dcd6a0c425` | `TTF/SourceSerif4-{Regular,It,Semibold,Bold,BoldIt}.ttf`, unmodified; `LICENSE.md` from the archive |
| IBM Plex Mono | `google/fonts` commit **47d54ca1ebb14fd864695f1d39dc734f39017c33**, `ofl/ibmplexmono/` | static TTFs at that commit, unmodified |
| Liberation Sans / Serif / Mono | `liberationfonts/liberation-fonts` release **2.1.5**, archive `liberation-fonts-ttf-2.1.5.tar.gz`, SHA-256 `7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0` | the archive's `ttf/` files, unmodified; its `LICENSE` |
| Gelasio | `SorkinType/Gelasio` commit **7ab20e7e5c42791e603b9ee3201a0b49849cfdb2** | `fonts/ttf/Gelasio-{Regular,Italic,Bold,BoldItalic}.ttf`, unmodified |
| Jost | `google/fonts` commit **47d54ca1ebb14fd864695f1d39dc734f39017c33**, `ofl/jost/` | **static instances** of `Jost[wght].ttf` and `Jost-Italic[wght].ttf` at weights 400/500/600/700 and 400/700; the family has no Reserved Font Name |
| Mulish | `googlefonts/mulish` commit **5503f7a18ce79870148b9f9cb8e592ac44c044c3** | `fonts/ttf/Mulish-{Regular,Italic,SemiBold,Bold,BoldItalic}.ttf`, unmodified |
| Nunito | `googlefonts/nunito` commit **8c6a9bb9732545b9ed53f29ec5e1ab0ff53c4e6f** | **static instances** of `Nunito[wght].ttf` and `Nunito-Italic[wght].ttf` at 400 and 700; the family has no Reserved Font Name |
| Libre Baskerville | `impallari/Libre-Baskerville` commit **9852edf75ece3af500a5ec61245f94788c3d4633** | `fonts/ttf/LibreBaskerville-*.ttf`, unmodified. The family reserves its name, so it is NOT instanced here |
| EB Garamond | `octaviopardo/EBGaramond12` commit **106a4a6d377987459ae5e68673a4570f13b957fb** | `fonts/ttf/EBGaramond-{Regular,Italic,SemiBold,Bold,BoldItalic}.ttf`, unmodified |
| Cinzel | `NDISCOVER/Cinzel` commit **dd598495b0fb2ad84270d5cc75d642d2f1e8eabf** | `fonts/ttf/Cinzel-{Regular,Bold,Black}.ttf`, unmodified |
| Zilla Slab, Fira Sans, Comic Neue, Pinyon Script, Pacifico, Patrick Hand, Anton, Noto Sans Symbols 2, Noto Sans Math | `google/fonts` commit **47d54ca1ebb14fd864695f1d39dc734f39017c33**, `ofl/<family>/` | static TTFs at that commit, unmodified |
| Special Elite | `google/fonts` commit **47d54ca1eb…**, `apache/specialelite/` | the static TTF and the Apache 2.0 licence; `NOTICE.txt` records the copyright line from the face |
| Noto Emoji | `google/fonts` commit **47d54ca1eb…**, `ofl/notoemoji/` | **static instance** of `NotoEmoji[wght].ttf` at weight 400; the family has no Reserved Font Name |

**The four instanced families are the only modified faces.** A static instance of a variable font is a Modified
Version under the SIL Open Font License; each of those four upstream copyright lines declares **no Reserved Font
Name**, so the modified file may keep the family name, and the licence and this note travel with it. The instance
was made with fontTools `varLib.instancer --update-name-table`; the exact axis values are in the table above. The
version-1 `Jost.zip` release was inspected and **not used**, because its family name is spelled `Jost*` and its
static files are older than the variable font; the variable font is the upstream this set actually ships.

## What is deliberately not vendored

- **CJK** (Hiragino, PingFang, Songti, Apple SD Gothic Neo): Noto Sans CJK is tens of megabytes per region and the
  release is not the place to add it without a size decision. The registry still searches the host's
  `/usr/share/fonts`, so a distribution's Noto CJK is found; a future `fonts/` language-pack split is the place for
  a vendored copy.
- **The Indic, Arabic, Hebrew, Thai and other Noto script faces.** Same reason: a defaults package should not
  silently double the download for scripts a given install may not need. They are ordinary host fonts and resolve
  through the same registry.
- **Apple Color Emoji.** The engine draws monochrome outlines today; Noto Emoji is the honest default until a
  colour-glyph path exists. A colour font without a renderer is a large file that draws nothing.
- **TeX Gyre Pagella and other GUST-licensed Palatino clones.** Palatino is mapped to Source Serif 4 rather than
  adding a second serif licence family for one slot.
- **Playfair Display / Didot.** The family reserves its name, and the only static builds available here are not the
  upstream release; Didot maps to Source Serif 4 instead of shipping a face this tree cannot style.

## Updating the set

1. Take the new upstream release/commit; do not edit a face in place.
2. Replace the family's `.ttf` files and its licence file together; if the family is instanced (Figtree, Jost,
   Nunito and Noto Emoji today), run
   `fonttools varLib.instancer --update-name-table -o <out>.ttf <variable>.ttf wght=<value>` and record the new
   upstream commit and values here.
3. `cd fonts && find . -name '*.ttf' -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS`
4. Keep the licence and copyright text with the face. OFL requires the copyright notice and licence to travel with
   the font; Apache 2.0 requires the licence and any NOTICE.
5. Rebuild and run `ctest --test-dir libagora-drawing/build -R fonts` — the suite checks that every family the alias
   table names resolves, and that a missing name still answers NULL.
