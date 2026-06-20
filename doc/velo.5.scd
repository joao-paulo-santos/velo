velo(5)

# NAME

velo - configuration file

# DESCRIPTION

Config file location: _$XDG_CONFIG_HOME/velo/config_

Options can also be passed as CLI flags (see _velo_(1)).
Values are: strings, integers, booleans, or percentages (e.g. *50%*).
Color values live in palettes, not the config. See _PALETTES_ below.

# WINDOW

*anchor* <string>
	Window position on screen. One of: *top-left*, *top*, *top-right*,
	*right*, *bottom-right*, *bottom*, *bottom-left*, *left*, *center*.

*width* <value>
	Window width. Pixels or percentage of output width.

*height* <value>
	Window height. Pixels or percentage of output height.

*margin-top* <value>
	Top margin. Pixels or percentage.

*margin-bottom* <value>
	Bottom margin. Pixels or percentage.

*margin-left* <value>
	Left margin. Pixels or percentage.

*margin-right* <value>
	Right margin. Pixels or percentage.

*output* <string>
	Output name to display on. Default: first detected output.

*autosize* <bool>
	Dynamic window height. Window shrinks/grows to fit visible results,
	snapping to exact row boundaries. When enabled, anchor is forced to
	top and the window is centered vertically within the output. Default:
	false.

# STYLE

*font* <string>
	Font name or path to font file. Supports ~ prefix for home directory.

*font-size* <int>
	Font size in pixels. Default: 24.

*background-opacity* <float>
	Background transparency. Range 0.1 to 1.0. Default: 1.0.

*border-width* <int>
	Border width in pixels. Default: 0.

*corner-radius* <int>
	Window corner radius in pixels. Default: 0.

*padding* <int>
	Inner padding on all sides. Default: 8.

*padding-top* <int>
	Override top padding. Set after _padding_.

*padding-bottom* <int>
	Override bottom padding. Set after _padding_.

*padding-left* <int>
	Override left padding. Set after _padding_.

*padding-right* <int>
	Override right padding. Set after _padding_.

*prompt-text* <string>
	Default prompt text. Default: *run: *.

# PALETTES

Colors are defined by palettes: small JSON files with a dark and a light
variant, stored in _$XDG_CONFIG_HOME/velo/palettes/<name>.json_. A palette
holds six color roles (surface, onSurface, primary, onPrimary, secondary,
outline); of these surface, onSurface, primary, and outline are rendered
and the rest are reserved. See the palette documentation for the full
format and how to create your own.

*palette* <string>
	Palette name (the JSON filename without the _.json_ suffix).
	Default: *breeze*.

*darkmode* <bool>
	Use the palette's dark (*true*) or light (*false*) variant.
	Default: *true*.

*selection-box* <bool>
	Render the selected entry as a filled bar (primary background with
	onPrimary text) instead of colored text. Default: *false*.

	List installed palettes with *velo --list-palettes*. The palette format
	and how to remap roles via _palette_color_mapping.json_ are documented
	in the palette docs.

# PLUGINS

*plugins* <string>
	Comma-separated plugin filter. Use *all* to enable all, prefix
	with *-* to exclude. Example: *all,-drun* or *tmux,wifi*.

# EXAMPLE

```
font = "Sans"
font-size = 24
palette = "tokyo-night"
darkmode = true
background-opacity = 0.9
width = 50%
border-width = 2
corner-radius = 8
padding = 16
padding-bottom = 8
prompt-text = "run: "
autosize = true
plugins = all
```

# SEE ALSO

_velo_(1)
