#include <string.h>
#include "builtin.h"
#include "drun.h"
#include "log.h"
#include "nav.h"
#include "xmalloc.h"

static struct desktop_vec cached_apps = {0};
static bool apps_loaded = false;

bool builtin_is_builtin(const char *cmd)
{
	return cmd && cmd[0] == '@';
}

static void ensure_apps_loaded(void)
{
	if (!apps_loaded) {
		cached_apps = drun_generate_cached();
		apps_loaded = true;
	}
}

static void builtin_list_apps(struct wl_list *results)
{
	ensure_apps_loaded();

	for (size_t i = 0; i < cached_apps.count; i++) {
		struct desktop_entry *app = &cached_apps.buf[i];

		struct nav_result *res = nav_result_create();
		strncpy(res->label, app->name, NAV_LABEL_MAX - 1);
		strncpy(res->value, app->id, NAV_VALUE_MAX - 1);
		res->action.selection_type = SELECTION_SELF;
		res->action.execution_type = EXECUTION_EXEC;
		snprintf(res->action.template, NAV_TEMPLATE_MAX - 1, "@launch %s", app->id);

		wl_list_insert(results->prev, &res->link);
	}
}

void builtin_run_list_cmd(const char *cmd, const char *template, const char *as,
	struct wl_list *results)
{
	if (!cmd || !cmd[0]) {
		return;
	}

	if (strcmp(cmd, "@apps") == 0) {
		builtin_list_apps(results);
		if (template || as) {
			struct nav_result *res;
			wl_list_for_each(res, results, link) {
				if (template) {
					strncpy(res->action.template, template, NAV_TEMPLATE_MAX - 1);
				}
				if (as) {
					strncpy(res->action.as, as, NAV_KEY_MAX - 1);
				}
			}
		}
		return;
	}

	log_error("Unknown builtin list command: %s\n", cmd);
}

static bool builtin_launch_app(const char *app_id)
{
	ensure_apps_loaded();

	for (size_t i = 0; i < cached_apps.count; i++) {
		struct desktop_entry *app = &cached_apps.buf[i];
		if (strcmp(app->id, app_id) == 0) {
			drun_launch(app->path);
			return true;
		}
	}

	log_error("App not found: %s\n", app_id);
	return false;
}

bool builtin_execute(const char *cmd, struct value_dict *dict)
{
	(void)dict;

	if (!cmd || !cmd[0]) {
		return false;
	}

	if (strncmp(cmd, "@launch ", 8) == 0) {
		const char *app_id = cmd + 8;
		return builtin_launch_app(app_id);
	}

	log_error("Unknown builtin execute command: %s\n", cmd);
	return false;
}

void builtin_cleanup(void)
{
	if (apps_loaded) {
		desktop_vec_destroy(&cached_apps);
		apps_loaded = false;
	}
}
