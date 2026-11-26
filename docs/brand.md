# Jungle Leopard Display brand (KruxSYS)

Jungle Leopard Display is a KruxSYS product. This file records where the app's colours, type and
marks come from, so they can be checked against the source and changed together. It follows
[Plexdio's brand file](https://github.com/KruxSYS/plexdio/blob/main/docs/brand.md), the sister
product that set these rules first.

Sources:

| Source | What was taken |
|---|---|
| KruxSYS stylesheet (`https://kruxsys.com/assets/css/site.*.css`), as recorded in Plexdio's `docs/brand.md` | the colour tokens, shape rules and type stacks |
| KruxSYS logo `https://kruxsys.com/assets/brand/logo.ce36e02f37.svg` | [`assets/brand/kruxsys.svg`](../assets/brand/kruxsys.svg), byte for byte (sha256 `ce36e02f37…`, the hash in the site's file name) |
| Plexdio `docs/brand.md` | the derived tokens (`Selected`, `AccentDim`), the error/ok colours, and the icon-edge grey |

## Colour tokens

Every colour the manager draws is a key in
[`JLDisplayManager/Themes/KruxSYS.xaml`](../JLDisplayManager/Themes/KruxSYS.xaml): a `Color`
named `<Key>Color` and a `SolidColorBrush` named `<Key>`. XAML uses `{StaticResource Key}`; code uses
`Theme.Brush("Key")`, `Theme.Color("Key")`, or `Theme.Gdi("Key")` for the Windows Forms tray menu
(`Themes/Theme.cs`). No other file writes a colour down.

### From the KruxSYS stylesheet

| App key | Value | Site token | Used for |
|---|---|---|---|
| `Bg` | `#07090c` | `--bg` | window backgrounds, title bars, text inputs, thumbnail wells |
| `Panel` | `#0c1016` | `--panel` | panels, cards, menus, dialogs, combo popups |
| `Panel2` | `#111620` | `--panel-2` | secondary buttons, chips, library tiles, playlist rows, tooltips |
| `Line` | `#1a212c` | `--line` | panel and card borders; hovered rows; pressed buttons |
| `Line2` | `#2a3442` | `--line-2` | button, input and menu borders; rules; hovered buttons; slider track |
| `Text` | `#e8ecf2` | `--text` | body text |
| `TextMuted` | `#8e9bb0` | `--muted` | labels, hints, section headings |
| `TextSoft` | `#c9d1dd` | literal (`.prose p`) | long-form text: About, dialogs |
| `Accent` | `#22d3ee` | `--signal` | primary buttons, the selected tab, focus rings, checkboxes, sliders, links, selection bars, corner brackets |
| `AccentHover` | `#67e8f9` | literal (`.button.primary:hover`) | primary button and slider thumb hover, link hover |
| `OnAccent` | `#031014` | literal (`.button.primary`) | text and ticks on cyan |

`--amber` (`#ffb547`) is the site's release-status colour. It is **not used in the app**: the
README's status badge is the only place it appears. Jungle Leopard Display's old identity was amber
(`#F0A030`), and a second amber in the UI would blur the rebrand.

### Derived (from Plexdio)

| App key | Value | How |
|---|---|---|
| `Selected` | `#0e242c` | `--signal` at 10 % over `--panel`, opaque. Selected rows also get a 2 px cyan bar on the left, so the state never rests on the faint fill alone. |
| `AccentDim` | `#0f2b34` | `--signal-dim` (14 % cyan) over `--panel`. Defined for parity; never carries `OnAccent`. |
| icon edge | `#3a4658` | the frame of the 16 and 24 px icons only, one step lighter than `--line-2`, so the tile keeps an edge on a dark taskbar |

### Not in the stylesheet

| App key | Value | Source |
|---|---|---|
| `Error` | `#ff8a80` | Krunk dark `--danger` (via Plexdio): status dot when disconnected, error text, the dialog bar for errors and warnings, the tray's AI error line |
| `Ok` | `#6fdc95` | Krunk dark `--ok` (via Plexdio) |
| `CloseHover` | `#c42b1c` | the Windows 11 caption close hover |

### Not brand colours at all

These are chrome, but they depict something rather than style it, so they are deliberately neutral:

| App key | Value | Why |
|---|---|---|
| `Glass` | `#000000` | the panel preview when nothing is lit: the LCD is black |
| `Scrim` | `#B007090C` | badges (VIDEO, AI, PINNED) laid over a thumbnail |
| `EditorBackdrop`, `EditorDark`, `EditorGrey`, `EditorCheckerLight`, `EditorCheckerDark`, `EditorGrid` | neutral greys | the overlay editor's backdrops: an overlay's own colours have to be judged against nothing tinted |
| `HandleFill` | `#FFFFFF` | resize handles in the editor (with a cyan edge) |

### User content is not chrome

The colours an overlay draws onto the panel (`Models/Overlay/OverlayTheme.cs`, layer defaults in
`Models/Overlay/Layers.cs` and `OverlayLayer.cs`, `Services/Overlay/Palette.cs`, the role map in
`OverlayProfile.cs`) belong to people's layouts. The rebrand did not touch them, and the hex grep
below excludes them by name.

```sh
grep -rnE '#[0-9A-Fa-f]{6}' --include=*.xaml --include=*.cs JLDisplayManager \
  | grep -v -e /obj/ -e Themes/KruxSYS.xaml -e Models/Overlay/ -e Services/Overlay/Palette.cs
# expected: nothing
```

## Contrast

WCAG 2 ratios, computed from the values above. Everything that carries text is AA (4.5:1) or better:

| Foreground | Background | Ratio |
|---|---|---|
| `OnAccent` | `Accent` / `AccentHover` | 10.7 / 13.3 |
| `Text` | `Bg` / `Panel` / `Panel2` | 16.8 / 16.1 / 15.3 |
| `Text` | `Line` / `Line2` | 13.7 / 10.6 |
| `TextMuted` | `Bg` / `Panel` / `Panel2` / `Line` | 7.1 / 6.8 / 6.4 / 5.75 |
| `TextSoft` | `Panel` | 12.4 |
| `Accent` | `Panel` / `Panel2` / `Line` | 10.6 / 10.0 / 9.0 |
| `Error` / `Ok` | `Panel` | 8.4 / 11.2 |
| `Text` / `TextMuted` / `Accent` | `Selected` | 13.6 / 5.7 / 8.9 |
| `Text` / `Accent` | `AccentDim` | 12.5 / 8.2 |

Two pairs are below AA, so the app never uses them:

- `TextMuted` on `Line2` is **4.47**. Hovered buttons lift to `Line2` with `Text` on them, and nothing
  muted is ever put on `Line2`.
- `OnAccent` on `AccentDim` is **1.3**. Dim-cyan fills carry `Text` or `Accent` only.

`Line2` on `Panel` is 1.5:1: fine for a decorative border, which never carries information alone.

## Type

The KruxSYS site loads no web fonts, so neither does the app: WPF's default (Segoe UI) for text, and
`Mono` (`Cascadia Mono, Consolas, Courier New`) for small technical labels, following the site's
split. Section headings (`ON THE PANEL`, `PLAYLIST`, `ABOUT`…), the version line and the **SYS** of
the wordmark are monospace.

**Wordmark:** "Krux" bold in `Text`, then "SYS" in `Accent`, monospace.

## Shape

- **Corners are square.** `RadiusControl` (2) on buttons, inputs, combos, checkboxes, tooltips;
  `RadiusPanel` (0) on panels, cards, menus and popups.
- **Borders are 1 px** (`BorderThin`): `Line` on panels and cards, `Line2` on controls.
- **Round only where roundness means something:** slider thumbs and the status dots.
- **Corner brackets** (cyan, 12 px, top-left and bottom-right) on the About card and the installer's
  wizard image, as on the site's `.panel`.
- **Focus** is a 1 px `Accent` ring (`FocusRing`), or an `Accent` border on inputs and checkboxes.
- **Title bars** are drawn in `Bg` with `Text` captions (`Theme.HookWindows`, DWM attributes; on
  Windows 10 the caption is dark but the colours are the system's).

## Marks

| File | What |
|---|---|
| [`assets/brand/kruxsys.svg`](../assets/brand/kruxsys.svg) | KruxSYS logo, the official SVG unchanged |
| [`assets/brand/jungle-leopard.svg`](../assets/brand/jungle-leopard.svg) | product mark, master for 32 px and up |
| [`assets/brand/jungle-leopard-24.svg`](../assets/brand/jungle-leopard-24.svg) | pixel-aligned variant for the 24 px icon frame |
| [`assets/brand/jungle-leopard-small.svg`](../assets/brand/jungle-leopard-small.svg) | pixel-aligned variant for the 16 px icon frame (tray, title bar) |

The product mark uses the KruxSYS logo's grammar: a square `Panel` tile, a `Line2` frame, cyan
brackets top-left and bottom-right, and bold strokes with miter joins in `Text` and `Accent` only.
The motif is the product itself: a **pump-head ring** (the cooler the panel sits on) around a
two-tone **JL**, with a cyan J and a white L, echoing the logo's cyan bar and white chevron. The ring
and both letters do not survive below 32 px, so:

- **24 px** keeps the ring and the letters, with every stroke 2 px on the pixel grid.
- **16 px** drops the ring and draws the JL as large as the tile allows.

`tools/make_icons.py` (Inkscape and Pillow) renders them into:

- `JLDisplayManager/app.ico` and `Jungle Leopard Display/app.ico` (16/24/32/48/64/128/256, one designed
  frame per size, checked after writing),
- `assets/brand/jungle-leopard-{64,128,256}.png` (README, About card),
- `installer/wizard-*.bmp` (the Inno Setup side image and corner mark).

The outputs are committed, so building never needs Python. Edit an SVG, run the script, and commit
both.
