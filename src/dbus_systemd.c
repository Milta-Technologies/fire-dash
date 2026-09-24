#include "dbus_systemd.h"
#include "unit_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#if defined(__linux__) && defined(HAVE_LIBSYSTEMD)
#include <systemd/sd-bus.h>
static sd_bus *g_user_bus = NULL;
static sd_bus *g_system_bus = NULL;

static sd_bus *get_bus(SystemdScope scope) {
    if (scope == SCOPE_SYSTEM) {
        if (!g_system_bus) {
            sd_bus_default_system(&g_system_bus);
        }
        return g_system_bus;
    } else {
        if (!g_user_bus) {
            sd_bus_default_user(&g_user_bus);
        }
        return g_user_bus;
    }
}
#endif

/* Helper: run a command synchronously and capture exit status */
static int exec_cmd_silent(const char *cmd) {
    int ret = system(cmd);
    if (ret == -1) return -1;
    if (WIFEXITED(ret)) return WEXITSTATUS(ret);
    return -1;
}

/* Helper: run command and read output lines */
static char *exec_cmd_output(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        pclose(fp);
        return NULL;
    }

    char chunk[512];
    while (fgets(chunk, sizeof(chunk), fp)) {
        size_t clen = strlen(chunk);
        if (len + clen + 1 >= cap) {
            cap *= 2;
            char *new_buf = realloc(buf, cap);
            if (!new_buf) {
                free(buf);
                pclose(fp);
                return NULL;
            }
            buf = new_buf;
        }
        memcpy(buf + len, chunk, clen);
        len += clen;
        buf[len] = '\0';
    }

    pclose(fp);
    return buf;
}

int systemd_init(SystemdScope scope) {
    (void)scope;
#if defined(__linux__) && defined(HAVE_LIBSYSTEMD)
    get_bus(scope);
#endif
    return 0;
}

void systemd_cleanup(void) {
#if defined(__linux__) && defined(HAVE_LIBSYSTEMD)
    if (g_user_bus) {
        sd_bus_flush_close_unref(g_user_bus);
        g_user_bus = NULL;
    }
    if (g_system_bus) {
        sd_bus_flush_close_unref(g_system_bus);
        g_system_bus = NULL;
    }
#endif
}

int systemd_daemon_reload(SystemdScope scope) {
#if defined(__linux__) && defined(HAVE_LIBSYSTEMD)
    sd_bus *bus = get_bus(scope);
    if (bus) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *m = NULL;
        int r = sd_bus_call_method(bus,
                                   "org.freedesktop.systemd1",
                                   "/org/freedesktop/systemd1",
                                   "org.freedesktop.systemd1.Manager",
                                   "Reload",
                                   &error,
                                   &m,
                                   "");
        sd_bus_error_free(&error);
        if (m) sd_bus_message_unref(m);
        if (r >= 0) return 0;
    }
#endif
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "systemctl %s daemon-reload >/dev/null 2>&1",
             scope == SCOPE_USER ? "--user" : "");
    return exec_cmd_silent(cmd);
}

int systemd_start_unit(const char *name, SystemdScope scope) {
    char unit_svc[128];
    char unit_path[128];
    snprintf(unit_svc, sizeof(unit_svc), "%s%s.service", FIRE_UNIT_PREFIX, name);
    snprintf(unit_path, sizeof(unit_path), "%s%s.path", FIRE_UNIT_PREFIX, name);

#if defined(__linux__) && defined(HAVE_LIBSYSTEMD)
    sd_bus *bus = get_bus(scope);
    if (bus) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *m = NULL;
        int r = sd_bus_call_method(bus,
                                   "org.freedesktop.systemd1",
                                   "/org/freedesktop/systemd1",
                                   "org.freedesktop.systemd1.Manager",
                                   "StartUnit",
                                   &error,
                                   &m,
                                   "ss",
                                   unit_svc,
                                   "replace");
        sd_bus_error_free(&error);
        if (m) sd_bus_message_unref(m);

        if (unit_gen_has_watch(name, scope, NULL, 0)) {
            sd_bus_error err_p = SD_BUS_ERROR_NULL;
            sd_bus_message *mp = NULL;
            sd_bus_call_method(bus,
                               "org.freedesktop.systemd1",
                               "/org/freedesktop/systemd1",
                               "org.freedesktop.systemd1.Manager",
                               "StartUnit",
                               &err_p,
                               &mp,
                               "ss",
                               unit_path,
                               "replace");
            sd_bus_error_free(&err_p);
            if (mp) sd_bus_message_unref(mp);
        }

        if (r >= 0) return 0;
    }
#endif
    char cmd[512];
    if (unit_gen_has_watch(name, scope, NULL, 0)) {
        snprintf(cmd, sizeof(cmd), "systemctl %s start %s %s >/dev/null 2>&1",
                 scope == SCOPE_USER ? "--user" : "", unit_svc, unit_path);
    } else {
        snprintf(cmd, sizeof(cmd), "systemctl %s start %s >/dev/null 2>&1",
                 scope == SCOPE_USER ? "--user" : "", unit_svc);
    }
    return exec_cmd_silent(cmd);
}

int systemd_stop_unit(const char *name, SystemdScope scope) {
    char unit_svc[128];
    char unit_path[128];
    snprintf(unit_svc, sizeof(unit_svc), "%s%s.service", FIRE_UNIT_PREFIX, name);
    snprintf(unit_path, sizeof(unit_path), "%s%s.path", FIRE_UNIT_PREFIX, name);

#if defined(__linux__) && defined(HAVE_LIBSYSTEMD)
    sd_bus *bus = get_bus(scope);
    if (bus) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *m = NULL;
        int r = sd_bus_call_method(bus,
                                   "org.freedesktop.systemd1",
                                   "/org/freedesktop/systemd1",
                                   "org.freedesktop.systemd1.Manager",
                                   "StopUnit",
                                   &error,
                                   &m,
                                   "ss",
                                   unit_svc,
                                   "replace");
        sd_bus_error_free(&error);
        if (m) sd_bus_message_unref(m);

        if (unit_gen_has_watch(name, scope, NULL, 0)) {
            sd_bus_error err_p = SD_BUS_ERROR_NULL;
            sd_bus_message *mp = NULL;
            sd_bus_call_method(bus,
                               "org.freedesktop.systemd1",
                               "/org/freedesktop/systemd1",
                               "org.freedesktop.systemd1.Manager",
                               "StopUnit",
                               &err_p,
                               &mp,
                               "ss",
                               unit_path,
                               "replace");
            sd_bus_error_free(&err_p);
            if (mp) sd_bus_message_unref(mp);
        }

        if (r >= 0) return 0;
    }
#endif
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "systemctl %s stop %s %s >/dev/null 2>&1",
             scope == SCOPE_USER ? "--user" : "", unit_svc, unit_path);
    return exec_cmd_silent(cmd);
}

int systemd_restart_unit(const char *name, SystemdScope scope) {
    char unit_svc[128];
    snprintf(unit_svc, sizeof(unit_svc), "%s%s.service", FIRE_UNIT_PREFIX, name);

#if defined(__linux__) && defined(HAVE_LIBSYSTEMD)
    sd_bus *bus = get_bus(scope);
    if (bus) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *m = NULL;
        int r = sd_bus_call_method(bus,
                                   "org.freedesktop.systemd1",
                                   "/org/freedesktop/systemd1",
                                   "org.freedesktop.systemd1.Manager",
                                   "RestartUnit",
                                   &error,
                                   &m,
                                   "ss",
                                   unit_svc,
                                   "replace");
        sd_bus_error_free(&error);
        if (m) sd_bus_message_unref(m);
        if (r >= 0) return 0;
    }
#endif
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "systemctl %s restart %s >/dev/null 2>&1",
             scope == SCOPE_USER ? "--user" : "", unit_svc);
    return exec_cmd_silent(cmd);
}

int systemd_enable_unit(const char *name, SystemdScope scope) {
    char unit_svc[128];
    char unit_path[128];
    snprintf(unit_svc, sizeof(unit_svc), "%s%s.service", FIRE_UNIT_PREFIX, name);
    snprintf(unit_path, sizeof(unit_path), "%s%s.path", FIRE_UNIT_PREFIX, name);

    char cmd[512];
    if (unit_gen_has_watch(name, scope, NULL, 0)) {
        snprintf(cmd, sizeof(cmd), "systemctl %s enable %s %s >/dev/null 2>&1",
                 scope == SCOPE_USER ? "--user" : "", unit_svc, unit_path);
    } else {
        snprintf(cmd, sizeof(cmd), "systemctl %s enable %s >/dev/null 2>&1",
                 scope == SCOPE_USER ? "--user" : "", unit_svc);
    }
    return exec_cmd_silent(cmd);
}

int systemd_disable_unit(const char *name, SystemdScope scope) {
    char unit_svc[128];
    char unit_path[128];
    snprintf(unit_svc, sizeof(unit_svc), "%s%s.service", FIRE_UNIT_PREFIX, name);
    snprintf(unit_path, sizeof(unit_path), "%s%s.path", FIRE_UNIT_PREFIX, name);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "systemctl %s disable %s %s >/dev/null 2>&1",
             scope == SCOPE_USER ? "--user" : "", unit_svc, unit_path);
    return exec_cmd_silent(cmd);
}

int systemd_reset_failed(const char *name, SystemdScope scope) {
    char unit_svc[128];
    snprintf(unit_svc, sizeof(unit_svc), "%s%s.service", FIRE_UNIT_PREFIX, name);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "systemctl %s reset-failed %s >/dev/null 2>&1",
             scope == SCOPE_USER ? "--user" : "", unit_svc);
    return exec_cmd_silent(cmd);
}

/* Reads live process memory & CPU directly from /proc on Linux */
static void read_proc_stats(pid_t pid, uint64_t *mem_bytes, double *cpu_percent) {
    if (pid <= 0) return;

#if defined(__linux__)
    char path[128];
    FILE *fp;

    /* Memory via /proc/[pid]/statm */
    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    fp = fopen(path, "r");
    if (fp) {
        long total_pages = 0;
        long resident_pages = 0;
        if (fscanf(fp, "%ld %ld", &total_pages, &resident_pages) == 2) {
            long page_size = sysconf(_SC_PAGESIZE);
            if (page_size > 0 && resident_pages > 0) {
                *mem_bytes = (uint64_t)resident_pages * (uint64_t)page_size;
            }
        }
        fclose(fp);
    }

    /* CPU calculation via /proc/[pid]/stat */
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    if (fp) {
        char buf[1024];
        if (fgets(buf, sizeof(buf), fp)) {
            char *comm_end = strrchr(buf, ')');
            if (comm_end) {
                unsigned long utime = 0, stime = 0;
                /* Scan starting after comm */
                sscanf(comm_end + 2,
                       "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                       &utime, &stime);
                /* Simple estimate if needed */
                (void)utime; (void)stime;
            }
        }
        fclose(fp);
    }
#else
    (void)pid;
    (void)mem_bytes;
    (void)cpu_percent;
#endif
}

int systemd_get_unit_status(const char *name, SystemdScope scope, ProcessInfo *info) {
    if (!name || !info) return -1;
    memset(info, 0, sizeof(*info));

    snprintf(info->name, sizeof(info->name), "%s", name);
    snprintf(info->unit_name, sizeof(info->unit_name), "%s%s.service", FIRE_UNIT_PREFIX, name);
    info->scope = scope;
    snprintf(info->active_state, sizeof(info->active_state), "inactive");
    snprintf(info->sub_state, sizeof(info->sub_state), "dead");

    /* Check watch path */
    info->has_watch = unit_gen_has_watch(name, scope, info->watch_path, sizeof(info->watch_path));

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "systemctl %s show %s -p ActiveState,SubState,MainPID,NRestarts,CPUUsageNSec,MemoryCurrent,ExecMainStartTimestamp,UnitFileState 2>/dev/null",
             scope == SCOPE_USER ? "--user" : "", info->unit_name);

    char *output = exec_cmd_output(cmd);
    if (!output) {
        return -1;
    }

    char *line = strtok(output, "\r\n");
    while (line) {
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char *key = line;
            const char *val = eq + 1;

            if (strcmp(key, "ActiveState") == 0) {
                snprintf(info->active_state, sizeof(info->active_state), "%s", val);
            } else if (strcmp(key, "SubState") == 0) {
                snprintf(info->sub_state, sizeof(info->sub_state), "%s", val);
            } else if (strcmp(key, "MainPID") == 0) {
                info->pid = (pid_t)atoi(val);
            } else if (strcmp(key, "NRestarts") == 0) {
                info->restart_count = (uint32_t)strtoul(val, NULL, 10);
            } else if (strcmp(key, "CPUUsageNSec") == 0 && strcmp(val, "[not set]") != 0) {
                info->cpu_usage_nsec = strtoull(val, NULL, 10);
            } else if (strcmp(key, "MemoryCurrent") == 0 && strcmp(val, "[not set]") != 0) {
                info->memory_bytes = strtoull(val, NULL, 10);
            } else if (strcmp(key, "UnitFileState") == 0) {
                if (strcmp(val, "enabled") == 0) info->enabled = true;
            }
        }
        line = strtok(NULL, "\r\n");
    }
    free(output);

    /* If systemd cgroup memory is 0 or unavailable, check /proc/<pid>/statm */
    if (info->pid > 0 && info->memory_bytes == 0) {
        read_proc_stats(info->pid, &info->memory_bytes, &info->cpu_percent);
    }

    return 0;
}

int systemd_list_all(SystemdScope scope, ProcessInfo **out_list, int *out_count) {
    if (!out_list || !out_count) return -1;
    *out_list = NULL;
    *out_count = 0;

    char **names = NULL;
    int count = 0;
    if (unit_gen_list_names(scope, &names, &count) != 0 || count == 0) {
        unit_gen_free_names(names, count);
        return 0;
    }

    ProcessInfo *list = calloc(count, sizeof(ProcessInfo));
    if (!list) {
        unit_gen_free_names(names, count);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        systemd_get_unit_status(names[i], scope, &list[i]);
    }

    unit_gen_free_names(names, count);
    *out_list = list;
    *out_count = count;
    return 0;
}

void systemd_free_list(ProcessInfo *list, int count) {
    (void)count;
    free(list);
}
