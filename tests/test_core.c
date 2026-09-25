#include "app.h"
#include "config.h"
#include "unit_gen.h"
#include "log_viewer.h"
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
    test_config_loader();
    test_unit_generator();
    test_log_viewer_core();
    printf("All fire-dash tests passed successfully!\n");
    return 0;
}
