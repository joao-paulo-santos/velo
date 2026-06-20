# Changelog

## [Unreleased]

Initial release of the plugin-extensible Wayland launcher.

### Core
- TOML plugin system with 5 types: `list`, `select`, `input`, `preview`, `exec`
- Stack-based navigation with dictionary flow (`{key}` template substitution)
- Plugin chaining via `next` (forward) and `return` (pass to parent)
- Teleport: type `name:` to jump directly to any plugin
- Fuzzy search with score-based ranking (contiguous matches first)
- Dependency checking: plugins auto-hide if required binaries missing

### Theming
- Palette system: JSON palettes with `dark`/`light` variants (`palette`, `darkmode`); 61 bundled, `--list-palettes` to list
- Color derivation: auto-adjusts colors that would be invisible against the background (`color-derivation`, default on)
- Derived selection color, guaranteed visible on every palette (dark and light)
- Match highlighting: recolour the matched characters in each result (`match-highlight`)
- Filled selection bar: primary fill with onPrimary text (`selection-box`)
- Remappable color roles via `palette_color_mapping.json`
- Noctalia-compatible loader: a stock noctalia palette JSON drops in unmodified

### Rendering
- Autosize mode: dynamic window height, snaps to exact row boundaries
- Background opacity control (`background-opacity`, 0.1-1.0)
- Per-side padding overrides (`padding-top`, `padding-bottom`, etc.)
- VELO_DEBUG env var for clip/border visualization

### Pipe Modes
- `--pick`: dmenu replacement (stdin list, stdout selection)
- `--input`: text entry pipe-out (zenity --entry replacement)
- `--sensitive`: password masking for `--input`

### Bundled Plugins
- **drun** - desktop application launcher
- **calculator** - qalc-based live preview (Enter copies to clipboard)
- **url** - URL opener
- **tmux** - session manager (freeze/unfreeze/cold storage via tmux-fridge)
- **hyprland** - window focus and workspace switching
- **wifi** - connect/reconnect/forget networks
- **themes** - switch the velo palette plus kitty, waybar, and hyprland themes
- **enter-password** - reusable password prompt (chains with `next`)

### CLI
- `-e <plugin>` - teleport directly to plugin at startup
- `-p <list>` - show only specified plugins as root menu
- `-f <filter>` - filter global plugins (`all,-drun`)
- `--palette <name>` - override the color palette
- `--darkmode <bool>` - use the palette's dark (true) or light (false) variant
- `-L` / `--list-palettes` - list available palettes
