#ifndef TOFI_H
#define TOFI_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "clipboard.h"
#include "color.h"
#include "matching.h"
#include "surface.h"
#include "wlr-layer-shell-unstable-v1.h"
#include "fractional-scale-v1.h"
#include "nav.h"
#include "view.h"
#include "renderer.h"

#define MAX_OUTPUT_NAME_LEN 256
#define MAX_PROMPT_LENGTH 256

struct output_list_element {
	struct wl_list link;
	struct wl_output *wl_output;
	char *name;
	uint32_t width;
	uint32_t height;
	int32_t scale;
	int32_t transform;
};

struct tofi {
	struct wl_display *wl_display;
	struct wl_registry *wl_registry;
	struct wl_compositor *wl_compositor;
	struct wl_seat *wl_seat;
	struct wl_shm *wl_shm;
	struct wl_data_device_manager *wl_data_device_manager;
	struct wl_data_device *wl_data_device;
	struct wp_viewporter *wp_viewporter;
	struct wp_fractional_scale_manager_v1 *wp_fractional_scale_manager;
	struct zwlr_layer_shell_v1 *zwlr_layer_shell;
	struct wl_list output_list;
	struct output_list_element *default_output;

	struct wl_keyboard *wl_keyboard;
	struct wl_pointer *wl_pointer;

	int32_t pointer_x;
	int32_t pointer_y;

	struct xkb_state *xkb_state;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;

	bool submit;
	bool closed;
	int32_t output_width;
	int32_t output_height;
	struct clipboard clipboard;
	struct {
		struct surface surface;
		struct wp_viewport *wp_viewport;
		struct zwlr_layer_surface_v1 *zwlr_layer_surface;
		uint32_t width;
		uint32_t height;
		uint32_t scale;
		uint32_t fractional_scale;
		int32_t transform;
		int32_t margin_top;
		int32_t margin_bottom;
		int32_t margin_left;
		int32_t margin_right;
		bool width_is_percent;
		bool height_is_percent;
		bool margin_top_is_percent;
		bool margin_bottom_is_percent;
		bool margin_left_is_percent;
		bool margin_right_is_percent;
	} window;
	struct {
		uint32_t rate;
		uint32_t delay;
		uint32_t keycode;
		uint32_t next;
		bool active;
	} repeat;
	struct {
		pid_t pid;
		int fd;
		uint32_t start_time;
		bool active;
		int loading_frame;
	} feedback_process;

	struct wl_list nav_stack;
	struct nav_level *nav_current;
	struct wl_list base_results;
	struct value_dict *base_dict;
	char base_prompt[MAX_PROMPT_LENGTH];
	char base_input_buffer[4 * VIEW_MAX_INPUT];
	uint32_t base_input_length;
	uint32_t base_selection;
	uint32_t base_first_result;

	struct view_theme view_theme;
	struct view_state view_state;
	struct view_layout view_layout;
	struct renderer *renderer;

	bool entry_only;
	uint32_t anchor;
	bool use_scale;
	char target_output_name[MAX_OUTPUT_NAME_LEN];
	char theme_name[64];
};

struct plugin;

void nav_push_level(struct tofi *tofi, struct nav_level *level);
void update_view_state_from_level(struct tofi *tofi, struct nav_level *level);
bool navigate_to_plugin(struct tofi *tofi, struct plugin *target, struct value_dict *dict);

#endif
