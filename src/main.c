#include "app.h"
#include "unit_gen.h"
#include "dbus_systemd.h"
#include "config.h"
#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <libgen.h>

static void print_usage(const char *prog) {
    printf("\033[38;5;208;1m🔥 fire-dash\033[0m \033[38;5;244mv%s - The ultra-lightweight systemd wrapper & process dashboard\033[0m\n\n", FIRE_VERSION);
    printf("\033[1mUSAGE:\033[0m\n");
    printf("  %s [command] [options]\n\n", prog);
    printf("\033[1mCOMMANDS:\033[0m\n");
    printf("  \033[38;5;51m(no args)\033[0m                 Launch the interactive live TUI dashboard\n");
    printf("  \033[38;5;51mmonit\033[0m                     Alias to launch the live TUI dashboard\n");
    printf("  \033[38;5;51mstart\033[0m <script|config.json> Start an app or start all apps from a config file\n");
    printf("  \033[38;5;51mstop\033[0m <name|all>            Stop a running app or all apps\n");
    printf("  \033[38;5;51mrestart\033[0m <name|all>         Restart an app or all apps\n");
    printf("  \033[38;5;51mdelete\033[0m <name|all>          Stop, unregister and delete service units\n");
    printf("  \033[38;5;51mlist\033[0m, \033[38;5;51mls\033[0m                 Print formatted status table and exit\n");
    printf("  \033[38;5;51mlogs\033[0m [name] [-f] [-n num]  Tail or view journal logs for an app\n");
    printf("  \033[38;5;51msave\033[0m                      Enable all units to auto-start on boot\n");
    printf("  \033[38;5;51mhelp\033[0m                      Show this help message\n\n");
    printf("\033[1mSTART OPTIONS:\033[0m\n");
    printf("  \033[38;5;214m--name, -n <name>\033[0m         Custom name for the process\n");
    printf("  \033[38;5;214m--watch, -w <path>\033[0m        Auto-restart on file change via systemd .path unit\n");
    printf("  \033[38;5;214m--cwd, -c <path>\033[0m          Working directory for the process\n");
    printf("  \033[38;5;214m--restart-sec <sec>\033[0m       Restart delay in seconds (default: 2)\n\n");
    printf("\033[1mGLOBAL FLAGS:\033[0m\n");
    printf("  \033[38;5;220m--user\033[0m                    Target systemd user scope (~/.config/systemd/user) [default]\n");
    printf("  \033[38;5;220m--system\033[0m                  Target systemd system scope (/etc/systemd/system)\n");
    printf("  \033[38;5;220m-v, --version\033[0m             Show version\n");
    printf("  \033[38;5;220m-h, --help\033[0m                Show help\n\n");
}

static void format_mem(uint64_t bytes, char *out, size_t max_len) {
    if (bytes == 0) {
        snprintf(out, max_len, "0 B");
    } else if (bytes < 1024) {
        snprintf(out, max_len, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, max_len, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        snprintf(out, max_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, max_len, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

static int cmd_list(SystemdScope scope) {
    ProcessInfo *list = NULL;
    int count = 0;

    systemd_list_all(scope, &list, &count);

    printf("\033[38;5;208;1m🔥 fire-dash\033[0m \033[38;5;244mv%s\033[0m [Scope: \033[38;5;51;1m%s\033[0m]\n\n",
           FIRE_VERSION, scope == SCOPE_SYSTEM ? "SYSTEM" : "USER");

    if (count == 0) {
        printf("  No fire-dash managed applications found.\n");
        printf("  Start one with: fire-dash start <script>\n\n");
        return 0;
    }

    printf("  %-4s %-20s %-12s %-8s %-12s %-10s %-10s %-10s\n",
           "ID", "NAME", "STATUS", "PID", "MEMORY", "RESTARTS", "WATCH", "AUTOSTART");
    printf("  ──── ──────────────────── ──────────── ──────── ──────────── ────────── ────────── ──────────\n");

    for (int i = 0; i < count; i++) {
        char mem_str[32];
        format_mem(list[i].memory_bytes, mem_str, sizeof(mem_str));

        char status_str[64];
        if (strcmp(list[i].active_state, "active") == 0) {
            snprintf(status_str, sizeof(status_str), "\033[38;5;48;1m● active\033[0m");
        } else if (strcmp(list[i].active_state, "failed") == 0) {
            snprintf(status_str, sizeof(status_str), "\033[38;5;196;1m▲ failed\033[0m");
        } else {
            snprintf(status_str, sizeof(status_str), "\033[38;5;244m■ stopped\033[0m");
        }

        char pid_str[16];
        if (list[i].pid > 0) {
            snprintf(pid_str, sizeof(pid_str), "%d", list[i].pid);
        } else {
            snprintf(pid_str, sizeof(pid_str), "-");
        }

        printf("  %-4d \033[1m%-20s\033[0m %-12s %-8s %-12s %-10u %-10s %-10s\n",
               i,
               list[i].name,
               status_str,
               pid_str,
               mem_str,
               list[i].restart_count,
               list[i].has_watch ? "\033[38;5;51m✓ active\033[0m" : "-",
               list[i].enabled ? "\033[38;5;48menabled\033[0m" : "disabled");
    }
    printf("\n");

    systemd_free_list(list, count);
    return 0;
}

static int start_single_app(const AppService *app, SystemdScope scope) {
    char err_buf[256];
    printf("  Deploying unit \033[1m%s%s.service\033[0m... ", FIRE_UNIT_PREFIX, app->name);
    if (unit_gen_create_service(app, err_buf, sizeof(err_buf)) != 0) {
        printf("\033[31;1mFAILED\033[0m (%s)\n", err_buf);
        return -1;
    }
    printf("\033[32mOK\033[0m\n");

    if (app->watch[0]) {
        printf("  Deploying watch trigger \033[1m%s%s.path\033[0m (%s)... ",
               FIRE_UNIT_PREFIX, app->name, app->watch);
        if (unit_gen_create_path(app, err_buf, sizeof(err_buf)) != 0) {
            printf("\033[31;1mFAILED\033[0m (%s)\n", err_buf);
        } else {
            printf("\033[32mOK\033[0m\n");
        }
    }

    printf("  Reloading systemd daemon... ");
    systemd_daemon_reload(scope);
    printf("\033[32mOK\033[0m\n");

    printf("  Starting service \033[1m%s\033[0m... ", app->name);
    if (systemd_start_unit(app->name, scope) == 0) {
        printf("\033[32;1mSUCCESS\033[0m\n");
    } else {
        printf("\033[33;1mTRIGGERED\033[0m\n");
    }
    return 0;
}

static int cmd_start(int argc, char **argv, SystemdScope scope) {
    char name[MAX_NAME_LEN] = "";
    char watch[MAX_PATH_LEN] = "";
    char cwd[MAX_PATH_LEN] = "";
    int restart_sec = 2;
    char target[MAX_PATH_LEN] = "";

    /* Parse start specific options */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 || strcmp(argv[i], "-n") == 0) {
            if (i + 1 < argc) strncpy(name, argv[++i], sizeof(name) - 1);
        } else if (strcmp(argv[i], "--watch") == 0 || strcmp(argv[i], "-w") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                strncpy(watch, argv[++i], sizeof(watch) - 1);
            } else {
                /* Default watch to current directory */
                strncpy(watch, ".", sizeof(watch) - 1);
            }
        } else if (strcmp(argv[i], "--cwd") == 0 || strcmp(argv[i], "-c") == 0) {
            if (i + 1 < argc) strncpy(cwd, argv[++i], sizeof(cwd) - 1);
        } else if (strcmp(argv[i], "--restart-sec") == 0) {
            if (i + 1 < argc) restart_sec = atoi(argv[++i]);
        } else if (argv[i][0] != '-' && target[0] == '\0') {
            strncpy(target, argv[i], sizeof(target) - 1);
        }
    }

    /* Check if target is a JSON config file or config exists */
    bool is_config = false;
    if (target[0] && strstr(target, ".json") != NULL) {
        is_config = true;
    } else if (target[0] == '\0') {
        char found_config[MAX_PATH_LEN];
        if (config_find_default(found_config, sizeof(found_config)) == 0) {
            strncpy(target, found_config, sizeof(target) - 1);
            is_config = true;
        }
    }

    if (is_config) {
        printf("\033[38;5;208;1m🔥 fire-dash\033[0m: Loading configuration from '\033[1m%s\033[0m'...\n", target);
        AppService *apps = NULL;
        int count = 0;
        char err_buf[256];
        if (config_load(target, scope, &apps, &count, err_buf, sizeof(err_buf)) != 0) {
            fprintf(stderr, "\033[31;1mError loading config:\033[0m %s\n", err_buf);
            return 1;
        }

        printf("Found %d application%s defined in config.\n\n", count, count == 1 ? "" : "s");
        for (int i = 0; i < count; i++) {
            start_single_app(&apps[i], apps[i].scope);
            printf("\n");
        }
        config_free(apps, count);
        cmd_list(scope);
        return 0;
    }

    if (target[0] == '\0') {
        fprintf(stderr, "\033[31;1mError:\033[0m No script, command, or config file specified.\n");
        fprintf(stderr, "Usage: fire-dash start <script|command|config.json> [--name <name>]\n");
        return 1;
    }

    /* Target is a direct script or command */
    AppService app;
    memset(&app, 0, sizeof(app));
    app.scope = scope;
    app.restart_sec = restart_sec;

    /* Script command */
    if (target[0] != '/') {
        snprintf(app.script, sizeof(app.script), "/bin/sh -c \"%s\"", target);
    } else {
        snprintf(app.script, sizeof(app.script), "%s", target);
    }

    /* App name */
    if (name[0]) {
        snprintf(app.name, sizeof(app.name), "%s", name);
    } else {
        /* Derive name from target */
        char tmp_target[MAX_PATH_LEN];
        strncpy(tmp_target, target, sizeof(tmp_target) - 1);
        char *bname = basename(tmp_target);
        char *dot = strrchr(bname, '.');
        if (dot && dot != bname) *dot = '\0';
        snprintf(app.name, sizeof(app.name), "%s", bname);
    }

    /* CWD */
    if (cwd[0]) {
        snprintf(app.cwd, sizeof(app.cwd), "%s", cwd);
    } else {
        if (!getcwd(app.cwd, sizeof(app.cwd))) {
            snprintf(app.cwd, sizeof(app.cwd), ".");
        }
    }

    /* Watch */
    if (watch[0]) {
        if (watch[0] == '/') {
            snprintf(app.watch, sizeof(app.watch), "%s", watch);
        } else {
            snprintf(app.watch, sizeof(app.watch), "%s/%s", app.cwd, watch);
        }
    }

    printf("\033[38;5;208;1m🔥 fire-dash\033[0m: Registering app '\033[1m%s\033[0m'...\n", app.name);
    start_single_app(&app, scope);
    printf("\n");
    cmd_list(scope);
    return 0;
}

static int cmd_stop(const char *target, SystemdScope scope) {
    if (!target || !*target) {
        fprintf(stderr, "\033[31;1mError:\033[0m Missing app name or 'all'\n");
        return 1;
    }

    if (strcmp(target, "all") == 0) {
        char **names = NULL;
        int count = 0;
        unit_gen_list_names(scope, &names, &count);
        for (int i = 0; i < count; i++) {
            printf("Stopping '\033[1m%s\033[0m'... ", names[i]);
            systemd_stop_unit(names[i], scope);
            printf("\033[32mOK\033[0m\n");
        }
        unit_gen_free_names(names, count);
    } else {
        printf("Stopping '\033[1m%s\033[0m'... ", target);
        systemd_stop_unit(target, scope);
        printf("\033[32mOK\033[0m\n");
    }
    return 0;
}

static int cmd_restart(const char *target, SystemdScope scope) {
    if (!target || !*target) {
        fprintf(stderr, "\033[31;1mError:\033[0m Missing app name or 'all'\n");
        return 1;
    }

    if (strcmp(target, "all") == 0) {
        char **names = NULL;
        int count = 0;
        unit_gen_list_names(scope, &names, &count);
        for (int i = 0; i < count; i++) {
            printf("Restarting '\033[1m%s\033[0m'... ", names[i]);
            systemd_restart_unit(names[i], scope);
            printf("\033[32mOK\033[0m\n");
        }
        unit_gen_free_names(names, count);
    } else {
        printf("Restarting '\033[1m%s\033[0m'... ", target);
        systemd_restart_unit(target, scope);
        printf("\033[32mOK\033[0m\n");
    }
    return 0;
}

static int cmd_delete(const char *target, SystemdScope scope) {
    if (!target || !*target) {
        fprintf(stderr, "\033[31;1mError:\033[0m Missing app name or 'all'\n");
        return 1;
    }

    if (strcmp(target, "all") == 0) {
        char **names = NULL;
        int count = 0;
        unit_gen_list_names(scope, &names, &count);
        for (int i = 0; i < count; i++) {
            printf("Deleting '\033[1m%s\033[0m'... ", names[i]);
            systemd_stop_unit(names[i], scope);
            systemd_disable_unit(names[i], scope);
            unit_gen_delete(names[i], scope, NULL, 0);
            printf("\033[32mOK\033[0m\n");
        }
        unit_gen_free_names(names, count);
        systemd_daemon_reload(scope);
    } else {
        printf("Deleting '\033[1m%s\033[0m'... ", target);
        systemd_stop_unit(target, scope);
        systemd_disable_unit(target, scope);
        unit_gen_delete(target, scope, NULL, 0);
        systemd_daemon_reload(scope);
        printf("\033[32mOK\033[0m\n");
    }
    return 0;
}

static int cmd_save(SystemdScope scope) {
    char **names = NULL;
    int count = 0;
    unit_gen_list_names(scope, &names, &count);
    if (count == 0) {
        printf("No fire-dash services found to save.\n");
        return 0;
    }

    printf("\033[38;5;208;1m🔥 fire-dash\033[0m: Enabling %d services to persist on boot...\n", count);
    for (int i = 0; i < count; i++) {
        printf("  Enabling \033[1m%s\033[0m... ", names[i]);
        systemd_enable_unit(names[i], scope);
        printf("\033[32mOK\033[0m\n");
    }
    unit_gen_free_names(names, count);
    printf("✔ All fire-dash services enabled for auto-start.\n");
    return 0;
}

static int cmd_logs(int argc, char **argv, SystemdScope scope) {
    char name[MAX_NAME_LEN] = "";
    bool follow = false;
    int lines = 50;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--follow") == 0) {
            follow = true;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--lines") == 0) {
            if (i + 1 < argc) lines = atoi(argv[++i]);
        } else if (argv[i][0] != '-' && name[0] == '\0') {
            strncpy(name, argv[i], sizeof(name) - 1);
        }
    }

    char cmd[512];
    if (name[0]) {
        snprintf(cmd, sizeof(cmd), "journalctl %s -u %s%s.service -n %d %s",
                 scope == SCOPE_USER ? "--user" : "",
                 FIRE_UNIT_PREFIX, name,
                 lines,
                 follow ? "-f" : "--no-pager");
    } else {
        snprintf(cmd, sizeof(cmd), "journalctl %s -u '%s*.service' -n %d %s",
                 scope == SCOPE_USER ? "--user" : "",
                 FIRE_UNIT_PREFIX,
                 lines,
                 follow ? "-f" : "--no-pager");
    }

    return system(cmd);
}

int main(int argc, char **argv) {
    SystemdScope scope = SCOPE_USER;

    /* Parse global flags */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--system") == 0) {
            scope = SCOPE_SYSTEM;
        } else if (strcmp(argv[i], "--user") == 0) {
            scope = SCOPE_USER;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("fire-dash version %s\n", FIRE_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Filter out global flags for command dispatch */
    char *filtered_args[64];
    int filtered_count = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--system") == 0 || strcmp(argv[i], "--user") == 0) {
            continue;
        }
        filtered_args[filtered_count++] = argv[i];
    }

    /* Default with no subcommands: Launch interactive TUI */
    if (filtered_count == 0 || strcmp(filtered_args[0], "monit") == 0) {
        return tui_run(scope);
    }

    const char *subcmd = filtered_args[0];

    if (strcmp(subcmd, "start") == 0) {
        return cmd_start(filtered_count - 1, &filtered_args[1], scope);
    } else if (strcmp(subcmd, "stop") == 0) {
        const char *target = filtered_count > 1 ? filtered_args[1] : NULL;
        return cmd_stop(target, scope);
    } else if (strcmp(subcmd, "restart") == 0) {
        const char *target = filtered_count > 1 ? filtered_args[1] : NULL;
        return cmd_restart(target, scope);
    } else if (strcmp(subcmd, "delete") == 0 || strcmp(subcmd, "del") == 0) {
        const char *target = filtered_count > 1 ? filtered_args[1] : NULL;
        return cmd_delete(target, scope);
    } else if (strcmp(subcmd, "list") == 0 || strcmp(subcmd, "ls") == 0) {
        return cmd_list(scope);
    } else if (strcmp(subcmd, "logs") == 0 || strcmp(subcmd, "log") == 0) {
        return cmd_logs(filtered_count - 1, &filtered_args[1], scope);
    } else if (strcmp(subcmd, "save") == 0) {
        return cmd_save(scope);
    } else {
        fprintf(stderr, "\033[31;1mUnknown command:\033[0m '%s'\n\n", subcmd);
        print_usage(argv[0]);
        return 1;
    }
}
