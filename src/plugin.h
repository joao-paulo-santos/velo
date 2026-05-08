#ifndef PLUGIN_H
#define PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-client.h>
#include "nav.h"
#include "string_vec.h"

#define PLUGIN_NAME_MAX 64
#define PLUGIN_PATH_MAX 256

struct plugin;
struct wl_list;

typedef void (*plugin_populate_fn)(struct plugin *plugin, struct wl_list *results);

struct plugin_action {
	struct wl_list link;
	char label[NAV_LABEL_MAX];
	char display_prefix[NAV_LABEL_MAX];
	struct action_def action;
};

struct plugin {
	struct wl_list link;
	char name[PLUGIN_NAME_MAX];
	char display_prefix[NAV_LABEL_MAX];
	char context_name[NAV_LABEL_MAX];
	bool global;
	bool enabled;
	
	bool is_builtin;
	plugin_populate_fn populate_fn;
	
	char **depends;
	size_t depends_count;
	
	bool has_provider;
	char list_cmd[NAV_CMD_MAX];
	format_t format;
	char label_field[NAV_FIELD_MAX];
	char value_field[NAV_FIELD_MAX];
	struct action_def provider_action;
	
	struct wl_list actions;
	
	bool loaded;
	bool deps_satisfied;
};

void plugin_init(void);
void plugin_destroy(void);

void plugin_register_builtin(struct plugin *plugin);
void plugin_load_directory(const char *path);
struct plugin *plugin_get(const char *name);
size_t plugin_count(void);

void plugin_set_all_enabled(bool enabled);
void plugin_set_enabled(const char *name, bool enabled);
void plugin_apply_filter(const char *filter_string);

void plugin_populate_results(struct wl_list *results);
void plugin_populate_plugin_actions(struct plugin *plugin, struct wl_list *results);
void plugin_populate_all(struct plugin *plugin, struct wl_list *results);
struct plugin *plugin_match_prefix(const char *prefix);
void plugin_run_list_cmd(const char *list_cmd, format_t format,
	const char *label_field, const char *value_field,
	struct action_def *on_select, const char *template, const char *as,
	struct wl_list *results);

#endif
