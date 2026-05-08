#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "builtin.h"
#include "json.h"
#include "log.h"
#include "matching.h"
#include "plugin.h"
#include "string_vec.h"
#include "xmalloc.h"

#define MAX_LINE_LEN 1024
#define MAX_ARRAY_ITEMS 32

static struct wl_list plugins;

void plugin_init(void)
{
	wl_list_init(&plugins);
}

void plugin_register_builtin(struct plugin *plugin)
{
	plugin->is_builtin = true;
	plugin->loaded = true;
	plugin->enabled = true;
	plugin->deps_satisfied = true;
	wl_list_insert(&plugins, &plugin->link);
}

void plugin_set_all_enabled(bool enabled)
{
	struct plugin *p;
	wl_list_for_each(p, &plugins, link) {
		p->enabled = enabled;
	}
}

void plugin_set_enabled(const char *name, bool enabled)
{
	struct plugin *p = plugin_get(name);
	if (p) {
		p->enabled = enabled;
	}
}

void plugin_apply_filter(const char *filter_string)
{
	if (!filter_string || !*filter_string) {
		return;
	}
	
	plugin_set_all_enabled(false);
	
	char *copy = xstrdup(filter_string);
	char *saveptr = NULL;
	char *token = strtok_r(copy, ",", &saveptr);
	
	while (token != NULL) {
		while (*token == ' ') token++;
		
		bool is_exclude = (token[0] == '-');
		if (is_exclude) {
			token++;
		}
		
		if (strcmp(token, "all") == 0) {
			if (is_exclude) {
				plugin_set_all_enabled(false);
			} else {
				plugin_set_all_enabled(true);
			}
		} else {
			plugin_set_enabled(token, !is_exclude);
		}
		
		token = strtok_r(NULL, ",", &saveptr);
	}
	
	free(copy);
}

void plugin_destroy(void)
{
	struct plugin *p, *tmp;
	wl_list_for_each_safe(p, tmp, &plugins, link) {
		if (!p->is_builtin) {
			if (p->depends) {
				for (size_t i = 0; i < p->depends_count; i++) {
					free(p->depends[i]);
				}
				free(p->depends);
			}
			
			struct plugin_action *a, *atmp;
			wl_list_for_each_safe(a, atmp, &p->actions, link) {
				wl_list_remove(&a->link);
				action_def_destroy(a->action.on_select);
				free(a);
			}
			
			action_def_destroy(p->provider_action.on_select);
			free(p);
		}
	}
}

static char *trim(char *str)
{
	while (isspace(*str)) str++;
	if (*str == '\0') return str;
	char *end = str + strlen(str) - 1;
	while (end > str && isspace(*end)) end--;
	*(end + 1) = '\0';
	return str;
}

static char *parse_string_value(char *value)
{
	value = trim(value);
	
	if (*value == '\'') {
		value++;
		char *end = strrchr(value, '\'');
		if (end) *end = '\0';
		
		char *read = value;
		char *write = value;
		while (*read) {
			if (*read == '\'' && *(read + 1) == '\'') {
				*write++ = '\'';
				read += 2;
			} else {
				*write++ = *read++;
			}
		}
		*write = '\0';
		return value;
	}
	
	if (*value == '"') {
		value++;
		char *end = strrchr(value, '"');
		if (end) *end = '\0';
		
		char *read = value;
		char *write = value;
		while (*read) {
			if (*read == '\\' && *(read + 1)) {
				read++;
				switch (*read) {
					case 'n': *write++ = '\n'; break;
					case 't': *write++ = '\t'; break;
					case 'r': *write++ = '\r'; break;
					case '\\': *write++ = '\\'; break;
					case '"': *write++ = '"'; break;
					default: *write++ = *read; break;
				}
				read++;
			} else {
				*write++ = *read++;
			}
		}
		*write = '\0';
		return value;
	}
	
	return value;
}

static int parse_string_array(char *value, char **out, size_t max)
{
	value = trim(value);
	if (*value != '[') return 0;
	
	value++;
	int count = 0;
	char *token = strtok(value, ",]");
	
	while (token && count < (int)max) {
		token = trim(token);
		token = parse_string_value(token);
		if (*token) {
			out[count++] = xstrdup(token);
		}
		token = strtok(NULL, ",]");
	}
	
	return count;
}

static bool parse_bool_value(char *value)
{
	value = trim(value);
	return (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "1") == 0);
}

static selection_type_t parse_selection_type(const char *value)
{
	if (strcmp(value, "input") == 0) return SELECTION_INPUT;
	if (strcmp(value, "select") == 0) return SELECTION_SELECT;
	if (strcmp(value, "plugin") == 0) return SELECTION_PLUGIN;
	if (strcmp(value, "feedback") == 0) return SELECTION_FEEDBACK;
	return SELECTION_SELF;
}

static execution_type_t parse_execution_type(const char *value)
{
	if (strcmp(value, "return") == 0) return EXECUTION_RETURN;
	return EXECUTION_EXEC;
}

static format_t parse_format(const char *value)
{
	if (strcmp(value, "json") == 0) return FORMAT_JSON;
	return FORMAT_LINES;
}

static bool check_dependency(const char *binary)
{
	char *path_env = getenv("PATH");
	if (!path_env) return false;
	
	char *paths = xstrdup(path_env);
	char *dir = strtok(paths, ":");
	
	while (dir) {
		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", dir, binary);
		if (access(full_path, X_OK) == 0) {
			free(paths);
			return true;
		}
		dir = strtok(NULL, ":");
	}
	
	free(paths);
	return false;
}

static bool check_dependencies(struct plugin *plugin)
{
	for (size_t i = 0; i < plugin->depends_count; i++) {
		if (!check_dependency(plugin->depends[i])) {
			log_debug("Plugin '%s' missing dependency: %s\n", plugin->name, plugin->depends[i]);
			return false;
		}
	}
	return true;
}

static struct plugin *plugin_create(void)
{
	struct plugin *p = xcalloc(1, sizeof(*p));
	p->global = true;
	p->enabled = true;
	p->is_builtin = false;
	p->has_provider = false;
	p->loaded = false;
	p->deps_satisfied = false;
	p->depends = NULL;
	p->depends_count = 0;
	p->populate_fn = NULL;
	wl_list_init(&p->actions);
	return p;
}

static void parse_action_fields(char *key, char *value, struct action_def *action, bool is_on_select)
{
	if (strcmp(key, "selection_type") == 0) {
		action->selection_type = parse_selection_type(parse_string_value(value));
	} else if (strcmp(key, "execution_type") == 0) {
		action->execution_type = parse_execution_type(parse_string_value(value));
	} else if (strcmp(key, "as") == 0) {
		snprintf(action->as, NAV_KEY_MAX, "%s", parse_string_value(value));
	} else if (strcmp(key, "template") == 0) {
		snprintf(action->template, NAV_TEMPLATE_MAX, "%s", parse_string_value(value));
	} else if (strcmp(key, "prompt") == 0) {
		snprintf(action->prompt, NAV_PROMPT_MAX, "%s", parse_string_value(value));
	} else if (strcmp(key, "sensitive") == 0) {
		action->sensitive = parse_bool_value(value);
	} else if (strcmp(key, "list_cmd") == 0) {
		snprintf(action->list_cmd, NAV_CMD_MAX, "%s", parse_string_value(value));
	} else if (strcmp(key, "format") == 0) {
		action->format = parse_format(parse_string_value(value));
	} else if (strcmp(key, "label_field") == 0) {
		snprintf(action->label_field, NAV_FIELD_MAX, "%s", parse_string_value(value));
	} else if (strcmp(key, "value_field") == 0) {
		snprintf(action->value_field, NAV_FIELD_MAX, "%s", parse_string_value(value));
	} else if (strcmp(key, "plugin") == 0) {
		snprintf(action->plugin_ref, NAV_NAME_MAX, "%s", parse_string_value(value));
	} else if (strcmp(key, "eval_cmd") == 0) {
		snprintf(action->eval_cmd, NAV_CMD_MAX, "%s", parse_string_value(value));
	} else if (strcmp(key, "display_input") == 0) {
		snprintf(action->display_input, NAV_TEMPLATE_MAX, "%s", parse_string_value(value));
	} else if (strcmp(key, "display_result") == 0) {
		snprintf(action->display_result, NAV_TEMPLATE_MAX, "%s", parse_string_value(value));
	} else if (strcmp(key, "show_input") == 0) {
		action->show_input = parse_bool_value(value);
	} else if (strcmp(key, "history_limit") == 0) {
		action->history_limit = atoi(value);
	} else if (strcmp(key, "persist_history") == 0) {
		action->persist_history = parse_bool_value(value);
	} else if (strcmp(key, "history_name") == 0) {
		snprintf(action->history_name, NAV_NAME_MAX, "%s", parse_string_value(value));
	}
}

static struct plugin *parse_toml_file(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp) {
		log_error("Failed to open plugin file: %s\n", path);
		return NULL;
	}
	
	struct plugin *plugin = plugin_create();
	struct plugin_action *current_action = NULL;
	struct action_def *parent_action = NULL;
	char line[MAX_LINE_LEN];
	
	while (fgets(line, sizeof(line), fp)) {
		char *trimmed = trim(line);
		
		if (*trimmed == '\0' || *trimmed == '#') continue;
		
		if (strcmp(trimmed, "[[action]]") == 0) {
			current_action = xcalloc(1, sizeof(*current_action));
			current_action->action.selection_type = SELECTION_SELF;
			current_action->action.execution_type = EXECUTION_EXEC;
			current_action->action.show_input = true;
			current_action->action.history_limit = 20;
			current_action->action.persist_history = false;
			wl_list_insert(&plugin->actions, &current_action->link);
			parent_action = &current_action->action;
			continue;
		}
		
		if (strncmp(trimmed, "[action.", 8) == 0) {
			continue;
		}
		
		char *eq = strchr(trimmed, '=');
		if (!eq) continue;
		
		*eq = '\0';
		char *key = trim(trimmed);
		char *value = trim(eq + 1);
		
		bool is_on_select_field = strncmp(key, "on_select.", 10) == 0;
		if (is_on_select_field) {
			char *subkey = key + 10;
			
			if (!parent_action->on_select) {
				parent_action->on_select = action_def_create();
			}
			parse_action_fields(subkey, value, parent_action->on_select, true);
		} else if (current_action) {
			if (strcmp(key, "label") == 0) {
				snprintf(current_action->label, NAV_LABEL_MAX, "%s", parse_string_value(value));
			} else if (strcmp(key, "display_prefix") == 0) {
				snprintf(current_action->display_prefix, NAV_LABEL_MAX, "%s", parse_string_value(value));
			} else {
				parse_action_fields(key, value, &current_action->action, false);
			}
		} else {
			if (strcmp(key, "name") == 0) {
				snprintf(plugin->name, PLUGIN_NAME_MAX, "%s", parse_string_value(value));
			} else if (strcmp(key, "display_prefix") == 0) {
				snprintf(plugin->display_prefix, NAV_LABEL_MAX, "%s", parse_string_value(value));
			} else if (strcmp(key, "context_name") == 0) {
				snprintf(plugin->context_name, NAV_LABEL_MAX, "%s", parse_string_value(value));
			} else if (strcmp(key, "global") == 0) {
				plugin->global = parse_bool_value(value);
			} else if (strcmp(key, "depends") == 0) {
				plugin->depends = xcalloc(MAX_ARRAY_ITEMS, sizeof(char *));
				plugin->depends_count = parse_string_array(value, plugin->depends, MAX_ARRAY_ITEMS);
			} else if (strcmp(key, "list_cmd") == 0) {
				snprintf(plugin->list_cmd, NAV_CMD_MAX, "%s", parse_string_value(value));
				plugin->has_provider = true;
			} else if (strcmp(key, "format") == 0) {
				plugin->format = parse_format(parse_string_value(value));
			} else if (strcmp(key, "label_field") == 0) {
				snprintf(plugin->label_field, NAV_FIELD_MAX, "%s", parse_string_value(value));
			} else if (strcmp(key, "value_field") == 0) {
				snprintf(plugin->value_field, NAV_FIELD_MAX, "%s", parse_string_value(value));
			} else if (strcmp(key, "template") == 0) {
				snprintf(plugin->provider_action.template, NAV_TEMPLATE_MAX, "%s", parse_string_value(value));
			} else if (strcmp(key, "as") == 0) {
				snprintf(plugin->provider_action.as, NAV_KEY_MAX, "%s", parse_string_value(value));
			}
		}
	}
	
	fclose(fp);
	
	if (!plugin->name[0]) {
		log_error("Plugin missing name: %s\n", path);
		free(plugin);
		return NULL;
	}
	
	plugin->deps_satisfied = check_dependencies(plugin);
	plugin->loaded = true;
	
	return plugin;
}

void plugin_load_directory(const char *path)
{
	DIR *dir = opendir(path);
	if (!dir) {
		log_debug("Plugin directory not found: %s\n", path);
		return;
	}
	
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		char full_path[PLUGIN_PATH_MAX];
		snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
		
		if (entry->d_type == DT_DIR) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}
			plugin_load_directory(full_path);
			continue;
		}
		
		if (entry->d_type != DT_REG && entry->d_type != DT_LNK) continue;
		
		char *ext = strrchr(entry->d_name, '.');
		if (!ext || strcmp(ext, ".toml") != 0) continue;
		
		struct plugin *plugin = parse_toml_file(full_path);
		if (plugin) {
			wl_list_insert(&plugins, &plugin->link);
			log_debug("Loaded plugin: %s (global=%s, deps=%s)\n",
				plugin->name,
				plugin->global ? "yes" : "no",
				plugin->deps_satisfied ? "ok" : "missing");
		}
	}
	
	closedir(dir);
}

struct plugin *plugin_get(const char *name)
{
	struct plugin *p;
	wl_list_for_each(p, &plugins, link) {
		if (strcmp(p->name, name) == 0) {
			return p;
		}
	}
	return NULL;
}

size_t plugin_count(void)
{
	size_t count = 0;
	struct plugin *p;
	wl_list_for_each(p, &plugins, link) {
		count++;
	}
	return count;
}

static char *run_command(const char *cmd)
{
	FILE *fp = popen(cmd, "r");
	if (!fp) {
		return NULL;
	}
	
	char *buffer = NULL;
	size_t buffer_size = 0;
	size_t total = 0;
	char chunk[1024];
	
	while (fgets(chunk, sizeof(chunk), fp)) {
		size_t chunk_len = strlen(chunk);
		char *new_buffer = realloc(buffer, buffer_size + chunk_len + 1);
		if (!new_buffer) {
			free(buffer);
			pclose(fp);
			return NULL;
		}
		buffer = new_buffer;
		memcpy(buffer + total, chunk, chunk_len);
		total += chunk_len;
		buffer_size += chunk_len;
	}
	
	pclose(fp);
	
	if (buffer) {
		buffer[total] = '\0';
	}
	
	return buffer;
}

void plugin_populate_results(struct wl_list *results)
{
	wl_list_init(results);
	
	struct plugin *p;
	wl_list_for_each(p, &plugins, link) {
		if (!p->global || !p->enabled || !p->deps_satisfied) {
			continue;
		}
		
		if (p->is_builtin && p->populate_fn) {
			p->populate_fn(p, results);
			continue;
		}
		
		if (p->has_provider) {
			struct wl_list provider_results;
			wl_list_init(&provider_results);
			plugin_run_list_cmd(p->list_cmd, p->format, p->label_field, p->value_field,
				p->provider_action.on_select, p->provider_action.template, p->provider_action.as,
				&provider_results);
			
			struct nav_result *pr;
			wl_list_for_each(pr, &provider_results, link) {
				struct nav_result *copy = nav_result_create();
				strncpy(copy->label, pr->label, NAV_LABEL_MAX - 1);
				strncpy(copy->value, pr->value, NAV_VALUE_MAX - 1);
				strncpy(copy->source_plugin, p->name, NAV_NAME_MAX - 1);
				copy->action = pr->action;
				if (pr->action.on_select) {
					copy->action.on_select = action_def_copy(pr->action.on_select);
				}
				wl_list_insert(results, &copy->link);
			}
			nav_results_destroy(&provider_results);
		}
		
		struct plugin_action *action;
		wl_list_for_each(action, &p->actions, link) {
			struct nav_result *res = nav_result_create();
			strncpy(res->label, action->label, NAV_LABEL_MAX - 1);
			strncpy(res->value, action->label, NAV_VALUE_MAX - 1);
			strncpy(res->source_plugin, p->name, NAV_NAME_MAX - 1);
			res->action = action->action;
			if (action->action.on_select) {
				res->action.on_select = action_def_copy(action->action.on_select);
			}
			wl_list_insert(results, &res->link);
		}
	}
}

void plugin_populate_plugin_actions(struct plugin *plugin, struct wl_list *results)
{
	wl_list_init(results);
	
	if (!plugin || !plugin->deps_satisfied) {
		return;
	}
	
	struct plugin_action *action;
	wl_list_for_each(action, &plugin->actions, link) {
		struct nav_result *res = nav_result_create();
		strncpy(res->label, action->label, NAV_LABEL_MAX - 1);
		strncpy(res->value, action->label, NAV_VALUE_MAX - 1);
		strncpy(res->source_plugin, plugin->name, NAV_NAME_MAX - 1);
		res->action = action->action;
		if (action->action.on_select) {
			res->action.on_select = action_def_copy(action->action.on_select);
		}
		wl_list_insert(results, &res->link);
	}
}

void plugin_populate_all(struct plugin *plugin, struct wl_list *results)
{
	wl_list_init(results);
	
	if (!plugin) {
		return;
	}
	
	if (plugin->is_builtin && plugin->populate_fn) {
		plugin->populate_fn(plugin, results);
		return;
	}
	
	if (plugin->has_provider) {
		struct wl_list provider_results;
		wl_list_init(&provider_results);
		plugin_run_list_cmd(plugin->list_cmd, plugin->format, plugin->label_field, plugin->value_field,
			plugin->provider_action.on_select, plugin->provider_action.template, plugin->provider_action.as,
			&provider_results);
		
		struct nav_result *pr;
		wl_list_for_each(pr, &provider_results, link) {
			struct nav_result *copy = nav_result_create();
			strncpy(copy->label, pr->label, NAV_LABEL_MAX - 1);
			strncpy(copy->value, pr->value, NAV_VALUE_MAX - 1);
			strncpy(copy->source_plugin, plugin->name, NAV_NAME_MAX - 1);
			copy->action = pr->action;
			if (pr->action.on_select) {
				copy->action.on_select = action_def_copy(pr->action.on_select);
			}
			wl_list_insert(results, &copy->link);
		}
		nav_results_destroy(&provider_results);
	}
	
	struct plugin_action *action;
	wl_list_for_each(action, &plugin->actions, link) {
		struct nav_result *res = nav_result_create();
		strncpy(res->label, action->label, NAV_LABEL_MAX - 1);
		strncpy(res->value, action->label, NAV_VALUE_MAX - 1);
		strncpy(res->source_plugin, plugin->name, NAV_NAME_MAX - 1);
		res->action = action->action;
		if (action->action.on_select) {
			res->action.on_select = action_def_copy(action->action.on_select);
		}
		wl_list_insert(results, &res->link);
	}
}

struct plugin *plugin_match_prefix(const char *prefix)
{
	if (!prefix || !prefix[0]) {
		return NULL;
	}
	
	size_t prefix_len = strlen(prefix);
	struct plugin *p;
	wl_list_for_each(p, &plugins, link) {
		if (!p->deps_satisfied) {
			continue;
		}
		if (strncasecmp(p->name, prefix, prefix_len) == 0) {
			return p;
		}
	}
	return NULL;
}

static bool parse_action_from_json(json_parser_t *p, struct action_def *action)
{
	if (!json_object_begin(p)) {
		return false;
	}
	
	char key[256];
	bool has_more;
	
	while (json_object_next(p, key, sizeof(key), &has_more) && has_more) {
		if (strcmp(key, "selection_type") == 0) {
			char val[32];
			if (json_parse_string(p, val, sizeof(val))) {
				action->selection_type = parse_selection_type(val);
			}
		} else if (strcmp(key, "execution_type") == 0) {
			char val[32];
			if (json_parse_string(p, val, sizeof(val))) {
				action->execution_type = parse_execution_type(val);
			}
		} else if (strcmp(key, "template") == 0) {
			json_parse_string(p, action->template, NAV_TEMPLATE_MAX);
		} else if (strcmp(key, "as") == 0) {
			json_parse_string(p, action->as, NAV_KEY_MAX);
		} else if (strcmp(key, "prompt") == 0) {
			json_parse_string(p, action->prompt, NAV_PROMPT_MAX);
		} else if (strcmp(key, "sensitive") == 0) {
			json_parse_bool(p, &action->sensitive);
		} else if (strcmp(key, "list_cmd") == 0) {
			json_parse_string(p, action->list_cmd, NAV_CMD_MAX);
		} else if (strcmp(key, "format") == 0) {
			char val[32];
			if (json_parse_string(p, val, sizeof(val))) {
				action->format = parse_format(val);
			}
		} else if (strcmp(key, "label_field") == 0) {
			json_parse_string(p, action->label_field, NAV_FIELD_MAX);
		} else if (strcmp(key, "value_field") == 0) {
			json_parse_string(p, action->value_field, NAV_FIELD_MAX);
		} else if (strcmp(key, "plugin") == 0) {
			json_parse_string(p, action->plugin_ref, NAV_NAME_MAX);
		} else if (strcmp(key, "on_select") == 0) {
			action->on_select = action_def_create();
			if (!parse_action_from_json(p, action->on_select)) {
				action_def_destroy(action->on_select);
				action->on_select = NULL;
			}
		} else if (strcmp(key, "eval_cmd") == 0) {
			json_parse_string(p, action->eval_cmd, NAV_CMD_MAX);
		} else if (strcmp(key, "display_input") == 0) {
			json_parse_string(p, action->display_input, NAV_TEMPLATE_MAX);
		} else if (strcmp(key, "display_result") == 0) {
			json_parse_string(p, action->display_result, NAV_TEMPLATE_MAX);
		} else if (strcmp(key, "show_input") == 0) {
			json_parse_bool(p, &action->show_input);
		} else if (strcmp(key, "history_limit") == 0) {
			long val;
			if (json_parse_int(p, &val)) {
				action->history_limit = (int)val;
			}
		} else if (strcmp(key, "persist_history") == 0) {
			json_parse_bool(p, &action->persist_history);
		} else if (strcmp(key, "history_name") == 0) {
			json_parse_string(p, action->history_name, NAV_NAME_MAX);
		} else {
			json_skip_value(p);
		}
		
		if (json_peek_char(p, ',')) {
			json_expect_char(p, ',');
		}
	}
	
	json_object_end(p);
	return true;
}

void plugin_run_list_cmd(const char *list_cmd, format_t format,
	const char *label_field, const char *value_field,
	struct action_def *on_select, const char *template, const char *as,
	struct wl_list *results)
{
	wl_list_init(results);
	
	if (builtin_is_builtin(list_cmd)) {
		builtin_run_list_cmd(list_cmd, results);
		return;
	}
	
	char *output = run_command(list_cmd);
	if (!output) {
		return;
	}
	
	if (format == FORMAT_LINES) {
		char *line = strtok(output, "\n");
		while (line) {
			char *trimmed = trim(line);
			if (*trimmed) {
				struct nav_result *res = nav_result_create();
				strncpy(res->label, trimmed, NAV_LABEL_MAX - 1);
				strncpy(res->value, trimmed, NAV_VALUE_MAX - 1);
				res->action.selection_type = SELECTION_SELF;
				res->action.execution_type = EXECUTION_EXEC;
				
				if (on_select) {
					res->action = *on_select;
					if (on_select->on_select) {
						res->action.on_select = action_def_copy(on_select->on_select);
					}
				} else {
					if (template) {
						strncpy(res->action.template, template, NAV_TEMPLATE_MAX - 1);
					}
					if (as) {
						strncpy(res->action.as, as, NAV_KEY_MAX - 1);
					}
				}
				
				wl_list_insert(results, &res->link);
			}
			line = strtok(NULL, "\n");
		}
	} else if (format == FORMAT_JSON) {
		json_parser_t parser;
		json_parser_init(&parser, output);
		
		if (json_peek_char(&parser, '[')) {
			if (!json_array_begin(&parser)) {
				free(output);
				return;
			}
			
			bool has_more;
			while (json_array_next(&parser, &has_more) && has_more) {
				if (!json_object_begin(&parser)) break;
				
				char label_val[NAV_LABEL_MAX] = "";
				char value_val[NAV_LABEL_MAX] = "";
				bool has_json_action = false;
				struct action_def json_action = {
					.selection_type = SELECTION_SELF,
					.execution_type = EXECUTION_EXEC,
					.show_input = true,
					.history_limit = 20,
				};
				
				char key[256];
				bool obj_has_more;
				while (json_object_next(&parser, key, sizeof(key), &obj_has_more) && obj_has_more) {
					if (strcmp(key, label_field) == 0) {
						json_parse_string(&parser, label_val, sizeof(label_val));
					} else if (strcmp(key, value_field) == 0) {
						json_parse_string(&parser, value_val, sizeof(value_val));
					} else if (strcmp(key, "action") == 0) {
						has_json_action = parse_action_from_json(&parser, &json_action);
					} else {
						json_skip_value(&parser);
					}
					if (json_peek_char(&parser, ',')) {
						json_expect_char(&parser, ',');
					}
				}
				
				json_object_end(&parser);
				
				if (json_peek_char(&parser, ',')) {
					json_expect_char(&parser, ',');
				}
				
				if (label_val[0]) {
					struct nav_result *res = nav_result_create();
					strncpy(res->label, label_val, NAV_LABEL_MAX - 1);
					strncpy(res->value, value_val[0] ? value_val : label_val, NAV_VALUE_MAX - 1);
					
					if (has_json_action) {
						// Use JSON-provided action
						res->action = json_action;
						if (json_action.on_select) {
							res->action.on_select = action_def_copy(json_action.on_select);
						}
					} else if (on_select) {
						res->action = *on_select;
						if (on_select->on_select) {
							res->action.on_select = action_def_copy(on_select->on_select);
						}
					} else {
						res->action.selection_type = SELECTION_SELF;
						res->action.execution_type = EXECUTION_EXEC;
						if (template) {
							strncpy(res->action.template, template, NAV_TEMPLATE_MAX - 1);
						}
						if (as) {
							strncpy(res->action.as, as, NAV_KEY_MAX - 1);
						}
					}
					
					wl_list_insert(results, &res->link);
				}
			}
			
			json_array_end(&parser);
		} else {
			while (*parser.pos) {
				json_skip_ws(&parser);
				if (!*parser.pos) break;
				
				if (!json_object_begin(&parser)) break;
				
				char label_val[NAV_LABEL_MAX] = "";
				char value_val[NAV_LABEL_MAX] = "";
				
				char key[256];
				bool obj_has_more;
				while (json_object_next(&parser, key, sizeof(key), &obj_has_more) && obj_has_more) {
					if (strcmp(key, label_field) == 0) {
						json_parse_string(&parser, label_val, sizeof(label_val));
					} else if (strcmp(key, value_field) == 0) {
						json_parse_string(&parser, value_val, sizeof(value_val));
					} else {
						json_skip_value(&parser);
					}
					if (json_peek_char(&parser, ',')) {
						json_expect_char(&parser, ',');
					}
				}
				
				json_object_end(&parser);
				
				if (label_val[0]) {
					struct nav_result *res = nav_result_create();
					strncpy(res->label, label_val, NAV_LABEL_MAX - 1);
					strncpy(res->value, value_val[0] ? value_val : label_val, NAV_VALUE_MAX - 1);
					res->action.selection_type = SELECTION_SELF;
					res->action.execution_type = EXECUTION_EXEC;
					
					if (on_select) {
						res->action = *on_select;
						if (on_select->on_select) {
							res->action.on_select = action_def_copy(on_select->on_select);
						}
					} else {
						if (template) {
							strncpy(res->action.template, template, NAV_TEMPLATE_MAX - 1);
						}
						if (as) {
							strncpy(res->action.as, as, NAV_KEY_MAX - 1);
						}
					}
					
					wl_list_insert(results, &res->link);
				}
			}
		}
	}
	
	free(output);
}
