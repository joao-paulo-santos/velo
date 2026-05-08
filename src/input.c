#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <unistd.h>
#include "input.h"
#include "log.h"
#include "nav.h"
#include "matching.h"
#include "nelem.h"
#include "plugin.h"
#include "string_vec.h"
#include "tofi.h"
#include "unicode.h"
#include "xmalloc.h"
#include "view.h"

static void add_character(struct tofi *tofi, xkb_keycode_t keycode);
static void delete_character(struct tofi *tofi);
static void delete_word(struct tofi *tofi);
static void clear_input(struct tofi *tofi);
static void paste(struct tofi *tofi);
static void select_previous_result(struct tofi *tofi);
static void select_next_result(struct tofi *tofi);
static void select_previous_page(struct tofi *tofi);
static void select_next_page(struct tofi *tofi);
static void next_cursor_or_result(struct tofi *tofi);
static void previous_cursor_or_result(struct tofi *tofi);
static void reset_selection(struct tofi *tofi);
static void nav_filter_results(struct tofi *tofi, const char *filter);
static void nav_pop_and_restore(struct tofi *tofi);
static bool try_teleport(struct tofi *tofi);

void input_scroll_up(struct tofi *tofi)
{
	select_previous_result(tofi);
	tofi->window.surface.redraw = true;
}

void input_scroll_down(struct tofi *tofi)
{
	select_next_result(tofi);
	tofi->window.surface.redraw = true;
}

void input_select_result(struct tofi *tofi, uint32_t index)
{
	struct view_state *state = &tofi->view_state;
	if (index < state->num_results_drawn) {
		state->selection = index;
		if (tofi->nav_current) {
			tofi->nav_current->selection = index;
		} else {
			tofi->base_selection = index;
		}
		tofi->window.surface.redraw = true;
	}
}

static void nav_filter_results(struct tofi *tofi, const char *filter)
{
	struct view_state *state = &tofi->view_state;
	struct nav_level *level = tofi->nav_current;
	
	if (!level) {
		return;
	}
	
	nav_results_destroy(&level->results);
	string_ref_vec_destroy(&state->results);
	state->results = string_ref_vec_create();
	wl_list_init(&level->results);
	
	struct nav_result *res;
	wl_list_for_each(res, &level->backup_results, link) {
		int32_t score = match_words(MATCHING_ALGORITHM_FUZZY, filter, res->label);
		if (!filter || !filter[0] || score != INT32_MIN) {
			struct nav_result *copy = nav_result_create();
			strncpy(copy->label, res->label, NAV_LABEL_MAX - 1);
			strncpy(copy->value, res->value, NAV_VALUE_MAX - 1);
			copy->action = res->action;
			if (res->action.on_select) {
				copy->action.on_select = action_def_copy(res->action.on_select);
			}
			wl_list_insert(&level->results, &copy->link);
			string_ref_vec_add(&state->results, copy->label);
		}
	}
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

static void nav_pop_and_restore(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;
	
	if (!tofi->nav_current) {
		tofi->closed = true;
		return;
	}
	
	struct nav_level *current = tofi->nav_current;
	
	if (current->mode == SELECTION_FEEDBACK) {
		feedback_history_save(current);
	}
	
	wl_list_remove(&current->link);
	nav_level_destroy(current);
	
	if (wl_list_empty(&tofi->nav_stack)) {
		tofi->nav_current = NULL;
		snprintf(state->prompt, VIEW_MAX_PROMPT, "%s", tofi->base_prompt);
		string_ref_vec_destroy(&state->results);
		if (tofi->base_input_buffer[0] == '\0') {
			state->results = string_ref_vec_copy(&state->commands);
		} else {
			state->results = string_ref_vec_filter(&state->commands, tofi->base_input_buffer, MATCHING_ALGORITHM_FUZZY);
		}
		state->selection = tofi->base_selection;
		state->first_result = tofi->base_first_result;
		state->sensitive = false;
		restore_input_from_utf8(state, tofi->base_input_buffer, tofi->base_input_length);
	} else {
		tofi->nav_current = wl_container_of(tofi->nav_stack.next, tofi->nav_current, link);
		
		string_ref_vec_destroy(&state->results);
		state->results = string_ref_vec_create();
		struct nav_result *res;
		wl_list_for_each(res, &tofi->nav_current->results, link) {
			string_ref_vec_add(&state->results, res->label);
		}
		
		state->selection = tofi->nav_current->selection;
		state->first_result = tofi->nav_current->first_result;
		state->sensitive = tofi->nav_current->sensitive;
		restore_input_from_utf8(state, tofi->nav_current->input_buffer, tofi->nav_current->input_length);
		
		if (tofi->nav_current->display_prompt[0]) {
			snprintf(state->prompt, VIEW_MAX_PROMPT, "%s", tofi->nav_current->display_prompt);
		} else {
			snprintf(state->prompt, VIEW_MAX_PROMPT, "%s", tofi->base_prompt);
		}
	}
	
	tofi->window.surface.redraw = true;
}

static void update_level_input(struct nav_level *level, struct view_state *state)
{
	if (!level || (level->mode != SELECTION_INPUT && level->mode != SELECTION_FEEDBACK)) {
		return;
	}
	
	strncpy(level->input_buffer, state->input_utf8, NAV_INPUT_MAX - 1);
	level->input_length = state->input_utf8_length;
}

void input_handle_keypress(struct tofi *tofi, xkb_keycode_t keycode)
{
	if (tofi->xkb_state == NULL) {
		return;
	}

	bool ctrl = xkb_state_mod_name_is_active(
			tofi->xkb_state,
			XKB_MOD_NAME_CTRL,
			XKB_STATE_MODS_EFFECTIVE);
	bool alt = xkb_state_mod_name_is_active(
			tofi->xkb_state,
			XKB_MOD_NAME_ALT,
			XKB_STATE_MODS_EFFECTIVE);
	bool shift = xkb_state_mod_name_is_active(
			tofi->xkb_state,
			XKB_MOD_NAME_SHIFT,
			XKB_STATE_MODS_EFFECTIVE);

	uint32_t ch = xkb_state_key_get_utf32(tofi->xkb_state, keycode);

	uint32_t key = keycode - 8;

	if (utf32_isprint(ch) && !ctrl && !alt) {
		add_character(tofi, keycode);
	} else if ((key == KEY_BACKSPACE || key == KEY_W) && ctrl) {
		delete_word(tofi);
	} else if (key == KEY_BACKSPACE
			|| (key == KEY_H && ctrl)) {
		delete_character(tofi);
	} else if (key == KEY_U && ctrl) {
		clear_input(tofi);
	} else if (key == KEY_V && ctrl) {
		paste(tofi);
	} else if (key == KEY_LEFT) {
		previous_cursor_or_result(tofi);
	} else if (key == KEY_RIGHT) {
		next_cursor_or_result(tofi);
	} else if (key == KEY_UP
			|| key == KEY_LEFT
			|| (key == KEY_TAB && shift)
			|| (key == KEY_H && alt)
			|| ((key == KEY_K || key == KEY_P || key == KEY_B) && (ctrl || alt))) {
		select_previous_result(tofi);
	} else if (key == KEY_DOWN
			|| key == KEY_RIGHT
			|| key == KEY_TAB
			|| (key == KEY_L && alt)
			|| ((key == KEY_J || key == KEY_N || key == KEY_F) && (ctrl || alt))) {
		select_next_result(tofi);
	} else if (key == KEY_HOME) {
		reset_selection(tofi);
	} else if (key == KEY_PAGEUP) {
		select_previous_page(tofi);
	} else if (key == KEY_PAGEDOWN) {
		select_next_page(tofi);
	} else if (key == KEY_ESC) {
		if (tofi->nav_current && (tofi->nav_current->mode == SELECTION_INPUT || tofi->nav_current->mode == SELECTION_FEEDBACK)) {
			nav_pop_and_restore(tofi);
		} else if (tofi->view_state.input_utf32_length > 0) {
			clear_input(tofi);
			tofi->window.surface.redraw = true;
		} else if (tofi->nav_current) {
			nav_pop_and_restore(tofi);
		} else {
			tofi->closed = true;
		}
		return;
	} else if ((key == KEY_C || key == KEY_LEFTBRACE || key == KEY_G) && ctrl) {
		tofi->closed = true;
		return;
	} else if (key == KEY_ENTER
			|| key == KEY_KPENTER
			|| (key == KEY_M && ctrl)) {
		tofi->submit = true;
		return;
	}

	tofi->window.surface.redraw = true;
}

void reset_selection(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;
	state->selection = 0;
	state->first_result = 0;
	if (tofi->nav_current) {
		tofi->nav_current->selection = 0;
		tofi->nav_current->first_result = 0;
	} else {
		tofi->base_selection = 0;
		tofi->base_first_result = 0;
	}
}

static bool try_teleport(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;
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
	
	struct nav_level *level = tofi->nav_current;
	
	if (level) {
		strncpy(level->input_buffer, state->input_utf8, NAV_INPUT_MAX - 1);
		level->input_buffer[NAV_INPUT_MAX - 1] = '\0';
		level->input_length = state->input_utf8_length;
		
		size_t strip_len = prefix_len + 1;
		memmove(level->input_buffer, level->input_buffer + strip_len, level->input_length - strip_len + 1);
		level->input_length -= strip_len;
		
		nav_filter_results(tofi, level->input_buffer);
		reset_selection(tofi);
	} else {
		strncpy(tofi->base_input_buffer, state->input_utf8, 4 * VIEW_MAX_INPUT - 1);
		tofi->base_input_buffer[4 * VIEW_MAX_INPUT - 1] = '\0';
		tofi->base_input_length = state->input_utf8_length;
		
		size_t strip_len = prefix_len + 1;
		memmove(tofi->base_input_buffer, tofi->base_input_buffer + strip_len, tofi->base_input_length - strip_len + 1);
		tofi->base_input_length -= strip_len;
		
		string_ref_vec_destroy(&state->results);
		if (tofi->base_input_buffer[0] == '\0') {
			state->results = string_ref_vec_copy(&state->commands);
		} else {
			state->results = string_ref_vec_filter(&state->commands, tofi->base_input_buffer, MATCHING_ALGORITHM_FUZZY);
		}
		reset_selection(tofi);
	}
	
	struct value_dict *dict = dict_create();
	struct nav_level *new_level = nav_level_create(SELECTION_PLUGIN, dict);
	strncpy(new_level->plugin_ref, target->name, NAV_NAME_MAX - 1);
	
	plugin_populate_all(target, &new_level->results);
	nav_results_copy(&new_level->backup_results, &new_level->results);
	
	if (target->context_name[0]) {
		snprintf(new_level->display_prompt, NAV_PROMPT_MAX, "%s: ", target->context_name);
	}
	
	nav_push_level(tofi, new_level);
	update_view_state_from_level(tofi, new_level);
	
	state->input_utf32_length = 0;
	state->input_utf8_length = 0;
	state->input_utf8[0] = '\0';
	state->cursor_position = 0;
	state->selection = 0;
	state->first_result = 0;
	
	tofi->window.surface.redraw = true;
	log_debug("Teleported to plugin: %s\n", target->name);
	return true;
}

void add_character(struct tofi *tofi, xkb_keycode_t keycode)
{
	struct view_state *state = &tofi->view_state;

	if (state->input_utf32_length >= N_ELEM(state->input_utf32) - 1) {
		return;
	}

	char buf[5];
	int len = xkb_state_key_get_utf8(
			tofi->xkb_state,
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

		if (try_teleport(tofi)) {
			return;
		}

		struct nav_level *level = tofi->nav_current;
		if (level) {
			switch (level->mode) {
			case SELECTION_INPUT:
			case SELECTION_FEEDBACK:
				update_level_input(level, state);
				break;
			case SELECTION_SELECT:
			case SELECTION_PLUGIN:
				strncpy(level->input_buffer, state->input_utf8, NAV_INPUT_MAX - 1);
				level->input_buffer[NAV_INPUT_MAX - 1] = '\0';
				level->input_length = state->input_utf8_length;
				nav_filter_results(tofi, state->input_utf8);
				reset_selection(tofi);
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
			strncpy(tofi->base_input_buffer, state->input_utf8, 4 * VIEW_MAX_INPUT - 1);
			tofi->base_input_buffer[4 * VIEW_MAX_INPUT - 1] = '\0';
			tofi->base_input_length = state->input_utf8_length;
			reset_selection(tofi);
		}
	} else {
		for (size_t i = state->input_utf32_length; i > state->cursor_position; i--) {
			state->input_utf32[i] = state->input_utf32[i - 1];
		}
		state->input_utf32[state->cursor_position] = utf8_to_utf32(buf);
		state->input_utf32_length++;
		state->input_utf32[state->input_utf32_length] = U'\0';

		input_refresh_results(tofi);
	}

	state->cursor_position++;
}

void input_refresh_results(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;

	size_t bytes_written = 0;
	for (size_t i = 0; i < state->input_utf32_length; i++) {
		bytes_written += utf32_to_utf8(
				state->input_utf32[i],
				&state->input_utf8[bytes_written]);
	}
	state->input_utf8[bytes_written] = '\0';
	state->input_utf8_length = bytes_written;

	struct nav_level *level = tofi->nav_current;
	if (level) {
		switch (level->mode) {
		case SELECTION_INPUT:
		case SELECTION_FEEDBACK:
			update_level_input(level, state);
			return;
		case SELECTION_SELECT:
		case SELECTION_PLUGIN:
			strncpy(level->input_buffer, state->input_utf8, NAV_INPUT_MAX - 1);
			level->input_buffer[NAV_INPUT_MAX - 1] = '\0';
			level->input_length = state->input_utf8_length;
			nav_filter_results(tofi, state->input_utf8);
			reset_selection(tofi);
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
	
	strncpy(tofi->base_input_buffer, state->input_utf8, 4 * VIEW_MAX_INPUT - 1);
	tofi->base_input_buffer[4 * VIEW_MAX_INPUT - 1] = '\0';
	tofi->base_input_length = state->input_utf8_length;
	
	reset_selection(tofi);
}

void delete_character(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;

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

	input_refresh_results(tofi);
}

void delete_word(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;

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
	input_refresh_results(tofi);
}

void clear_input(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;

	state->cursor_position = 0;
	state->input_utf32_length = 0;
	state->input_utf32[0] = U'\0';

	input_refresh_results(tofi);
}

void paste(struct tofi *tofi)
{
	if (tofi->clipboard.wl_data_offer == NULL || tofi->clipboard.mime_type == NULL) {
		return;
	}

	errno = 0;
	int fildes[2];
	if (pipe2(fildes, O_CLOEXEC | O_NONBLOCK) == -1) {
		log_error("Failed to open pipe for clipboard: %s\n", strerror(errno));
		return;
	}
	wl_data_offer_receive(tofi->clipboard.wl_data_offer, tofi->clipboard.mime_type, fildes[1]);
	close(fildes[1]);

	tofi->clipboard.fd = fildes[0];
}

void select_previous_result(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;

	if (state->selection > 0) {
		state->selection--;
		if (tofi->nav_current) {
			tofi->nav_current->selection = state->selection;
		} else {
			tofi->base_selection = state->selection;
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
	
	if (tofi->nav_current) {
		tofi->nav_current->selection = state->selection;
		tofi->nav_current->first_result = state->first_result;
	} else {
		tofi->base_selection = state->selection;
		tofi->base_first_result = state->first_result;
	}
}

void select_next_result(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;

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
	
	if (tofi->nav_current) {
		tofi->nav_current->selection = state->selection;
		tofi->nav_current->first_result = state->first_result;
	} else {
		tofi->base_selection = state->selection;
		tofi->base_first_result = state->first_result;
	}
}

void previous_cursor_or_result(struct tofi *tofi)
{
	select_previous_result(tofi);
}

void next_cursor_or_result(struct tofi *tofi)
{
	select_next_result(tofi);
}

void select_previous_page(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;

	if (state->first_result >= state->last_num_results_drawn) {
		state->first_result -= state->last_num_results_drawn;
	} else {
		state->first_result = 0;
	}
	state->selection = 0;
	state->last_num_results_drawn = state->num_results_drawn;
	
	if (tofi->nav_current) {
		tofi->nav_current->selection = state->selection;
		tofi->nav_current->first_result = state->first_result;
	} else {
		tofi->base_selection = state->selection;
		tofi->base_first_result = state->first_result;
	}
}

void select_next_page(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;

	state->first_result += state->num_results_drawn;
	if (state->first_result >= state->results.count) {
		state->first_result = 0;
	}
	state->selection = 0;
	state->last_num_results_drawn = state->num_results_drawn;
	
	if (tofi->nav_current) {
		tofi->nav_current->selection = state->selection;
		tofi->nav_current->first_result = state->first_result;
	} else {
		tofi->base_selection = state->selection;
		tofi->base_first_result = state->first_result;
	}
}
