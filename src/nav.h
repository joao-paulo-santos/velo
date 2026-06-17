#ifndef NAV_H
#define NAV_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-client.h>
#include "string_vec.h"

#define NAV_KEY_MAX 32
#define NAV_VALUE_MAX 4096
#define NAV_LABEL_MAX 256
#define NAV_TEMPLATE_MAX 512
#define NAV_PROMPT_MAX 64
#define NAV_CMD_MAX 512
#define NAV_FIELD_MAX 64
#define NAV_NAME_MAX 64
#define NAV_INPUT_MAX 256

typedef enum {
	SELECTION_SELF,
	SELECTION_INPUT,
	SELECTION_SELECT,
	SELECTION_PLUGIN,
	SELECTION_FEEDBACK,
} selection_type_t;

typedef enum {
	EXECUTION_EXEC,
} execution_type_t;

typedef enum {
	FORMAT_LINES,
	FORMAT_JSON,
} format_t;

struct value_dict {
	char key[NAV_KEY_MAX];
	char value[NAV_VALUE_MAX];
	struct value_dict *next;
};

struct action_def {
	selection_type_t selection_type;
	execution_type_t execution_type;

	char as[NAV_KEY_MAX];
	char template[NAV_TEMPLATE_MAX];
	char plugin_ref[NAV_NAME_MAX];
};

struct nav_result {
	struct wl_list link;
	char label[NAV_LABEL_MAX];
	char value[NAV_VALUE_MAX];
	char source_plugin[NAV_NAME_MAX];
	struct action_def action;
};

struct feedback_entry {
	struct wl_list link;
	bool is_user;
	char content[NAV_VALUE_MAX];
};

struct nav_level {
	struct wl_list link;

	selection_type_t mode;

	struct value_dict *dict;

	execution_type_t execution_type;
	char template[NAV_TEMPLATE_MAX];

	char prompt[NAV_PROMPT_MAX];
	char as[NAV_KEY_MAX];
	char input_buffer[NAV_INPUT_MAX];
	size_t input_length;
	bool sensitive;

	char list_cmd[NAV_CMD_MAX];
	format_t format;
	char label_field[NAV_FIELD_MAX];
	char value_field[NAV_FIELD_MAX];

	char next_plugin[NAV_NAME_MAX];
	bool return_to_parent;

	char plugin_ref[NAV_NAME_MAX];

	struct wl_list results;
	struct wl_list backup_results;
	uint32_t selection;
	uint32_t first_result;

	char display_prompt[NAV_PROMPT_MAX];

	char eval_cmd[NAV_CMD_MAX];
	char display_input[NAV_TEMPLATE_MAX];
	char display_result[NAV_TEMPLATE_MAX];
	bool show_input;
	int history_limit;
	bool persist_history;
	char history_name[NAV_NAME_MAX];
	bool feedback_loading;
};

struct value_dict *dict_create(void);
struct value_dict *dict_copy(struct value_dict *src);
const char *dict_get(struct value_dict *dict, const char *key);
void dict_set(struct value_dict **dict, const char *key, const char *value);
void dict_destroy(struct value_dict *dict);

struct nav_result *nav_result_create(void);
void nav_result_destroy(struct nav_result *result);
void nav_results_destroy(struct wl_list *results);
void nav_results_copy(struct wl_list *dest, struct wl_list *src);

struct feedback_entry *feedback_entry_create(void);
void feedback_entry_destroy(struct feedback_entry *entry);
void feedback_entries_destroy(struct wl_list *entries);
void feedback_history_save(struct nav_level *level);

struct nav_level *nav_level_create(selection_type_t mode, struct value_dict *dict);
void nav_level_destroy(struct nav_level *level);

char *template_resolve(const char *template, struct value_dict *dict);

#endif
