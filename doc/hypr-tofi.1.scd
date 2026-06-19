hypr-tofi(1)

# NAME

hypr-tofi - fast, plugin-extensible launcher for Wayland

# SYNOPSIS

*hypr-tofi* [options...]

# DESCRIPTION

hypr-tofi is a dynamic menu for Wayland compositors, forked from tofi.
All functionality is defined via TOML plugins - no compiled modes.

On launch, the main menu shows all global plugins. Type to fuzzy-filter
results. Press Enter to select. Press Esc to go back or close.

Type a plugin name followed by *:*
to teleport directly to it (e.g. *calc:*, *tmux:*, *url:*).

# OPTIONS

*-h, --help*
	Print usage and exit.

*-c, --config* <path>
	Config file path. Default: _$XDG_CONFIG_HOME/hypr-tofi/config_

*-f, --filter* <filter>
	Filter global plugins on root menu. Comma-separated.
	Use *all* to enable all, prefix with *-* to exclude.
	Example: *all,-drun* or *tmux,wifi*

*-p, --plugins* <list>
	Show only these plugins as the root menu. Comma-separated
	plugin names. Esc closes the window.
	Example: *tmux-freeze,tmux-unfreeze*

*-e, --entry* <plugin>
	Teleport directly to a plugin at startup. The window closes when
	leaving the plugin (no root menu). Example: *-e drun*

*-t, --theme* <name>
	Set theme. Overrides the *theme* config option. Themes are loaded
	from _$XDG_CONFIG_HOME/hypr-tofi/themes/<name>.toml_

*-L, --list-themes*
	List available theme names and exit.

*--pick*
	dmenu-compatible picker mode. Read newline-separated items from
	stdin, let the user fuzzy-search and select one, print the selection
	to stdout and exit. Cannot be combined with *-e*, *-p*, or *--input*.

	Example: *echo -e "a\\nb\\nc" | hypr-tofi --pick*

	Exit codes: 0 on selection, 1 on no selection or empty stdin.

*--input*
	Input mode. Show a text entry prompt, print the typed text to stdout
	on Enter. Cannot be combined with *-e*, *-p*, or *--pick*.

	Example: *hypr-tofi --input --prompt-text 'Password: '* > /tmp/pw

	Exit codes: 0 on submit, 1 on Escape/close.

*--sensitive*
	Mask input with asterisks. For use with *--input*.

*--background-opacity* <float>
	Background transparency. 0.1-1.0. Overrides any alpha in the
	background color. Default: 1.0.

All config file options (see _hypr-tofi_(5)) can also be passed as
long flags. Example: *--font "Monospace"* *--font-size 14*

# ENVIRONMENT

*HYPR_TOFI_DEBUG=1*
	Draw debug overlay: green rectangle at clip boundary, red rectangle
	at border boundary. Useful for tuning padding and autosize.

# KEY BINDINGS

*Enter*
	Submit selection or confirm input.

*Escape*
	Go back one level. If input is non-empty, clear it first.
	If at root, close the window.

*Ctrl+C*, *Ctrl+G*
	Close immediately.

*Tab*, *Shift+Tab*
	Next/previous result.

*Up*, *Down*
	Previous/next result.

*Ctrl+W*
	Delete word before cursor.

*Ctrl+U*
	Clear entire input.

*Ctrl+V*
	Paste from clipboard.

# FILES

_$XDG_CONFIG_HOME/hypr-tofi/config_
	Default config file.

_$XDG_CONFIG_HOME/hypr-tofi/plugins/_
	Plugin directory. Any *\*.toml* file in any subdirectory is loaded.

_$XDG_CONFIG_HOME/hypr-tofi/history/_
	Feedback history files (when *persist_history = true*).

# SEE ALSO

_hypr-tofi_(5)
