# Changelog

## [Unreleased]

Initial release of the plugin-extensible Wayland launcher.

### Core
- TOML plugin system with 5 types: `list`, `select`, `input`, `feedback`, `exec`
- Stack-based navigation with dictionary flow (`{key}` template substitution)
- Plugin chaining via `next` (forward) and `return` (pass to parent)
- Teleport: type `name:` to jump directly to any plugin
- Fuzzy search with score-based ranking (contiguous matches first)
- Dependency checking: plugins auto-hide if required binaries missing

### Rendering
- Autosize mode: dynamic window height, snaps to exact row boundaries
- Theming system: `theme = "name"` loads visual presets, `--list-themes` to list
- Background opacity control (`background-opacity`, 0.1-1.0)
- Per-side padding overrides (`padding-top`, `padding-bottom`, etc.)
- HYPR_TOFI_DEBUG env var for clip/border visualization

### Pipe Modes
- `--pick`: dmenu replacement (stdin list, stdout selection)
- `--input`: text entry pipe-out (zenity --entry replacement)
- `--sensitive`: password masking for `--input`

### Bundled Plugins
- **drun** — desktop application launcher
- **calculator** — qalc-based with persistent history
- **opencode** — AI chat via opencode
- **url** — URL opener
- **tmux** — session manager (freeze/unfreeze/cold storage via tmux-fridge)
- **hyprland** — window focus and workspace switching
- **wifi** — connect/reconnect/forget networks
- **theme** — switch themes for hypr-tofi, kitty, waybar, hyprland
- **enter-password** — reusable password prompt (chains with `next`)

### CLI
- `-e <plugin>` — teleport directly to plugin at startup
- `-p <list>` — show only specified plugins as root menu
- `-f <filter>` — filter global plugins (`all,-drun`)
- `-t <name>` / `--theme <name>` — override theme
- `-L` / `--list-themes` — list available themes
