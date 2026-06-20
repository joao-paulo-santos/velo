#ifndef PLUGIN_H
#define PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-client.h>
#include "nav.h"
#include "string_vec.h"

#define PLUGIN_NAME_MAX 64
#define PLUGIN_PATH_MAX 256
#define MAX_CHILDREN 32

typedef enum {
	PLUGIN_LIST,
	PLUGIN_SELECT,
	PLUGIN_INPUT,
	PLUGIN_PREVIEW,
	PLUGIN_EXEC,
} plugin_type_t;

struct plugin;
struct wl_list;

typedef void (*plugin_populate_fn)(struct plugin *plugin, struct wl_list *results);

struct plugin {
	struct wl_list link;

	char name[PLUGIN_NAME_MAX];
	char display_label[NAV_LABEL_MAX];
	char display_prefix[NAV_LABEL_MAX];
	char context_name[NAV_LABEL_MAX];
	bool global;

	char **depends;
	size_t depends_count;

	plugin_type_t type;

	char children[MAX_CHILDREN][PLUGIN_NAME_MAX];
	size_t children_count;

	char template[NAV_TEMPLATE_MAX];
	char as[NAV_KEY_MAX];
	execution_type_t execution_type;

	char list_cmd[NAV_CMD_MAX];
	format_t format;
	char label_field[NAV_FIELD_MAX];
	char value_field[NAV_FIELD_MAX];

	char prompt[NAV_PROMPT_MAX];
	bool sensitive;

	char next[PLUGIN_NAME_MAX];
	bool return_to_parent;

	bool teleport;

	char eval_cmd[NAV_CMD_MAX];
	char copy_cmd[NAV_CMD_MAX];

	bool is_builtin;
	plugin_populate_fn populate_fn;

	bool enabled;
	bool deps_satisfied;
};

void plugin_init(void);
void plugin_destroy(void);

void plugin_load_directory(const char *path);
struct plugin *plugin_get(const char *name);
size_t plugin_count(void);

void plugin_set_all_enabled(bool enabled);
void plugin_set_enabled(const char *name, bool enabled);
void plugin_apply_filter(const char *filter_string);

void plugin_populate_results(struct wl_list *results);
void plugin_populate_from_children(struct plugin *plugin, struct wl_list *results);
struct plugin *plugin_match_prefix(const char *prefix);
void plugin_run_list_cmd(const char *list_cmd, format_t format,
	const char *label_field, const char *value_field,
	const char *template, const char *as,
	struct wl_list *results);
char *plugin_resolve_command(const char *cmd);

#endif
