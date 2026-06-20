# velo Plugins

Everything velo does is a **plugin**: a single TOML file that defines what the user sees and what happens when they act. No code, no scripts, no compilation. Drop a `.toml` into `~/.config/velo/plugins/` (any subdirectory) and it's loaded.

## Overview

A plugin has a **type** that determines its behavior:

| Type | Behavior |
|------|----------|
| `list` | Shows child plugins as a menu |
| `select` | Shows dynamic items from a command; user picks one |
| `input` | Shows a text prompt; user types a value |
| `preview` | Live one-line result as you type; Enter copies to the clipboard |
| `exec` | Immediately executes a command and closes |

Plugins are discovered recursively from `~/.config/velo/plugins/`. Any `*.toml` in any subdirectory is loaded. Plugins reference each other by `name`, not by file path, so directory structure is purely for organization.

## Plugin Types

### `list` — Menu of child plugins

Shows a static menu of child plugins. Used to group related actions.

```toml
name = "tmux"
display_label = "Tmux"
context_name = "Tmux"
type = "list"
global = true
teleport = true
depends = ["tmux"]
children = ["tmux-attach", "tmux-close", "tmux-new", "tmux-freeze", "tmux-unfreeze"]
```

Each child appears as a selectable item; selecting one navigates to it. Children with unmet dependencies are hidden.

**Fields:** `name`, `display_label`, `context_name`, `children`, `global`, `teleport`, `depends`

### `select` — Dynamic list from a command

Runs `list_cmd` to populate a list; the user picks an item. The selected value is stored in the dictionary under the `as` key, then `template` executes.

```toml
name = "tmux-attach"
display_label = "Attach"
type = "select"
teleport = true
list_cmd = "tmux list-sessions -F '#{session_name}'"
format = "lines"
as = "session"
template = "nohup $TERMINAL tmux attach-session -t {session} >/dev/null 2>&1 &"
execution_type = "exec"
```

**Lines format** (`format = "lines"`) — each line of output becomes an item; both label and value are the line text.

**JSON format** (`format = "json"`) — output must be a JSON array of objects; use `label_field` and `value_field` to pick the keys:

```toml
list_cmd = "hyprctl clients -j | jq -c '.[] | {name: .class, address: .address}'"
format = "json"
label_field = "name"
value_field = "address"
```

**Builtin `@apps`** — use `list_cmd = "@apps"` to list installed desktop applications:

```toml
list_cmd = "@apps"
format = "json"
label_field = "name"
value_field = "id"
template = "@launch {selection}"
```

**Fields:** `name`, `display_label`, `context_name`, `list_cmd`, `format`, `label_field`, `value_field`, `template`, `as`, `next`, `return`, `teleport`, `depends`, `live_apply_palette`

### `input` — Text prompt

Shows a prompt; the user types text. The input is stored in the dictionary under the `as` key, then `template` executes.

```toml
name = "url"
display_label = "Open URL"
context_name = "Open URL"
type = "input"
global = true
teleport = true
prompt = "URL: "
as = "url"
template = "bash -c 'u=\"{url}\"; [[ \"$u\" == http* ]] || u=\"https://$u\"; xdg-open \"$u\"'"
execution_type = "exec"
```

**Password mode** — set `sensitive = true` to mask input:

```toml
name = "enter-password"
type = "input"
prompt = "Password: "
as = "password"
sensitive = true
return = true
```

**Fields:** `name`, `display_label`, `context_name`, `prompt`, `template`, `as`, `sensitive`, `next`, `return`, `teleport`, `depends`

### `preview` — Live result preview

Shows a live, single-line result of evaluating the input as you type (debounced). Enter copies the result to the clipboard and dismisses the launcher; Escape cancels. Intended for fast, local commands (calculators, encoders, lookups). Slow or networked commands will stall the UI, since the command runs synchronously on the main thread.

```
┌─────────────────────────────┐
│ Calculator:                 │
│ > 100 * 1.08                │   input (top, with prompt)
│ 108                         │   live preview below the input
└─────────────────────────────┘
```

```toml
name = "calculator"
display_label = "Calculator"
context_name = "Calculator"
type = "preview"
global = true
teleport = true
eval_cmd = "qalc '{input}'"
# copy_cmd = "wl-copy"   # default
```

**How it works:**
1. User types; after a short debounce, `eval_cmd` is resolved with `{input}` substituted and run synchronously.
2. The first line of stdout is shown as a single preview row beneath the input.
3. Enter copies the preview to the clipboard via `copy_cmd` (default `wl-copy`, fed through stdin so no shell escaping is needed) and closes.
4. Escape cancels without copying.

There is deliberately no history and no async subprocess.

**Fields:** `name`, `display_label`, `context_name`, `eval_cmd`, `copy_cmd`, `teleport`, `depends`

### `exec` — Immediate execution

Immediately executes `template` and closes. No navigation level is created.

```toml
name = "lock-screen"
display_label = "Lock Screen"
type = "exec"
global = true
template = "hyprlock"
execution_type = "exec"
```

**Fields:** `name`, `display_label`, `context_name`, `template`, `teleport`, `global`, `depends`

## Field Reference

### Common Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | required | Unique identifier. Used for teleport (`name:`), child references, and `next` chaining |
| `display_label` | string | falls back to `name` | Label shown in parent menus |
| `display_prefix` | string | `""` | Prepended on the root menu: `"Prefix > Label"` |
| `context_name` | string | `""` | Override prompt when navigated to (shows `"Name: "`) |
| `type` | string | required | One of: `list`, `select`, `input`, `preview`, `exec` |
| `global` | bool | `false` | Show on the root menu |
| `teleport` | bool | `false` | Allow teleporting via the `name:` prefix |
| `depends` | string[] | `[]` | Binary names that must exist on `$PATH`; the plugin is hidden if any are missing |
| `template` | string | `""` | Command template with `{key}` placeholders |
| `as` | string | `""` | Dictionary key under which the selected/typed value is stored |
| `execution_type` | string | `"exec"` | Always `"exec"` (the only value) |
| `next` | string | `""` | Navigate to this plugin after completion; the dictionary accumulates |
| `return` | bool | `false` | Pop the nav level and pass the dictionary to the parent for execution |

### Type-Specific Fields

**`select`:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `list_cmd` | string | `""` | Shell command (or `@apps`) that populates the list |
| `format` | string | `"lines"` | `"lines"` or `"json"` |
| `label_field` | string | `""` | JSON key for the display text (JSON format only) |
| `value_field` | string | `""` | JSON key for the value (JSON format only; falls back to the label) |
| `live_apply_palette` | bool | `false` | Treat list items as velo palette names and apply the highlighted one live as the user scrolls (used by the palette switcher). See [Theming](#theming). |

**`input`:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `prompt` | string | `""` | Prompt text shown to the user |
| `sensitive` | bool | `false` | Hide input characters (password mode) |

**`preview`:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `eval_cmd` | string | `""` | Command template run on each (debounced) input change. `{input}` is the user's input; the first line of its stdout becomes the preview. |
| `copy_cmd` | string | `wl-copy` | Command run on Enter; the preview text is piped to its stdin (no shell escaping needed) |

**`list`:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `children` | string[] | `[]` | Names of child plugins (max 32) |

## Templates and Dictionary Flow

### Template resolution

Templates use `{key}` placeholders substituted from the accumulated dictionary:

```toml
template = "nmcli device wifi connect '{network}' password '{password}'"
```

With `network = "MyWiFi"` and `password = "secret"` in the dictionary, the resolved command is:

```
nmcli device wifi connect 'MyWiFi' password 'secret'
```

Keys not present in the dictionary resolve to empty strings. Substitution is **raw** (no shell escaping), so user-typed text is evaluated by the shell. Commands run via `system()`, except `@`-prefixed builtins (see below).

### Dictionary flow

The dictionary is a set of key-value pairs that accumulates through the navigation stack:

1. When a plugin is navigated to, its level gets a **copy** of the parent's dictionary.
2. When the user selects/types, the value is added: `dict[as] = value`.
3. If `next` is set, the accumulated dict passes to the next plugin.
4. If `return` is set, the accumulated dict passes **up** to the parent.

This enables multi-step workflows where each step contributes a value.

### Chaining with `next` and `return`

**`next` — forward chain.** After completing, navigate to another plugin; the dictionary accumulates. Example: a wifi-connect plugin chains to enter-password:

```toml
# wifi-connect.toml
name = "wifi-connect"
type = "select"
list_cmd = "bash -c '~/.config/velo/plugins/wifi/list.sh'"
format = "json"
label_field = "name"
value_field = "id"
as = "network"
template = "nmcli device wifi connect '{network}' password '{password}'"
next = "enter-password"
```

Flow: pick network -> `dict["network"] = "MyWiFi"` -> navigate to enter-password -> type password -> `dict["password"] = "secret"` -> return to parent -> execute the template with both values.

**`return` — return to parent.** After completing, pop the level and pass the accumulated dict to the parent; if the parent has a `template`, it executes with the full dict. This makes a plugin reusable: any plugin needing a password chains via `next = "enter-password"`.

```toml
# utils/enter-password.toml
name = "enter-password"
type = "input"
prompt = "Password: "
as = "password"
sensitive = true
return = true
```

**Both set:** if both `next` and `return` are set, `next` wins (it is checked first). Circular chains are allowed.

## Teleport

Type a plugin's name followed by `:` to jump directly to it. Only plugins with `teleport = true` are eligible.

- Case-insensitive: `calc:`, `Calc:`, `CALC:` all work.
- Must be at the start of the input (or be the entire input).
- Current input state is saved and restored on Escape.

From the CLI, use `-e` to teleport at startup:

```sh
velo -e drun         # opens directly in the app launcher
velo -e calculator   # opens directly in the calculator
```

With `-e`, closing the plugin closes the window entirely (no root menu to fall back to).

## Dependencies

Plugins declare required binaries via `depends`:

```toml
depends = ["tmux"]
depends = ["nmcli", "jq"]
```

At load time each binary is searched on `$PATH`. If any are missing, the plugin is hidden from the root menu, from parent `children` lists, and from teleport matching.

## Builtin Commands

Commands starting with `@` are handled internally rather than via the shell:

| Command | Description |
|---------|-------------|
| `@apps` | List installed `.desktop` applications. Use as `list_cmd`. |
| `@launch {id}` | Launch a desktop app by ID. Use as `template`. |

When `list_cmd = "@apps"`, the plugin's own `template` and `as` override the builtin defaults on each result.

## Multiple Plugins per File

A single `.toml` file can define several plugins, separated by a line containing exactly `---`:

```toml
name = "theme"
display_label = "Themes"
type = "list"
global = true
children = ["palette-velo", "theme-kitty"]
---
name = "palette-velo"
display_label = "velo"
type = "select"
list_cmd = "ls -1 ~/.config/velo/palettes/ | sed 's/\\.json$//'"
format = "lines"
as = "palette"
template = "sed -i 's/^palette = .*/palette = \"{palette}\"/' \"$HOME/.config/velo/config\""
execution_type = "exec"
```

Files without `---` define a single plugin as before. This lets related plugins share a file (the bundled `themes/theme.toml` groups all the theme switchers this way).

## File Organization

```
~/.config/velo/plugins/
├── drun.toml              # App launcher (select)
├── url.toml               # URL opener (input)
├── calculator.toml        # Calculator (preview)
├── tmux/                  # Tmux manager (list + select/input children)
├── hyprland/              # Window/workspace switcher (list + select children)
├── wifi/                  # WiFi manager (list + select/exec children)
├── themes/theme.toml      # Theme switchers (list + select children, one file)
└── utils/enter-password.toml  # Reusable password input (input, return=true)
```

Directory structure is for organization only; plugins reference each other by `name`.

## Environment Variables in Templates

Templates run through the shell, so any environment variable is available:

- `$TERMINAL` - the detected terminal emulator
- `$HOME`, `$XDG_CONFIG_HOME`, `$XDG_BIN_HOME` - standard paths
- Anything exported in the shell context velo was launched from

## Theming

velo is themed with **palettes** (JSON files with `dark`/`light` variants), not per-plugin. See [`doc/palette.md`](doc/palette.md) for the palette format and how to create one.

The active palette is the `palette = "name"` line in `~/.config/velo/config` (plus `darkmode = true|false`). A plugin can switch it by rewriting that line; the bundled `themes/theme.toml` `palette-velo` entry does exactly that, and sets `live_apply_palette = true` so the running instance previews each palette as you scroll.

## Plugin Safety

Plugins execute **arbitrary shell commands** as your user (`list_cmd`, `template`, `eval_cmd`). Only install plugins from sources you trust, and read the TOML before copying it into `~/.config/velo/plugins/`. Treat a downloaded plugin the same way you treat a shell script.
