# Agora's default font set

This is the data behind the desktop's typography: open, redistributable faces that fill the roles macOS fills
with its own (non-redistributable) catalogue. `PROVENANCE.md` records every upstream, version, licence and
checksum; `SHA256SUMS` verifies the files.

## How it is found

`libagora-drawing` compiles the installed location into the font registry:

```
<prefix>/share/fonts/agora
```

That directory is searched **first**, then the host's `/System/...`, `/Library/Fonts`, `/usr/share/fonts`,
`/usr/local/share/fonts` and the X11/FreeBSD trees. A name that is not found in the bundle falls through to the
host, so a distribution's fonts still work and a user's own set still wins over nothing.

| Variable | Effect |
|---|---|
| `FN_FONT_PATH` | Colon-separated directory list that **replaces** the whole search path. This is the test/probe door. |
| `AGORA_UI_FONT` | Colon-separated UI family preference list; replaces Core Text's built-in UI list. |
| `AGORA_MONO_FONT` | Colon-separated fixed-pitch family preference list; replaces the built-in monospace list. |

The macOS-name compatibility table lives in `../CGFontAliases.c`. It is consulted **only after** an exact name
search fails, so a real `Helvetica` on the host is used when present, and a fresh Agora install gets
Liberation Sans instead of nothing. `AGORA_UI_FONT`/`AGORA_MONO_FONT` name **real family names** (`Inter`,
`IBM Plex Mono`), not the macOS aliases.

## The faces

| Role | Family |
|---|---|
| UI sans | Inter, Inter Display |
| UI serif | Source Serif 4 |
| UI monospace | IBM Plex Mono |
| Helvetica / Arial | Liberation Sans |
| Times | Liberation Serif |
| Courier | Liberation Mono |
| Georgia | Gelasio |
| Futura | Jost |
| Avenir | Mulish |
| SF Pro Rounded | Nunito |
| Baskerville | Libre Baskerville |
| Garamond / Hoefler | EB Garamond |
| Copperplate | Cinzel |
| Rockwell / Clarendon | Zilla Slab |
| Humanist sans | Fira Sans |
| Impact | Anton |
| Comic Sans / Chalkboard | Comic Neue |
| Handwriting | Patrick Hand, Pacifico, Pinyon Script |
| Typewriter | Special Elite |
| Symbols / math | Noto Sans Symbols 2, Noto Sans Math |
| Emoji | Noto Emoji |

Each family keeps its upstream licence file beside its faces. The three static instances (Jost, Nunito, Noto
Emoji) are recorded as Modified Versions in `PROVENANCE.md`; the families that reserve a name are shipped
unmodified.
