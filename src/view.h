#ifndef VIEW_H
#define VIEW_H

#include "color.h"
#include "string_vec.h"
#include <stdint.h>
#include <stdbool.h>
#include <uchar.h>

#define VIEW_MAX_INPUT 256
#define VIEW_MAX_PROMPT 256
#define VIEW_MAX_RESULTS 1024
#define VIEW_MAX_FONT_NAME 256
#define VIEW_MAX_FONT_FEATURES 128
#define VIEW_MAX_FONT_VARIATIONS 128

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

	bool foreground_specified;
	bool background_specified;
	bool padding_specified;
	bool radius_specified;
};

struct view_theme {
	struct color background_color;
	struct color foreground_color;
	struct color accent_color;
	struct color selection_highlight_color;
	
	char font_name[VIEW_MAX_FONT_NAME];
	uint32_t font_size;
	char font_features[VIEW_MAX_FONT_FEATURES];
	char font_variations[VIEW_MAX_FONT_VARIATIONS];
	
	uint32_t border_width;
	uint32_t corner_radius;
	uint32_t padding_top;
	uint32_t padding_right;
	uint32_t padding_bottom;
	uint32_t padding_left;
	bool padding_top_is_percent;
	bool padding_right_is_percent;
	bool padding_bottom_is_percent;
	bool padding_left_is_percent;
	
	uint32_t input_width;
	bool horizontal;
	uint32_t num_results;
	int32_t result_spacing;
	uint32_t prompt_padding;
	
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
	
	struct string_ref_vec results;
	struct string_ref_vec commands;
	uint32_t selection;
	uint32_t first_result;
	uint32_t num_results_drawn;
	uint32_t last_num_results_drawn;
};

struct view_layout {
	int32_t result_start_y;
	int32_t result_row_height;
};

#endif
