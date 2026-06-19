#ifndef VELO_CONFIG_H
#define VELO_CONFIG_H

#include <stdbool.h>
#include "velo.h"

void config_load(struct velo *velo, const char *filename);
void config_load_theme(struct velo *velo);
bool config_apply(struct velo *velo, const char *option, const char *value);
void config_fixup_values(struct velo *velo);
void config_list_themes(void);
void config_seed_if_needed(void);
char *get_user_config_dir(void);

#endif /* VELO_CONFIG_H */
