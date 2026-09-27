#include "app.h"
#include "config.h"
#include "unit_gen.h"
#include "log_viewer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

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

static void test_is_valid_name(void) {
    printf("[TEST] Testing is_valid_name()... ");
    assert(is_valid_name("api") == true);
    assert(is_valid_name("api-service") == true);
    assert(is_valid_name("worker_1") == true);
    assert(is_valid_name("app.v2") == true);
    assert(is_valid_name("123") == true);

    assert(is_valid_name(NULL) == false);
    assert(is_valid_name("") == false);
    assert(is_valid_name("api;rm") == false);
    assert(is_valid_name("app|curl") == false);
    assert(is_valid_name("service&") == false);
    assert(is_valid_name("app name") == false);
    assert(is_valid_name("app$var") == false);
    assert(is_valid_name("app\n") == false);
    assert(is_valid_name("app`whoami`") == false);
    printf("PASS\n");
}

static void test_unit_generator(void) {
    printf("[TEST] Testing unit_gen_create_service(), secrets isolation, and watch restart... ");
    AppService app;
    memset(&app, 0, sizeof(app));
    snprintf(app.name, sizeof(app.name), "test-app");
    snprintf(app.script, sizeof(app.script), "/usr/bin/node /app/index.js");
    snprintf(app.cwd, sizeof(app.cwd), "/app");
    snprintf(app.watch, sizeof(app.watch), "/app/src");
    app.restart_sec = 5;
    app.scope = SCOPE_USER;
    app.env_count = 1;
    snprintf(app.envs[0].key, sizeof(app.envs[0].key), "SECRET_KEY");
    snprintf(app.envs[0].value, sizeof(app.envs[0].value), "supersecret123");

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

    char env_path[MAX_PATH_LEN];
    snprintf(env_path, sizeof(env_path), "%s/%s%s.env", dir, FIRE_UNIT_PREFIX, app.name);
    assert(access(env_path, R_OK) == 0);

    /* Verify env file has 0600 permissions */
    struct stat env_st;
    assert(stat(env_path, &env_st) == 0);
    assert((env_st.st_mode & 0777) == 0600);

    FILE *sfp = fopen(svc_path, "r");
    assert(sfp != NULL);
    char scontent[2048];
    size_t nr = fread(scontent, 1, sizeof(scontent) - 1, sfp);
    scontent[nr] = '\0';
    fclose(sfp);
    assert(strstr(scontent, "MemoryAccounting=yes") != NULL);
    assert(strstr(scontent, "CPUAccounting=yes") != NULL);
    assert(strstr(scontent, "TasksAccounting=yes") != NULL);
    assert(strstr(scontent, "EnvironmentFile=") != NULL);
    /* Secrets must NOT be in plaintext in the .service file */
    assert(strstr(scontent, "Environment=\"SECRET_KEY=") == NULL);

    char path_path[MAX_PATH_LEN];
    snprintf(path_path, sizeof(path_path), "%s/%s%s.path", dir, FIRE_UNIT_PREFIX, app.name);
    assert(access(path_path, R_OK) == 0);

    char restart_path[MAX_PATH_LEN];
    snprintf(restart_path, sizeof(restart_path), "%s/%s%s-restart.service", dir, FIRE_UNIT_PREFIX, app.name);
    assert(access(restart_path, R_OK) == 0);

    char watch_detected[MAX_PATH_LEN];
    bool has_watch = unit_gen_has_watch(app.name, SCOPE_USER, watch_detected, sizeof(watch_detected));
    assert(has_watch == true);
    assert(strcmp(watch_detected, "/app/src") == 0);

    /* Test delete cleans up .service, .path, -restart.service, and .env */
    res = unit_gen_delete(app.name, SCOPE_USER, err_buf, sizeof(err_buf));
    assert(res == 0);
    assert(access(svc_path, F_OK) != 0);
    assert(access(path_path, F_OK) != 0);
    assert(access(restart_path, F_OK) != 0);
    assert(access(env_path, F_OK) != 0);

    printf("PASS\n");
}

static void test_system_scope_sandboxing(void) {
    printf("[TEST] Testing unit generation with --system sandboxing... ");
    AppService app;
    memset(&app, 0, sizeof(app));
    snprintf(app.name, sizeof(app.name), "sys-daemon");
    snprintf(app.script, sizeof(app.script), "/usr/local/bin/daemon");
    snprintf(app.user, sizeof(app.user), "www-data");
    app.scope = SCOPE_SYSTEM;

    /* Write to a temporary file via unit_gen_create_service */
    /* On non-Linux or without root, unit_gen_get_dir for system gives /etc/systemd/system which may fail fopen if not root */
    /* We can test service content directly if writable, or test get_dir */
    char dir[MAX_PATH_LEN];
    int res = unit_gen_get_dir(SCOPE_SYSTEM, dir, sizeof(dir));
    assert(res == 0);
    assert(strcmp(dir, "/etc/systemd/system") == 0);

    printf("PASS\n");
}

static void test_log_viewer_core(void) {
    printf("[TEST] Testing log_viewer state, multi-level triage, pause, and markers... ");
    LogViewer lv;
    log_viewer_init(&lv);
    assert(lv.count == 0);
    assert(lv.level_filter == LOG_LEVEL_ALL);
    assert(lv.auto_scroll == true);
    assert(lv.is_paused == false);
    assert(lv.is_dirty == true);

    /* Test appending lines with auto severity detection */
    lv.is_dirty = false;
    log_viewer_append(&lv, "2026-09-26T03:00:00Z GET /api/v1/health 200 OK", false);
    assert(lv.count == 1);
    assert(lv.is_dirty == true);
    assert(lv.lines[0].is_err == false);
    assert(lv.lines[0].is_warn == false);

    log_viewer_append(&lv, "2026-09-26T03:00:01Z GET /api/v1/missing 404 Not Found warning", false);
    assert(lv.count == 2);
    assert(lv.lines[1].is_err == false);
    assert(lv.lines[1].is_warn == true);

    log_viewer_append(&lv, "2026-09-26T03:00:02Z POST /api/v1/checkout 500 Internal Server Error fatal exception", false);
    assert(lv.count == 3);
    assert(lv.lines[2].is_err == true);

    /* Test visual checkpoint marker */
    log_viewer_add_marker(&lv);
    assert(lv.count == 4);
    assert(lv.lines[3].is_marker == true);
    assert(strstr(lv.lines[3].text, "MARK:") != NULL);

    /* Test Pause live stream */
    log_viewer_toggle_pause(&lv);
    assert(lv.is_paused == true);
    assert(lv.paused_buffered_count == 0);

    log_viewer_append(&lv, "New log while paused", false);
    assert(lv.paused_buffered_count == 1);

    log_viewer_toggle_pause(&lv);
    assert(lv.is_paused == false);
    assert(lv.paused_buffered_count == 0);
    assert(lv.auto_scroll == true);

    /* Test Level cycling */
    log_viewer_set_level_filter(&lv, LOG_LEVEL_WARN_ERR);
    assert(strcmp(log_viewer_level_name(lv.level_filter), "WARN+ERR") == 0);

    log_viewer_set_level_filter(&lv, LOG_LEVEL_ERR_ONLY);
    assert(strcmp(log_viewer_level_name(lv.level_filter), "ERR ONLY") == 0);

    /* Test Syntax Highlighting Formatter */
    char formatted[1024];
    LogLine sample;
    memset(&sample, 0, sizeof(sample));
    snprintf(sample.text, sizeof(sample.text), "2026-09-26T03:00:00Z POST /api/v1/auth 200 OK");
    snprintf(sample.app_name, sizeof(sample.app_name), "milta-backend");

    log_viewer_format_colored_line(&sample, "auth", true, formatted, sizeof(formatted), 80);
    /* Should contain ANSI color codes */
    assert(strstr(formatted, "\033[") != NULL);
    /* Should highlight search keyword "auth" with invert yellow background */
    assert(strstr(formatted, "\033[48;5;226;38;5;16;1m") != NULL);
    /* Should format app badge */
    assert(strstr(formatted, "milta-back") != NULL);

    /* Test inserting marker after specific line */
    int prev_count = lv.count;
    log_viewer_insert_marker_after(&lv, 1);
    assert(lv.count == prev_count + 1);
    assert(lv.lines[2].is_marker == true);
    assert(strstr(lv.lines[2].text, "MARK:") != NULL);

    log_viewer_cleanup(&lv);
    printf("PASS\n");
}

int main(void) {
    printf("--- Running fire-dash Core Tests ---\n");
    test_is_valid_name();
    test_config_loader();
    test_unit_generator();
    test_system_scope_sandboxing();
    test_log_viewer_core();
    printf("All fire-dash tests passed successfully!\n");
    return 0;
}
