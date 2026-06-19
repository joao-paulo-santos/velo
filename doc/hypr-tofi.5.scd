hypr-tofi(5)

# NAME

hypr-tofi - configuration file

# DESCRIPTION

Config file location: _$XDG_CONFIG_HOME/hypr-tofi/config_

Options can also be passed as CLI flags (see _hypr-tofi_(1)).
Values are: strings, integers, hex colors (#RRGGBB or #RRGGBBAA),
or percentages (e.g. *50%*).

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

*text-color* <color>
	Default text color. Default: *#FFFFFF*.

*background-color* <color>
	Window background color. Alpha is ignored — use *background-opacity*
	instead. Default: *#000000*.

*background-opacity* <float>
	Background transparency. Range 0.1 to 1.0. Overrides any alpha value
	in *background-color*. Default: 1.0.

*accent-color* <color>
	Accent color for border, highlighted result, and separator.
	Default: *#FFFF00*.

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

# THEMES

*theme* <string>
	Theme name. Loads _$XDG_CONFIG_HOME/hypr-tofi/themes/<name>.toml_
	after the main config file. Theme files use the same options as the
	config (visual only: colors, fonts, borders, padding, corners).
	The *-t* CLI flag overrides this.

	Available themes can be listed with *hypr-tofi --list-themes*.

# PLUGINS

*plugins* <string>
	Comma-separated plugin filter. Use *all* to enable all, prefix
	with *-* to exclude. Example: *all,-drun* or *tmux,wifi*.

# EXAMPLE

```
font = "Sans"
font-size = 24
text-color = "#FFFFFF"
background-color = "#1a1b26"
background-opacity = 0.9
accent-color = "#7aa2f7"
width = 50%
border-width = 2
corner-radius = 8
padding = 16
padding-bottom = 8
prompt-text = "run: "
autosize = true
theme = "tokyo-night"
plugins = all
```

# SEE ALSO

_hypr-tofi_(1)
