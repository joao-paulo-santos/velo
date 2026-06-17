#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <poll.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <threads.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>
#include "tofi.h"
#include "builtin.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "plugin.h"
#include "nav.h"
#include "nelem.h"
#include "scale.h"
#include "shm.h"
#include "string_vec.h"
#include "unicode.h"
#include "viewporter.h"
#include "xmalloc.h"
#include "json.h"

#undef MAX
#undef MIN
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static const char *mime_type_text_plain = "text/plain";
static const char *mime_type_text_plain_utf8 = "text/plain;charset=utf-8";

static uint32_t gettime_ms() {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);

	uint32_t ms = t.tv_sec * 1000;
	ms += t.tv_nsec / 1000000;
	return ms;
}

static void zwlr_layer_surface_configure(
		void *data,
		struct zwlr_layer_surface_v1 *zwlr_layer_surface,
		uint32_t serial,
		uint32_t width,
		uint32_t height)
{
	struct tofi *tofi = data;
	if (width == 0 || height == 0) {
		/* Compositor is deferring to us, so don't do anything. */
		log_debug("Layer surface configure with no width or height.\n");
		return;
	}
	log_debug("Layer surface configure, %u x %u.\n", width, height);

	/*
	 * Resize the main window.
	 * We want actual pixel width / height, so we have to scale the
	 * values provided by Wayland.
	 */
	if (tofi->window.fractional_scale != 0) {
		tofi->window.surface.width = scale_apply(width, tofi->window.fractional_scale);
		tofi->window.surface.height = scale_apply(height, tofi->window.fractional_scale);
	} else {
		tofi->window.surface.width = width * tofi->window.scale;
		tofi->window.surface.height = height * tofi->window.scale;
	}

	zwlr_layer_surface_v1_ack_configure(
			tofi->window.zwlr_layer_surface,
			serial);
}

static void zwlr_layer_surface_close(
		void *data,
		struct zwlr_layer_surface_v1 *zwlr_layer_surface)
{
	struct tofi *tofi = data;
	tofi->closed = true;
	log_debug("Layer surface close.\n");
}

static const struct zwlr_layer_surface_v1_listener zwlr_layer_surface_listener = {
	.configure = zwlr_layer_surface_configure,
	.closed = zwlr_layer_surface_close
};

static void wl_keyboard_keymap(
		void *data,
		struct wl_keyboard *wl_keyboard,
		uint32_t format,
		int32_t fd,
		uint32_t size)
{
	struct tofi *tofi = data;
	assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);

	char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	assert(map_shm != MAP_FAILED);

	log_debug("Configuring keyboard.\n");
	struct xkb_keymap *xkb_keymap = xkb_keymap_new_from_string(
			tofi->xkb_context,
			map_shm,
			XKB_KEYMAP_FORMAT_TEXT_V1,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map_shm, size);
	close(fd);

	struct xkb_state *xkb_state = xkb_state_new(xkb_keymap);
	xkb_keymap_unref(tofi->xkb_keymap);
	xkb_state_unref(tofi->xkb_state);
	tofi->xkb_keymap = xkb_keymap;
	tofi->xkb_state = xkb_state;
	log_debug("Keyboard configured.\n");
}

static void wl_keyboard_enter(
		void *data,
		struct wl_keyboard *wl_keyboard,
		uint32_t serial,
		struct wl_surface *surface,
		struct wl_array *keys)
{
	/* Deliberately left blank */
}

static void wl_keyboard_leave(
		void *data,
		struct wl_keyboard *wl_keyboard,
		uint32_t serial,
		struct wl_surface *surface)
{
	/* Deliberately left blank */
}

static void wl_keyboard_key(
		void *data,
		struct wl_keyboard *wl_keyboard,
		uint32_t serial,
		uint32_t time,
		uint32_t key,
		uint32_t state)
{
	struct tofi *tofi = data;

	/*
	 * If this wasn't a keypress (i.e. was a key release), just update key
	 * repeat info and return.
	 */
	uint32_t keycode = key + 8;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (keycode == tofi->repeat.keycode) {
			tofi->repeat.active = false;
		} else {
			tofi->repeat.next = gettime_ms() + tofi->repeat.delay;
		}
		return;
	}

	/* A rate of 0 disables key repeat */
	if (xkb_keymap_key_repeats(tofi->xkb_keymap, keycode) && tofi->repeat.rate != 0) {
		tofi->repeat.active = true;
		tofi->repeat.keycode = keycode;
		tofi->repeat.next = gettime_ms() + tofi->repeat.delay;
	}
	input_handle_keypress(tofi, keycode);
}

static void wl_keyboard_modifiers(
		void *data,
		struct wl_keyboard *wl_keyboard,
		uint32_t serial,
		uint32_t mods_depressed,
		uint32_t mods_latched,
		uint32_t mods_locked,
		uint32_t group)
{
	struct tofi *tofi = data;
	if (tofi->xkb_state == NULL) {
		return;
	}
	xkb_state_update_mask(
			tofi->xkb_state,
			mods_depressed,
			mods_latched,
			mods_locked,
			0,
			0,
			group);
}

static void wl_keyboard_repeat_info(
		void *data,
		struct wl_keyboard *wl_keyboard,
		int32_t rate,
		int32_t delay)
{
	struct tofi *tofi = data;
	tofi->repeat.rate = rate;
	tofi->repeat.delay = delay;
	if (rate > 0) {
		log_debug("Key repeat every %u ms after %u ms.\n",
				1000 / rate,
				delay);
	} else {
		log_debug("Key repeat disabled.\n");
	}
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
	.keymap = wl_keyboard_keymap,
	.enter = wl_keyboard_enter,
	.leave = wl_keyboard_leave,
	.key = wl_keyboard_key,
	.modifiers = wl_keyboard_modifiers,
	.repeat_info = wl_keyboard_repeat_info,
};

static void wl_pointer_enter(
		void *data,
		struct wl_pointer *pointer,
		uint32_t serial,
		struct wl_surface *surface,
		wl_fixed_t surface_x,
		wl_fixed_t surface_y)
{
	(void)data;
	(void)surface_x;
	(void)surface_y;
}

static void wl_pointer_leave(
		void *data,
		struct wl_pointer *pointer,
		uint32_t serial,
		struct wl_surface *surface)
{
	/* Deliberately left blank */
}

static void wl_pointer_motion(
		void *data,
		struct wl_pointer *pointer,
		uint32_t time,
		wl_fixed_t surface_x,
		wl_fixed_t surface_y)
{
	struct tofi *tofi = data;
	tofi->pointer_x = wl_fixed_to_int(surface_x);
	tofi->pointer_y = wl_fixed_to_int(surface_y);
}

static void wl_pointer_button(
		void *data,
		struct wl_pointer *pointer,
		uint32_t serial,
		uint32_t time,
		uint32_t button,
		enum wl_pointer_button_state state)
{
	struct tofi *tofi = data;

	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}

	if (button != BTN_LEFT) {
		return;
	}

	if (tofi->pointer_x < 0 || tofi->pointer_y < 0 ||
	    tofi->pointer_x >= (int32_t)tofi->window.width ||
	    tofi->pointer_y >= (int32_t)tofi->window.height) {
		tofi->closed = true;
		return;
	}

	if (tofi->view_layout.result_row_height <= 0 || tofi->view_state.num_results_drawn == 0) {
		return;
	}

	int32_t rel_y = tofi->pointer_y - tofi->view_layout.result_start_y;
	if (rel_y < 0) {
		return;
	}

	uint32_t clicked_index = (uint32_t)(rel_y / tofi->view_layout.result_row_height);
	if (clicked_index >= tofi->view_state.num_results_drawn) {
		return;
	}

	if (clicked_index == tofi->view_state.selection) {
		tofi->submit = true;
	} else {
		input_select_result(tofi, clicked_index);
	}
}

static void wl_pointer_axis(
		void *data,
		struct wl_pointer *pointer,
		uint32_t time,
		enum wl_pointer_axis axis,
		wl_fixed_t value)
{
	struct tofi *tofi = data;

	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}

	double scroll = wl_fixed_to_double(value);
	if (scroll > 0) {
		input_scroll_down(tofi);
	} else if (scroll < 0) {
		input_scroll_up(tofi);
	}
}

static void wl_pointer_frame(void *data, struct wl_pointer *pointer)
{
	/* Deliberately left blank */
}

static void wl_pointer_axis_source(
		void *data,
		struct wl_pointer *pointer,
		enum wl_pointer_axis_source axis_source)
{
	/* Deliberately left blank */
}

static void wl_pointer_axis_stop(
		void *data,
		struct wl_pointer *pointer,
		uint32_t time,
		enum wl_pointer_axis axis)
{
	/* Deliberately left blank */
}

static void wl_pointer_axis_discrete(
		void *data,
		struct wl_pointer *pointer,
		enum wl_pointer_axis axis,
		int32_t discrete)
{
	/* Deliberately left blank */
}

static const struct wl_pointer_listener wl_pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
	.frame = wl_pointer_frame,
	.axis_source = wl_pointer_axis_source,
	.axis_stop = wl_pointer_axis_stop,
	.axis_discrete = wl_pointer_axis_discrete
};

static void wl_seat_capabilities(
		void *data,
		struct wl_seat *wl_seat,
		uint32_t capabilities)
{
	struct tofi *tofi = data;

	bool have_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
	bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;

	if (have_keyboard && tofi->wl_keyboard == NULL) {
		tofi->wl_keyboard = wl_seat_get_keyboard(tofi->wl_seat);
		wl_keyboard_add_listener(
				tofi->wl_keyboard,
				&wl_keyboard_listener,
				tofi);
		log_debug("Got keyboard from seat.\n");
	} else if (!have_keyboard && tofi->wl_keyboard != NULL) {
		wl_keyboard_release(tofi->wl_keyboard);
		tofi->wl_keyboard = NULL;
		log_debug("Released keyboard.\n");
	}

	if (have_pointer && tofi->wl_pointer == NULL) {
		tofi->wl_pointer = wl_seat_get_pointer(tofi->wl_seat);
		wl_pointer_add_listener(
				tofi->wl_pointer,
				&wl_pointer_listener,
				tofi);
		log_debug("Got pointer from seat.\n");
	} else if (!have_pointer && tofi->wl_pointer != NULL) {
		wl_pointer_release(tofi->wl_pointer);
		tofi->wl_pointer = NULL;
		log_debug("Released pointer.\n");
	}
}

static void wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
	/* Deliberately left blank */
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = wl_seat_capabilities,
	.name = wl_seat_name,
};

static void wl_data_offer_offer(
		void *data,
		struct wl_data_offer *wl_data_offer,
		const char *mime_type)
{
	struct clipboard *clipboard = data;

	/* Only accept plain text, and prefer utf-8. */
	if (!strcmp(mime_type, mime_type_text_plain)) {
		if (clipboard->mime_type != NULL) {
			clipboard->mime_type = mime_type_text_plain;
		}
	} else if (!strcmp(mime_type, mime_type_text_plain_utf8)) {
		clipboard->mime_type = mime_type_text_plain_utf8;
	}
}

static void wl_data_offer_source_actions(
		void *data,
		struct wl_data_offer *wl_data_offer,
		uint32_t source_actions)
{
	/* Deliberately left blank */
}

static void wl_data_offer_action(
		void *data,
		struct wl_data_offer *wl_data_offer,
		uint32_t action)
{
	/* Deliberately left blank */
}

static const struct wl_data_offer_listener wl_data_offer_listener = {
	.offer = wl_data_offer_offer,
	.source_actions = wl_data_offer_source_actions,
	.action = wl_data_offer_action
};

static void wl_data_device_data_offer(
		void *data,
		struct wl_data_device *wl_data_device,
		struct wl_data_offer *wl_data_offer)
{
	struct clipboard *clipboard = data;
	clipboard_reset(clipboard);
	clipboard->wl_data_offer = wl_data_offer;
	wl_data_offer_add_listener(
			wl_data_offer,
			&wl_data_offer_listener,
			clipboard);
}

static void wl_data_device_enter(
		void *data,
		struct wl_data_device *wl_data_device,
		uint32_t serial,
		struct wl_surface *wl_surface,
		int32_t x,
		int32_t y,
		struct wl_data_offer *wl_data_offer)
{
	/* Drag-and-drop is just ignored for now. */
	wl_data_offer_accept(
			wl_data_offer,
			serial,
			NULL);
	wl_data_offer_set_actions(
			wl_data_offer,
			WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE,
			WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
}

static void wl_data_device_leave(
		void *data,
		struct wl_data_device *wl_data_device)
{
	/* Deliberately left blank */
}

static void wl_data_device_motion(
		void *data,
		struct wl_data_device *wl_data_device,
		uint32_t time,
		int32_t x,
		int32_t y)
{
	/* Deliberately left blank */
}

static void wl_data_device_drop(
		void *data,
		struct wl_data_device *wl_data_device)
{
	/* Deliberately left blank */
}

static void wl_data_device_selection(
		void *data,
		struct wl_data_device *wl_data_device,
		struct wl_data_offer *wl_data_offer)
{
	struct clipboard *clipboard = data;
	if (wl_data_offer == NULL) {
		clipboard_reset(clipboard);
	}
}

static const struct wl_data_device_listener wl_data_device_listener = {
	.data_offer = wl_data_device_data_offer,
	.enter = wl_data_device_enter,
	.leave = wl_data_device_leave,
	.motion = wl_data_device_motion,
	.drop = wl_data_device_drop,
	.selection = wl_data_device_selection
};

static void output_geometry(
		void *data,
		struct wl_output *wl_output,
		int32_t x,
		int32_t y,
		int32_t physical_width,
		int32_t physical_height,
		int32_t subpixel,
		const char *make,
		const char *model,
		int32_t transform)
{
	struct tofi *tofi = data;
	struct output_list_element *el;
	wl_list_for_each(el, &tofi->output_list, link) {
		if (el->wl_output == wl_output) {
			el->transform = transform;
		}
	}
}

static void output_mode(
		void *data,
		struct wl_output *wl_output,
		uint32_t flags,
		int32_t width,
		int32_t height,
		int32_t refresh)
{
	struct tofi *tofi = data;
	struct output_list_element *el;
	wl_list_for_each(el, &tofi->output_list, link) {
		if (el->wl_output == wl_output) {
			if (flags & WL_OUTPUT_MODE_CURRENT) {
				el->width = width;
				el->height = height;
			}
		}
	}
}

static void output_scale(
		void *data,
		struct wl_output *wl_output,
		int32_t factor)
{
	struct tofi *tofi = data;
	struct output_list_element *el;
	wl_list_for_each(el, &tofi->output_list, link) {
		if (el->wl_output == wl_output) {
			el->scale = factor;
		}
	}
}

static void output_name(
		void *data,
		struct wl_output *wl_output,
		const char *name)
{
	struct tofi *tofi = data;
	struct output_list_element *el;
	wl_list_for_each(el, &tofi->output_list, link) {
		if (el->wl_output == wl_output) {
			el->name = xstrdup(name);
		}
	}
}

static void output_description(
		void *data,
		struct wl_output *wl_output,
		const char *description)
{
	/* Deliberately left blank */
}

static void output_done(void *data, struct wl_output *wl_output)
{
	log_debug("Output configuration done.\n");
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
#ifndef NO_WL_OUTPUT_NAME
	.name = output_name,
	.description = output_description,
#endif
};

static void registry_global(
		void *data,
		struct wl_registry *wl_registry,
		uint32_t name,
		const char *interface,
		uint32_t version)
{
	struct tofi *tofi = data;
	//log_debug("Registry %u: %s v%u.\n", name, interface, version);
	if (!strcmp(interface, wl_compositor_interface.name)) {
		tofi->wl_compositor = wl_registry_bind(
				wl_registry,
				name,
				&wl_compositor_interface,
				4);
		log_debug("Bound to compositor %u.\n", name);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		tofi->wl_seat = wl_registry_bind(
				wl_registry,
				name,
				&wl_seat_interface,
				7);
		wl_seat_add_listener(
				tofi->wl_seat,
				&wl_seat_listener,
				tofi);
		log_debug("Bound to seat %u.\n", name);
	} else if (!strcmp(interface, wl_output_interface.name)) {
		struct output_list_element *el = xmalloc(sizeof(*el));
		if (version < 4) {
			el->name = xstrdup("");
			log_warning("Using an outdated compositor, "
					"output selection will not work.\n");
		} else {
			version = 4;
		}
		el->wl_output = wl_registry_bind(
				wl_registry,
				name,
				&wl_output_interface,
				version);
		wl_output_add_listener(
				el->wl_output,
				&wl_output_listener,
				tofi);
		wl_list_insert(&tofi->output_list, &el->link);
		log_debug("Bound to output %u.\n", name);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		tofi->wl_shm = wl_registry_bind(
				wl_registry,
				name,
				&wl_shm_interface,
				1);
		log_debug("Bound to shm %u.\n", name);
	} else if (!strcmp(interface, wl_data_device_manager_interface.name)) {
		tofi->wl_data_device_manager = wl_registry_bind(
				wl_registry,
				name,
				&wl_data_device_manager_interface,
				3);
		log_debug("Bound to data device manager  %u.\n", name);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		if (version < 3) {
			log_warning("Using an outdated compositor, "
					"screen anchoring may not work.\n");
		} else {
			version = 3;
		}
		tofi->zwlr_layer_shell = wl_registry_bind(
				wl_registry,
				name,
				&zwlr_layer_shell_v1_interface,
				version);
		log_debug("Bound to zwlr_layer_shell_v1 %u.\n", name);
	} else if (!strcmp(interface, wp_viewporter_interface.name)) {
		tofi->wp_viewporter = wl_registry_bind(
				wl_registry,
				name,
				&wp_viewporter_interface,
				1);
		log_debug("Bound to wp_viewporter %u.\n", name);
	} else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
		tofi->wp_fractional_scale_manager = wl_registry_bind(
				wl_registry,
				name,
				&wp_fractional_scale_manager_v1_interface,
				1);
		log_debug("Bound to wp_fractional_scale_manager_v1 %u.\n", name);
	}
}

static void registry_global_remove(
		void *data,
		struct wl_registry *wl_registry,
		uint32_t name)
{
	/* Deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void surface_enter(
		void *data,
		struct wl_surface *wl_surface,
		struct wl_output *wl_output)
{
	log_debug("Surface entered output.\n");
}

static void surface_leave(
		void *data,
		struct wl_surface *wl_surface,
		struct wl_output *wl_output)
{
	/* Deliberately left blank */
}

static const struct wl_surface_listener wl_surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave
};

/*
 * These "dummy_*" functions are callbacks just for the dummy surface used to
 * select the default output if there's more than one.
 */
static void dummy_layer_surface_configure(
		void *data,
		struct zwlr_layer_surface_v1 *zwlr_layer_surface,
		uint32_t serial,
		uint32_t width,
		uint32_t height)
{
	zwlr_layer_surface_v1_ack_configure(
			zwlr_layer_surface,
			serial);
}

static void dummy_layer_surface_close(
		void *data,
		struct zwlr_layer_surface_v1 *zwlr_layer_surface)
{
}

static const struct zwlr_layer_surface_v1_listener dummy_layer_surface_listener = {
	.configure = dummy_layer_surface_configure,
	.closed = dummy_layer_surface_close
};

static void dummy_fractional_scale_preferred_scale(
		void *data,
		struct wp_fractional_scale_v1 *wp_fractional_scale,
		uint32_t scale)
{
	struct tofi *tofi = data;
	tofi->window.fractional_scale = scale;
}

static const struct wp_fractional_scale_v1_listener dummy_fractional_scale_listener = {
	.preferred_scale = dummy_fractional_scale_preferred_scale
};

static void dummy_surface_enter(
		void *data,
		struct wl_surface *wl_surface,
		struct wl_output *wl_output)
{
	struct tofi *tofi = data;
	struct output_list_element *el;
	wl_list_for_each(el, &tofi->output_list, link) {
		if (el->wl_output == wl_output) {
			tofi->default_output = el;
			break;
		}
	}
}

static void dummy_surface_leave(
		void *data,
		struct wl_surface *wl_surface,
		struct wl_output *wl_output)
{
	/* Deliberately left blank */
}

static const struct wl_surface_listener dummy_surface_listener = {
	.enter = dummy_surface_enter,
	.leave = dummy_surface_leave
};


static void usage(bool err)
{
	fprintf(err ? stderr : stdout, "%s",
"Usage: hypr-tofi [options]\n"
"\n"
"Options:\n"
"  -h, --help                  Print this message and exit.\n"
"  -c, --config <path>         Specify a config file.\n"
"  -f, --filter <plugins>     Filter plugins (comma-separated: all,-drun,tmux,wifi).\n"
"  -p, --plugins <list>       Show only these plugins as root menu.\n"
"  -e, --entry <plugin>       Teleport directly to a plugin's scene at startup.\n"
"  -t, --theme <name>         Set theme (overrides config's theme setting).\n"
"      --pick                 dmenu-compatible picker mode: read stdin lines, print selection to stdout.\n"
"      --input                Input mode: show prompt, print typed text to stdout on Enter.\n"
"      --sensitive            Mask input with asterisks (for use with --input).\n"
"      --font <name>           Font name.\n"
"      --font-size <px>        Font size.\n"
"      --prompt-text <string>  Prompt text.\n"
"      --width <px|%>          Width of the window.\n"
"      --height <px|%>         Height of the window.\n"
"      --output <name>         Output to display on.\n"
"      --anchor <position>     Anchor position (top, bottom, left, right, center).\n"
"      --padding <px>          Padding inside border.\n"
"      --margin-* <px|%>       Margins (top, bottom, left, right).\n"
"      --background-color      Background color (#RRGGBB or #RRGGBBAA).\n"
"      --text-color            Text color.\n"
"      --border-width <px>     Border width.\n"
"      --accent-color          Accent color (border, selection, separator).\n"
"      --corner-radius <px>    Corner radius.\n"
"\n"
"Config file: ~/.config/hypr-tofi/config\n"
"Plugins dir: ~/.config/hypr-tofi/plugins/\n"
	);
}

const struct option long_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"config", required_argument, NULL, 'c'},
	{"filter", required_argument, NULL, 'f'},
	{"plugins", required_argument, NULL, 'p'},
	{"entry", required_argument, NULL, 'e'},
	{"theme", required_argument, NULL, 't'},
	{"pick", no_argument, NULL, 'P'},
	{"input", no_argument, NULL, 'I'},
	{"sensitive", no_argument, NULL, 'S'},
	{"anchor", required_argument, NULL, 0},
	{"background-color", required_argument, NULL, 0},
	{"corner-radius", required_argument, NULL, 0},
	{"output", required_argument, NULL, 0},
	{"font", required_argument, NULL, 0},
	{"font-size", required_argument, NULL, 0},
	{"prompt-text", required_argument, NULL, 0},
	{"border-width", required_argument, NULL, 0},
	{"text-color", required_argument, NULL, 0},
	{"accent-color", required_argument, NULL, 0},
	{"width", required_argument, NULL, 0},
	{"height", required_argument, NULL, 0},
	{"margin-top", required_argument, NULL, 0},
	{"margin-bottom", required_argument, NULL, 0},
	{"margin-left", required_argument, NULL, 0},
	{"margin-right", required_argument, NULL, 0},
	{"padding", required_argument, NULL, 0},
	{NULL, 0, NULL, 0}
};
const char *short_options = ":hc:f:p:e:t:PIS";

static void parse_args(struct tofi *tofi, int argc, char *argv[], const char **entry_plugin, const char **plugin_list)
{

	bool load_default_config = true;
	const char *cli_theme = NULL;
	int option_index = 0;
	*entry_plugin = NULL;
	*plugin_list = NULL;

	/* Handle errors ourselves. */
	opterr = 0;

	/* First pass, just check for config file, help, and errors. */
	optind = 1;
	int opt = getopt_long(argc, argv, short_options, long_options, &option_index);
	while (opt != -1) {
		if (opt == 'h') {
			usage(false);
			exit(EXIT_SUCCESS);
		} else if (opt == 'c') {
			config_load(tofi, optarg);
			load_default_config = false;
		} else if (opt == 'f') {
			plugin_apply_filter(optarg);
		} else if (opt == 'p') {
			*plugin_list = optarg;
		} else if (opt == 'e') {
			*entry_plugin = optarg;
		} else if (opt == 't') {
			cli_theme = optarg;
		} else if (opt == 'P') {
			tofi->picker_mode = true;
		} else if (opt == 'I') {
			tofi->input_mode = true;
		} else if (opt == 'S') {
			tofi->view_state.sensitive = true;
		} else if (opt == ':') {
			log_error("Option %s requires an argument.\n", argv[optind - 1]);
			usage(true);
			exit(EXIT_FAILURE);
		} else if (opt == '?') {
			if (optopt) {
				log_error("Unknown option -%c.\n", optopt);
			} else {
				log_error("Unknown option %s.\n", argv[optind - 1]);
			}
			usage(true);
			exit(EXIT_FAILURE);
		}
		opt = getopt_long(argc, argv, short_options, long_options, &option_index);
	}
	if (load_default_config) {
		config_load(tofi, NULL);
	}

	if ((tofi->picker_mode || tofi->input_mode) && (*entry_plugin || *plugin_list)) {
		log_error("--pick/--input cannot be combined with -e or -p.\n");
		exit(EXIT_FAILURE);
	}
	if (tofi->picker_mode && tofi->input_mode) {
		log_error("--pick and --input cannot be combined.\n");
		exit(EXIT_FAILURE);
	}

	if (cli_theme) {
		snprintf(tofi->theme_name, N_ELEM(tofi->theme_name), "%s", cli_theme);
	}

	config_load_theme(tofi);

	/* Second pass, parse everything else. */
	optind = 1;
	opt = getopt_long(argc, argv, short_options, long_options, &option_index);
	while (opt != -1) {
		if (opt == 0) {
			if (!config_apply(tofi, long_options[option_index].name, optarg)) {
				exit(EXIT_FAILURE);
			}
		}
		opt = getopt_long(argc, argv, short_options, long_options, &option_index);
	}

	if (optind < argc) {
		log_error("Unexpected non-option argument '%s'.\n", argv[optind]);
		usage(true);
		exit(EXIT_FAILURE);
	}
}

static struct nav_result *find_nav_result(struct nav_level *level, const char *label)
{
	struct nav_result *res;
	wl_list_for_each(res, &level->results, link) {
		if (strcmp(res->label, label) == 0) {
			return res;
		}
	}
	return NULL;
}

void nav_push_level(struct tofi *tofi, struct nav_level *level)
{
	wl_list_insert(&tofi->nav_stack, &level->link);
	tofi->nav_current = level;
}

static void nav_pop_level(struct tofi *tofi)
{
	if (!tofi->nav_current) {
		return;
	}
	
	struct nav_level *current = tofi->nav_current;
	
	if (current->mode == SELECTION_FEEDBACK) {
		feedback_history_save(current);
		
		if (tofi->feedback_process.active) {
			kill(tofi->feedback_process.pid, SIGKILL);
			close(tofi->feedback_process.fd);
			tofi->feedback_process.active = false;
		}
	}
	
	wl_list_remove(&current->link);
	
	if (wl_list_empty(&tofi->nav_stack)) {
		tofi->nav_current = NULL;
	} else {
		tofi->nav_current = wl_container_of(tofi->nav_stack.next, tofi->nav_current, link);
	}
	
	nav_level_destroy(current);
}

void update_view_state_from_level(struct tofi *tofi, struct nav_level *level)
{
	string_ref_vec_destroy(&tofi->view_state.results);
	tofi->view_state.results = string_ref_vec_create();
	
	struct nav_result *res;
	wl_list_for_each(res, &level->results, link) {
		string_ref_vec_add(&tofi->view_state.results, res->label);
	}
	tofi->view_state.selection = level->selection;
	if (level->display_prompt[0]) {
		snprintf(tofi->view_state.prompt, VIEW_MAX_PROMPT, "%s", level->display_prompt);
	}
}

static void feedback_history_load(struct nav_level *level);
static void update_entry_from_feedback_level(struct tofi *tofi, struct nav_level *level);
static void execute_command(const char *template, struct value_dict *dict);

bool navigate_to_plugin(struct tofi *tofi, struct plugin *target, struct value_dict *dict)
{
	struct view_state *state = &tofi->view_state;

	switch (target->type) {
	case PLUGIN_LIST: {
		struct nav_level *new_level = nav_level_create(SELECTION_PLUGIN, dict);
		strncpy(new_level->plugin_ref, target->name, NAV_NAME_MAX - 1);
		plugin_populate_from_children(target, &new_level->results);
		nav_results_copy(&new_level->backup_results, &new_level->results);
		if (target->context_name[0]) {
			snprintf(new_level->display_prompt, NAV_PROMPT_MAX, "%s: ", target->context_name);
		}
		nav_push_level(tofi, new_level);
		update_view_state_from_level(tofi, new_level);
		break;
	}
	case PLUGIN_SELECT: {
		struct nav_level *new_level = nav_level_create(SELECTION_SELECT, dict);
		strncpy(new_level->plugin_ref, target->name, NAV_NAME_MAX - 1);
		strncpy(new_level->list_cmd, target->list_cmd, NAV_CMD_MAX - 1);
		new_level->format = target->format;
		strncpy(new_level->label_field, target->label_field, NAV_FIELD_MAX - 1);
		strncpy(new_level->value_field, target->value_field, NAV_FIELD_MAX - 1);
		strncpy(new_level->template, target->template, NAV_TEMPLATE_MAX - 1);
		strncpy(new_level->as, target->as, NAV_KEY_MAX - 1);
		new_level->execution_type = target->execution_type;
		strncpy(new_level->next_plugin, target->next, NAV_NAME_MAX - 1);
		new_level->return_to_parent = target->return_to_parent;
		plugin_run_list_cmd(target->list_cmd, target->format,
			target->label_field, target->value_field,
			target->template, target->as, &new_level->results);
		nav_results_copy(&new_level->backup_results, &new_level->results);
		if (target->context_name[0]) {
			snprintf(new_level->display_prompt, NAV_PROMPT_MAX, "%s: ", target->context_name);
		}
		nav_push_level(tofi, new_level);
		update_view_state_from_level(tofi, new_level);
		break;
	}
	case PLUGIN_INPUT: {
		struct nav_level *new_level = nav_level_create(SELECTION_INPUT, dict);
		strncpy(new_level->template, target->template, NAV_TEMPLATE_MAX - 1);
		strncpy(new_level->prompt, target->prompt, NAV_PROMPT_MAX - 1);
		strncpy(new_level->as, target->as, NAV_KEY_MAX - 1);
		new_level->execution_type = target->execution_type;
		new_level->sensitive = target->sensitive;
		strncpy(new_level->next_plugin, target->next, NAV_NAME_MAX - 1);
		new_level->return_to_parent = target->return_to_parent;
		if (target->context_name[0]) {
			snprintf(new_level->display_prompt, NAV_PROMPT_MAX, "%s: ", target->context_name);
		} else if (target->prompt[0]) {
			snprintf(new_level->display_prompt, NAV_PROMPT_MAX, "%s", target->prompt);
		}
		nav_push_level(tofi, new_level);
		update_view_state_from_level(tofi, new_level);
		state->sensitive = target->sensitive;
		break;
	}
	case PLUGIN_FEEDBACK: {
		struct nav_level *new_level = nav_level_create(SELECTION_FEEDBACK, dict);
		strncpy(new_level->eval_cmd, target->eval_cmd, NAV_CMD_MAX - 1);
		strncpy(new_level->display_input, target->display_input, NAV_TEMPLATE_MAX - 1);
		strncpy(new_level->display_result, target->display_result, NAV_TEMPLATE_MAX - 1);
		new_level->show_input = target->show_input;
		new_level->history_limit = target->history_limit;
		new_level->persist_history = target->persist_history;
		if (target->history_name[0]) {
			strncpy(new_level->history_name, target->history_name, NAV_NAME_MAX - 1);
		} else {
			strncpy(new_level->history_name, target->name, NAV_NAME_MAX - 1);
		}
		if (target->context_name[0]) {
			snprintf(new_level->display_prompt, NAV_PROMPT_MAX, "%s: ", target->context_name);
		}
		wl_list_init(&new_level->results);
		feedback_history_load(new_level);
		nav_push_level(tofi, new_level);
		update_entry_from_feedback_level(tofi, new_level);
		break;
	}
	case PLUGIN_EXEC: {
		execute_command(target->template, dict);
		dict_destroy(dict);
		tofi->closed = true;
		return true;
	}
	}

	state->input_utf32[0] = U'\0';
	state->input_utf32_length = 0;
	state->input_utf8[0] = '\0';
	state->input_utf8_length = 0;
	state->cursor_position = 0;
	state->selection = 0;
	state->first_result = 0;
	tofi->window.surface.redraw = true;
	return false;
}

#define FEEDBACK_HISTORY_DIR "/.config/hypr-tofi/history/"

static void feedback_history_path(char *buf, size_t size, const char *name)
{
	const char *home = getenv("HOME");
	snprintf(buf, size, "%s" FEEDBACK_HISTORY_DIR "%s.json", home ? home : "/tmp", name);
}

static void feedback_history_load(struct nav_level *level)
{
	if (!level->history_name[0] || !level->persist_history) {
		return;
	}
	
	char path[512];
	feedback_history_path(path, sizeof(path), level->history_name);
	
	FILE *fp = fopen(path, "r");
	if (!fp) {
		return;
	}
	
	fseek(fp, 0, SEEK_END);
	long file_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	
	char *json = xcalloc(1, file_size + 1);
	if (fread(json, 1, file_size, fp) != (size_t)file_size) {
		free(json);
		fclose(fp);
		return;
	}
	fclose(fp);
	
	json_parser_t parser;
	json_parser_init(&parser, json);
	
	if (!json_object_begin(&parser)) {
		free(json);
		return;
	}
	
	char key[64];
	bool has_more;
	
	while (json_object_next(&parser, key, sizeof(key), &has_more) && has_more) {
		if (strcmp(key, "entries") == 0) {
			if (!json_array_begin(&parser)) break;
			
			bool arr_has_more;
			while (json_array_next(&parser, &arr_has_more) && arr_has_more) {
				if (!json_object_begin(&parser)) break;
				
				bool is_user = false;
				char content[NAV_VALUE_MAX] = "";
				bool has_is_user = false;
				bool has_content = false;
				
				char obj_key[64];
				bool obj_has_more;
				while (json_object_next(&parser, obj_key, sizeof(obj_key), &obj_has_more) && obj_has_more) {
					if (strcmp(obj_key, "is_user") == 0) {
						if (json_parse_bool(&parser, &is_user)) {
							has_is_user = true;
						}
					} else if (strcmp(obj_key, "content") == 0) {
						if (json_parse_string(&parser, content, sizeof(content))) {
							has_content = true;
						}
					} else {
						json_skip_value(&parser);
					}
					
					if (json_peek_char(&parser, ',')) {
						json_expect_char(&parser, ',');
					}
				}
				
				json_object_end(&parser);
				
				if (has_is_user && has_content) {
					struct feedback_entry *entry = feedback_entry_create();
					entry->is_user = is_user;
					strncpy(entry->content, content, NAV_VALUE_MAX - 1);
					wl_list_insert(&level->results, &entry->link);
				}
				
				if (json_peek_char(&parser, ',')) {
					json_expect_char(&parser, ',');
				}
			}
			
			json_array_end(&parser);
		} else {
			json_skip_value(&parser);
		}
		
		if (json_peek_char(&parser, ',')) {
			json_expect_char(&parser, ',');
		}
	}
	
	free(json);
}

void feedback_history_save(struct nav_level *level)
{
	if (!level->history_name[0] || !level->persist_history) {
		return;
	}
	
	int total = 0;
	struct feedback_entry *e;
	wl_list_for_each(e, &level->results, link) {
		total++;
	}
	
	char path[512];
	feedback_history_path(path, sizeof(path), level->history_name);
	
	char dir_path[512];
	const char *home = getenv("HOME");
	snprintf(dir_path, sizeof(dir_path), "%s" FEEDBACK_HISTORY_DIR, home ? home : "/tmp");
	
	char *mkdir_cmd = NULL;
	if (asprintf(&mkdir_cmd, "mkdir -p '%s'", dir_path) >= 0 && mkdir_cmd) {
		int ret = system(mkdir_cmd);
		(void)ret;
		free(mkdir_cmd);
	}
	
	FILE *fp = fopen(path, "w");
	if (!fp) {
		log_error("Failed to open history file for writing: %s\n", path);
		return;
	}
	
	fprintf(fp, "{\n  \"entries\": [\n");
	
	struct feedback_entry *entry;
	int to_write = (total > level->history_limit) ? level->history_limit : total;
	int written = 0;
	
	wl_list_for_each_reverse(entry, &level->results, link) {
		if (written >= to_write) break;
		
		char escaped[NAV_VALUE_MAX * 2];
		json_escape_string(entry->content, escaped, sizeof(escaped));
		fprintf(fp, "    {\"is_user\": %s, \"content\": %s}", 
		        entry->is_user ? "true" : "false", escaped);
		
		written++;
		if (written < to_write) {
			fprintf(fp, ",");
		}
		fprintf(fp, "\n");
	}
	
	fprintf(fp, "  ]\n}\n");
	fclose(fp);
}

static void update_entry_from_feedback_level(struct tofi *tofi, struct nav_level *level)
{
	string_ref_vec_destroy(&tofi->view_state.results);
	tofi->view_state.results = string_ref_vec_create();
	
	struct feedback_entry *fe;
	wl_list_for_each(fe, &level->results, link) {
		string_ref_vec_add(&tofi->view_state.results, fe->content);
	}
	tofi->view_state.selection = 0;
	tofi->view_state.first_result = 0;
	if (level->display_prompt[0]) {
		snprintf(tofi->view_state.prompt, VIEW_MAX_PROMPT, "%s", level->display_prompt);
	}
}

static void feedback_spawn_process(struct tofi *tofi, struct nav_level *level)
{
	if (tofi->feedback_process.active) {
		return;
	}
	
	if (!level->input_buffer[0]) {
		return;
	}
	
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		log_error("Failed to create pipe for feedback process\n");
		return;
	}
	
	struct value_dict *dict = dict_copy(level->dict);
	dict_set(&dict, "input", level->input_buffer);
	char *cmd = template_resolve(level->eval_cmd, dict);
	dict_destroy(dict);
	
	if (!cmd) {
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}
	
	char *argv[] = {"sh", "-c", cmd, NULL};
	char *envp[] = {NULL};
	
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, pipefd[0]);
	posix_spawn_file_actions_addclose(&actions, pipefd[1]);
	
	pid_t pid;
	int spawn_result = posix_spawnp(&pid, "sh", &actions, NULL, argv, envp);
	
	posix_spawn_file_actions_destroy(&actions);
	close(pipefd[1]);
	free(cmd);
	
	if (spawn_result != 0) {
		log_error("Failed to spawn feedback process\n");
		close(pipefd[0]);
		return;
	}
	
	tofi->feedback_process.pid = pid;
	tofi->feedback_process.fd = pipefd[0];
	tofi->feedback_process.start_time = gettime_ms();
	tofi->feedback_process.active = true;
	tofi->feedback_process.loading_frame = 0;
	level->feedback_loading = true;
	
	if (level->show_input && level->display_input[0]) {
		struct value_dict *input_dict = dict_copy(level->dict);
		dict_set(&input_dict, "input", level->input_buffer);
		char *formatted = template_resolve(level->display_input, input_dict);
		dict_destroy(input_dict);
		
		if (formatted) {
			struct feedback_entry *user_entry = feedback_entry_create();
			user_entry->is_user = true;
			strncpy(user_entry->content, formatted, NAV_VALUE_MAX - 1);
			wl_list_insert(&level->results, &user_entry->link);
			free(formatted);
		}
	}
	
	struct feedback_entry *loading_entry = feedback_entry_create();
	loading_entry->is_user = false;
	strcpy(loading_entry->content, ".");
	wl_list_insert(&level->results, &loading_entry->link);
	
	level->input_buffer[0] = '\0';
	level->input_length = 0;
	
	tofi->view_state.input_utf32_length = 0;
	tofi->view_state.input_utf8_length = 0;
	tofi->view_state.input_utf8[0] = '\0';
	tofi->view_state.cursor_position = 0;
	
	string_ref_vec_destroy(&tofi->view_state.results);
	tofi->view_state.results = string_ref_vec_create();
	struct feedback_entry *fe;
	wl_list_for_each(fe, &level->results, link) {
		string_ref_vec_add(&tofi->view_state.results, fe->content);
	}
	tofi->window.surface.redraw = true;
}

#define FEEDBACK_TIMEOUT_MS (3 * 60 * 1000)
#define FEEDBACK_BUFFER_SIZE 4096

static bool is_loading_indicator(const char *content)
{
	return (strcmp(content, ".") == 0 || 
	        strcmp(content, "..") == 0 || 
	        strcmp(content, "...") == 0);
}

static void feedback_process_complete(struct tofi *tofi)
{
	struct nav_level *level = tofi->nav_current;
	if (!level || level->mode != SELECTION_FEEDBACK) {
		tofi->feedback_process.active = false;
		return;
	}
	
	level->feedback_loading = false;
	
	if (!wl_list_empty(&level->results)) {
		struct feedback_entry *first = wl_container_of(level->results.next, first, link);
		if (is_loading_indicator(first->content)) {
			wl_list_remove(&first->link);
			feedback_entry_destroy(first);
		}
	}
	
	ssize_t total = 0;
	char result[FEEDBACK_BUFFER_SIZE];
	result[0] = '\0';
	
	while (total < (ssize_t)sizeof(result) - 1) {
		ssize_t bytes = read(tofi->feedback_process.fd, 
		                     result + total, 
		                     sizeof(result) - 1 - total);
		if (bytes <= 0) break;
		total += bytes;
	}
	result[total] = '\0';
	
	close(tofi->feedback_process.fd);
	
	int status;
	waitpid(tofi->feedback_process.pid, &status, 0);
	
	tofi->feedback_process.active = false;
	level->feedback_loading = false;
	
	while (total > 0 && (result[total-1] == '\n' || result[total-1] == '\r')) {
		result[--total] = '\0';
	}
	
	if (total > 0 && level->display_result[0]) {
		struct value_dict *dict = dict_copy(level->dict);
		dict_set(&dict, "input", level->input_buffer);
		dict_set(&dict, "result", result);
		char *formatted = template_resolve(level->display_result, dict);
		dict_destroy(dict);
		
		if (formatted) {
			struct feedback_entry *result_entry = feedback_entry_create();
			result_entry->is_user = false;
			strncpy(result_entry->content, formatted, NAV_VALUE_MAX - 1);
			wl_list_insert(&level->results, &result_entry->link);
			free(formatted);
		}
	} else if (total > 0) {
		struct feedback_entry *result_entry = feedback_entry_create();
		result_entry->is_user = false;
		strncpy(result_entry->content, result, NAV_VALUE_MAX - 1);
		wl_list_insert(&level->results, &result_entry->link);
	} else {
		struct feedback_entry *error_entry = feedback_entry_create();
		error_entry->is_user = false;
		strncpy(error_entry->content, "Error: no output", NAV_VALUE_MAX - 1);
		wl_list_insert(&level->results, &error_entry->link);
	}
	
	while (wl_list_length(&level->results) > (int)level->history_limit) {
		struct feedback_entry *last = wl_container_of(level->results.prev, last, link);
		wl_list_remove(&last->link);
		feedback_entry_destroy(last);
	}
	
	update_entry_from_feedback_level(tofi, level);
	tofi->window.surface.redraw = true;
}

static void feedback_process_check_timeout(struct tofi *tofi)
{
	if (!tofi->feedback_process.active) {
		return;
	}
	
	uint32_t elapsed = gettime_ms() - tofi->feedback_process.start_time;
	if (elapsed >= FEEDBACK_TIMEOUT_MS) {
		log_error("Feedback process timeout, killing\n");
		kill(tofi->feedback_process.pid, SIGKILL);
		close(tofi->feedback_process.fd);
		
		struct nav_level *level = tofi->nav_current;
		if (level && level->mode == SELECTION_FEEDBACK) {
			level->feedback_loading = false;
			
			if (!wl_list_empty(&level->results)) {
				struct feedback_entry *first = wl_container_of(level->results.next, first, link);
				if (is_loading_indicator(first->content)) {
					wl_list_remove(&first->link);
					feedback_entry_destroy(first);
				}
			}
			
			struct feedback_entry *error_entry = feedback_entry_create();
			error_entry->is_user = false;
			strncpy(error_entry->content, "Error: timeout", NAV_VALUE_MAX - 1);
			wl_list_insert(&level->results, &error_entry->link);
			
			update_entry_from_feedback_level(tofi, level);
			tofi->window.surface.redraw = true;
		}
		
		tofi->feedback_process.active = false;
	}
}

static void feedback_update_loading_animation(struct tofi *tofi)
{
	if (!tofi->feedback_process.active) {
		return;
	}
	
	struct nav_level *level = tofi->nav_current;
	if (!level || level->mode != SELECTION_FEEDBACK) {
		return;
	}
	
	if (wl_list_empty(&level->results)) {
		return;
	}
	
	struct feedback_entry *first = wl_container_of(level->results.next, first, link);
	if (!is_loading_indicator(first->content)) {
		return;
	}
	
	tofi->feedback_process.loading_frame = (tofi->feedback_process.loading_frame + 1) % 3;
	const char *frames[] = {".", "..", "..."};
	strcpy(first->content, frames[tofi->feedback_process.loading_frame]);
	
	string_ref_vec_destroy(&tofi->view_state.results);
	tofi->view_state.results = string_ref_vec_create();
	struct feedback_entry *fe;
	wl_list_for_each(fe, &level->results, link) {
		string_ref_vec_add(&tofi->view_state.results, fe->content);
	}
	tofi->window.surface.redraw = true;
}

static void execute_command(const char *template, struct value_dict *dict)
{
	char *cmd = template_resolve(template, dict);
	if (!cmd) {
		log_error("Failed to resolve template\n");
		return;
	}
	
	log_debug("Executing: %s\n", cmd);
	
	if (builtin_is_builtin(cmd)) {
		builtin_execute(cmd, dict);
		free(cmd);
		return;
	}
	
	char *resolved = plugin_resolve_command(cmd);
	free(cmd);
	int ret = system(resolved);
	if (ret != 0) {
		log_error("Command failed: %d\n", ret);
	}
	free(resolved);
}

static bool do_submit(struct tofi *tofi)
{
	struct nav_level *level = tofi->nav_current;

	if (level && level->mode == SELECTION_INPUT) {
		if (tofi->input_mode) {
			snprintf(tofi->pipe_output, N_ELEM(tofi->pipe_output), "%s", level->input_buffer);
			return true;
		}
		struct value_dict *dict = dict_copy(level->dict);
		dict_set(&dict, level->as, level->input_buffer);

		if (level->next_plugin[0]) {
			struct plugin *next_p = plugin_get(level->next_plugin);
			if (next_p) return navigate_to_plugin(tofi, next_p, dict);
		}

		if (level->return_to_parent) {
			nav_pop_level(tofi);
			if (tofi->nav_current) {
				struct nav_level *parent = tofi->nav_current;
				dict_destroy(parent->dict);
				parent->dict = dict;

				if (parent->execution_type == EXECUTION_EXEC) {
					execute_command(parent->template, parent->dict);
					return true;
				}

				update_view_state_from_level(tofi, parent);
				tofi->view_state.input_utf32_length = 0;
				tofi->view_state.input_utf8_length = 0;
				tofi->view_state.input_utf8[0] = '\0';
				tofi->view_state.cursor_position = 0;
				tofi->window.surface.redraw = true;
			}
			return false;
		}

		execute_command(level->template, dict);
		dict_destroy(dict);
		return true;
	}

	if (level && level->mode == SELECTION_FEEDBACK) {
		if (!level->input_buffer[0]) {
			return false;
		}
		feedback_spawn_process(tofi, level);
		return false;
	}

	uint32_t selection = tofi->view_state.selection + tofi->view_state.first_result;

	if (tofi->view_state.results.count == 0) {
		return false;
	}

	char *res = tofi->view_state.results.buf[selection].string;

	struct nav_result *nav_res = NULL;
	if (level) {
		nav_res = find_nav_result(level, res);
	}

	if (!level && !wl_list_empty(&tofi->base_results)) {
		struct nav_result *r;
		wl_list_for_each(r, &tofi->base_results, link) {
			if (strcmp(res, r->label) == 0) {
				nav_res = r;
				break;
			}
		}
	}

	if (nav_res) {
		struct action_def *action = &nav_res->action;
		struct value_dict *dict = level ? dict_copy(level->dict) : dict_create();

		switch (action->selection_type) {
		case SELECTION_SELF:
			if (tofi->picker_mode) {
				snprintf(tofi->pipe_output, N_ELEM(tofi->pipe_output), "%s", nav_res->value);
				dict_destroy(dict);
				return true;
			}
			if (action->as[0]) {
				dict_set(&dict, action->as, nav_res->value);
			}

			if (level && level->next_plugin[0]) {
				struct plugin *next_p = plugin_get(level->next_plugin);
				if (next_p) return navigate_to_plugin(tofi, next_p, dict);
			}

			if (level && level->return_to_parent) {
				nav_pop_level(tofi);
				if (tofi->nav_current) {
					struct nav_level *parent = tofi->nav_current;
					dict_destroy(parent->dict);
					parent->dict = dict;

					if (parent->execution_type == EXECUTION_EXEC) {
						execute_command(parent->template, parent->dict);
						return true;
					}

					update_view_state_from_level(tofi, parent);
					tofi->view_state.input_utf32_length = 0;
					tofi->view_state.input_utf8_length = 0;
					tofi->view_state.input_utf8[0] = '\0';
					tofi->view_state.cursor_position = 0;
					tofi->window.surface.redraw = true;
				}
				return false;
			}

			execute_command(action->template, dict);
			dict_destroy(dict);
			return true;

		case SELECTION_PLUGIN: {
			struct plugin *target_plugin = plugin_get(action->plugin_ref);
			if (!target_plugin) {
				log_error("Plugin not found: %s\n", action->plugin_ref);
				dict_destroy(dict);
				return false;
			}
			return navigate_to_plugin(tofi, target_plugin, dict);
		}

		default:
			dict_destroy(dict);
			break;
		}
	}

	return false;
}

static void read_clipboard(struct tofi *tofi)
{
	struct view_state *state = &tofi->view_state;

	/* Make a copy of any text after the cursor. */
	uint32_t *end_text = NULL;
	size_t end_text_length = state->input_utf32_length - state->cursor_position;
	if (end_text_length > 0) {
		end_text = xcalloc(end_text_length, sizeof(*state->input_utf32));
		memcpy(end_text,
				&state->input_utf32[state->cursor_position],
				end_text_length * sizeof(*state->input_utf32));
	}
	/* Buffer for 4 UTF-8 bytes plus a null terminator. */
	char buffer[5];
	memset(buffer, 0, N_ELEM(buffer));
	errno = 0;
	bool eof = false;
	while (state->cursor_position < N_ELEM(state->input_utf32)) {
		for (size_t i = 0; i < 4; i++) {
			/*
			 * Read input 1 byte at a time. This is slow, but easy,
			 * and speed of pasting shouldn't really matter.
			 */
			int res = read(tofi->clipboard.fd, &buffer[i], 1);
			if (res == 0) {
				eof = true;
				break;
			} else if (res == -1) {
				if (errno == EAGAIN) {
					/*
					 * There was no more data to be read,
					 * but EOF hasn't been reached yet.
					 *
					 * This could mean more than a pipe's
					 * capacity (64k) of data was sent, in
					 * which case we'd potentially skip
					 * a character, but we should hit the
					 * input length limit long before that.
					 */
					input_refresh_results(tofi);
					tofi->window.surface.redraw = true;
					return;
				}
				log_error("Failed to read clipboard: %s\n", strerror(errno));
				clipboard_finish_paste(&tofi->clipboard);
				return;
			}
			uint32_t unichar = utf8_to_utf32_validate(buffer);
			if (unichar == (uint32_t)-2) {
				/* The current character isn't complete yet. */
				continue;
			} else if (unichar == (uint32_t)-1) {
				log_error("Invalid UTF-8 character in clipboard: %s\n", buffer);
				break;
			} else {
				state->input_utf32[state->cursor_position] = unichar;
				state->cursor_position++;
				break;
			}
		}
		memset(buffer, 0, N_ELEM(buffer));
		if (eof) {
			break;
		}
	}
	state->input_utf32_length = state->cursor_position;

	/* If there was any text after the cursor, re-insert it now. */
	if (end_text != NULL) {
		for (size_t i = 0; i < end_text_length; i++) {
			if (state->input_utf32_length == N_ELEM(state->input_utf32)) {
				break;
			}
			state->input_utf32[state->input_utf32_length] = end_text[i];
			state->input_utf32_length++;
		}
		free(end_text);
	}
	state->input_utf32[MIN(state->input_utf32_length, N_ELEM(state->input_utf32) - 1)] = U'\0';

	clipboard_finish_paste(&tofi->clipboard);

	input_refresh_results(tofi);
	tofi->window.surface.redraw = true;
}

int main(int argc, char *argv[])
{
	/* Call log_debug to initialise the timers we use for perf checking. */
	log_debug("This is tofi.\n");

	/*
	 * Set the locale to the user's default, so we can deal with non-ASCII
	 * characters.
	 */
	setlocale(LC_ALL, "");

	/* Default options. */
	struct tofi tofi = {
		.window = {
			.scale = 1,
			.width = 1280,
			.height = 720,
		},
		.view_theme = {
			.font_name = "Sans",
			.font_size = 24,
			.padding_top = 8,
			.padding_bottom = 8,
			.padding_left = 8,
			.padding_right = 8,
			.border_width = 12,
			.background_color = {0.106f, 0.114f, 0.118f, 1.0f},
			.foreground_color = {1.0f, 1.0f, 1.0f, 1.0f},
			.accent_color = {0.976f, 0.149f, 0.447f, 1.0f},
		},
		.view_state = {
			.prompt = "run: ",
		},
		.anchor =  ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
		.use_scale = true,
	};
	wl_list_init(&tofi.output_list);
	wl_list_init(&tofi.nav_stack);
	tofi.nav_current = NULL;
	tofi.base_dict = dict_create();
	wl_list_init(&tofi.base_results);
	
	plugin_init();
	const char *home = getenv("HOME");
	if (home) {
		char plugin_dir[512];
		snprintf(plugin_dir, sizeof(plugin_dir), "%s/.config/hypr-tofi/plugins", home);
		log_debug("Loading plugins from: %s\n", plugin_dir);
		plugin_load_directory(plugin_dir);
	}
	log_debug("Loaded %zu plugins.\n", plugin_count());
	
	const char *entry_plugin = NULL;
	const char *plugin_list = NULL;
	parse_args(&tofi, argc, argv, &entry_plugin, &plugin_list);
	log_debug("Config done.\n");

	/*
	 * Initial Wayland & XKB setup.
	 * The first thing to do is connect a listener to the global registry,
	 * so that we can bind to the various global objects and start talking
	 * to Wayland.
	 */
	
	snprintf(tofi.base_prompt, MAX_PROMPT_LENGTH, "%s", tofi.view_state.prompt);

	log_debug("Connecting to Wayland display.\n");
	tofi.wl_display = wl_display_connect(NULL);
	if (tofi.wl_display == NULL) {
		log_error("Couldn't connect to Wayland display.\n");
		exit(EXIT_FAILURE);
	}
	tofi.wl_registry = wl_display_get_registry(tofi.wl_display);
	log_debug("Creating xkb context.\n");
	tofi.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (tofi.xkb_context == NULL) {
		log_error("Couldn't create an XKB context.\n");
		exit(EXIT_FAILURE);
	}
	wl_registry_add_listener(
			tofi.wl_registry,
			&wl_registry_listener,
			&tofi);

	/*
	 * After this first roundtrip, the only thing that should have happened
	 * is our registry_global() function being called and setting up the
	 * various global object bindings.
	 */
	log_debug("First roundtrip start.\n");
	log_indent();
	wl_display_roundtrip(tofi.wl_display);
	log_unindent();
	log_debug("First roundtrip done.\n");

	/*
	 * The next roundtrip causes the listeners we set up in
	 * registry_global() to be called. Notably, the output should be
	 * configured, telling us the scale factor and size.
	 */
	log_debug("Second roundtrip start.\n");
	log_indent();
	wl_display_roundtrip(tofi.wl_display);
	log_unindent();
	log_debug("Second roundtrip done.\n");

	{
		/*
		 * Determine the output we're going to appear on, and get its
		 * fractional scale if supported.
		 *
		 * This seems like an ugly solution, but as far as I know
		 * there's no way to determine the default output other than to
		 * call get_layer_surface with NULL as the output and see which
		 * output our surface turns up on.
		 *
		 * Additionally, determining fractional scale factors can
		 * currently only be done by attaching a wp_fractional_scale to
		 * a surface and displaying it.
		 *
		 * Here we set up a single pixel surface, perform the required
		 * two roundtrips, then tear it down. tofi.default_output
		 * should then contain the output our surface was assigned to,
		 * and tofi.window.fractional_scale should have the scale
		 * factor.
		 */
		log_debug("Determining output.\n");
		log_indent();
		struct surface surface = {
			.width = 1,
			.height = 1
		};
		surface.wl_surface =
			wl_compositor_create_surface(tofi.wl_compositor);
		wl_surface_add_listener(
				surface.wl_surface,
				&dummy_surface_listener,
				&tofi);

		struct wp_fractional_scale_v1 *wp_fractional_scale = NULL;
		if (tofi.wp_fractional_scale_manager != NULL) {
			wp_fractional_scale =
				wp_fractional_scale_manager_v1_get_fractional_scale(
						tofi.wp_fractional_scale_manager,
						surface.wl_surface);
			wp_fractional_scale_v1_add_listener(
					wp_fractional_scale,
					&dummy_fractional_scale_listener,
					&tofi);
		}

		/*
		 * If we have a desired output, make sure we appear on it so we
		 * can determine the correct fractional scale.
		 */
		struct wl_output *wl_output = NULL;
		if (tofi.target_output_name[0] != '\0') {
			struct output_list_element *el;
			wl_list_for_each(el, &tofi.output_list, link) {
				if (!strcmp(tofi.target_output_name, el->name)) {
					wl_output = el->wl_output;
					break;
				}
			}
		}

		struct zwlr_layer_surface_v1 *zwlr_layer_surface =
			zwlr_layer_shell_v1_get_layer_surface(
					tofi.zwlr_layer_shell,
					surface.wl_surface,
					wl_output,
					ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
					"dummy");
		/*
		 * Workaround for Hyprland, where if this is not set the dummy
		 * surface never enters an output for some reason.
		 */
		zwlr_layer_surface_v1_set_keyboard_interactivity(
				zwlr_layer_surface,
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
				);
		zwlr_layer_surface_v1_add_listener(
				zwlr_layer_surface,
				&dummy_layer_surface_listener,
				&tofi);
		zwlr_layer_surface_v1_set_size(
				zwlr_layer_surface,
				1,
				1);
		wl_surface_commit(surface.wl_surface);
		log_debug("First dummy roundtrip start.\n");
		log_indent();
		wl_display_roundtrip(tofi.wl_display);
		log_unindent();
		log_debug("First dummy roundtrip done.\n");
		log_debug("Initialising dummy surface.\n");
		log_indent();
		surface_init(&surface, tofi.wl_shm);
		surface_draw(&surface);
		log_unindent();
		log_debug("Dummy surface initialised.\n");
		log_debug("Second dummy roundtrip start.\n");
		log_indent();
		wl_display_roundtrip(tofi.wl_display);
		log_unindent();
		log_debug("Second dummy roundtrip done.\n");
		surface_destroy(&surface);
		zwlr_layer_surface_v1_destroy(zwlr_layer_surface);
		if (wp_fractional_scale != NULL) {
			wp_fractional_scale_v1_destroy(wp_fractional_scale);
		}
		wl_surface_destroy(surface.wl_surface);

		/*
		 * Walk through our output list and select the one we want if
		 * the user's asked for a specific one, otherwise just get the
		 * default one.
		 */
		bool found_target = false;
		struct output_list_element *head;
		head = wl_container_of(tofi.output_list.next, head, link);

		struct output_list_element *el;
		struct output_list_element *tmp;
		if (tofi.target_output_name[0] != 0) {
			log_debug("Looking for output %s.\n", tofi.target_output_name);
		} else if (tofi.default_output != NULL) {
			snprintf(
					tofi.target_output_name,
					N_ELEM(tofi.target_output_name),
					"%s",
					tofi.default_output->name);
			/* We don't need this anymore. */
			tofi.default_output = NULL;
		}
		wl_list_for_each_reverse_safe(el, tmp, &tofi.output_list, link) {
			if (!strcmp(tofi.target_output_name, el->name)) {
				found_target = true;
				continue;
			}
			/*
			 * If we've already found the output we're looking for
			 * or this isn't the first output in the list, remove
			 * it.
			 */
			if (found_target || el != head) {
				wl_list_remove(&el->link);
				wl_output_release(el->wl_output);
				free(el->name);
				free(el);
			}
		}

		/*
		 * The only output left should either be the one we want, or
		 * the first that was advertised.
		 */
		el = wl_container_of(tofi.output_list.next, el, link);

		/*
		 * If we're rotated 90 degrees, we need to swap width and
		 * height to calculate percentages.
		 */
		switch (el->transform) {
			case WL_OUTPUT_TRANSFORM_90:
			case WL_OUTPUT_TRANSFORM_270:
			case WL_OUTPUT_TRANSFORM_FLIPPED_90:
			case WL_OUTPUT_TRANSFORM_FLIPPED_270:
				tofi.output_width = el->height;
				tofi.output_height = el->width;
				break;
			default:
				tofi.output_width = el->width;
				tofi.output_height = el->height;
		}
		tofi.window.scale = el->scale;
		tofi.window.transform = el->transform;
		log_unindent();
		log_debug("Selected output %s.\n", el->name);
	}

	/*
	 * We can now scale values and calculate any percentages, as we know
	 * the output size and scale.
	 */
	config_fixup_values(&tofi);

	log_debug("Loading plugin results.\n");
	log_indent();
	
	struct string_ref_vec commands = string_ref_vec_create();
	
	if (tofi.picker_mode) {
		wl_list_init(&tofi.base_results);
		char *line = NULL;
		size_t cap = 0;
		ssize_t len;
		while ((len = getline(&line, &cap, stdin)) != -1) {
			if (len > 0 && line[len - 1] == '\n') {
				line[--len] = '\0';
			}
			if (len == 0) {
				continue;
			}
			struct nav_result *res = nav_result_create();
			strncpy(res->label, line, NAV_LABEL_MAX - 1);
			strncpy(res->value, line, NAV_VALUE_MAX - 1);
			res->action.selection_type = SELECTION_SELF;
			res->action.execution_type = EXECUTION_EXEC;
			wl_list_insert(tofi.base_results.prev, &res->link);
		}
		free(line);
		if (wl_list_empty(&tofi.base_results)) {
			log_debug("Picker mode: empty stdin, exiting.\n");
			return EXIT_FAILURE;
		}
	} else if (plugin_list) {
		wl_list_init(&tofi.base_results);
		char *copy = xstrdup(plugin_list);
		char *saveptr = NULL;
		char *token = strtok_r(copy, ",", &saveptr);
		while (token) {
			while (*token == ' ') token++;
			struct plugin *p = plugin_get(token);
			if (p && p->deps_satisfied) {
				struct nav_result *res = nav_result_create();
				const char *label = p->display_label[0] ? p->display_label : p->name;
				strncpy(res->label, label, NAV_LABEL_MAX - 1);
				strncpy(res->value, label, NAV_VALUE_MAX - 1);
				strncpy(res->source_plugin, p->name, NAV_NAME_MAX - 1);
				res->action.selection_type = SELECTION_PLUGIN;
				res->action.execution_type = EXECUTION_EXEC;
				strncpy(res->action.plugin_ref, p->name, NAV_NAME_MAX - 1);
				wl_list_insert(tofi.base_results.prev, &res->link);
			}
			token = strtok_r(NULL, ",", &saveptr);
		}
		free(copy);
	} else {
		plugin_populate_results(&tofi.base_results);
	}
	int plugin_result_count = 0;
	struct nav_result *pr;
	wl_list_for_each(pr, &tofi.base_results, link) {
		plugin_result_count++;
		struct plugin *plugin = plugin_get(pr->source_plugin);
		const char *prefix = plugin ? plugin->display_prefix : "";
		char *display = xmalloc(512);
		if (prefix && *prefix) {
			snprintf(display, 512, "%s > %s", prefix, pr->label);
		} else {
			strncpy(display, pr->label, 511);
			display[511] = '\0';
		}
		string_ref_vec_add(&commands, display);
		
		strncpy(pr->label, display, NAV_LABEL_MAX - 1);
	}
	
	tofi.view_state.commands = commands;
	
	log_debug("Loaded %d plugin results.\n", plugin_result_count);
	log_debug("Commands count: %zu\n", tofi.view_state.commands.count);
	log_unindent();
	log_debug("Plugin list generated.\n");
	tofi.view_state.results = string_ref_vec_copy(&tofi.view_state.commands);
	snprintf(tofi.view_state.prompt, VIEW_MAX_PROMPT, "%s", tofi.base_prompt);

	if (entry_plugin) {
		struct plugin *p = plugin_get(entry_plugin);
		if (p) {
			tofi.entry_only = true;
			navigate_to_plugin(&tofi, p, dict_create());
		}
	}

	if (tofi.input_mode) {
		struct nav_level *level = nav_level_create(SELECTION_INPUT, dict_create());
		level->sensitive = tofi.view_state.sensitive;
		if (tofi.view_state.prompt[0]) {
			snprintf(level->display_prompt, NAV_PROMPT_MAX, "%s", tofi.view_state.prompt);
		}
		nav_push_level(&tofi, level);
		update_view_state_from_level(&tofi, level);
		tofi.view_state.sensitive = level->sensitive;
	}

	/*
	 * Next, we create the Wayland surface, which takes on the
	 * layer shell role.
	 */
	log_debug("Creating main window surface.\n");
	tofi.window.surface.wl_surface =
		wl_compositor_create_surface(tofi.wl_compositor);
	wl_surface_add_listener(
			tofi.window.surface.wl_surface,
			&wl_surface_listener,
			&tofi);
	if (tofi.window.width == 0 || tofi.window.height == 0) {
		/*
		 * Workaround for compatibility with legacy behaviour.
		 *
		 * Before the fractional_scale protocol was released, there was
		 * no way for a client to know whether a fractional scale
		 * factor had been set, meaning percentage-based dimensions
		 * were incorrect. As a workaround for full-size windows, we
		 * allowed specifying 0 for the width / height, which caused
		 * zwlr_layer_shell to tell us the correct size to use.
		 *
		 * To make fractional scaling work, we have to use
		 * wp_viewporter, and no longer need to set the buffer scale.
		 * However, viewporter doesn't allow specifying 0 for
		 * destination width or height. As a workaround, if 0 size is
		 * set, don't use viewporter, warn the user and set the buffer
		 * scale here.
		 */
		log_warning("Width or height set to 0, disabling fractional scaling support.\n");
		log_warning("If your compositor supports the fractional scale protocol, percentages are preferred.\n");
		tofi.window.fractional_scale = 0;
		wl_surface_set_buffer_scale(
				tofi.window.surface.wl_surface,
				tofi.window.scale);
	} else if (tofi.wp_viewporter == NULL) {
		/*
		 * We also could be running on a Wayland compositor which
		 * doesn't support wp_viewporter, in which case we need to use
		 * the old scaling method.
		 */
		log_warning("Using an outdated compositor, "
				"fractional scaling will not work properly.\n");
		tofi.window.fractional_scale = 0;
		wl_surface_set_buffer_scale(
				tofi.window.surface.wl_surface,
				tofi.window.scale);
	}

	/* Grab the first (and only remaining) output from our list. */
	struct wl_output *wl_output;
	{
		struct output_list_element *el;
		el = wl_container_of(tofi.output_list.next, el, link);
		wl_output = el->wl_output;
	}

	tofi.window.zwlr_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			tofi.zwlr_layer_shell,
			tofi.window.surface.wl_surface,
			wl_output,
			ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
			"launcher");
	zwlr_layer_surface_v1_set_keyboard_interactivity(
			tofi.window.zwlr_layer_surface,
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	zwlr_layer_surface_v1_add_listener(
			tofi.window.zwlr_layer_surface,
			&zwlr_layer_surface_listener,
			&tofi);
	zwlr_layer_surface_v1_set_anchor(
			tofi.window.zwlr_layer_surface,
			tofi.anchor);
	zwlr_layer_surface_v1_set_exclusive_zone(
			tofi.window.zwlr_layer_surface,
			-1);
	zwlr_layer_surface_v1_set_margin(
			tofi.window.zwlr_layer_surface,
			tofi.window.margin_top,
			tofi.window.margin_right,
			tofi.window.margin_bottom,
			tofi.window.margin_left);
	/*
	 * No matter whether we're scaling via Cairo or not, we're presenting a
	 * scaled buffer to Wayland, so scale the window size here if we
	 * haven't already done so.
	 */
	zwlr_layer_surface_v1_set_size(
			tofi.window.zwlr_layer_surface,
			tofi.window.width,
			tofi.window.height);

	/*
	 * Set up a viewport for our surface, necessary for fractional scaling.
	 */
	if (tofi.wp_viewporter != NULL) {
		tofi.window.wp_viewport = wp_viewporter_get_viewport(
				tofi.wp_viewporter,
				tofi.window.surface.wl_surface);
		if (tofi.window.width > 0 && tofi.window.height > 0) {
			wp_viewport_set_destination(
					tofi.window.wp_viewport,
					tofi.window.width,
					tofi.window.height);
		}
	}

	/* Commit the surface to finalise setup. */
	wl_surface_commit(tofi.window.surface.wl_surface);

	/*
	 * Create a data device and setup a listener for data offers. This is
	 * required for clipboard support.
	 */
	tofi.wl_data_device = wl_data_device_manager_get_data_device(
			tofi.wl_data_device_manager,
			tofi.wl_seat);
	wl_data_device_add_listener(
			tofi.wl_data_device,
			&wl_data_device_listener,
			&tofi.clipboard);

	/*
	 * Now that we've done all our Wayland-related setup, we do another
	 * roundtrip. This should cause the layer surface window to be
	 * configured, after which we're ready to start drawing to the screen.
	 */
	log_debug("Third roundtrip start.\n");
	log_indent();
	wl_display_roundtrip(tofi.wl_display);
	log_unindent();
	log_debug("Third roundtrip done.\n");


	/*
	 * Create the various structures for our window surface. This needs to
	 * be done before initializing the renderer, which needs the buffers
	 * for drawing.
	 */
	log_debug("Initialising window surface.\n");
	log_indent();
	surface_init(&tofi.window.surface, tofi.wl_shm);
	log_unindent();
	log_debug("Window surface initialised.\n");

	/*
	 * Initialise the structures for rendering the entry.
	 * Cairo needs to know the size of the surface it's creating, and
	 * there's no way to resize it aside from tearing everything down and
	 * starting again, so we make sure to do this after we've determined
	 * our output's scale factor. This stops us being able to change the
	 * scale factor after startup, but this is just a launcher, which
	 * shouldn't be moving between outputs while running.
	 */
	log_debug("Initialising renderer.\n");
	log_indent();
	{
		/*
		 * No matter how we're scaling (with fractions, integers or not
		 * at all), we pass a fractional scale factor (the numerator of
		 * a fraction with denominator 120) to our setup function for
		 * ease.
		 */
		uint32_t scale = 120;
		if (tofi.use_scale) {
			if (tofi.window.fractional_scale != 0) {
				scale = tofi.window.fractional_scale;
			} else {
				scale = tofi.window.scale * 120;
			}
		}
		
		tofi.renderer = renderer_cairo_create();
		tofi.renderer->init(
				tofi.renderer,
				tofi.window.surface.shm_pool_data,
				tofi.window.surface.width,
				tofi.window.surface.height,
				(double)scale / 120.0,
				&tofi.view_theme);
		
		tofi.renderer->begin_frame(tofi.renderer);
		tofi.renderer->render(tofi.renderer, &tofi.view_state, &tofi.view_theme, &tofi.view_layout);
		tofi.renderer->end_frame(tofi.renderer);
	}
	log_unindent();
	log_debug("Renderer initialised.\n");

	surface_draw(&tofi.window.surface);

	wl_display_roundtrip(tofi.wl_display);

	tofi.window.surface.redraw = false;

	/*
	 * Main event loop.
	 * See the wl_display(3) man page for an explanation of the
	 * order of the various functions called here.
	 */
	while (!tofi.closed) {
		struct pollfd pollfds[3] = {{0}, {0}, {0}};
		pollfds[0].fd = wl_display_get_fd(tofi.wl_display);

		/* Make sure we're ready to receive events on the main queue. */
		while (wl_display_prepare_read(tofi.wl_display) != 0) {
			wl_display_dispatch_pending(tofi.wl_display);
		}

		/* Make sure all our requests have been sent to the server. */
		while (wl_display_flush(tofi.wl_display) != 0) {
			pollfds[0].events = POLLOUT;
			poll(&pollfds[0], 1, -1);
		}

		/*
		 * Set time to wait for poll() to -1 (unlimited), unless
		 * there's some key repeating going on.
		 */
		int timeout = -1;
		if (tofi.repeat.active) {
			int64_t wait = (int64_t)tofi.repeat.next - (int64_t)gettime_ms();
			if (wait >= 0) {
				timeout = wait;
			} else {
				timeout = 0;
			}
		}
		
		if (tofi.feedback_process.active) {
			int64_t wait = FEEDBACK_TIMEOUT_MS - ((int64_t)gettime_ms() - (int64_t)tofi.feedback_process.start_time);
			if (wait <= 0) {
				timeout = 0;
			} else if (timeout < 0 || wait < timeout) {
				timeout = wait;
			}
			
			int64_t anim_wait = 400;
			if (timeout < 0 || anim_wait < timeout) {
				timeout = anim_wait;
			}
		}

		pollfds[0].events = POLLIN | POLLPRI;
		int nfds = 1;
		
		if (tofi.clipboard.fd > 0) {
			pollfds[nfds].fd = tofi.clipboard.fd;
			pollfds[nfds].events = POLLIN | POLLPRI;
			nfds++;
		}
		
		if (tofi.feedback_process.active) {
			pollfds[nfds].fd = tofi.feedback_process.fd;
			pollfds[nfds].events = POLLIN | POLLHUP;
			nfds++;
		}
		
		int res = poll(pollfds, nfds, timeout);
		
		if (res == 0) {
			/*
			 * No events to process and no error - we presumably
			 * have a key repeat to handle.
			 */
			wl_display_cancel_read(tofi.wl_display);
			if (tofi.repeat.active) {
				int64_t wait = (int64_t)tofi.repeat.next - (int64_t)gettime_ms();
				if (wait <= 0) {
					input_handle_keypress(&tofi, tofi.repeat.keycode);
					tofi.repeat.next += 1000 / tofi.repeat.rate;
				}
			}
			feedback_process_check_timeout(&tofi);
			feedback_update_loading_animation(&tofi);
		} else if (res < 0) {
			/* There was an error polling the display. */
			wl_display_cancel_read(tofi.wl_display);
		} else {
			if (pollfds[0].revents & (POLLIN | POLLPRI)) {
				/* Events to read, so put them on the queue. */
				wl_display_read_events(tofi.wl_display);
			} else {
				/*
				 * No events to read - we were woken up to
				 * handle clipboard data.
				 */
				wl_display_cancel_read(tofi.wl_display);
			}
			if (tofi.clipboard.fd > 0 && (pollfds[1].revents & (POLLIN | POLLPRI))) {
				/* Read clipboard data. */
				read_clipboard(&tofi);
			}
			if (tofi.clipboard.fd > 0 && (pollfds[1].revents & POLLHUP)) {
				/*
				 * The other end of the clipboard pipe has
				 * closed, cleanup.
				 */
				clipboard_finish_paste(&tofi.clipboard);
			}
			if (tofi.feedback_process.active) {
				int feedback_idx = 1;
				if (tofi.clipboard.fd > 0) {
					feedback_idx = 2;
				}
				if (pollfds[feedback_idx].revents & POLLHUP) {
					feedback_process_complete(&tofi);
				}
			}
		}

		/* Handle any events we read. */
		wl_display_dispatch_pending(tofi.wl_display);

		if (tofi.window.surface.redraw) {
			tofi.renderer->begin_frame(tofi.renderer);
			tofi.renderer->render(tofi.renderer, &tofi.view_state, &tofi.view_theme, &tofi.view_layout);
			tofi.renderer->end_frame(tofi.renderer);
			
			surface_draw(&tofi.window.surface);
			tofi.window.surface.redraw = false;
		}
		if (tofi.submit) {
			tofi.submit = false;
			if (do_submit(&tofi)) {
				break;
			}
		}

	}

	log_debug("Window closed, performing cleanup.\n");
#ifdef DEBUG
	/*
	 * For debug builds, try to cleanup as much as possible, to make using
	 * e.g. Valgrind easier. There's still a few unavoidable leaks though,
	 * mostly from Pango, and Cairo holds onto quite a bit of cached data
	 * (without leaking it)
	 */
	surface_destroy(&tofi.window.surface);
	if (tofi.renderer) {
		tofi.renderer->destroy(tofi.renderer);
		free(tofi.renderer);
	}
	if (tofi.window.wp_viewport != NULL) {
		wp_viewport_destroy(tofi.window.wp_viewport);
	}
	zwlr_layer_surface_v1_destroy(tofi.window.zwlr_layer_surface);
	wl_surface_destroy(tofi.window.surface.wl_surface);
	if (tofi.wl_keyboard != NULL) {
		wl_keyboard_release(tofi.wl_keyboard);
	}
	if (tofi.wl_pointer != NULL) {
		wl_pointer_release(tofi.wl_pointer);
	}
	wl_compositor_destroy(tofi.wl_compositor);
	if (tofi.clipboard.wl_data_offer != NULL) {
		wl_data_offer_destroy(tofi.clipboard.wl_data_offer);
	}
	wl_data_device_release(tofi.wl_data_device);
	wl_data_device_manager_destroy(tofi.wl_data_device_manager);
	wl_seat_release(tofi.wl_seat);
	{
		struct output_list_element *el;
		struct output_list_element *tmp;
		wl_list_for_each_safe(el, tmp, &tofi.output_list, link) {
			wl_list_remove(&el->link);
			wl_output_release(el->wl_output);
			free(el->name);
			free(el);
		}
	}
	wl_shm_destroy(tofi.wl_shm);
	if (tofi.wp_fractional_scale_manager != NULL) {
		wp_fractional_scale_manager_v1_destroy(tofi.wp_fractional_scale_manager);
	}
	if (tofi.wp_viewporter != NULL) {
		wp_viewporter_destroy(tofi.wp_viewporter);
	}
	zwlr_layer_shell_v1_destroy(tofi.zwlr_layer_shell);
	xkb_state_unref(tofi.xkb_state);
	xkb_keymap_unref(tofi.xkb_keymap);
	xkb_context_unref(tofi.xkb_context);
	wl_registry_destroy(tofi.wl_registry);
	string_ref_vec_destroy(&tofi.view_state.commands);
	string_ref_vec_destroy(&tofi.view_state.results);
	
	struct nav_level *lvl;
	wl_list_for_each(lvl, &tofi.nav_stack, link) {
		if (lvl->mode == SELECTION_FEEDBACK) {
			feedback_history_save(lvl);
		}
	}
	
	plugin_destroy();
	builtin_cleanup();
	nav_results_destroy(&tofi.base_results);
	dict_destroy(tofi.base_dict);
#endif
	/*
	 * For release builds, skip straight to display disconnection and quit.
	 */
	wl_display_roundtrip(tofi.wl_display);
	wl_display_disconnect(tofi.wl_display);

	log_debug("Finished, exiting.\n");
	if (tofi.picker_mode || tofi.input_mode) {
		if (tofi.pipe_output[0]) {
			printf("%s\n", tofi.pipe_output);
			fflush(stdout);
			return EXIT_SUCCESS;
		}
		return EXIT_FAILURE;
	}
	if (tofi.closed) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
