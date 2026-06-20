#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
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
#include "velo.h"
#include "builtin.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "palette.h"
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
	struct velo *velo = data;
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
	if (velo->window.fractional_scale != 0) {
		velo->window.surface.width = scale_apply(width, velo->window.fractional_scale);
		velo->window.surface.height = scale_apply(height, velo->window.fractional_scale);
	} else {
		velo->window.surface.width = width * velo->window.scale;
		velo->window.surface.height = height * velo->window.scale;
	}

	zwlr_layer_surface_v1_ack_configure(
			velo->window.zwlr_layer_surface,
			serial);
}

static void zwlr_layer_surface_close(
		void *data,
		struct zwlr_layer_surface_v1 *zwlr_layer_surface)
{
	struct velo *velo = data;
	velo->closed = true;
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
	struct velo *velo = data;
	assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);

	char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	assert(map_shm != MAP_FAILED);

	log_debug("Configuring keyboard.\n");
	struct xkb_keymap *xkb_keymap = xkb_keymap_new_from_string(
			velo->xkb_context,
			map_shm,
			XKB_KEYMAP_FORMAT_TEXT_V1,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map_shm, size);
	close(fd);

	struct xkb_state *xkb_state = xkb_state_new(xkb_keymap);
	xkb_keymap_unref(velo->xkb_keymap);
	xkb_state_unref(velo->xkb_state);
	velo->xkb_keymap = xkb_keymap;
	velo->xkb_state = xkb_state;
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
	struct velo *velo = data;

	/*
	 * If this wasn't a keypress (i.e. was a key release), just update key
	 * repeat info and return.
	 */
	uint32_t keycode = key + 8;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (keycode == velo->repeat.keycode) {
			velo->repeat.active = false;
		} else {
			velo->repeat.next = gettime_ms() + velo->repeat.delay;
		}
		return;
	}

	/* A rate of 0 disables key repeat */
	if (xkb_keymap_key_repeats(velo->xkb_keymap, keycode) && velo->repeat.rate != 0) {
		velo->repeat.active = true;
		velo->repeat.keycode = keycode;
		velo->repeat.next = gettime_ms() + velo->repeat.delay;
	}
	input_handle_keypress(velo, keycode);
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
	struct velo *velo = data;
	if (velo->xkb_state == NULL) {
		return;
	}
	xkb_state_update_mask(
			velo->xkb_state,
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
	struct velo *velo = data;
	velo->repeat.rate = rate;
	velo->repeat.delay = delay;
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
	struct velo *velo = data;
	velo->pointer_x = wl_fixed_to_int(surface_x);
	velo->pointer_y = wl_fixed_to_int(surface_y);
}

static void wl_pointer_button(
		void *data,
		struct wl_pointer *pointer,
		uint32_t serial,
		uint32_t time,
		uint32_t button,
		enum wl_pointer_button_state state)
{
	struct velo *velo = data;

	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}

	if (button != BTN_LEFT) {
		return;
	}

	if (velo->pointer_x < 0 || velo->pointer_y < 0 ||
	    velo->pointer_x >= (int32_t)velo->window.width ||
	    velo->pointer_y >= (int32_t)velo->window.height) {
		velo->closed = true;
		return;
	}

	if (velo->view_layout.result_row_height <= 0 || velo->view_state.num_results_drawn == 0) {
		return;
	}

	int32_t rel_y = velo->pointer_y - velo->view_layout.result_start_y;
	if (rel_y < 0) {
		return;
	}

	uint32_t clicked_index = (uint32_t)(rel_y / velo->view_layout.result_row_height);
	if (clicked_index >= velo->view_state.num_results_drawn) {
		return;
	}

	if (clicked_index == velo->view_state.selection) {
		velo->submit = true;
	} else {
		input_select_result(velo, clicked_index);
	}
}

static void wl_pointer_axis(
		void *data,
		struct wl_pointer *pointer,
		uint32_t time,
		enum wl_pointer_axis axis,
		wl_fixed_t value)
{
	struct velo *velo = data;

	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}

	double scroll = wl_fixed_to_double(value);
	if (scroll > 0) {
		input_scroll_down(velo);
	} else if (scroll < 0) {
		input_scroll_up(velo);
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
	struct velo *velo = data;

	bool have_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
	bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;

	if (have_keyboard && velo->wl_keyboard == NULL) {
		velo->wl_keyboard = wl_seat_get_keyboard(velo->wl_seat);
		wl_keyboard_add_listener(
				velo->wl_keyboard,
				&wl_keyboard_listener,
				velo);
		log_debug("Got keyboard from seat.\n");
	} else if (!have_keyboard && velo->wl_keyboard != NULL) {
		wl_keyboard_release(velo->wl_keyboard);
		velo->wl_keyboard = NULL;
		log_debug("Released keyboard.\n");
	}

	if (have_pointer && velo->wl_pointer == NULL) {
		velo->wl_pointer = wl_seat_get_pointer(velo->wl_seat);
		wl_pointer_add_listener(
				velo->wl_pointer,
				&wl_pointer_listener,
				velo);
		log_debug("Got pointer from seat.\n");
	} else if (!have_pointer && velo->wl_pointer != NULL) {
		wl_pointer_release(velo->wl_pointer);
		velo->wl_pointer = NULL;
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
	struct velo *velo = data;
	struct output_list_element *el;
	wl_list_for_each(el, &velo->output_list, link) {
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
	struct velo *velo = data;
	struct output_list_element *el;
	wl_list_for_each(el, &velo->output_list, link) {
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
	struct velo *velo = data;
	struct output_list_element *el;
	wl_list_for_each(el, &velo->output_list, link) {
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
	struct velo *velo = data;
	struct output_list_element *el;
	wl_list_for_each(el, &velo->output_list, link) {
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
	struct velo *velo = data;
	//log_debug("Registry %u: %s v%u.\n", name, interface, version);
	if (!strcmp(interface, wl_compositor_interface.name)) {
		velo->wl_compositor = wl_registry_bind(
				wl_registry,
				name,
				&wl_compositor_interface,
				4);
		log_debug("Bound to compositor %u.\n", name);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		velo->wl_seat = wl_registry_bind(
				wl_registry,
				name,
				&wl_seat_interface,
				7);
		wl_seat_add_listener(
				velo->wl_seat,
				&wl_seat_listener,
				velo);
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
				velo);
		wl_list_insert(&velo->output_list, &el->link);
		log_debug("Bound to output %u.\n", name);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		velo->wl_shm = wl_registry_bind(
				wl_registry,
				name,
				&wl_shm_interface,
				1);
		log_debug("Bound to shm %u.\n", name);
	} else if (!strcmp(interface, wl_data_device_manager_interface.name)) {
		velo->wl_data_device_manager = wl_registry_bind(
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
		velo->zwlr_layer_shell = wl_registry_bind(
				wl_registry,
				name,
				&zwlr_layer_shell_v1_interface,
				version);
		log_debug("Bound to zwlr_layer_shell_v1 %u.\n", name);
	} else if (!strcmp(interface, wp_viewporter_interface.name)) {
		velo->wp_viewporter = wl_registry_bind(
				wl_registry,
				name,
				&wp_viewporter_interface,
				1);
		log_debug("Bound to wp_viewporter %u.\n", name);
	} else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
		velo->wp_fractional_scale_manager = wl_registry_bind(
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
	struct velo *velo = data;
	velo->window.fractional_scale = scale;
}

static const struct wp_fractional_scale_v1_listener dummy_fractional_scale_listener = {
	.preferred_scale = dummy_fractional_scale_preferred_scale
};

static void dummy_surface_enter(
		void *data,
		struct wl_surface *wl_surface,
		struct wl_output *wl_output)
{
	struct velo *velo = data;
	struct output_list_element *el;
	wl_list_for_each(el, &velo->output_list, link) {
		if (el->wl_output == wl_output) {
			velo->default_output = el;
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
"Usage: velo [options]\n"
"\n"
"Options:\n"
"  -h, --help                  Print this message and exit.\n"
"  -c, --config <path>         Specify a config file.\n"
"  -f, --filter <plugins>     Filter plugins (comma-separated: all,-drun,tmux,wifi).\n"
"  -p, --plugins <list>       Show only these plugins as root menu.\n"
"  -e, --entry <plugin>       Teleport directly to a plugin's scene at startup.\n"
"      --palette <name>       Color palette (filename in ~/.config/velo/palettes/).\n"
"      --darkmode <bool>      Use the palette's dark (true) or light (false) variant.\n"
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
"      --border-width <px>     Border width.\n"
"      --corner-radius <px>    Corner radius.\n"
"  -L, --list-palettes         List available palettes and exit.\n"
"\n"
"Config file: ~/.config/velo/config\n"
"Plugins dir: ~/.config/velo/plugins/\n"
	);
}

const struct option long_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"config", required_argument, NULL, 'c'},
	{"filter", required_argument, NULL, 'f'},
	{"plugins", required_argument, NULL, 'p'},
	{"entry", required_argument, NULL, 'e'},
	{"palette", required_argument, NULL, 0},
	{"darkmode", required_argument, NULL, 0},
	{"pick", no_argument, NULL, 'P'},
	{"input", no_argument, NULL, 'I'},
	{"sensitive", no_argument, NULL, 'S'},
	{"anchor", required_argument, NULL, 0},
	{"corner-radius", required_argument, NULL, 0},
	{"output", required_argument, NULL, 0},
	{"font", required_argument, NULL, 0},
	{"font-size", required_argument, NULL, 0},
	{"prompt-text", required_argument, NULL, 0},
	{"border-width", required_argument, NULL, 0},
	{"width", required_argument, NULL, 0},
	{"height", required_argument, NULL, 0},
	{"margin-top", required_argument, NULL, 0},
	{"margin-bottom", required_argument, NULL, 0},
	{"margin-left", required_argument, NULL, 0},
	{"margin-right", required_argument, NULL, 0},
	{"padding", required_argument, NULL, 0},
	{"list-palettes", no_argument, NULL, 'L'},
	{NULL, 0, NULL, 0}
};
const char *short_options = ":hc:f:p:e:PISL";

static void parse_args(struct velo *velo, int argc, char *argv[], const char **entry_plugin, const char **plugin_list)
{

	bool load_default_config = true;
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
	} else if (opt == 'L') {
		palette_list();
		exit(EXIT_SUCCESS);
		} else if (opt == 'c') {
			config_load(velo, optarg);
			load_default_config = false;
		} else if (opt == 'f') {
			plugin_apply_filter(optarg);
		} else if (opt == 'p') {
			*plugin_list = optarg;
		} else if (opt == 'e') {
			*entry_plugin = optarg;
	} else if (opt == 'P') {
			velo->picker_mode = true;
		} else if (opt == 'I') {
			velo->input_mode = true;
		} else if (opt == 'S') {
			velo->view_state.sensitive = true;
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
		config_load(velo, NULL);
	}

	if ((velo->picker_mode || velo->input_mode) && (*entry_plugin || *plugin_list)) {
		log_error("--pick/--input cannot be combined with -e or -p.\n");
		exit(EXIT_FAILURE);
	}
	if (velo->picker_mode && velo->input_mode) {
		log_error("--pick and --input cannot be combined.\n");
		exit(EXIT_FAILURE);
	}

	/* Second pass, parse everything else. */
	optind = 1;
	opt = getopt_long(argc, argv, short_options, long_options, &option_index);
	while (opt != -1) {
		if (opt == 0) {
			if (!config_apply(velo, long_options[option_index].name, optarg)) {
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

	/* Colors come from the palette, loaded once after all overrides resolve. */
	config_load_palette(velo);
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

void nav_push_level(struct velo *velo, struct nav_level *level)
{
	wl_list_insert(&velo->nav_stack, &level->link);
	velo->nav_current = level;
}

static void nav_pop_level(struct velo *velo)
{
	if (!velo->nav_current) {
		return;
	}
	
	struct nav_level *current = velo->nav_current;

	wl_list_remove(&current->link);
	
	if (wl_list_empty(&velo->nav_stack)) {
		velo->nav_current = NULL;
	} else {
		velo->nav_current = wl_container_of(velo->nav_stack.next, velo->nav_current, link);
	}
	
	nav_level_destroy(current);
}

void update_view_state_from_level(struct velo *velo, struct nav_level *level)
{
	string_ref_vec_destroy(&velo->view_state.results);
	velo->view_state.results = string_ref_vec_create();
	
	struct nav_result *res;
	wl_list_for_each(res, &level->results, link) {
		string_ref_vec_add(&velo->view_state.results, res->label);
	}
	velo->view_state.selection = level->selection;
	if (level->display_prompt[0]) {
		snprintf(velo->view_state.prompt, VIEW_MAX_PROMPT, "%s", level->display_prompt);
	}
}

static void execute_command(const char *template, struct value_dict *dict);

bool navigate_to_plugin(struct velo *velo, struct plugin *target, struct value_dict *dict)
{
	struct view_state *state = &velo->view_state;

	switch (target->type) {
	case PLUGIN_LIST: {
		struct nav_level *new_level = nav_level_create(SELECTION_PLUGIN, dict);
		strncpy(new_level->plugin_ref, target->name, NAV_NAME_MAX - 1);
		plugin_populate_from_children(target, &new_level->results);
		nav_results_copy(&new_level->backup_results, &new_level->results);
		if (target->context_name[0]) {
			snprintf(new_level->display_prompt, NAV_PROMPT_MAX, "%s: ", target->context_name);
		}
		nav_push_level(velo, new_level);
		update_view_state_from_level(velo, new_level);
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
		new_level->live_apply_palette = target->live_apply_palette;
		if (target->live_apply_palette) {
			/* Snapshot the current palette so ESC can restore it, and start
			 * the highlight on it so entering the list doesn't jump themes. */
			snprintf(new_level->saved_palette, sizeof(new_level->saved_palette),
					"%s", velo->palette_name);
			uint32_t i = 0;
			struct nav_result *r;
			wl_list_for_each(r, &new_level->results, link) {
				if (strcmp(r->label, velo->palette_name) == 0) {
					new_level->selection = i;
					break;
				}
				i++;
			}
		}
		if (target->context_name[0]) {
			snprintf(new_level->display_prompt, NAV_PROMPT_MAX, "%s: ", target->context_name);
		}
		nav_push_level(velo, new_level);
		update_view_state_from_level(velo, new_level);
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
		nav_push_level(velo, new_level);
		update_view_state_from_level(velo, new_level);
		state->sensitive = target->sensitive;
		break;
	}
	case PLUGIN_PREVIEW: {
		struct nav_level *new_level = nav_level_create(SELECTION_PREVIEW, dict);
		strncpy(new_level->eval_cmd, target->eval_cmd, NAV_CMD_MAX - 1);
		strncpy(new_level->copy_cmd, target->copy_cmd, NAV_CMD_MAX - 1);
		if (target->context_name[0]) {
			snprintf(new_level->display_prompt, NAV_PROMPT_MAX, "%s: ", target->context_name);
		} else if (target->prompt[0]) {
			snprintf(new_level->display_prompt, NAV_PROMPT_MAX, "%s", target->prompt);
		}
		nav_push_level(velo, new_level);
		update_view_state_from_level(velo, new_level);
		break;
	}
	case PLUGIN_EXEC: {
		execute_command(target->template, dict);
		dict_destroy(dict);
		velo->closed = true;
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
	velo->window.surface.redraw = true;
	return false;
}

#define PREVIEW_DEBOUNCE_MS 120

/*
 * Synchronously evaluate the preview command for the current input and store
 * the first line of its output as the preview result. Runs on the main thread
 * only after the input has been quiet for PREVIEW_DEBOUNCE_MS, so it suits fast
 * local commands (qalc, encode, local lookups) and is deliberately not used
 * for slow or networked ones, which would stall the UI.
 */
static void preview_eval(struct velo *velo, struct nav_level *level)
{
	if (!level->input_buffer[0] || !level->eval_cmd[0]) {
		level->preview_result[0] = '\0';
		string_ref_vec_destroy(&velo->view_state.results);
		velo->view_state.results = string_ref_vec_create();
		velo->view_state.selection = 0;
		velo->view_state.first_result = 0;
		velo->window.surface.redraw = true;
		return;
	}

	struct value_dict *dict = dict_copy(level->dict);
	dict_set(&dict, "input", level->input_buffer);
	char *cmd = template_resolve(level->eval_cmd, dict);
	dict_destroy(dict);
	if (!cmd) {
		return;
	}

	FILE *fp = popen(cmd, "r");
	free(cmd);
	if (!fp) {
		return;
	}

	char line[NAV_VALUE_MAX];
	if (fgets(line, sizeof(line), fp)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}
		snprintf(level->preview_result, sizeof(level->preview_result), "%s", line);
	} else {
		level->preview_result[0] = '\0';
	}
	pclose(fp);

	string_ref_vec_destroy(&velo->view_state.results);
	velo->view_state.results = string_ref_vec_create();
	if (level->preview_result[0]) {
		string_ref_vec_add(&velo->view_state.results, level->preview_result);
	}
	velo->view_state.selection = 0;
	velo->view_state.first_result = 0;
	velo->window.surface.redraw = true;
}

/*
 * Copy the current preview result to the clipboard via the configured copy
 * command (default wl-copy), feeding the text through stdin so no shell
 * escaping is needed.
 */
static void preview_copy(struct velo *velo, struct nav_level *level)
{
	if (!level->preview_result[0]) {
		return;
	}
	FILE *fp = popen(level->copy_cmd[0] ? level->copy_cmd : "wl-copy", "w");
	if (!fp) {
		return;
	}
	fputs(level->preview_result, fp);
	pclose(fp);
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

static bool do_submit(struct velo *velo)
{
	struct nav_level *level = velo->nav_current;

	if (level && level->mode == SELECTION_INPUT) {
		if (velo->input_mode) {
			snprintf(velo->pipe_output, N_ELEM(velo->pipe_output), "%s", level->input_buffer);
			return true;
		}
		struct value_dict *dict = dict_copy(level->dict);
		dict_set(&dict, level->as, level->input_buffer);

		if (level->next_plugin[0]) {
			struct plugin *next_p = plugin_get(level->next_plugin);
			if (next_p) return navigate_to_plugin(velo, next_p, dict);
		}

		if (level->return_to_parent) {
			nav_pop_level(velo);
			if (velo->nav_current) {
				struct nav_level *parent = velo->nav_current;
				dict_destroy(parent->dict);
				parent->dict = dict;

				if (parent->execution_type == EXECUTION_EXEC) {
					execute_command(parent->template, parent->dict);
					return true;
				}

				update_view_state_from_level(velo, parent);
				velo->view_state.input_utf32_length = 0;
				velo->view_state.input_utf8_length = 0;
				velo->view_state.input_utf8[0] = '\0';
				velo->view_state.cursor_position = 0;
				velo->window.surface.redraw = true;
			}
			return false;
		}

		execute_command(level->template, dict);
		dict_destroy(dict);
		return true;
	}

	if (level && level->mode == SELECTION_PREVIEW) {
		preview_copy(velo, level);
		return true;
	}

	uint32_t selection = velo->view_state.selection + velo->view_state.first_result;

	if (velo->view_state.results.count == 0) {
		return false;
	}

	char *res = velo->view_state.results.buf[selection].string;

	struct nav_result *nav_res = NULL;
	if (level) {
		nav_res = find_nav_result(level, res);
	}

	if (!level && !wl_list_empty(&velo->base_results)) {
		struct nav_result *r;
		wl_list_for_each(r, &velo->base_results, link) {
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
			if (velo->picker_mode) {
				snprintf(velo->pipe_output, N_ELEM(velo->pipe_output), "%s", nav_res->value);
				dict_destroy(dict);
				return true;
			}
			if (action->as[0]) {
				dict_set(&dict, action->as, nav_res->value);
			}

			if (level && level->next_plugin[0]) {
				struct plugin *next_p = plugin_get(level->next_plugin);
				if (next_p) return navigate_to_plugin(velo, next_p, dict);
			}

			if (level && level->return_to_parent) {
				nav_pop_level(velo);
				if (velo->nav_current) {
					struct nav_level *parent = velo->nav_current;
					dict_destroy(parent->dict);
					parent->dict = dict;

					if (parent->execution_type == EXECUTION_EXEC) {
						execute_command(parent->template, parent->dict);
						return true;
					}

					update_view_state_from_level(velo, parent);
					velo->view_state.input_utf32_length = 0;
					velo->view_state.input_utf8_length = 0;
					velo->view_state.input_utf8[0] = '\0';
					velo->view_state.cursor_position = 0;
					velo->window.surface.redraw = true;
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
			return navigate_to_plugin(velo, target_plugin, dict);
		}

		default:
			dict_destroy(dict);
			break;
		}
	}

	return false;
}

static void read_clipboard(struct velo *velo)
{
	struct view_state *state = &velo->view_state;

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
			int res = read(velo->clipboard.fd, &buffer[i], 1);
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
					input_refresh_results(velo);
					velo->window.surface.redraw = true;
					return;
				}
				log_error("Failed to read clipboard: %s\n", strerror(errno));
				clipboard_finish_paste(&velo->clipboard);
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

	clipboard_finish_paste(&velo->clipboard);

	input_refresh_results(velo);
	velo->window.surface.redraw = true;
}

/*
 * Calculate the ideal window height for autosize mode.
 * Sizes to exact row boundaries; no partial-row gap at the bottom.
 */
static uint32_t autosize_calc_height(struct velo *velo)
{
	int32_t bottom = velo->view_theme.padding_bottom
		+ velo->view_theme.border_width
		+ (int32_t)(ceil(MAX((double)velo->view_theme.corner_radius
			- velo->view_theme.border_width, 0) * (1.0 - 1.0 / M_SQRT2)));

	int32_t overhead = velo->view_layout.result_start_y + bottom;
	int32_t row_h = velo->view_layout.result_row_height;

	if (velo->view_state.results.count == 0 || row_h <= 0) {
		return MAX(1, overhead);
	}

	int32_t available = (int32_t)velo->max_window_height - overhead;
	if (available <= 0) {
		return MAX(1, overhead);
	}

	uint32_t rows_that_fit = available / row_h;
	uint32_t rows = MIN(rows_that_fit, velo->view_state.results.count);

	return overhead + rows * row_h;
}

int main(int argc, char *argv[])
{
	/* Handle early-exit flags before any initialization. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--list-palettes") == 0 || strcmp(argv[i], "-L") == 0) {
			palette_list();
			return EXIT_SUCCESS;
		}
	}

	/* Call log_debug to initialise the timers we use for perf checking. */
	log_debug("This is velo.\n");

	/*
	 * Set the locale to the user's default, so we can deal with non-ASCII
	 * characters.
	 */
	setlocale(LC_ALL, "");

	/* Default options. */
	struct velo velo = {
		.window = {
			.scale = 1,
			.width = 600,
			.height = 400,
		},
		.view_theme = {
			.font_name = "Sans",
			.font_size = 14,
			.padding_top = 16,
			.padding_bottom = 16,
			.padding_left = 16,
			.padding_right = 16,
			.border_width = 2,
		.background_color = {0.125f, 0.133f, 0.141f, 1.0f},
		.background_opacity = 0.85f,
		.foreground_color = {0.988f, 0.988f, 0.988f, 1.0f},
		.selection_color = {0.239f, 0.682f, 0.914f, 1.0f},
		.selection_fill_color = {0.239f, 0.682f, 0.914f, 1.0f},
		.selection_text_color = {0.078f, 0.086f, 0.094f, 1.0f},
		.border_color = {0.439f, 0.490f, 0.541f, 1.0f},
		.prompt_color = {0.114f, 0.600f, 0.953f, 1.0f},
		.divider_color = {0.114f, 0.600f, 0.953f, 1.0f},
	},
		.view_state = {
			.prompt = "> ",
			.algorithm = MATCHING_ALGORITHM_FUZZY,
		},
	.autosize = true,
	.palette_name = "breeze",
	.darkmode = true,
	.color_derivation = true,
	.anchor =  ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
		.use_scale = true,
	};
	wl_list_init(&velo.output_list);
	wl_list_init(&velo.nav_stack);
	velo.nav_current = NULL;
	velo.base_dict = dict_create();
	wl_list_init(&velo.base_results);

	/*
	 * Modes that never touch plugins: the --pick/--input pipe modes and the
	 * early-exit --list-palettes/--help. Detect them now so we can skip
	 * parsing ~20 plugin TOMLs they don't use. plugin_init() still runs
	 * (it's cheap) so plugin_destroy() is safe on the empty list later.
	 */
	bool needs_plugins = true;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--pick") || !strcmp(a, "--input")
				|| !strcmp(a, "--list-palettes") || !strcmp(a, "--help")) {
			needs_plugins = false;
			break;
		}
		if (a[0] == '-' && a[1] != '-' && a[1] != '\0') {
			for (const char *c = a + 1; *c; c++) {
				if (*c == 'P' || *c == 'I' || *c == 'L' || *c == 'h') {
					needs_plugins = false;
					break;
				}
			}
			if (!needs_plugins) break;
		}
	}

	config_seed_if_needed();
	plugin_init();
	if (needs_plugins) {
		char *config_dir = get_user_config_dir();
		if (config_dir) {
			char plugin_dir[512];
			snprintf(plugin_dir, sizeof(plugin_dir), "%s/plugins", config_dir);
			log_debug("Loading plugins from: %s\n", plugin_dir);
			plugin_load_directory(plugin_dir);
			free(config_dir);
		}
		log_debug("Loaded %zu plugins.\n", plugin_count());
	}
	
	const char *entry_plugin = NULL;
	const char *plugin_list = NULL;
	parse_args(&velo, argc, argv, &entry_plugin, &plugin_list);
	log_debug("Config done.\n");

	/*
	 * Initial Wayland & XKB setup.
	 * The first thing to do is connect a listener to the global registry,
	 * so that we can bind to the various global objects and start talking
	 * to Wayland.
	 */
	
	snprintf(velo.base_prompt, MAX_PROMPT_LENGTH, "%s", velo.view_state.prompt);

	log_debug("Connecting to Wayland display.\n");
	velo.wl_display = wl_display_connect(NULL);
	if (velo.wl_display == NULL) {
		log_error("Couldn't connect to Wayland display.\n");
		exit(EXIT_FAILURE);
	}
	velo.wl_registry = wl_display_get_registry(velo.wl_display);
	log_debug("Creating xkb context.\n");
	velo.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (velo.xkb_context == NULL) {
		log_error("Couldn't create an XKB context.\n");
		exit(EXIT_FAILURE);
	}
	wl_registry_add_listener(
			velo.wl_registry,
			&wl_registry_listener,
			&velo);

	/*
	 * After this first roundtrip, the only thing that should have happened
	 * is our registry_global() function being called and setting up the
	 * various global object bindings.
	 */
	log_debug("First roundtrip start.\n");
	log_indent();
	wl_display_roundtrip(velo.wl_display);
	log_unindent();
	log_debug("First roundtrip done.\n");

	/*
	 * The next roundtrip causes the listeners we set up in
	 * registry_global() to be called. Notably, the output should be
	 * configured, telling us the scale factor and size.
	 */
	log_debug("Second roundtrip start.\n");
	log_indent();
	wl_display_roundtrip(velo.wl_display);
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
		 * two roundtrips, then tear it down. velo.default_output
		 * should then contain the output our surface was assigned to,
		 * and velo.window.fractional_scale should have the scale
		 * factor.
		 */
		log_debug("Determining output.\n");
		log_indent();
		struct surface surface = {
			.width = 1,
			.height = 1
		};
		surface.wl_surface =
			wl_compositor_create_surface(velo.wl_compositor);
		wl_surface_add_listener(
				surface.wl_surface,
				&dummy_surface_listener,
				&velo);

		struct wp_fractional_scale_v1 *wp_fractional_scale = NULL;
		if (velo.wp_fractional_scale_manager != NULL) {
			wp_fractional_scale =
				wp_fractional_scale_manager_v1_get_fractional_scale(
						velo.wp_fractional_scale_manager,
						surface.wl_surface);
			wp_fractional_scale_v1_add_listener(
					wp_fractional_scale,
					&dummy_fractional_scale_listener,
					&velo);
		}

		/*
		 * If we have a desired output, make sure we appear on it so we
		 * can determine the correct fractional scale.
		 */
		struct wl_output *wl_output = NULL;
		if (velo.target_output_name[0] != '\0') {
			struct output_list_element *el;
			wl_list_for_each(el, &velo.output_list, link) {
				if (!strcmp(velo.target_output_name, el->name)) {
					wl_output = el->wl_output;
					break;
				}
			}
		}

		struct zwlr_layer_surface_v1 *zwlr_layer_surface =
			zwlr_layer_shell_v1_get_layer_surface(
					velo.zwlr_layer_shell,
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
				&velo);
		zwlr_layer_surface_v1_set_size(
				zwlr_layer_surface,
				1,
				1);
		wl_surface_commit(surface.wl_surface);
		log_debug("First dummy roundtrip start.\n");
		log_indent();
		wl_display_roundtrip(velo.wl_display);
		log_unindent();
		log_debug("First dummy roundtrip done.\n");
		log_debug("Initialising dummy surface.\n");
		log_indent();
		surface_init(&surface, velo.wl_shm);
		surface_draw(&surface);
		log_unindent();
		log_debug("Dummy surface initialised.\n");
		log_debug("Second dummy roundtrip start.\n");
		log_indent();
		wl_display_roundtrip(velo.wl_display);
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
		head = wl_container_of(velo.output_list.next, head, link);

		struct output_list_element *el;
		struct output_list_element *tmp;
		if (velo.target_output_name[0] != 0) {
			log_debug("Looking for output %s.\n", velo.target_output_name);
		} else if (velo.default_output != NULL) {
			snprintf(
					velo.target_output_name,
					N_ELEM(velo.target_output_name),
					"%s",
					velo.default_output->name);
			/* We don't need this anymore. */
			velo.default_output = NULL;
		}
		wl_list_for_each_reverse_safe(el, tmp, &velo.output_list, link) {
			if (!strcmp(velo.target_output_name, el->name)) {
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
		el = wl_container_of(velo.output_list.next, el, link);

		/*
		 * If we're rotated 90 degrees, we need to swap width and
		 * height to calculate percentages.
		 */
		switch (el->transform) {
			case WL_OUTPUT_TRANSFORM_90:
			case WL_OUTPUT_TRANSFORM_270:
			case WL_OUTPUT_TRANSFORM_FLIPPED_90:
			case WL_OUTPUT_TRANSFORM_FLIPPED_270:
				velo.output_width = el->height;
				velo.output_height = el->width;
				break;
			default:
				velo.output_width = el->width;
				velo.output_height = el->height;
		}
		velo.window.scale = el->scale;
		log_unindent();
		log_debug("Selected output %s.\n", el->name);
	}

	/*
	 * We can now scale values and calculate any percentages, as we know
	 * the output size and scale.
	 */
	config_fixup_values(&velo);

	if (velo.autosize) {
		velo.max_window_height = velo.window.height;

		velo.anchor = (velo.anchor | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
			& ~ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

		int32_t available = velo.output_height - velo.window.margin_top - velo.window.margin_bottom;
		velo.window.margin_top = velo.window.margin_top
			+ (available - (int32_t)velo.window.height) / 2;
		velo.window.margin_bottom = 0;

		log_debug("Autosize: anchor=%u margin_top=%d output_h=%d max_h=%u\n",
			velo.anchor, velo.window.margin_top, velo.output_height, velo.max_window_height);
	}

	log_debug("Loading plugin results.\n");
	log_indent();
	
	struct string_ref_vec commands = string_ref_vec_create();
	
	if (velo.picker_mode) {
		wl_list_init(&velo.base_results);
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
			wl_list_insert(velo.base_results.prev, &res->link);
		}
		free(line);
		if (wl_list_empty(&velo.base_results)) {
			log_debug("Picker mode: empty stdin, exiting.\n");
			return EXIT_FAILURE;
		}
	} else if (plugin_list) {
		wl_list_init(&velo.base_results);
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
				wl_list_insert(velo.base_results.prev, &res->link);
			}
			token = strtok_r(NULL, ",", &saveptr);
		}
		free(copy);
	} else {
		plugin_populate_results(&velo.base_results);
	}
	int plugin_result_count = 0;
	struct nav_result *pr;
	wl_list_for_each(pr, &velo.base_results, link) {
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
	
	velo.view_state.commands = commands;
	
	log_debug("Loaded %d plugin results.\n", plugin_result_count);
	log_debug("Commands count: %zu\n", velo.view_state.commands.count);
	log_unindent();
	log_debug("Plugin list generated.\n");
	velo.view_state.results = string_ref_vec_copy(&velo.view_state.commands);
	snprintf(velo.view_state.prompt, VIEW_MAX_PROMPT, "%s", velo.base_prompt);

	if (entry_plugin) {
		struct plugin *p = plugin_get(entry_plugin);
		if (p) {
			velo.entry_only = true;
			navigate_to_plugin(&velo, p, dict_create());
		}
	}

	if (velo.input_mode) {
		struct nav_level *level = nav_level_create(SELECTION_INPUT, dict_create());
		level->sensitive = velo.view_state.sensitive;
		if (velo.view_state.prompt[0]) {
			snprintf(level->display_prompt, NAV_PROMPT_MAX, "%s", velo.view_state.prompt);
		}
		nav_push_level(&velo, level);
		update_view_state_from_level(&velo, level);
		velo.view_state.sensitive = level->sensitive;
	}

	/*
	 * Next, we create the Wayland surface, which takes on the
	 * layer shell role.
	 */
	log_debug("Creating main window surface.\n");
	velo.window.surface.wl_surface =
		wl_compositor_create_surface(velo.wl_compositor);
	wl_surface_add_listener(
			velo.window.surface.wl_surface,
			&wl_surface_listener,
			&velo);
	if (velo.window.width == 0 || velo.window.height == 0) {
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
		velo.window.fractional_scale = 0;
		wl_surface_set_buffer_scale(
				velo.window.surface.wl_surface,
				velo.window.scale);
	} else if (velo.wp_viewporter == NULL) {
		/*
		 * We also could be running on a Wayland compositor which
		 * doesn't support wp_viewporter, in which case we need to use
		 * the old scaling method.
		 */
		log_warning("Using an outdated compositor, "
				"fractional scaling will not work properly.\n");
		velo.window.fractional_scale = 0;
		wl_surface_set_buffer_scale(
				velo.window.surface.wl_surface,
				velo.window.scale);
	}

	/* Grab the first (and only remaining) output from our list. */
	struct wl_output *wl_output;
	{
		struct output_list_element *el;
		el = wl_container_of(velo.output_list.next, el, link);
		wl_output = el->wl_output;
	}

	velo.window.zwlr_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			velo.zwlr_layer_shell,
			velo.window.surface.wl_surface,
			wl_output,
			ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
			"launcher");
	zwlr_layer_surface_v1_set_keyboard_interactivity(
			velo.window.zwlr_layer_surface,
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	zwlr_layer_surface_v1_add_listener(
			velo.window.zwlr_layer_surface,
			&zwlr_layer_surface_listener,
			&velo);
	zwlr_layer_surface_v1_set_anchor(
			velo.window.zwlr_layer_surface,
			velo.anchor);
	zwlr_layer_surface_v1_set_exclusive_zone(
			velo.window.zwlr_layer_surface,
			-1);
	zwlr_layer_surface_v1_set_margin(
			velo.window.zwlr_layer_surface,
			velo.window.margin_top,
			velo.window.margin_right,
			velo.window.margin_bottom,
			velo.window.margin_left);
	/*
	 * No matter whether we're scaling via Cairo or not, we're presenting a
	 * scaled buffer to Wayland, so scale the window size here if we
	 * haven't already done so.
	 */
	zwlr_layer_surface_v1_set_size(
			velo.window.zwlr_layer_surface,
			velo.window.width,
			velo.window.height);

	/*
	 * Set up a viewport for our surface, necessary for fractional scaling.
	 */
	if (velo.wp_viewporter != NULL) {
		velo.window.wp_viewport = wp_viewporter_get_viewport(
				velo.wp_viewporter,
				velo.window.surface.wl_surface);
		if (velo.window.width > 0 && velo.window.height > 0) {
			wp_viewport_set_destination(
					velo.window.wp_viewport,
					velo.window.width,
					velo.window.height);
		}
	}

	/* Commit the surface to finalise setup. */
	wl_surface_commit(velo.window.surface.wl_surface);

	/*
	 * Create a data device and setup a listener for data offers. This is
	 * required for clipboard support.
	 */
	velo.wl_data_device = wl_data_device_manager_get_data_device(
			velo.wl_data_device_manager,
			velo.wl_seat);
	wl_data_device_add_listener(
			velo.wl_data_device,
			&wl_data_device_listener,
			&velo.clipboard);

	/*
	 * Now that we've done all our Wayland-related setup, we do another
	 * roundtrip. This should cause the layer surface window to be
	 * configured, after which we're ready to start drawing to the screen.
	 */
	log_debug("Third roundtrip start.\n");
	log_indent();
	wl_display_roundtrip(velo.wl_display);
	log_unindent();
	log_debug("Third roundtrip done.\n");


	/*
	 * Create the various structures for our window surface. This needs to
	 * be done before initializing the renderer, which needs the buffers
	 * for drawing.
	 */
	log_debug("Initialising window surface.\n");
	log_indent();
	surface_init(&velo.window.surface, velo.wl_shm);
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
		if (velo.use_scale) {
			if (velo.window.fractional_scale != 0) {
				scale = velo.window.fractional_scale;
			} else {
				scale = velo.window.scale * 120;
			}
		}
		
		velo.renderer = renderer_cairo_create();
		velo.renderer->init(
				velo.renderer,
				velo.window.surface.shm_pool_data,
				velo.window.surface.width,
				velo.window.surface.height,
				(double)scale / 120.0,
				&velo.view_theme);
		
		velo.renderer->begin_frame(velo.renderer);
		velo.renderer->render(velo.renderer, &velo.view_state, &velo.view_theme, &velo.view_layout);
		velo.renderer->end_frame(velo.renderer);
	}
	log_unindent();
	log_debug("Renderer initialised.\n");

	surface_draw(&velo.window.surface);

	wl_display_roundtrip(velo.wl_display);

	if (velo.autosize) {
		velo.view_state.render_height = autosize_calc_height(&velo);

		velo.renderer->begin_frame(velo.renderer);
		velo.renderer->render(velo.renderer, &velo.view_state, &velo.view_theme, &velo.view_layout);
		velo.renderer->end_frame(velo.renderer);
		surface_draw(&velo.window.surface);
	}

	velo.window.surface.redraw = false;

	/*
	 * Main event loop.
	 * See the wl_display(3) man page for an explanation of the
	 * order of the various functions called here.
	 */
	while (!velo.closed) {
		struct pollfd pollfds[3] = {{0}, {0}, {0}};
		pollfds[0].fd = wl_display_get_fd(velo.wl_display);

		/* Make sure we're ready to receive events on the main queue. */
		while (wl_display_prepare_read(velo.wl_display) != 0) {
			wl_display_dispatch_pending(velo.wl_display);
		}

		/* Make sure all our requests have been sent to the server. */
		while (wl_display_flush(velo.wl_display) != 0) {
			pollfds[0].events = POLLOUT;
			poll(&pollfds[0], 1, -1);
		}

		/*
		 * Set time to wait for poll() to -1 (unlimited), unless
		 * there's some key repeating going on.
		 */
		int timeout = -1;
		if (velo.repeat.active) {
			int64_t wait = (int64_t)velo.repeat.next - (int64_t)gettime_ms();
			if (wait >= 0) {
				timeout = wait;
			} else {
				timeout = 0;
			}
		}
		
		if (velo.nav_current && velo.nav_current->mode == SELECTION_PREVIEW
				&& velo.nav_current->preview_dirty) {
			int64_t wait = (int64_t)velo.nav_current->preview_input_time
					+ PREVIEW_DEBOUNCE_MS - (int64_t)gettime_ms();
			if (wait <= 0) {
				timeout = 0;
			} else if (timeout < 0 || wait < timeout) {
				timeout = wait;
			}
		}

		pollfds[0].events = POLLIN | POLLPRI;
		int nfds = 1;
		
		if (velo.clipboard.fd > 0) {
			pollfds[nfds].fd = velo.clipboard.fd;
			pollfds[nfds].events = POLLIN | POLLPRI;
			nfds++;
		}

		int res = poll(pollfds, nfds, timeout);
		
		if (res == 0) {
			/*
			 * No events to process and no error - we presumably
			 * have a key repeat to handle.
			 */
			wl_display_cancel_read(velo.wl_display);
			if (velo.repeat.active) {
				int64_t wait = (int64_t)velo.repeat.next - (int64_t)gettime_ms();
				if (wait <= 0) {
					input_handle_keypress(&velo, velo.repeat.keycode);
					velo.repeat.next += 1000 / velo.repeat.rate;
				}
			}
		} else if (res < 0) {
			/* There was an error polling the display. */
			wl_display_cancel_read(velo.wl_display);
		} else {
			if (pollfds[0].revents & (POLLIN | POLLPRI)) {
				/* Events to read, so put them on the queue. */
				wl_display_read_events(velo.wl_display);
			} else {
				/*
				 * No events to read - we were woken up to
				 * handle clipboard data.
				 */
				wl_display_cancel_read(velo.wl_display);
			}
			if (velo.clipboard.fd > 0 && (pollfds[1].revents & (POLLIN | POLLPRI))) {
				/* Read clipboard data. */
				read_clipboard(&velo);
			}
			if (velo.clipboard.fd > 0 && (pollfds[1].revents & POLLHUP)) {
				/*
				 * The other end of the clipboard pipe has
				 * closed, cleanup.
				 */
				clipboard_finish_paste(&velo.clipboard);
			}
		}

		/* Preview debounce: evaluate once the input has been quiet. */
		if (velo.nav_current && velo.nav_current->mode == SELECTION_PREVIEW
				&& velo.nav_current->preview_dirty) {
			int64_t wait = (int64_t)velo.nav_current->preview_input_time
					+ PREVIEW_DEBOUNCE_MS - (int64_t)gettime_ms();
			if (wait <= 0) {
				velo.nav_current->preview_dirty = false;
				preview_eval(&velo, velo.nav_current);
			}
		}

		/* Handle any events we read. */
		wl_display_dispatch_pending(velo.wl_display);

		/* Live palette preview: while browsing a live-apply palette list,
		 * keep the running instance's palette in sync with the highlight. */
		if (velo.nav_current && velo.nav_current->mode == SELECTION_SELECT
				&& velo.nav_current->live_apply_palette) {
			uint32_t idx = velo.view_state.selection + velo.view_state.first_result;
			if (idx < velo.view_state.results.count) {
				const char *sel = velo.view_state.results.buf[idx].string;
				if (strcmp(sel, velo.palette_name) != 0) {
					snprintf(velo.palette_name, sizeof(velo.palette_name),
							"%s", sel);
					config_load_palette(&velo);
					velo.window.surface.redraw = true;
				}
			}
		}

		if (velo.window.surface.redraw) {
			if (velo.autosize) {
				velo.view_state.render_height = autosize_calc_height(&velo);
			}

			velo.renderer->begin_frame(velo.renderer);
			velo.renderer->render(velo.renderer, &velo.view_state, &velo.view_theme, &velo.view_layout);
			velo.renderer->end_frame(velo.renderer);

			surface_draw(&velo.window.surface);
			velo.window.surface.redraw = false;
		}
		if (velo.submit) {
			velo.submit = false;
			if (do_submit(&velo)) {
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
	surface_destroy(&velo.window.surface);
	if (velo.renderer) {
		velo.renderer->destroy(velo.renderer);
		free(velo.renderer);
	}
	if (velo.window.wp_viewport != NULL) {
		wp_viewport_destroy(velo.window.wp_viewport);
	}
	zwlr_layer_surface_v1_destroy(velo.window.zwlr_layer_surface);
	wl_surface_destroy(velo.window.surface.wl_surface);
	if (velo.wl_keyboard != NULL) {
		wl_keyboard_release(velo.wl_keyboard);
	}
	if (velo.wl_pointer != NULL) {
		wl_pointer_release(velo.wl_pointer);
	}
	wl_compositor_destroy(velo.wl_compositor);
	if (velo.clipboard.wl_data_offer != NULL) {
		wl_data_offer_destroy(velo.clipboard.wl_data_offer);
	}
	wl_data_device_release(velo.wl_data_device);
	wl_data_device_manager_destroy(velo.wl_data_device_manager);
	wl_seat_release(velo.wl_seat);
	{
		struct output_list_element *el;
		struct output_list_element *tmp;
		wl_list_for_each_safe(el, tmp, &velo.output_list, link) {
			wl_list_remove(&el->link);
			wl_output_release(el->wl_output);
			free(el->name);
			free(el);
		}
	}
	wl_shm_destroy(velo.wl_shm);
	if (velo.wp_fractional_scale_manager != NULL) {
		wp_fractional_scale_manager_v1_destroy(velo.wp_fractional_scale_manager);
	}
	if (velo.wp_viewporter != NULL) {
		wp_viewporter_destroy(velo.wp_viewporter);
	}
	zwlr_layer_shell_v1_destroy(velo.zwlr_layer_shell);
	xkb_state_unref(velo.xkb_state);
	xkb_keymap_unref(velo.xkb_keymap);
	xkb_context_unref(velo.xkb_context);
	wl_registry_destroy(velo.wl_registry);
	string_ref_vec_destroy(&velo.view_state.commands);
	string_ref_vec_destroy(&velo.view_state.results);

	plugin_destroy();
	builtin_cleanup();
	nav_results_destroy(&velo.base_results);
	dict_destroy(velo.base_dict);
#endif
	/*
	 * For release builds, skip straight to display disconnection and quit.
	 */
	wl_display_roundtrip(velo.wl_display);
	wl_display_disconnect(velo.wl_display);

	log_debug("Finished, exiting.\n");
	if (velo.picker_mode || velo.input_mode) {
		if (velo.pipe_output[0]) {
			printf("%s\n", velo.pipe_output);
			fflush(stdout);
			return EXIT_SUCCESS;
		}
		return EXIT_FAILURE;
	}
	if (velo.closed) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
