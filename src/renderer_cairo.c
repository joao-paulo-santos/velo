#include <cairo/cairo.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <unistd.h>
#include "renderer.h"
#include "view.h"
#include "log.h"
#include "scale.h"
#include "xmalloc.h"

#undef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))

struct cairo_priv {
	cairo_surface_t *surfaces[2];
	cairo_t *contexts[2];
	int buffer_index;
	PangoLayout *pango_layout;
	uint32_t clip_x, clip_y, clip_width, clip_height;
	struct view_theme *theme;
	uint32_t stride;
};

static void rounded_rectangle(cairo_t *cr, uint32_t width, uint32_t height, uint32_t r)
{
	cairo_new_path(cr);
	cairo_arc(cr, r, r, r, -M_PI, -M_PI_2);
	cairo_arc(cr, width - r, r, r, -M_PI_2, 0);
	cairo_arc(cr, width - r, height - r, r, 0, M_PI_2);
	cairo_arc(cr, r, height - r, r, M_PI_2, M_PI);
	cairo_close_path(cr);
}

static void apply_text_theme_fallback(struct text_theme *theme, const struct text_theme *fallback)
{
	if (!theme->foreground_specified) {
		theme->foreground_color = fallback->foreground_color;
	}
	if (!theme->background_specified) {
		theme->background_color = fallback->background_color;
	}
	if (!theme->padding_specified) {
		theme->padding = fallback->padding;
	}
	if (!theme->radius_specified) {
		theme->background_corner_radius = fallback->background_corner_radius;
	}
}

static void render_text_themed(
		cairo_t *cr,
		struct cairo_priv *priv,
		const char *text,
		const struct text_theme *theme,
		PangoRectangle *ink_rect,
		PangoRectangle *logical_rect)
{
	struct color color = theme->foreground_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

	pango_layout_set_text(priv->pango_layout, text, -1);
	pango_cairo_update_layout(cr, priv->pango_layout);
	pango_cairo_show_layout(cr, priv->pango_layout);

	pango_layout_get_pixel_extents(priv->pango_layout, ink_rect, logical_rect);

	if (theme->background_color.a == 0) {
		return;
	}

	struct directional padding = theme->padding;

	cairo_matrix_t mat;
	cairo_get_matrix(cr, &mat);
	int32_t base_x = mat.x0 - priv->clip_x + ink_rect->x;
	int32_t base_y = mat.y0 - priv->clip_y;

	double padding_left = padding.left;
	double padding_right = padding.right;
	double padding_top = padding.top;
	double padding_bottom = padding.bottom;

	if (padding_left < 0) padding_left = base_x;
	if (padding_right < 0) padding_right = priv->clip_width - ink_rect->width - base_x;
	if (padding_top < 0) padding_top = base_y;
	if (padding_bottom < 0) padding_bottom = priv->clip_height - logical_rect->height - base_y;

	cairo_save(cr);
	color = theme->background_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_translate(cr, -padding_left + ink_rect->x, -padding_top);
	rounded_rectangle(cr,
		ceil(ink_rect->width + padding_left + padding_right),
		ceil(logical_rect->height + padding_top + padding_bottom),
		theme->background_corner_radius);
	cairo_fill(cr);
	cairo_restore(cr);

	color = theme->foreground_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	pango_cairo_show_layout(cr, priv->pango_layout);
}

static void render_input(
		cairo_t *cr,
		struct cairo_priv *priv,
		const char *text,
		const struct text_theme *theme,
		PangoRectangle *ink_rect,
		PangoRectangle *logical_rect)
{
	struct directional padding = theme->padding;
	struct color color = theme->foreground_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

	pango_layout_set_text(priv->pango_layout, text, -1);
	pango_cairo_update_layout(cr, priv->pango_layout);
	pango_cairo_show_layout(cr, priv->pango_layout);

	pango_layout_get_pixel_extents(priv->pango_layout, ink_rect, logical_rect);

	if (theme->background_color.a != 0) {
		cairo_save(cr);
		color = theme->background_color;
		cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
		cairo_translate(cr, floor(-padding.left + ink_rect->x), -padding.top);
		rounded_rectangle(cr,
			ceil(ink_rect->width + padding.left + padding.right),
			ceil(logical_rect->height + padding.top + padding.bottom),
			theme->background_corner_radius);
		cairo_fill(cr);
		cairo_restore(cr);

		color = theme->foreground_color;
		cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
		pango_cairo_show_layout(cr, priv->pango_layout);
	}
}

static bool size_overflows(struct cairo_priv *priv, cairo_t *cr, int32_t height)
{
	cairo_matrix_t mat;
	cairo_get_matrix(cr, &mat);
	return (mat.y0 - priv->clip_y + height > priv->clip_height);
}

static bool cairo_init(struct renderer *r, uint8_t *buffer, uint32_t width, uint32_t height, double scale,
                       struct view_theme *theme)
{
	struct cairo_priv *priv = xcalloc(1, sizeof(*priv));
	r->priv = priv;
	priv->theme = theme;
	priv->buffer_index = 0;
	priv->stride = width * sizeof(uint32_t);

	uint32_t scaled_width = scale_apply_inverse(width, scale * 120);
	uint32_t scaled_height = scale_apply_inverse(height, scale * 120);

	priv->surfaces[0] = cairo_image_surface_create_for_data(
		buffer,
		CAIRO_FORMAT_ARGB32,
		width, height,
		priv->stride
	);
	cairo_surface_set_device_scale(priv->surfaces[0], scale, scale);
	priv->contexts[0] = cairo_create(priv->surfaces[0]);

	priv->surfaces[1] = cairo_image_surface_create_for_data(
		buffer + height * priv->stride,
		CAIRO_FORMAT_ARGB32,
		width, height,
		priv->stride
	);
	cairo_surface_set_device_scale(priv->surfaces[1], scale, scale);
	priv->contexts[1] = cairo_create(priv->surfaces[1]);

	cairo_matrix_t mat;
	for (int buf_idx = 0; buf_idx < 2; buf_idx++) {
		cairo_t *cr = priv->contexts[buf_idx];

		struct color color = theme->background_color;
		cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(cr);

		cairo_set_line_width(cr, 2 * theme->border_width);
		rounded_rectangle(cr, scaled_width, scaled_height, theme->corner_radius);

		color = theme->accent_color;
		cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
		cairo_stroke_preserve(cr);

		cairo_rectangle(cr, 0, 0, scaled_width + 1, scaled_height + 1);
		cairo_set_source_rgba(cr, 0, 0, 0, 1);
		cairo_save(cr);
		cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_fill(cr);
		cairo_restore(cr);

		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

		double dx = theme->border_width;
		cairo_translate(cr, dx, dx);
		uint32_t w = scaled_width - 2 * dx;
		uint32_t h = scaled_height - 2 * dx;

		cairo_translate(cr, theme->padding_left, theme->padding_top);
		w -= theme->padding_left + theme->padding_right;
		h -= theme->padding_top + theme->padding_bottom;

		double inner_radius = MAX((double)theme->corner_radius - theme->border_width, 0);
		dx = ceil(inner_radius * (1.0 - 1.0 / M_SQRT2));
		cairo_translate(cr, dx, dx);
		w -= 2 * dx;
		h -= 2 * dx;
		cairo_rectangle(cr, 0, 0, w, h);
		cairo_clip(cr);

		if (buf_idx == 0) {
			cairo_get_matrix(cr, &mat);
			priv->clip_x = mat.x0;
			priv->clip_y = mat.y0;
			priv->clip_width = w;
			priv->clip_height = h;
		}
	}

	cairo_t *cr = priv->contexts[0];
	priv->pango_layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(theme->font_name);
	pango_font_description_set_size(desc, theme->font_size * PANGO_SCALE);
	pango_layout_set_font_description(priv->pango_layout, desc);
	pango_font_description_free(desc);

	const struct text_theme default_theme = {
		.foreground_color = theme->foreground_color,
		.background_color = (struct color) { .a = 0 },
		.padding = (struct directional) {0},
		.background_corner_radius = 0
	};
	apply_text_theme_fallback(&theme->prompt_theme, &default_theme);
	apply_text_theme_fallback(&theme->input_theme, &default_theme);
	apply_text_theme_fallback(&theme->result_theme, &default_theme);

	return true;
}

static void renderer_cairo_destroy(struct renderer *r)
{
	struct cairo_priv *priv = r->priv;
	if (!priv) return;

	if (priv->pango_layout) {
		g_object_unref(priv->pango_layout);
	}
	cairo_destroy(priv->contexts[0]);
	cairo_destroy(priv->contexts[1]);
	cairo_surface_destroy(priv->surfaces[0]);
	cairo_surface_destroy(priv->surfaces[1]);
	free(priv);
	r->priv = NULL;
}

static void cairo_begin_frame(struct renderer *r)
{
}

static void cairo_render(struct renderer *r, struct view_state *state,
                         struct view_theme *theme, struct view_layout *layout)
{
	struct cairo_priv *priv = r->priv;
	if (!priv) return;

	cairo_t *cr = priv->contexts[priv->buffer_index];

	struct color color = theme->background_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_restore(cr);

	cairo_save(cr);

	PangoRectangle ink_rect, logical_rect;

	render_text_themed(cr, priv, state->prompt, &theme->prompt_theme, &ink_rect, &logical_rect);
	cairo_translate(cr, logical_rect.width + logical_rect.x, 0);
	cairo_translate(cr, theme->prompt_padding, 0);

	if (state->input_utf8_length == 0) {
		render_input(cr, priv, "", &theme->input_theme, &ink_rect, &logical_rect);
	} else if (state->sensitive) {
		size_t nchars = state->input_utf32_length;
		char *buf = xmalloc(nchars + 1);
		for (size_t i = 0; i < nchars; i++) {
			buf[i] = '*';
		}
		buf[nchars] = '\0';
		render_input(cr, priv, buf, &theme->input_theme, &ink_rect, &logical_rect);
		free(buf);
	} else {
		render_input(cr, priv, state->input_utf8, &theme->input_theme, &ink_rect, &logical_rect);
	}
	logical_rect.width = MAX(logical_rect.width, (int)theme->input_width);

	cairo_translate(cr, 0, logical_rect.height);
	cairo_matrix_t mat;
	cairo_get_matrix(cr, &mat);
	mat.x0 = priv->clip_x;
	cairo_set_matrix(cr, &mat);

	uint32_t num_results = theme->num_results == 0 ? state->results.count : MIN(theme->num_results, state->results.count);

	if (num_results > 0) {
		cairo_translate(cr, 0, 2);
		struct color sep_color = theme->accent_color;
		cairo_set_source_rgba(cr, sep_color.r, sep_color.g, sep_color.b, sep_color.a);
		cairo_set_line_width(cr, 1);
		cairo_move_to(cr, 0, 0);
		cairo_line_to(cr, priv->clip_width, 0);
		cairo_stroke(cr);
		cairo_translate(cr, 0, 4);
	}

	cairo_matrix_t result_mat;
	cairo_get_matrix(cr, &result_mat);
	layout->result_start_y = (int32_t)result_mat.y0;

	size_t i;
	for (i = 0; i < num_results; i++) {
		if (i >= theme->num_results && theme->num_results > 0) {
			break;
		}

		size_t index = i + state->first_result;
		if (index >= state->results.count) break;

		const char *result = state->results.buf[index].string;

		pango_layout_set_text(priv->pango_layout, result, -1);
		pango_cairo_update_layout(cr, priv->pango_layout);
		pango_layout_get_pixel_extents(priv->pango_layout, &ink_rect, &logical_rect);

		if (theme->num_results == 0) {
			if (size_overflows(priv, cr, logical_rect.height)) break;
		}

		if (i == state->selection) {
			struct color sel_color = theme->accent_color;
			cairo_set_source_rgba(cr, sel_color.r, sel_color.g, sel_color.b, sel_color.a);
			pango_cairo_show_layout(cr, priv->pango_layout);
		} else {
			render_text_themed(cr, priv, result, &theme->result_theme, &ink_rect, &logical_rect);
		}

		if (!theme->horizontal) {
			if (i == 0) {
				layout->result_row_height = logical_rect.height + theme->result_spacing;
			}
			if (i + 1 < num_results) {
				cairo_translate(cr, 0, logical_rect.height + theme->result_spacing);
			}
		}
	}
	state->num_results_drawn = i;

	cairo_restore(cr);
}

static void cairo_end_frame(struct renderer *r)
{
	struct cairo_priv *priv = r->priv;
	if (!priv) return;
	priv->buffer_index = !priv->buffer_index;
}

struct renderer *renderer_cairo_create(void)
{
	struct renderer *r = xcalloc(1, sizeof(*r));
	r->name = "cairo";
	r->init = cairo_init;
	r->destroy = renderer_cairo_destroy;
	r->begin_frame = cairo_begin_frame;
	r->render = cairo_render;
	r->end_frame = cairo_end_frame;
	return r;
}
