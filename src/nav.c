#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "nav.h"
#include "xmalloc.h"

struct value_dict *dict_create(void)
{
	return NULL;
}

struct value_dict *dict_copy(struct value_dict *src)
{
	if (!src) {
		return NULL;
	}

	struct value_dict *head = NULL;
	struct value_dict *tail = NULL;
	struct value_dict *curr = src;

	while (curr) {
		struct value_dict *entry = xcalloc(1, sizeof(*entry));
		strncpy(entry->key, curr->key, NAV_KEY_MAX - 1);
		strncpy(entry->value, curr->value, NAV_VALUE_MAX - 1);
		entry->next = NULL;

		if (!head) {
			head = entry;
		} else {
			tail->next = entry;
		}
		tail = entry;
		curr = curr->next;
	}

	return head;
}

const char *dict_get(struct value_dict *dict, const char *key)
{
	struct value_dict *curr = dict;
	while (curr) {
		if (strcmp(curr->key, key) == 0) {
			return curr->value;
		}
		curr = curr->next;
	}
	return NULL;
}

void dict_set(struct value_dict **dict, const char *key, const char *value)
{
	if (!*dict) {
		struct value_dict *entry = xcalloc(1, sizeof(*entry));
		strncpy(entry->key, key, NAV_KEY_MAX - 1);
		strncpy(entry->value, value, NAV_VALUE_MAX - 1);
		entry->next = NULL;
		*dict = entry;
		return;
	}

	struct value_dict *curr = *dict;
	while (curr) {
		if (strcmp(curr->key, key) == 0) {
			strncpy(curr->value, value, NAV_VALUE_MAX - 1);
			return;
		}
		if (!curr->next) {
			break;
		}
		curr = curr->next;
	}

	struct value_dict *entry = xcalloc(1, sizeof(*entry));
	strncpy(entry->key, key, NAV_KEY_MAX - 1);
	strncpy(entry->value, value, NAV_VALUE_MAX - 1);
	entry->next = NULL;

	if (curr) {
		curr->next = entry;
	}
}

void dict_destroy(struct value_dict *dict)
{
	struct value_dict *curr = dict;
	while (curr) {
		struct value_dict *next = curr->next;
		free(curr);
		curr = next;
	}
}

struct action_def *action_def_create(void)
{
	struct action_def *action = xcalloc(1, sizeof(*action));
	action->selection_type = SELECTION_SELF;
	action->execution_type = EXECUTION_EXEC;
	action->show_input = true;
	action->history_limit = 20;
	action->persist_history = false;
	return action;
}

struct action_def *action_def_copy(struct action_def *src)
{
	if (!src) {
		return NULL;
	}

	struct action_def *copy = xcalloc(1, sizeof(*copy));
	*copy = *src;
	return copy;
}

void action_def_destroy(struct action_def *action)
{
	if (!action) {
		return;
	}
	free(action);
}

struct nav_result *nav_result_create(void)
{
	struct nav_result *result = xcalloc(1, sizeof(*result));
	return result;
}

void nav_result_destroy(struct nav_result *result)
{
	if (!result) {
		return;
	}
	free(result);
}

void nav_results_destroy(struct wl_list *results)
{
	if (!results || results->prev == NULL || results->next == NULL) {
		return;
	}

	struct nav_result *result, *tmp;
	wl_list_for_each_safe(result, tmp, results, link) {
		wl_list_remove(&result->link);
		nav_result_destroy(result);
	}
}

struct nav_result *nav_results_copy_single(struct nav_result *src)
{
	if (!src) {
		return NULL;
	}

	struct nav_result *copy = nav_result_create();
	strncpy(copy->label, src->label, NAV_LABEL_MAX - 1);
	strncpy(copy->value, src->value, NAV_VALUE_MAX - 1);
	copy->action = src->action;

	return copy;
}

void nav_results_copy(struct wl_list *dest, struct wl_list *src)
{
	wl_list_init(dest);
	struct nav_result *res;
	wl_list_for_each(res, src, link) {
		struct nav_result *copy = nav_result_create();
		strncpy(copy->label, res->label, NAV_LABEL_MAX - 1);
		strncpy(copy->value, res->value, NAV_VALUE_MAX - 1);
		copy->action = res->action;
		wl_list_insert(dest, &copy->link);
	}
}

struct nav_level *nav_level_create(selection_type_t mode, struct value_dict *dict)
{
	struct nav_level *level = xcalloc(1, sizeof(*level));
	level->mode = mode;
	level->dict = dict_copy(dict);
	level->execution_type = EXECUTION_EXEC;
	level->input_buffer[0] = '\0';
	level->input_length = 0;
	level->selection = 0;
	level->first_result = 0;
	level->show_input = true;
	level->history_limit = 20;
	level->persist_history = false;
	level->feedback_loading = false;
	wl_list_init(&level->results);
	wl_list_init(&level->backup_results);
	return level;
}

void nav_level_destroy(struct nav_level *level)
{
	if (!level) {
		return;
	}

	dict_destroy(level->dict);

	if (level->mode == SELECTION_FEEDBACK) {
		feedback_entries_destroy(&level->results);
		feedback_entries_destroy(&level->backup_results);
	} else {
		nav_results_destroy(&level->results);
		nav_results_destroy(&level->backup_results);
	}
	free(level);
}

struct feedback_entry *feedback_entry_create(void)
{
	struct feedback_entry *entry = xcalloc(1, sizeof(*entry));
	entry->is_user = false;
	entry->content[0] = '\0';
	return entry;
}

void feedback_entry_destroy(struct feedback_entry *entry)
{
	if (entry) {
		free(entry);
	}
}

void feedback_entries_destroy(struct wl_list *entries)
{
	if (!entries || entries->prev == NULL || entries->next == NULL) {
		return;
	}

	struct feedback_entry *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, entries, link) {
		wl_list_remove(&entry->link);
		feedback_entry_destroy(entry);
	}
}

char *template_resolve(const char *template, struct value_dict *dict)
{
	if (!template) {
		return NULL;
	}

	size_t template_len = strlen(template);
	char *result = xcalloc(1, template_len * 2 + NAV_VALUE_MAX + 1);
	size_t result_len = 0;
	size_t i = 0;

	while (template[i]) {
		if (template[i] == '{') {
			char key[NAV_KEY_MAX];
			size_t key_idx = 0;
			i++;

			while (template[i] && template[i] != '}' && key_idx < NAV_KEY_MAX - 1) {
				key[key_idx++] = template[i++];
			}
			key[key_idx] = '\0';

			if (template[i] == '}') {
				i++;
			}

			const char *value = dict_get(dict, key);
			if (value) {
				size_t value_len = strlen(value);
				memcpy(result + result_len, value, value_len);
				result_len += value_len;
			}
		} else {
			result[result_len++] = template[i++];
		}
	}

	result[result_len] = '\0';
	return result;
}
