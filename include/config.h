#ifndef FIRE_DASH_CONFIG_H
#define FIRE_DASH_CONFIG_H

#include "app.h"

/* Attempts to find default configuration file in current directory */
int config_find_default(char *out_path, size_t max_len);

/* Loads and parses a fire-dash or ecosystem JSON config file */
int config_load(const char *file_path,
                SystemdScope default_scope,
                AppService **out_apps,
                int *out_count,
                char *err_buf,
                size_t err_len);

/* Frees apps array returned by config_load */
void config_free(AppService *apps, int count);

#endif /* FIRE_DASH_CONFIG_H */
