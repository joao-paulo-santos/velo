#ifndef RENDERER_H
#define RENDERER_H

#include "view.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct renderer {
	const char *name;
	void *priv;
	
	bool (*init)(struct renderer *r, uint8_t *buffer, uint32_t width, uint32_t height, double scale,
	             struct view_theme *theme);
	void (*destroy)(struct renderer *r);
	void (*begin_frame)(struct renderer *r);
	void (*render)(struct renderer *r, struct view_state *state,
	               struct view_theme *theme, struct view_layout *layout);
	void (*end_frame)(struct renderer *r);
};

struct renderer *renderer_cairo_create(void);

#endif
