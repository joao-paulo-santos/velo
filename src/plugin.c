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

static int parse_children_array(char *value, char out[][PLUGIN_NAME_MAX], size_t max)
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
			strncpy(out[count], token, PLUGIN_NAME_MAX - 1);
			count++;
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

static plugin_type_t parse_plugin_type(const char *value)
{
	if (strcmp(value, "list") == 0) return PLUGIN_LIST;
	if (strcmp(value, "select") == 0) return PLUGIN_SELECT;
	if (strcmp(value, "input") == 0) return PLUGIN_INPUT;
	if (strcmp(value, "feedback") == 0) return PLUGIN_FEEDBACK;
	if (strcmp(value, "exec") == 0) return PLUGIN_EXEC;
	return PLUGIN_LIST;
}

static execution_type_t parse_execution_type(const char *value)
{
	(void)value;
	return EXECUTION_EXEC;
}

static format_t parse_format(const char *value)
{
	if (strcmp(value, "json") == 0) return FORMAT_JSON;
	return FORMAT_LINES;
}

static const char *xdg_bin_dir(void)
{
	const char *xdg = getenv("XDG_BIN_HOME");
	if (xdg) return xdg;
	const char *home = getenv("HOME");
	if (!home) return NULL;
	static char buf[512];
	snprintf(buf, sizeof(buf), "%s/.local/bin", home);
	return buf;
}

static bool check_dependency(const char *binary)
{
	char *path_env = getenv("PATH");
	if (path_env) {
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
	}

	const char *bin_dir = xdg_bin_dir();
	if (bin_dir) {
		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", bin_dir, binary);
		if (access(full_path, X_OK) == 0) {
			return true;
		}
	}

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
	p->type = PLUGIN_LIST;
	p->global = false;
	p->enabled = true;
	p->is_builtin = false;
	p->deps_satisfied = false;
	p->depends = NULL;
	p->depends_count = 0;
	p->populate_fn = NULL;
	p->execution_type = EXECUTION_EXEC;
	p->show_input = true;
	p->history_limit = 20;
	p->persist_history = false;
	return p;
}

static bool finalize_plugin(struct plugin *plugin, const char *path)
{
	if (!plugin->name[0]) {
		log_error("Plugin missing name: %s\n", path);
		free(plugin);
		return false;
	}
	plugin->deps_satisfied = check_dependencies(plugin);
	wl_list_insert(plugins.prev, &plugin->link);
	log_debug("Loaded plugin: %s (type=%d, global=%s, deps=%s)\n",
			plugin->name,
			plugin->type,
			plugin->global ? "yes" : "no",
			plugin->deps_satisfied ? "ok" : "missing");
	return true;
}

static int parse_toml_file(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp) {
		log_error("Failed to open plugin file: %s\n", path);
		return 0;
	}

	int count = 0;
	struct plugin *plugin = plugin_create();
	char line[MAX_LINE_LEN];

	while (fgets(line, sizeof(line), fp)) {
		char *trimmed = trim(line);

		if (*trimmed == '\0' || *trimmed == '#') continue;

		if (strcmp(trimmed, "---") == 0) {
			if (finalize_plugin(plugin, path)) count++;
			plugin = plugin_create();
			continue;
		}

		char *eq = strchr(trimmed, '=');
		if (!eq) continue;

		*eq = '\0';
		char *key = trim(trimmed);
		char *value = trim(eq + 1);

		if (strcmp(key, "name") == 0) {
			snprintf(plugin->name, PLUGIN_NAME_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "display_label") == 0) {
			snprintf(plugin->display_label, NAV_LABEL_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "display_prefix") == 0) {
			snprintf(plugin->display_prefix, NAV_LABEL_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "context_name") == 0) {
			snprintf(plugin->context_name, NAV_LABEL_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "global") == 0) {
			plugin->global = parse_bool_value(value);
		} else if (strcmp(key, "depends") == 0) {
			plugin->depends = xcalloc(MAX_ARRAY_ITEMS, sizeof(char *));
			plugin->depends_count = parse_string_array(value, plugin->depends, MAX_ARRAY_ITEMS);
		} else if (strcmp(key, "type") == 0) {
			plugin->type = parse_plugin_type(parse_string_value(value));
		} else if (strcmp(key, "children") == 0) {
			plugin->children_count = parse_children_array(value, plugin->children, MAX_CHILDREN);
		} else if (strcmp(key, "template") == 0) {
			snprintf(plugin->template, NAV_TEMPLATE_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "as") == 0) {
			snprintf(plugin->as, NAV_KEY_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "execution_type") == 0) {
			plugin->execution_type = parse_execution_type(parse_string_value(value));
		} else if (strcmp(key, "list_cmd") == 0) {
			snprintf(plugin->list_cmd, NAV_CMD_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "format") == 0) {
			plugin->format = parse_format(parse_string_value(value));
		} else if (strcmp(key, "label_field") == 0) {
			snprintf(plugin->label_field, NAV_FIELD_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "value_field") == 0) {
			snprintf(plugin->value_field, NAV_FIELD_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "prompt") == 0) {
			snprintf(plugin->prompt, NAV_PROMPT_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "sensitive") == 0) {
			plugin->sensitive = parse_bool_value(value);
		} else if (strcmp(key, "next") == 0) {
			snprintf(plugin->next, PLUGIN_NAME_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "return") == 0) {
			plugin->return_to_parent = parse_bool_value(value);
		} else if (strcmp(key, "teleport") == 0) {
			plugin->teleport = parse_bool_value(value);
		} else if (strcmp(key, "eval_cmd") == 0) {
			snprintf(plugin->eval_cmd, NAV_CMD_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "display_input") == 0) {
			snprintf(plugin->display_input, NAV_TEMPLATE_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "display_result") == 0) {
			snprintf(plugin->display_result, NAV_TEMPLATE_MAX, "%s", parse_string_value(value));
		} else if (strcmp(key, "show_input") == 0) {
			plugin->show_input = parse_bool_value(value);
		} else if (strcmp(key, "history_limit") == 0) {
			plugin->history_limit = atoi(value);
		} else if (strcmp(key, "persist_history") == 0) {
			plugin->persist_history = parse_bool_value(value);
		} else if (strcmp(key, "history_name") == 0) {
			snprintf(plugin->history_name, NAV_NAME_MAX, "%s", parse_string_value(value));
		}
	}

	fclose(fp);

	if (finalize_plugin(plugin, path)) count++;

	return count;
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

		parse_toml_file(full_path);
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

static char *resolve_command(const char *cmd)
{
	const char *space = strchr(cmd, ' ');
	size_t bin_len = space ? (size_t)(space - cmd) : strlen(cmd);

	char *path_env = getenv("PATH");
	if (path_env) {
		char *paths = xstrdup(path_env);
		char *dir = strtok(paths, ":");
		while (dir) {
			char full_path[512];
			snprintf(full_path, sizeof(full_path), "%s/%.*s", dir, (int)bin_len, cmd);
			if (access(full_path, X_OK) == 0) {
				free(paths);
				char *resolved = xmalloc(strlen(full_path) + strlen(cmd + bin_len) + 1);
				sprintf(resolved, "%s%s", full_path, cmd + bin_len);
				return resolved;
			}
			dir = strtok(NULL, ":");
		}
		free(paths);
	}

	const char *bin_dir = xdg_bin_dir();
	if (bin_dir) {
		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%.*s", bin_dir, (int)bin_len, cmd);
		if (access(full_path, X_OK) == 0) {
			char *resolved = xmalloc(strlen(full_path) + strlen(cmd + bin_len) + 1);
			sprintf(resolved, "%s%s", full_path, cmd + bin_len);
			return resolved;
		}
	}

	return xstrdup(cmd);
}

static char *run_command(const char *cmd)
{
	char *resolved = resolve_command(cmd);
	FILE *fp = popen(resolved, "r");
	free(resolved);
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

char *plugin_resolve_command(const char *cmd)
{
	return resolve_command(cmd);
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

		struct nav_result *res = nav_result_create();
		const char *label = p->display_label[0] ? p->display_label : p->name;
		strncpy(res->label, label, NAV_LABEL_MAX - 1);
		strncpy(res->value, label, NAV_VALUE_MAX - 1);
		strncpy(res->source_plugin, p->name, NAV_NAME_MAX - 1);
		res->action.selection_type = SELECTION_PLUGIN;
		res->action.execution_type = EXECUTION_EXEC;
		strncpy(res->action.plugin_ref, p->name, NAV_NAME_MAX - 1);
		wl_list_insert(results->prev, &res->link);
	}
}

void plugin_populate_from_children(struct plugin *plugin, struct wl_list *results)
{
	wl_list_init(results);

	for (size_t i = 0; i < plugin->children_count; i++) {
		struct plugin *child = plugin_get(plugin->children[i]);
		if (!child || !child->deps_satisfied) continue;
		struct nav_result *res = nav_result_create();
		const char *label = child->display_label[0] ? child->display_label : child->name;
		strncpy(res->label, label, NAV_LABEL_MAX - 1);
		strncpy(res->value, label, NAV_VALUE_MAX - 1);
		strncpy(res->source_plugin, child->name, NAV_NAME_MAX - 1);
		res->action.selection_type = SELECTION_PLUGIN;
		res->action.execution_type = EXECUTION_EXEC;
		strncpy(res->action.plugin_ref, child->name, NAV_NAME_MAX - 1);
		wl_list_insert(results->prev, &res->link);
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
		if (!p->deps_satisfied || !p->teleport) {
			continue;
		}
		if (strncasecmp(p->name, prefix, prefix_len) == 0) {
			return p;
		}
	}
	return NULL;
}

void plugin_run_list_cmd(const char *list_cmd, format_t format,
	const char *label_field, const char *value_field,
	const char *template, const char *as,
	struct wl_list *results)
{
	wl_list_init(results);

	if (builtin_is_builtin(list_cmd)) {
		builtin_run_list_cmd(list_cmd, template, as, results);
		return;
	}

	char *output = run_command(list_cmd);
	if (!output) {
		return;
	}

	if (format == FORMAT_LINES) {
		char *line = strtok(output, "\n");
		while (line) {
			char *trimmed_line = trim(line);
			if (*trimmed_line) {
				struct nav_result *res = nav_result_create();
				strncpy(res->label, trimmed_line, NAV_LABEL_MAX - 1);
				strncpy(res->value, trimmed_line, NAV_VALUE_MAX - 1);
				res->action.selection_type = SELECTION_SELF;
				res->action.execution_type = EXECUTION_EXEC;
				if (template) {
					strncpy(res->action.template, template, NAV_TEMPLATE_MAX - 1);
				}
				if (as) {
					strncpy(res->action.as, as, NAV_KEY_MAX - 1);
				}
				wl_list_insert(results->prev, &res->link);
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

				if (json_peek_char(&parser, ',')) {
					json_expect_char(&parser, ',');
				}

				if (label_val[0]) {
					struct nav_result *res = nav_result_create();
					strncpy(res->label, label_val, NAV_LABEL_MAX - 1);
					strncpy(res->value, value_val[0] ? value_val : label_val, NAV_VALUE_MAX - 1);
					res->action.selection_type = SELECTION_SELF;
					res->action.execution_type = EXECUTION_EXEC;
					if (template) {
						strncpy(res->action.template, template, NAV_TEMPLATE_MAX - 1);
					}
					if (as) {
						strncpy(res->action.as, as, NAV_KEY_MAX - 1);
					}
					wl_list_insert(results->prev, &res->link);
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
					if (template) {
						strncpy(res->action.template, template, NAV_TEMPLATE_MAX - 1);
					}
					if (as) {
						strncpy(res->action.as, as, NAV_KEY_MAX - 1);
					}
					wl_list_insert(results, &res->link);
				}
			}
		}
	}

	free(output);
}
