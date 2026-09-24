#include "app.h"
#include "config.h"
#include "unit_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

static void test_config_loader(void) {
    printf("[TEST] Testing config_load()... ");
    AppService *apps = NULL;
    int count = 0;
    char err_buf[256];

    int res = config_load("fire.config.json.example", SCOPE_USER, &apps, &count, err_buf, sizeof(err_buf));
    assert(res == 0);
    assert(count == 3);
    assert(strcmp(apps[0].name, "api-service") == 0);
    assert(apps[0].env_count == 2);
    assert(apps[0].restart_sec == 2);
    assert(apps[0].watch[0] != '\0');

    assert(strcmp(apps[1].name, "worker-queue") == 0);
    assert(apps[1].restart_sec == 3);

    assert(strcmp(apps[2].name, "metrics-agent") == 0);

    config_free(apps, count);
    printf("PASS\n");
}

static void test_unit_generator(void) {
    printf("[TEST] Testing unit_gen_create_service() and unit_gen_create_path()... ");
    AppService app;
    memset(&app, 0, sizeof(app));
    snprintf(app.name, sizeof(app.name), "test-app");
    snprintf(app.script, sizeof(app.script), "/usr/bin/node /app/index.js");
    snprintf(app.cwd, sizeof(app.cwd), "/app");
    snprintf(app.watch, sizeof(app.watch), "/app/src");
    app.restart_sec = 5;
    app.scope = SCOPE_USER;
    app.env_count = 1;
    snprintf(app.envs[0].key, sizeof(app.envs[0].key), "NODE_ENV");
    snprintf(app.envs[0].value, sizeof(app.envs[0].value), "test");

    char err_buf[256];
    int res = unit_gen_create_service(&app, err_buf, sizeof(err_buf));
    assert(res == 0);

    res = unit_gen_create_path(&app, err_buf, sizeof(err_buf));
    assert(res == 0);

    char dir[MAX_PATH_LEN];
    unit_gen_get_dir(SCOPE_USER, dir, sizeof(dir));

    char svc_path[MAX_PATH_LEN];
    snprintf(svc_path, sizeof(svc_path), "%s/%s%s.service", dir, FIRE_UNIT_PREFIX, app.name);
    assert(access(svc_path, R_OK) == 0);

    char path_path[MAX_PATH_LEN];
    snprintf(path_path, sizeof(path_path), "%s/%s%s.path", dir, FIRE_UNIT_PREFIX, app.name);
    assert(access(path_path, R_OK) == 0);

    char watch_detected[MAX_PATH_LEN];
    bool has_watch = unit_gen_has_watch(app.name, SCOPE_USER, watch_detected, sizeof(watch_detected));
    assert(has_watch == true);
    assert(strcmp(watch_detected, "/app/src") == 0);

    /* Test delete */
    res = unit_gen_delete(app.name, SCOPE_USER, err_buf, sizeof(err_buf));
    assert(res == 0);
    assert(access(svc_path, F_OK) != 0);
    assert(access(path_path, F_OK) != 0);

    printf("PASS\n");
}

int main(void) {
    printf("--- Running fire-dash Core Tests ---\n");
    test_config_loader();
    test_unit_generator();
    printf("All fire-dash tests passed successfully!\n");
    return 0;
}
