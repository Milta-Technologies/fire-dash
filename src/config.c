#include "config.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

int config_find_default(char *out_path, size_t max_len) {
    static const char *candidates[] = {
        "fire.config.json",
        "fire.json",
        "ecosystem.config.json",
        NULL
    };

    for (int i = 0; candidates[i] != NULL; i++) {
        if (access(candidates[i], R_OK) == 0) {
            snprintf(out_path, max_len, "%s", candidates[i]);
            return 0;
        }
    }
    return -1;
}

static char *read_file_to_string(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (length < 0 || length > 10 * 1024 * 1024) { /* 10 MB limit */
        fclose(fp);
        return NULL;
    }

    char *buffer = malloc(length + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }

    size_t read_bytes = fread(buffer, 1, length, fp);
    buffer[read_bytes] = '\0';
    fclose(fp);
    return buffer;
}

static int parse_app_item(cJSON *item, SystemdScope default_scope, AppService *app, char *err_buf, size_t err_len) {
    memset(app, 0, sizeof(*app));
    app->scope = default_scope;
    app->restart_sec = 2;

    /* cwd default to current working directory */
    if (!getcwd(app->cwd, sizeof(app->cwd))) {
        snprintf(app->cwd, sizeof(app->cwd), ".");
    }

    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(item, "name");
    if (!name_item || !cJSON_IsString(name_item) || !name_item->valuestring[0]) {
        if (err_buf) snprintf(err_buf, err_len, "Missing or invalid 'name' in app config");
        return -1;
    }
    snprintf(app->name, sizeof(app->name), "%s", name_item->valuestring);

    cJSON *script_item = cJSON_GetObjectItemCaseSensitive(item, "script");
    if (!script_item) {
        script_item = cJSON_GetObjectItemCaseSensitive(item, "exec");
    }
    if (!script_item || !cJSON_IsString(script_item) || !script_item->valuestring[0]) {
        if (err_buf) snprintf(err_buf, err_len, "App '%s' missing 'script' or 'exec'", app->name);
        return -1;
    }

    const char *script_val = script_item->valuestring;
    /* If script is not an absolute path, wrap with /bin/sh -c to support complex commands and relative scripts */
    if (script_val[0] != '/') {
        snprintf(app->script, sizeof(app->script), "/bin/sh -c \"%s\"", script_val);
    } else {
        snprintf(app->script, sizeof(app->script), "%s", script_val);
    }

    cJSON *cwd_item = cJSON_GetObjectItemCaseSensitive(item, "cwd");
    if (cwd_item && cJSON_IsString(cwd_item) && cwd_item->valuestring[0]) {
        if (cwd_item->valuestring[0] == '/') {
            snprintf(app->cwd, sizeof(app->cwd), "%s", cwd_item->valuestring);
        } else {
            char resolved[PATH_MAX];
            if (realpath(cwd_item->valuestring, resolved)) {
                snprintf(app->cwd, sizeof(app->cwd), "%s", resolved);
            } else {
                snprintf(app->cwd, sizeof(app->cwd), "%s", cwd_item->valuestring);
            }
        }
    }

    cJSON *env_item = cJSON_GetObjectItemCaseSensitive(item, "env");
    if (env_item && cJSON_IsObject(env_item)) {
        cJSON *var = NULL;
        cJSON_ArrayForEach(var, env_item) {
            if (app->env_count >= MAX_ENV_VARS) break;
            if (cJSON_IsString(var)) {
                snprintf(app->envs[app->env_count].key, sizeof(app->envs[0].key), "%s", var->string);
                snprintf(app->envs[app->env_count].value, sizeof(app->envs[0].value), "%s", var->valuestring);
                app->env_count++;
            } else if (cJSON_IsNumber(var)) {
                snprintf(app->envs[app->env_count].key, sizeof(app->envs[0].key), "%s", var->string);
                snprintf(app->envs[app->env_count].value, sizeof(app->envs[0].value), "%ld", (long)var->valuedouble);
                app->env_count++;
            }
        }
    }

    cJSON *watch_item = cJSON_GetObjectItemCaseSensitive(item, "watch");
    if (watch_item) {
        if (cJSON_IsBool(watch_item) && cJSON_IsTrue(watch_item)) {
            snprintf(app->watch, sizeof(app->watch), "%s", app->cwd);
        } else if (cJSON_IsString(watch_item) && watch_item->valuestring[0]) {
            if (watch_item->valuestring[0] == '/') {
                snprintf(app->watch, sizeof(app->watch), "%s", watch_item->valuestring);
            } else {
                snprintf(app->watch, sizeof(app->watch), "%s/%s", app->cwd, watch_item->valuestring);
            }
        } else if (cJSON_IsArray(watch_item)) {
            cJSON *first_elem = cJSON_GetArrayItem(watch_item, 0);
            if (first_elem && cJSON_IsString(first_elem) && first_elem->valuestring[0]) {
                if (first_elem->valuestring[0] == '/') {
                    snprintf(app->watch, sizeof(app->watch), "%s", first_elem->valuestring);
                } else {
                    snprintf(app->watch, sizeof(app->watch), "%s/%s", app->cwd, first_elem->valuestring);
                }
            }
        }
    }

    cJSON *restart_sec_item = cJSON_GetObjectItemCaseSensitive(item, "restart_sec");
    if (!restart_sec_item) {
        restart_sec_item = cJSON_GetObjectItemCaseSensitive(item, "restart_delay");
    }
    if (restart_sec_item && cJSON_IsNumber(restart_sec_item) && restart_sec_item->valueint >= 0) {
        app->restart_sec = restart_sec_item->valueint;
    }

    cJSON *scope_item = cJSON_GetObjectItemCaseSensitive(item, "scope");
    if (scope_item && cJSON_IsString(scope_item)) {
        if (strcasecmp(scope_item->valuestring, "system") == 0) {
            app->scope = SCOPE_SYSTEM;
        } else if (strcasecmp(scope_item->valuestring, "user") == 0) {
            app->scope = SCOPE_USER;
        }
    }

    return 0;
}

int config_load(const char *file_path,
                SystemdScope default_scope,
                AppService **out_apps,
                int *out_count,
                char *err_buf,
                size_t err_len) {
    if (!file_path || !out_apps || !out_count) return -1;
    *out_apps = NULL;
    *out_count = 0;

    char *json_str = read_file_to_string(file_path);
    if (!json_str) {
        if (err_buf) snprintf(err_buf, err_len, "Failed to read file '%s'", file_path);
        return -1;
    }

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (err_buf) {
            snprintf(err_buf, err_len, "JSON syntax error near: %s", error_ptr ? error_ptr : "unknown");
        }
        return -1;
    }

    cJSON *apps_array = NULL;
    if (cJSON_IsArray(root)) {
        apps_array = root;
    } else if (cJSON_IsObject(root)) {
        apps_array = cJSON_GetObjectItemCaseSensitive(root, "apps");
        if (!apps_array) {
            apps_array = cJSON_GetObjectItemCaseSensitive(root, "services");
        }
    }

    if (!apps_array || !cJSON_IsArray(apps_array)) {
        if (err_buf) snprintf(err_buf, err_len, "No 'apps' array found in config root");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(apps_array);
    if (count <= 0) {
        if (err_buf) snprintf(err_buf, err_len, "'apps' array is empty");
        cJSON_Delete(root);
        return -1;
    }

    AppService *apps = calloc(count, sizeof(AppService));
    if (!apps) {
        if (err_buf) snprintf(err_buf, err_len, "Out of memory allocating apps array");
        cJSON_Delete(root);
        return -1;
    }

    int valid_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, apps_array) {
        if (parse_app_item(item, default_scope, &apps[valid_count], err_buf, err_len) == 0) {
            valid_count++;
        } else {
            /* Error encountered */
            cJSON_Delete(root);
            free(apps);
            return -1;
        }
    }

    cJSON_Delete(root);
    *out_apps = apps;
    *out_count = valid_count;
    return 0;
}

void config_free(AppService *apps, int count) {
    (void)count;
    free(apps);
}
