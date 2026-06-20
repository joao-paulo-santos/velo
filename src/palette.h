#ifndef PALETTE_H
#define PALETTE_H

#include <stdbool.h>
#include "color.h"

/*
 * A colour palette. Six roles matching the noctalia fixed-palette vocabulary;
 * of these, surface/on_surface/primary/outline are rendered by velo today and
 * on_primary/secondary are reserved for future use. See doc/palette.md.
 */
struct palette {
	struct color surface;
	struct color on_surface;
	struct color primary;
	struct color on_primary;
	struct color secondary;
	struct color outline;
};

/*
 * Load the palette <name> from <config>/velo/palettes/<name>.json.
 * darkmode selects the "dark" (true) or "light" (false) variant; if the
 * requested variant is absent, the other is used. The reader is lenient:
 * it accepts both the unprefixed key form ("primary") and noctalia's
 * m-prefixed form ("mPrimary"), and ignores unknown keys (including the
 * "terminal" block), so a stock noctalia palette JSON drops in unmodified.
 *
 * On success (file found and parsed), returns true. On any failure, *out is
 * filled with the hardcoded breeze-dark fallback (see palette_apply_fallback)
 * and false is returned. Either way *out is left usable for rendering.
 */
bool palette_load(const char *name, bool darkmode, struct palette *out);

/* Derive a selection color guaranteed to contrast both the body text
 * (on_surface) and the background (surface) by keeping primary's hue and
 * saturation and optimising its lightness. See doc/palette.md. */
struct color palette_selection_color(const struct palette *p);

/*
 * Optional user overrides for which palette role fills each render slot.
 * A small JSON file (~/.config/velo/palette_color_mapping.json) maps slot
 * names to role names; the hardcoded default below is used when the file is
 * absent or invalid. See doc/palette.md.
 */
enum palette_role {
	ROLE_SURFACE,
	ROLE_ON_SURFACE,
	ROLE_PRIMARY,
	ROLE_ON_PRIMARY,
	ROLE_SECONDARY,
	ROLE_OUTLINE,
	ROLE_DERIVED,   /* special: the derived selection color */
	ROLE_BOX,       /* special: filled bar (primary bg + onPrimary text) */
	ROLE_INVALID,
};

struct color_mapping {
	enum palette_role background;
	enum palette_role text;
	enum palette_role selection;
	enum palette_role border;
	enum palette_role prompt;
	enum palette_role divider;
};

/* Resolve a role to a color from a loaded palette. ROLE_DERIVED returns the
 * derived selection color. */
struct color palette_role_color(const struct palette *p, enum palette_role r);

/* Load the user palette_color_mapping.json, overlaying valid entries on the
 * hardcoded default. Returns true if a file was found and parsed. */
bool palette_color_mapping_load(struct color_mapping *out);

/* Fill *out with the hardcoded breeze-dark fallback used when no palette is
 * configured or a palette file cannot be read. */
void palette_apply_fallback(struct palette *out);

/* Print the names (filenames without the .json suffix) of palettes in the
 * user palettes directory, one per line. */
void palette_list(void);

#endif
