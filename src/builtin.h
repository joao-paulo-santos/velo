#ifndef BUILTIN_H
#define BUILTIN_H

#include <stdbool.h>
#include <wayland-client.h>
#include "nav.h"

bool builtin_is_builtin(const char *cmd);

void builtin_run_list_cmd(const char *cmd, const char *template, const char *as,
	struct wl_list *results);

bool builtin_execute(const char *cmd, struct value_dict *dict);

void builtin_cleanup(void);

#endif
