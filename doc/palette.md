# velo Palettes

Colors in velo are defined by a **palette**: a small JSON file with a dark
and a light variant. Layout (font, sizes, padding, border width, corner
radius, background opacity) lives in the main config; a palette only
provides color.

## Selecting a palette

In `~/.config/velo/config`:

```toml
palette = "breeze"     # name = filename in ~/.config/velo/palettes/, minus .json
darkmode = true        # true (default) uses the dark variant; false uses light
```

CLI overrides: `velo --palette tokyo-night --darkmode=false`.
List installed palettes: `velo --list-palettes` (`-L`).

## File format

A palette is JSON with two objects, `dark` and `light` (`light` is optional
and falls back to `dark`). Each contains six `#rrggbb` colors. No alpha —
transparency is set via `background-opacity` in the main config.

```json
{
  "dark": {
    "surface":    "#202224",
    "onSurface":  "#fcfcfc",
    "primary":    "#3daee9",
    "onPrimary":  "#141618",
    "secondary":  "#1d99f3",
    "outline":    "#707d8a"
  },
  "light": {
    "surface":    "#ffffff",
    "onSurface":  "#232629",
    "primary":    "#3daee9",
    "onPrimary":  "#ffffff",
    "secondary":  "#2980b9",
    "outline":    "#e3e5e7"
  }
}
```

## Color roles

| Role | Used for in velo | Status |
|---|---|---|
| `surface` | Window background | active |
| `onSurface` | Body text: the input line and unselected results | active |
| `primary` | Hue/saturation source for the selected-result color (see below) | active |
| `secondary` | The prompt symbol and the input/results divider | active |
| `outline` | Window border | active |
| `onPrimary` | *(reserved for the optional filled-selection style)* | reserved |

### How the selected entry is colored

`primary` is a Material 3 *fill* tint, so as raw text it often collides with
the body text (e.g. on monochrome palettes like cherry-blossom). velo instead
**derives** the selection color at load: it keeps `primary`'s hue and
saturation and shifts its lightness to the value that contrasts both
`onSurface` (body text) and `surface` (background). This guarantees a visible
selection on every palette, in both dark and light mode.

A filled selection bar (`primary` fill + `onPrimary` text) is planned as an
optional style toggle; that is what `onPrimary` is reserved for.

## Defaults & fallback

With no config, velo uses `palette = "breeze"`, `darkmode = true`. If the
palette file cannot be read, a hardcoded breeze-dark fallback ensures velo
always renders.

## Creating a palette

Drop a `<name>.json` into `~/.config/velo/palettes/`. Only `surface`,
`onSurface`, `primary`, and `outline` are required for a usable look;
`secondary` and `onPrimary` may be omitted (they fall back to the defaults
above).

## Noctalia compatibility (import)

velo's loader is lenient: it also accepts the `m`-prefixed key form
(`mPrimary`, `mSurface`, ...) and ignores unknown keys including the
`terminal` block. A stock [noctalia](https://noctalia.dev) palette JSON can
therefore be dropped into `palettes/` unmodified and will work. (velo
palettes are authored in the minimal unprefixed form above.)
