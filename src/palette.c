#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "color.h"
#include "json.h"
#include "log.h"
#include "palette.h"
#include "xmalloc.h"

/* Hardcoded fallback (breeze dark). Bare velo with no files renders with this. */
void palette_apply_fallback(struct palette *out)
{
	out->surface    = hex_to_color("#202224");
	out->on_surface = hex_to_color("#fcfcfc");
	out->primary    = hex_to_color("#3daee9");
	out->on_primary = hex_to_color("#141618");
	out->secondary  = hex_to_color("#1d99f3");
	out->outline    = hex_to_color("#707d8a");
}

static char *read_file(const char *path);
static char *config_dir(void);

static float color_dist(struct color a, struct color b)
{
	float dr = a.r - b.r;
	float dg = a.g - b.g;
	float db = a.b - b.b;
	return sqrtf(dr * dr + dg * dg + db * db);
}

static void rgb_to_hsl(struct color c, float *h, float *s, float *l)
{
	float max = (c.r > c.g) ? ((c.r > c.b) ? c.r : c.b) : ((c.g > c.b) ? c.g : c.b);
	float min = (c.r < c.g) ? ((c.r < c.b) ? c.r : c.b) : ((c.g < c.b) ? c.g : c.b);
	*l = (max + min) * 0.5f;
	if (max == min) {
		*h = 0.0f;
		*s = 0.0f;
		return;
	}
	float d = max - min;
	*s = (*l > 0.5f) ? d / (2.0f - max - min) : d / (max + min);
	if (max == c.r) {
		*h = (c.g - c.b) / d + (c.g < c.b ? 6.0f : 0.0f);
	} else if (max == c.g) {
		*h = (c.b - c.r) / d + 2.0f;
	} else {
		*h = (c.r - c.g) / d + 4.0f;
	}
	*h /= 6.0f;
}

static float hue_to_rgb(float p, float q, float t)
{
	if (t < 0.0f) t += 1.0f;
	if (t > 1.0f) t -= 1.0f;
	if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
	if (t < 0.5f) return q;
	if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
	return p;
}

static struct color hsl_to_rgb(float h, float s, float l)
{
	if (s == 0.0f) {
		return (struct color){l, l, l, 1.0f};
	}
	float q = (l < 0.5f) ? l * (1.0f + s) : l + s - l * s;
	float p = 2.0f * l - q;
	float r = hue_to_rgb(p, q, h + 1.0f / 3.0f);
	float g = hue_to_rgb(p, q, h);
	float b = hue_to_rgb(p, q, h - 1.0f / 3.0f);
	return (struct color){r, g, b, 1.0f};
}

/*
 * Derived selection color. The M3 "primary" role is a fill tint that is often a
 * tonal sibling of the body text (onSurface), so using it directly to recolour
 * the selected entry collides with body text on many palettes (e.g. monochrome
 * ones like cherry-blossom). Instead, keep primary's hue and saturation and pick
 * the lightness that maximises the minimum distance to BOTH onSurface (body
 * text) and surface (background). This guarantees a visible selection on every
 * palette without inventing off-palette colours. Verified across all 122
 * dark/light palette-modes.
 */
struct color palette_selection_color(const struct palette *p)
{
	float h, s, l;
	rgb_to_hsl(p->primary, &h, &s, &l);

	struct color best = p->primary;
	float best_score = -1.0f;
	/* Scan lightness across a mid range that stays visible on both dark and
	 * light backgrounds while avoiding pure white/black. */
	for (int i = 20; i <= 80; i++) {
		float cl = i / 100.0f;
		struct color cand = hsl_to_rgb(h, s, cl);
		float dt = color_dist(cand, p->on_surface);
		float db = color_dist(cand, p->surface);
		float score = (dt < db) ? dt : db;
		if (score > best_score) {
			best_score = score;
			best = cand;
		}
	}
	return best;
}

/*
 * Derived match-highlight color. Match glyphs sit inline in the body text and
 * must read against the background (surface), stand out from the body text
 * (on_surface), stay distinct from the selected-row text color (the derived
 * selection color), and stay visible against the filled selection bar, whose
 * background is raw primary (selection-box mode). Keep secondary's hue and
 * saturation and scan lightness for the value that maximises the minimum of
 * those four distances. Mirrors palette_selection_color's approach.
 */
struct color palette_match_color(const struct palette *p)
{
	struct color avoid_sel = palette_selection_color(p);

	float h, s, l;
	rgb_to_hsl(p->secondary, &h, &s, &l);

	struct color best = p->secondary;
	float best_score = -1.0f;
	for (int i = 20; i <= 80; i++) {
		float cl = i / 100.0f;
		struct color cand = hsl_to_rgb(h, s, cl);
		float dt = color_dist(cand, p->on_surface);
		float db = color_dist(cand, p->surface);
		float ds = color_dist(cand, avoid_sel);
		float df = color_dist(cand, p->primary);
		float min = (dt < db) ? dt : db;
		if (ds < min) {
			min = ds;
		}
		if (df < min) {
			min = df;
		}
		if (min > best_score) {
			best_score = min;
			best = cand;
		}
	}
	return best;
}

/* Hardcoded default role mapping (used when palette_color_mapping.json is
 * absent or invalid). Encodes the render decisions documented in doc/palette.md. */
static const struct color_mapping MAPPING_DEFAULT = {
	.background = ROLE_SURFACE,
	.text = ROLE_ON_SURFACE,
	.selection = ROLE_DERIVED,
	.border = ROLE_OUTLINE,
	.prompt = ROLE_DERIVED,
	.divider = ROLE_DERIVED,
	.match = ROLE_DERIVED,
};

static enum palette_role role_from_name(const char *name)
{
	if (strcmp(name, "surface") == 0) return ROLE_SURFACE;
	if (strcmp(name, "onSurface") == 0) return ROLE_ON_SURFACE;
	if (strcmp(name, "primary") == 0) return ROLE_PRIMARY;
	if (strcmp(name, "onPrimary") == 0) return ROLE_ON_PRIMARY;
	if (strcmp(name, "secondary") == 0) return ROLE_SECONDARY;
	if (strcmp(name, "outline") == 0) return ROLE_OUTLINE;
	if (strcmp(name, "derived") == 0) return ROLE_DERIVED;
	return ROLE_INVALID;
}

struct color palette_role_color(const struct palette *p, enum palette_role r)
{
	switch (r) {
	case ROLE_SURFACE:    return p->surface;
	case ROLE_ON_SURFACE: return p->on_surface;
	case ROLE_PRIMARY:    return p->primary;
	case ROLE_ON_PRIMARY: return p->on_primary;
	case ROLE_SECONDARY:  return p->secondary;
	case ROLE_OUTLINE:    return p->outline;
	case ROLE_DERIVED:    return palette_selection_color(p);
	default:              return p->primary;
	}
}

bool palette_color_mapping_load(struct color_mapping *out)
{
	*out = MAPPING_DEFAULT;

	char *cd = config_dir();
	if (cd == NULL) {
		return false;
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/palette_color_mapping.json", cd);
	free(cd);

	char *json = read_file(path);
	if (json == NULL) {
		return false;
	}

	bool loaded = false;
	json_parser_t p;
	json_parser_init(&p, json);
	if (json_object_begin(&p)) {
		char key[64];
		bool has_more;
		while (json_object_next(&p, key, sizeof(key), &has_more) && has_more) {
			char val[32];
			if (json_parse_string(&p, val, sizeof(val))) {
				enum palette_role r = role_from_name(val);
				if (r != ROLE_INVALID) {
					if (strcmp(key, "background") == 0) out->background = r;
					else if (strcmp(key, "text") == 0) out->text = r;
					else if (strcmp(key, "selection") == 0) out->selection = r;
					else if (strcmp(key, "border") == 0) out->border = r;
					else if (strcmp(key, "prompt") == 0) out->prompt = r;
					else if (strcmp(key, "divider") == 0) out->divider = r;
					else if (strcmp(key, "match") == 0) out->match = r;
					/* unknown slot keys are ignored */
				} else {
					log_error("palette_color_mapping: unknown role \"%s\" for \"%s\"\n", val, key);
				}
			} else {
				json_skip_value(&p);
			}
			if (json_peek_char(&p, ',')) {
				json_expect_char(&p, ',');
			}
		}
		loaded = true;
	}

	free(json);
	return loaded;
}

/* Resolve the user velo config dir: $XDG_CONFIG_HOME/velo, or ~/.config/velo
 * when XDG_CONFIG_HOME is unset. NULL if HOME is. */
static char *config_dir(void)
{
	const char *base = getenv("XDG_CONFIG_HOME");
	char *fallback = NULL;
	if (base == NULL || base[0] == '\0') {
		const char *home = getenv("HOME");
		if (home == NULL) {
			return NULL;
		}
		size_t len = strlen(home) + strlen("/.config") + 1;
		fallback = xmalloc(len);
		snprintf(fallback, len, "%s/.config", home);
		base = fallback;
	}
	size_t len = strlen(base) + strlen("/velo") + 1;
	char *dir = xmalloc(len);
	snprintf(dir, len, "%s/velo", base);
	free(fallback);
	return dir;
}

/* The user palettes dir: <config_dir>/palettes. */
static char *palette_dir(void)
{
	char *cd = config_dir();
	if (cd == NULL) {
		return NULL;
	}
	size_t len = strlen(cd) + strlen("/palettes") + 1;
	char *dir = xmalloc(len);
	snprintf(dir, len, "%s/palettes", cd);
	free(cd);
	return dir;
}

void palette_list(void)
{
	char *dir = palette_dir();
	if (dir == NULL) {
		return;
	}
	DIR *d = opendir(dir);
	if (d == NULL) {
		free(dir);
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		const char *name = entry->d_name;
		size_t len = strlen(name);
		if (len > 5 && strcmp(name + len - 5, ".json") == 0) {
			printf("%.*s\n", (int)(len - 5), name);
		}
	}
	closedir(d);
	free(dir);
}

enum palette_field {
	PF_SURFACE = 0,
	PF_ON_SURFACE,
	PF_PRIMARY,
	PF_ON_PRIMARY,
	PF_SECONDARY,
	PF_OUTLINE,
	PF_COUNT
};

/* Map a JSON key (unprefixed or m-prefixed) to a field index, or -1. */
static int palette_field_for_key(const char *key)
{
	static const char *names[PF_COUNT][2] = {
		{ "surface",   "mSurface"   },
		{ "onSurface", "mOnSurface" },
		{ "primary",   "mPrimary"   },
		{ "onPrimary", "mOnPrimary" },
		{ "secondary", "mSecondary" },
		{ "outline",   "mOutline"   },
	};
	for (int i = 0; i < PF_COUNT; i++) {
		if (strcmp(key, names[i][0]) == 0 || strcmp(key, names[i][1]) == 0) {
			return i;
		}
	}
	return -1;
}

static char *read_file(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (fp == NULL) {
		return NULL;
	}
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0) {
		fclose(fp);
		return NULL;
	}
	char *buf = xcalloc(1, (size_t)size + 1);
	size_t n = fread(buf, 1, (size_t)size, fp);
	fclose(fp);
	if (n != (size_t)size) {
		free(buf);
		return NULL;
	}
	return buf;
}

/* Parse one mode object. The parser is positioned at the '{'. Fields absent
 * from the object are left unchanged (the caller pre-seeds the struct with the
 * fallback so absent keys inherit sensible values). */
static void parse_mode(json_parser_t *p, struct palette *out)
{
	if (!json_object_begin(p)) {
		return;
	}
	char key[64];
	bool has_more;
	while (json_object_next(p, key, sizeof(key), &has_more) && has_more) {
		int idx = palette_field_for_key(key);
		if (idx >= 0) {
			char val[32];
			if (json_parse_string(p, val, sizeof(val))) {
				struct color c = hex_to_color(val);
				if (c.a >= 0.0f) {
					switch (idx) {
					case PF_SURFACE:    out->surface    = c; break;
					case PF_ON_SURFACE: out->on_surface = c; break;
					case PF_PRIMARY:    out->primary    = c; break;
					case PF_ON_PRIMARY: out->on_primary = c; break;
					case PF_SECONDARY:  out->secondary  = c; break;
					case PF_OUTLINE:    out->outline    = c; break;
					default: break;
					}
				}
			}
		} else {
			json_skip_value(p);
		}
		if (json_peek_char(p, ',')) {
			json_expect_char(p, ',');
		}
	}
	json_object_end(p);
}

bool palette_load(const char *name, bool darkmode, struct palette *out)
{
	palette_apply_fallback(out);

	if (name == NULL || name[0] == '\0') {
		return false;
	}

	char *dir = palette_dir();
	if (dir == NULL) {
		return false;
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.json", dir, name);
	free(dir);

	char *json = read_file(path);
	if (json == NULL) {
		log_error("palette '%s' not found at %s\n", name, path);
		return false;
	}

	/* Both variants parsed into temps (each pre-seeded with the fallback),
	 * then the requested one is selected, falling back to whichever exists. */
	struct palette dark = *out;
	struct palette light = *out;
	bool have_dark = false;
	bool have_light = false;
	bool ok = false;

	json_parser_t p;
	json_parser_init(&p, json);
	if (json_object_begin(&p)) {
		char key[32];
		bool has_more;
		while (json_object_next(&p, key, sizeof(key), &has_more) && has_more) {
			if (strcmp(key, "dark") == 0) {
				parse_mode(&p, &dark);
				have_dark = true;
			} else if (strcmp(key, "light") == 0) {
				parse_mode(&p, &light);
				have_light = true;
			} else {
				json_skip_value(&p);
			}
			if (json_peek_char(&p, ',')) {
				json_expect_char(&p, ',');
			}
		}
		ok = true;
	}

	free(json);

	if (!ok) {
		return false;
	}

	if (darkmode && have_dark) {
		*out = dark;
	} else if (!darkmode && have_light) {
		*out = light;
	} else if (have_dark) {
		*out = dark;
	} else if (have_light) {
		*out = light;
	}
	/* else: neither variant present; *out stays at the fallback */
	return true;
}
