#ifndef FIRE_DASH_UNIT_GEN_H
#define FIRE_DASH_UNIT_GEN_H

#include "app.h"

/* Resolves unit directory path for user or system scope */
int unit_gen_get_dir(SystemdScope scope, char *out_path, size_t max_len);

/* Writes fire-<name>.service file */
int unit_gen_create_service(const AppService *app, char *err_buf, size_t err_len);

/* Writes fire-<name>.path file if watch path is defined */
int unit_gen_create_path(const AppService *app, char *err_buf, size_t err_len);

/* Deletes both .service and .path files for a given app */
int unit_gen_delete(const char *name, SystemdScope scope, char *err_buf, size_t err_len);

/* Scans unit directory and returns list of fire-<name> service names */
int unit_gen_list_names(SystemdScope scope, char ***out_names, int *out_count);

/* Frees name list allocated by unit_gen_list_names */
void unit_gen_free_names(char **names, int count);

/* Checks if a watch unit exists for the given service */
bool unit_gen_has_watch(const char *name, SystemdScope scope, char *out_watch_path, size_t max_len);

#endif /* FIRE_DASH_UNIT_GEN_H */
