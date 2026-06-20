#ifndef VIEW_H
#define VIEW_H

#include "color.h"
#include "string_vec.h"
#include "matching.h"
#include <stdint.h>
#include <stdbool.h>
#include <uchar.h>

#define VIEW_MAX_INPUT 256
#define VIEW_MAX_PROMPT 256
#define VIEW_MAX_FONT_NAME 256

struct directional {
	int32_t top;
	int32_t right;
	int32_t bottom;
	int32_t left;
};

struct text_theme {
	struct color foreground_color;
	struct color background_color;
	struct directional padding;
	uint32_t background_corner_radius;
};

struct view_theme {
	struct color background_color;
	struct color foreground_color;
	struct color selection_color;
	struct color selection_fill_color;
	struct color selection_text_color;
	struct color border_color;
	struct color prompt_color;
	struct color divider_color;
	struct color match_color;
	float background_opacity;
	bool selection_box;
	bool match_highlight;

	char font_name[VIEW_MAX_FONT_NAME];
	uint32_t font_size;

	uint32_t border_width;
	uint32_t corner_radius;
	uint32_t padding_top;
	uint32_t padding_right;
	uint32_t padding_bottom;
	uint32_t padding_left;

	struct text_theme prompt_theme;
	struct text_theme input_theme;
	struct text_theme result_theme;
};

struct view_state {
	char prompt[VIEW_MAX_PROMPT];
	
	char32_t input_utf32[VIEW_MAX_INPUT];
	char input_utf8[4 * VIEW_MAX_INPUT];
	uint32_t input_utf32_length;
	uint32_t input_utf8_length;
	uint32_t cursor_position;
	bool sensitive;
	enum matching_algorithm algorithm;
	
	struct string_ref_vec results;
	struct string_ref_vec commands;
	uint32_t selection;
	uint32_t first_result;
	uint32_t num_results_drawn;
	uint32_t last_num_results_drawn;
	uint32_t render_height;
};

struct view_layout {
	int32_t result_start_y;
	int32_t result_row_height;
};

#endif
