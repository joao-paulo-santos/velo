#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "config.h"
#include "input.h"
#include "log.h"
#include "nav.h"
#include "matching.h"
#include "nelem.h"
#include "plugin.h"
#include "string_vec.h"
#include "velo.h"
#include "unicode.h"
#include "xmalloc.h"
#include "view.h"

static void add_character(struct velo *velo, xkb_keycode_t keycode);
static void delete_character(struct velo *velo);
static void delete_word(struct velo *velo);
static void clear_input(struct velo *velo);
static void paste(struct velo *velo);
static void select_previous_result(struct velo *velo);
static void select_next_result(struct velo *velo);
static void select_previous_page(struct velo *velo);
static void select_next_page(struct velo *velo);
static void next_cursor_or_result(struct velo *velo);
static void previous_cursor_or_result(struct velo *velo);
static void reset_selection(struct velo *velo);
static void nav_filter_results(struct velo *velo, const char *filter);
static void nav_pop_and_restore(struct velo *velo);
static bool try_teleport(struct velo *velo);

void input_scroll_up(struct velo *velo)
{
	select_previous_result(velo);
	velo->window.surface.redraw = true;
}

void input_scroll_down(struct velo *velo)
{
	select_next_result(velo);
	velo->window.surface.redraw = true;
}

void input_select_result(struct velo *velo, uint32_t index)
{
	struct view_state *state = &velo->view_state;
	if (index < state->num_results_drawn) {
		state->selection = index;
		if (velo->nav_current) {
			velo->nav_current->selection = index;
		} else {
			velo->base_selection = index;
		}
		velo->window.surface.redraw = true;
	}
}

struct scored_result {
	int32_t score;
	struct nav_result *result;
};

static int cmp_scored_result(const void *a, const void *b)
{
	const struct scored_result *sa = a;
	const struct scored_result *sb = b;
	return sb->score - sa->score;
}

static void nav_filter_results(struct velo *velo, const char *filter)
{
	struct view_state *state = &velo->view_state;
	struct nav_level *level = velo->nav_current;

	if (!level) {
		return;
	}

	nav_results_destroy(&level->results);
	string_ref_vec_destroy(&state->results);
	state->results = string_ref_vec_create();
	wl_list_init(&level->results);

	bool has_filter = filter && filter[0];

	size_t total = wl_list_length(&level->backup_results);
	if (total == 0) {
		return;
	}

	struct scored_result *entries = xmalloc(total * sizeof(*entries));
	size_t count = 0;

	struct nav_result *res;
	wl_list_for_each(res, &level->backup_results, link) {
		int32_t score = has_filter
			? match_words(MATCHING_ALGORITHM_FUZZY, filter, res->label)
			: 0;
		if (!has_filter || score != INT32_MIN) {
			entries[count].score = score;
			entries[count].result = res;
			count++;
		}
	}

	if (has_filter && count > 1) {
		qsort(entries, count, sizeof(*entries), cmp_scored_result);
	}

	for (size_t i = 0; i < count; i++) {
		struct nav_result *copy = nav_result_create();
		strncpy(copy->label, entries[i].result->label, NAV_LABEL_MAX - 1);
		strncpy(copy->value, entries[i].result->value, NAV_VALUE_MAX - 1);
		copy->action = entries[i].result->action;
		wl_list_insert(level->results.prev, &copy->link);
		string_ref_vec_add(&state->results, copy->label);
	}

	free(entries);
}

static void restore_input_from_utf8(struct view_state *state, const char *utf8, size_t length)
{
	strncpy(state->input_utf8, utf8, 4 * VIEW_MAX_INPUT - 1);
	state->input_utf8[4 * VIEW_MAX_INPUT - 1] = '\0';
	state->input_utf8_length = length;
	
	size_t j = 0;
	for (size_t i = 0; i < length && j < VIEW_MAX_INPUT - 1; ) {
		char32_t ch = 0;
		unsigned char c = (unsigned char)utf8[i];
		int len = 0;
		if ((c & 0x80) == 0) { len = 1; ch = c; }
		else if ((c & 0xE0) == 0xC0) { len = 2; ch = c & 0x1F; }
		else if ((c & 0xF0) == 0xE0) { len = 3; ch = c & 0x0F; }
		else if ((c & 0xF8) == 0xF0) { len = 4; ch = c & 0x07; }
		else { i++; continue; }
		for (int k = 1; k < len && i + k < length; k++) {
			ch = (ch << 6) | ((unsigned char)utf8[i + k] & 0x3F);
		}
		state->input_utf32[j++] = ch;
		i += len;
	}
	state->input_utf32_length = j;
	state->input_utf32[j] = U'\0';
	state->cursor_position = j;
}

static void nav_pop_and_restore(struct velo *velo)
{
	struct view_state *state = &velo->view_state;
	
	if (!velo->nav_current) {
		velo->closed = true;
		return;
	}
	
	struct nav_level *current = velo->nav_current;

	if (current->live_apply_palette && current->saved_palette[0]) {
		snprintf(velo->palette_name, sizeof(velo->palette_name), "%s", current->saved_palette);
		config_load_palette(velo);
	}

	wl_list_remove(&current->link);
	nav_level_destroy(current);
	
	if (wl_list_empty(&velo->nav_stack)) {
		velo->nav_current = NULL;
		if (velo->entry_only) {
			velo->closed = true;
			return;
		}
		snprintf(state->prompt, VIEW_MAX_PROMPT, "%s", velo->base_prompt);
		string_ref_vec_destroy(&state->results);
		if (velo->base_input_buffer[0] == '\0') {
			state->results = string_ref_vec_copy(&state->commands);
		} else {
			state->results = string_ref_vec_filter(&state->commands, velo->base_input_buffer, MATCHING_ALGORITHM_FUZZY);
		}
		state->selection = velo->base_selection;
		state->first_result = velo->base_first_result;
		state->sensitive = false;
		restore_input_from_utf8(state, velo->base_input_buffer, velo->base_input_length);
	} else {
		velo->nav_current = wl_container_of(velo->nav_stack.next, velo->nav_current, link);
		
		string_ref_vec_destroy(&state->results);
		state->results = string_ref_vec_create();
		struct nav_result *res;
		wl_list_for_each(res, &velo->nav_current->results, link) {
			string_ref_vec_add(&state->results, res->label);
		}
		
		state->selection = velo->nav_current->selection;
		state->first_result = velo->nav_current->first_result;
		state->sensitive = velo->nav_current->sensitive;
		restore_input_from_utf8(state, velo->nav_current->input_buffer, velo->nav_current->input_length);
		
		if (velo->nav_current->display_prompt[0]) {
			snprintf(state->prompt, VIEW_MAX_PROMPT, "%s", velo->nav_current->display_prompt);
		} else {
			snprintf(state->prompt, VIEW_MAX_PROMPT, "%s", velo->base_prompt);
		}
	}
	
	velo->window.surface.redraw = true;
}

/* Monotonic milliseconds, matching main.c's gettime_ms so preview timestamps
 * set here are comparable to the debounce check in the event loop. */
static uint32_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

static void update_level_input(struct nav_level *level, struct view_state *state)
{
	if (!level || (level->mode != SELECTION_INPUT && level->mode != SELECTION_PREVIEW)) {
		return;
	}

	strncpy(level->input_buffer, state->input_utf8, NAV_INPUT_MAX - 1);
	level->input_length = state->input_utf8_length;

	/* Any edit in a preview level arms the debounced re-evaluation. */
	if (level->mode == SELECTION_PREVIEW) {
		level->preview_dirty = true;
		level->preview_input_time = now_ms();
	}
}

void input_handle_keypress(struct velo *velo, xkb_keycode_t keycode)
{
	if (velo->xkb_state == NULL) {
		return;
	}

	bool ctrl = xkb_state_mod_name_is_active(
			velo->xkb_state,
			XKB_MOD_NAME_CTRL,
			XKB_STATE_MODS_EFFECTIVE);
	bool alt = xkb_state_mod_name_is_active(
			velo->xkb_state,
			XKB_MOD_NAME_ALT,
			XKB_STATE_MODS_EFFECTIVE);
	bool shift = xkb_state_mod_name_is_active(
			velo->xkb_state,
			XKB_MOD_NAME_SHIFT,
			XKB_STATE_MODS_EFFECTIVE);

	uint32_t ch = xkb_state_key_get_utf32(velo->xkb_state, keycode);

	uint32_t key = keycode - 8;

	if (utf32_isprint(ch) && !ctrl && !alt) {
		add_character(velo, keycode);
	} else if ((key == KEY_BACKSPACE || key == KEY_W) && ctrl) {
		delete_word(velo);
	} else if (key == KEY_BACKSPACE
			|| (key == KEY_H && ctrl)) {
		delete_character(velo);
	} else if (key == KEY_U && ctrl) {
		clear_input(velo);
	} else if (key == KEY_V && ctrl) {
		paste(velo);
	} else if (key == KEY_LEFT) {
		previous_cursor_or_result(velo);
	} else if (key == KEY_RIGHT) {
		next_cursor_or_result(velo);
	} else if (key == KEY_UP
			|| key == KEY_LEFT
			|| (key == KEY_TAB && shift)
			|| (key == KEY_H && alt)
			|| ((key == KEY_K || key == KEY_P || key == KEY_B) && (ctrl || alt))) {
		select_previous_result(velo);
	} else if (key == KEY_DOWN
			|| key == KEY_RIGHT
			|| key == KEY_TAB
			|| (key == KEY_L && alt)
			|| ((key == KEY_J || key == KEY_N || key == KEY_F) && (ctrl || alt))) {
		select_next_result(velo);
	} else if (key == KEY_HOME) {
		reset_selection(velo);
	} else if (key == KEY_PAGEUP) {
		select_previous_page(velo);
	} else if (key == KEY_PAGEDOWN) {
		select_next_page(velo);
	} else if (key == KEY_ESC) {
		if (velo->nav_current && (velo->nav_current->mode == SELECTION_INPUT || velo->nav_current->mode == SELECTION_PREVIEW)) {
			nav_pop_and_restore(velo);
		} else if (velo->view_state.input_utf32_length > 0) {
			clear_input(velo);
			velo->window.surface.redraw = true;
		} else if (velo->nav_current) {
			nav_pop_and_restore(velo);
		} else {
			velo->closed = true;
		}
		return;
	} else if ((key == KEY_C || key == KEY_LEFTBRACE || key == KEY_G) && ctrl) {
		velo->closed = true;
		return;
	} else if (key == KEY_ENTER
			|| key == KEY_KPENTER
			|| (key == KEY_M && ctrl)) {
		velo->submit = true;
		return;
	}

	velo->window.surface.redraw = true;
}

void reset_selection(struct velo *velo)
{
	struct view_state *state = &velo->view_state;
	state->selection = 0;
	state->first_result = 0;
	if (velo->nav_current) {
		velo->nav_current->selection = 0;
		velo->nav_current->first_result = 0;
	} else {
		velo->base_selection = 0;
		velo->base_first_result = 0;
	}
}

static bool try_teleport(struct velo *velo)
{
	struct view_state *state = &velo->view_state;
	const char *input = state->input_utf8;
	size_t input_len = state->input_utf8_length;
	
	if (input_len < 2 || input[input_len - 1] != ':') {
		return false;
	}
	
	const char *colon = &input[input_len - 1];
	if (colon == input) {
		return false;
	}
	
	size_t prefix_len = colon - input;
	char prefix[PLUGIN_NAME_MAX];
	if (prefix_len >= PLUGIN_NAME_MAX) {
		return false;
	}
	memcpy(prefix, input, prefix_len);
	prefix[prefix_len] = '\0';
	
	struct plugin *target = plugin_match_prefix(prefix);
	if (!target) {
		return false;
	}
	
	struct nav_level *level = velo->nav_current;
	
	if (level) {
		strncpy(level->input_buffer, state->input_utf8, NAV_INPUT_MAX - 1);
		level->input_buffer[NAV_INPUT_MAX - 1] = '\0';
		level->input_length = state->input_utf8_length;
		
		size_t strip_len = prefix_len + 1;
		memmove(level->input_buffer, level->input_buffer + strip_len, level->input_length - strip_len + 1);
		level->input_length -= strip_len;
		
		nav_filter_results(velo, level->input_buffer);
		reset_selection(velo);
	} else {
		strncpy(velo->base_input_buffer, state->input_utf8, 4 * VIEW_MAX_INPUT - 1);
		velo->base_input_buffer[4 * VIEW_MAX_INPUT - 1] = '\0';
		velo->base_input_length = state->input_utf8_length;
		
		size_t strip_len = prefix_len + 1;
		memmove(velo->base_input_buffer, velo->base_input_buffer + strip_len, velo->base_input_length - strip_len + 1);
		velo->base_input_length -= strip_len;
		
		string_ref_vec_destroy(&state->results);
		if (velo->base_input_buffer[0] == '\0') {
			state->results = string_ref_vec_copy(&state->commands);
		} else {
			state->results = string_ref_vec_filter(&state->commands, velo->base_input_buffer, MATCHING_ALGORITHM_FUZZY);
		}
		reset_selection(velo);
	}
	
	struct value_dict *dict = dict_create();
	navigate_to_plugin(velo, target, dict);
	log_debug("Teleported to plugin: %s\n", target->name);
	return true;
}

void add_character(struct velo *velo, xkb_keycode_t keycode)
{
	struct view_state *state = &velo->view_state;

	if (state->input_utf32_length >= N_ELEM(state->input_utf32) - 1) {
		return;
	}

	char buf[5];
	int len = xkb_state_key_get_utf8(
			velo->xkb_state,
			keycode,
			buf,
			sizeof(buf));
	if (state->cursor_position == state->input_utf32_length) {
		state->input_utf32[state->input_utf32_length] = utf8_to_utf32(buf);
		state->input_utf32_length++;
		state->input_utf32[state->input_utf32_length] = U'\0';
		memcpy(&state->input_utf8[state->input_utf8_length],
				buf,
				N_ELEM(buf));
		state->input_utf8_length += len;
		state->input_utf8[state->input_utf8_length] = '\0';

		if (try_teleport(velo)) {
			return;
		}

		struct nav_level *level = velo->nav_current;
		if (level) {
			switch (level->mode) {
		case SELECTION_INPUT:
		case SELECTION_PREVIEW:
			update_level_input(level, state);
			break;
			case SELECTION_SELECT:
			case SELECTION_PLUGIN:
				strncpy(level->input_buffer, state->input_utf8, NAV_INPUT_MAX - 1);
				level->input_buffer[NAV_INPUT_MAX - 1] = '\0';
				level->input_length = state->input_utf8_length;
				nav_filter_results(velo, state->input_utf8);
				reset_selection(velo);
				break;
			default:
				break;
			}
		} else {
			string_ref_vec_destroy(&state->results);
			if (state->input_utf8[0] == '\0') {
				state->results = string_ref_vec_copy(&state->commands);
			} else {
				state->results = string_ref_vec_filter(&state->commands, state->input_utf8, MATCHING_ALGORITHM_FUZZY);
			}
			strncpy(velo->base_input_buffer, state->input_utf8, 4 * VIEW_MAX_INPUT - 1);
			velo->base_input_buffer[4 * VIEW_MAX_INPUT - 1] = '\0';
			velo->base_input_length = state->input_utf8_length;
			reset_selection(velo);
		}
	} else {
		for (size_t i = state->input_utf32_length; i > state->cursor_position; i--) {
			state->input_utf32[i] = state->input_utf32[i - 1];
		}
		state->input_utf32[state->cursor_position] = utf8_to_utf32(buf);
		state->input_utf32_length++;
		state->input_utf32[state->input_utf32_length] = U'\0';

		input_refresh_results(velo);
	}

	state->cursor_position++;
}

void input_refresh_results(struct velo *velo)
{
	struct view_state *state = &velo->view_state;

	size_t bytes_written = 0;
	for (size_t i = 0; i < state->input_utf32_length; i++) {
		bytes_written += utf32_to_utf8(
				state->input_utf32[i],
				&state->input_utf8[bytes_written]);
	}
	state->input_utf8[bytes_written] = '\0';
	state->input_utf8_length = bytes_written;

	struct nav_level *level = velo->nav_current;
	if (level) {
		switch (level->mode) {
		case SELECTION_INPUT:
		case SELECTION_PREVIEW:
			update_level_input(level, state);
			return;
		case SELECTION_SELECT:
		case SELECTION_PLUGIN:
			strncpy(level->input_buffer, state->input_utf8, NAV_INPUT_MAX - 1);
			level->input_buffer[NAV_INPUT_MAX - 1] = '\0';
			level->input_length = state->input_utf8_length;
			nav_filter_results(velo, state->input_utf8);
			reset_selection(velo);
			return;
		default:
			break;
		}
	}

	string_ref_vec_destroy(&state->results);

	if (state->input_utf8[0] == '\0') {
		state->results = string_ref_vec_copy(&state->commands);
	} else {
		state->results = string_ref_vec_filter(&state->commands, state->input_utf8, MATCHING_ALGORITHM_FUZZY);
	}
	
	strncpy(velo->base_input_buffer, state->input_utf8, 4 * VIEW_MAX_INPUT - 1);
	velo->base_input_buffer[4 * VIEW_MAX_INPUT - 1] = '\0';
	velo->base_input_length = state->input_utf8_length;
	
	reset_selection(velo);
}

void delete_character(struct velo *velo)
{
	struct view_state *state = &velo->view_state;

	if (state->input_utf32_length == 0) {
		return;
	}

	if (state->cursor_position == 0) {
		return;
	} else if (state->cursor_position == state->input_utf32_length) {
		state->cursor_position--;
		state->input_utf32_length--;
		state->input_utf32[state->input_utf32_length] = U'\0';
	} else {
		for (size_t i = state->cursor_position - 1; i < state->input_utf32_length - 1; i++) {
			state->input_utf32[i] = state->input_utf32[i + 1];
		}
		state->cursor_position--;
		state->input_utf32_length--;
		state->input_utf32[state->input_utf32_length] = U'\0';
	}

	input_refresh_results(velo);
}

void delete_word(struct velo *velo)
{
	struct view_state *state = &velo->view_state;

	if (state->cursor_position == 0) {
		return;
	}

	uint32_t new_cursor_pos = state->cursor_position;
	while (new_cursor_pos > 0 && utf32_isspace(state->input_utf32[new_cursor_pos - 1])) {
		new_cursor_pos--;
	}
	while (new_cursor_pos > 0 && !utf32_isspace(state->input_utf32[new_cursor_pos - 1])) {
		new_cursor_pos--;
	}
	uint32_t new_length = state->input_utf32_length - (state->cursor_position - new_cursor_pos);
	for (size_t i = 0; i < new_length; i++) {
		state->input_utf32[new_cursor_pos + i] = state->input_utf32[state->cursor_position + i];
	}
	state->input_utf32_length = new_length;
	state->input_utf32[state->input_utf32_length] = U'\0';

	state->cursor_position = new_cursor_pos;
	input_refresh_results(velo);
}

void clear_input(struct velo *velo)
{
	struct view_state *state = &velo->view_state;

	state->cursor_position = 0;
	state->input_utf32_length = 0;
	state->input_utf32[0] = U'\0';

	input_refresh_results(velo);
}

void paste(struct velo *velo)
{
	if (velo->clipboard.wl_data_offer == NULL || velo->clipboard.mime_type == NULL) {
		return;
	}

	errno = 0;
	int fildes[2];
	if (pipe2(fildes, O_CLOEXEC | O_NONBLOCK) == -1) {
		log_error("Failed to open pipe for clipboard: %s\n", strerror(errno));
		return;
	}
	wl_data_offer_receive(velo->clipboard.wl_data_offer, velo->clipboard.mime_type, fildes[1]);
	close(fildes[1]);

	velo->clipboard.fd = fildes[0];
}

void select_previous_result(struct velo *velo)
{
	struct view_state *state = &velo->view_state;

	if (state->selection > 0) {
		state->selection--;
		if (velo->nav_current) {
			velo->nav_current->selection = state->selection;
		} else {
			velo->base_selection = state->selection;
		}
		return;
	}

	uint32_t nsel = MAX(MIN(state->num_results_drawn, state->results.count), 1);

	if (state->first_result > nsel) {
		state->first_result -= state->last_num_results_drawn;
		state->selection = state->last_num_results_drawn - 1;
	} else if (state->first_result > 0) {
		state->selection = state->first_result - 1;
		state->first_result = 0;
	} else if (state->results.count > 0) {
		uint32_t page_size = state->num_results_drawn;
		uint32_t remaining = state->results.count % page_size;
		uint32_t last_page_size = (remaining > 0) ? remaining : page_size;
		state->first_result = state->results.count - last_page_size;
		state->selection = last_page_size - 1;
		state->last_num_results_drawn = page_size;
	}
	
	if (velo->nav_current) {
		velo->nav_current->selection = state->selection;
		velo->nav_current->first_result = state->first_result;
	} else {
		velo->base_selection = state->selection;
		velo->base_first_result = state->first_result;
	}
}

void select_next_result(struct velo *velo)
{
	struct view_state *state = &velo->view_state;

	uint32_t nsel = MAX(MIN(state->num_results_drawn, state->results.count), 1);

	state->selection++;
	if (state->selection >= nsel) {
		state->selection -= nsel;
		if (state->results.count > 0) {
			state->first_result += nsel;
			state->first_result %= state->results.count;
		} else {
			state->first_result = 0;
		}
		state->last_num_results_drawn = state->num_results_drawn;
	}
	
	if (velo->nav_current) {
		velo->nav_current->selection = state->selection;
		velo->nav_current->first_result = state->first_result;
	} else {
		velo->base_selection = state->selection;
		velo->base_first_result = state->first_result;
	}
}

void previous_cursor_or_result(struct velo *velo)
{
	select_previous_result(velo);
}

void next_cursor_or_result(struct velo *velo)
{
	select_next_result(velo);
}

void select_previous_page(struct velo *velo)
{
	struct view_state *state = &velo->view_state;

	if (state->first_result >= state->last_num_results_drawn) {
		state->first_result -= state->last_num_results_drawn;
	} else {
		state->first_result = 0;
	}
	state->selection = 0;
	state->last_num_results_drawn = state->num_results_drawn;
	
	if (velo->nav_current) {
		velo->nav_current->selection = state->selection;
		velo->nav_current->first_result = state->first_result;
	} else {
		velo->base_selection = state->selection;
		velo->base_first_result = state->first_result;
	}
}

void select_next_page(struct velo *velo)
{
	struct view_state *state = &velo->view_state;

	state->first_result += state->num_results_drawn;
	if (state->first_result >= state->results.count) {
		state->first_result = 0;
	}
	state->selection = 0;
	state->last_num_results_drawn = state->num_results_drawn;
	
	if (velo->nav_current) {
		velo->nav_current->selection = state->selection;
		velo->nav_current->first_result = state->first_result;
	} else {
		velo->base_selection = state->selection;
		velo->base_first_result = state->first_result;
	}
}
