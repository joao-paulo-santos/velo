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

# STYLE

*font* <string>
	Font name or path to font file. Supports ~ prefix for home directory.

*font-size* <int>
	Font size in pixels. Default: 24.

*text-color* <color>
	Default text color. Default: *#FFFFFF*.

*background-color* <color>
	Window background color. Default: *#000000*.

*accent-color* <color>
	Accent color for highlighted text. Default: *#FFFF00*.

*border-width* <int>
	Border width in pixels. Default: 0.

*corner-radius* <int>
	Window corner radius in pixels. Default: 0.

*padding* <value>
	Inner padding. Pixels or percentage.

*prompt-text* <string>
	Default prompt text. Default: *run: *.

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
accent-color = "#7aa2f7"
width = 50%
border-width = 2
corner-radius = 8
padding = 32
prompt-text = "run: "
plugins = all
```

# SEE ALSO

_hypr-tofi_(1)
