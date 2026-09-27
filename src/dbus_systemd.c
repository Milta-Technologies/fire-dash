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

#if defined(__APPLE__)
#include <libproc.h>
#endif

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

#include <fcntl.h>

/* Helper: run a command synchronously via fork+execvp and capture exit status (no shell) */
static int exec_argv_silent(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Helper: run command via fork+execvp and read output lines (no shell) */
static char *exec_argv_output(char *const argv[]) {
    int fds[2];
    if (pipe(fds) < 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(fds[1]);
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fds[0]);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    buf[0] = '\0';

    char chunk[512];
    ssize_t n;
    while ((n = read(fds[0], chunk, sizeof(chunk))) > 0) {
        if (len + (size_t)n + 1 >= cap) {
            cap = (cap * 2) + (size_t)n;
            char *new_buf = realloc(buf, cap);
            if (!new_buf) {
                free(buf);
                close(fds[0]);
                waitpid(pid, NULL, 0);
                return NULL;
            }
            buf = new_buf;
        }
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
        buf[len] = '\0';
    }
    close(fds[0]);
    waitpid(pid, NULL, 0);
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
    if (scope == SCOPE_USER) {
        char *argv[] = {"systemctl", "--user", "daemon-reload", NULL};
        return exec_argv_silent(argv);
    } else {
        char *argv[] = {"systemctl", "daemon-reload", NULL};
        return exec_argv_silent(argv);
    }
}

int systemd_start_unit(const char *name, SystemdScope scope) {
    if (!name || !is_valid_name(name)) return -1;

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
    bool has_watch = unit_gen_has_watch(name, scope, NULL, 0);
    if (scope == SCOPE_USER) {
        if (has_watch) {
            char *argv[] = {"systemctl", "--user", "start", unit_svc, unit_path, NULL};
            return exec_argv_silent(argv);
        } else {
            char *argv[] = {"systemctl", "--user", "start", unit_svc, NULL};
            return exec_argv_silent(argv);
        }
    } else {
        if (has_watch) {
            char *argv[] = {"systemctl", "start", unit_svc, unit_path, NULL};
            return exec_argv_silent(argv);
        } else {
            char *argv[] = {"systemctl", "start", unit_svc, NULL};
            return exec_argv_silent(argv);
        }
    }
}

int systemd_stop_unit(const char *name, SystemdScope scope) {
    if (!name || !is_valid_name(name)) return -1;

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
    if (scope == SCOPE_USER) {
        char *argv[] = {"systemctl", "--user", "stop", unit_svc, unit_path, NULL};
        return exec_argv_silent(argv);
    } else {
        char *argv[] = {"systemctl", "stop", unit_svc, unit_path, NULL};
        return exec_argv_silent(argv);
    }
}

int systemd_restart_unit(const char *name, SystemdScope scope) {
    if (!name || !is_valid_name(name)) return -1;

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
    if (scope == SCOPE_USER) {
        char *argv[] = {"systemctl", "--user", "restart", unit_svc, NULL};
        return exec_argv_silent(argv);
    } else {
        char *argv[] = {"systemctl", "restart", unit_svc, NULL};
        return exec_argv_silent(argv);
    }
}

int systemd_enable_unit(const char *name, SystemdScope scope) {
    if (!name || !is_valid_name(name)) return -1;

    char unit_svc[128];
    char unit_path[128];
    snprintf(unit_svc, sizeof(unit_svc), "%s%s.service", FIRE_UNIT_PREFIX, name);
    snprintf(unit_path, sizeof(unit_path), "%s%s.path", FIRE_UNIT_PREFIX, name);

    bool has_watch = unit_gen_has_watch(name, scope, NULL, 0);
    if (scope == SCOPE_USER) {
        if (has_watch) {
            char *argv[] = {"systemctl", "--user", "enable", unit_svc, unit_path, NULL};
            return exec_argv_silent(argv);
        } else {
            char *argv[] = {"systemctl", "--user", "enable", unit_svc, NULL};
            return exec_argv_silent(argv);
        }
    } else {
        if (has_watch) {
            char *argv[] = {"systemctl", "enable", unit_svc, unit_path, NULL};
            return exec_argv_silent(argv);
        } else {
            char *argv[] = {"systemctl", "enable", unit_svc, NULL};
            return exec_argv_silent(argv);
        }
    }
}

int systemd_disable_unit(const char *name, SystemdScope scope) {
    if (!name || !is_valid_name(name)) return -1;

    char unit_svc[128];
    char unit_path[128];
    snprintf(unit_svc, sizeof(unit_svc), "%s%s.service", FIRE_UNIT_PREFIX, name);
    snprintf(unit_path, sizeof(unit_path), "%s%s.path", FIRE_UNIT_PREFIX, name);

    if (scope == SCOPE_USER) {
        char *argv[] = {"systemctl", "--user", "disable", unit_svc, unit_path, NULL};
        return exec_argv_silent(argv);
    } else {
        char *argv[] = {"systemctl", "disable", unit_svc, unit_path, NULL};
        return exec_argv_silent(argv);
    }
}

int systemd_reset_failed(const char *name, SystemdScope scope) {
    if (!name || !is_valid_name(name)) return -1;

    char unit_svc[128];
    snprintf(unit_svc, sizeof(unit_svc), "%s%s.service", FIRE_UNIT_PREFIX, name);

    if (scope == SCOPE_USER) {
        char *argv[] = {"systemctl", "--user", "reset-failed", unit_svc, NULL};
        return exec_argv_silent(argv);
    } else {
        char *argv[] = {"systemctl", "reset-failed", unit_svc, NULL};
        return exec_argv_silent(argv);
    }
}

/* Reads live process memory & CPU directly from /proc on Linux or libproc on macOS */
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

    (void)cpu_percent;
#elif defined(__APPLE__)
    struct proc_taskinfo pti;
    int ret = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &pti, sizeof(pti));
    if (ret == (int)sizeof(pti) && pti.pti_resident_size > 0) {
        *mem_bytes = (uint64_t)pti.pti_resident_size;
    }
    (void)cpu_percent;
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

    char *argv_single[16];
    int s_idx = 0;
    argv_single[s_idx++] = "systemctl";
    if (scope == SCOPE_USER) {
        argv_single[s_idx++] = "--user";
    }
    argv_single[s_idx++] = "show";
    argv_single[s_idx++] = info->unit_name;
    argv_single[s_idx++] = "-p";
    argv_single[s_idx++] = "ActiveState,SubState,MainPID,NRestarts,CPUUsageNSec,MemoryCurrent,ExecMainStartTimestamp,UnitFileState";
    argv_single[s_idx] = NULL;

    char *output = exec_argv_output(argv_single);
    if (!output) {
        return -1;
    }

    char *saveptr = NULL;
    char *line = strtok_r(output, "\r\n", &saveptr);
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
            } else if (strcmp(key, "CPUUsageNSec") == 0 && strcmp(val, "[not set]") != 0 && strcmp(val, "18446744073709551615") != 0) {
                uint64_t cpu = strtoull(val, NULL, 10);
                if (cpu != (uint64_t)-1) info->cpu_usage_nsec = cpu;
            } else if (strcmp(key, "MemoryCurrent") == 0 && strcmp(val, "[not set]") != 0 && strcmp(val, "18446744073709551615") != 0) {
                uint64_t m = strtoull(val, NULL, 10);
                if (m != (uint64_t)-1) info->memory_bytes = m;
            } else if (strcmp(key, "UnitFileState") == 0) {
                if (strcmp(val, "enabled") == 0) info->enabled = true;
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
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

    ProcessInfo *list = calloc((size_t)count, sizeof(ProcessInfo));
    if (!list) {
        unit_gen_free_names(names, count);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        snprintf(list[i].name, sizeof(list[i].name), "%s", names[i]);
        snprintf(list[i].unit_name, sizeof(list[i].unit_name), "%s%s.service", FIRE_UNIT_PREFIX, names[i]);
        list[i].scope = scope;
        snprintf(list[i].active_state, sizeof(list[i].active_state), "inactive");
        snprintf(list[i].sub_state, sizeof(list[i].sub_state), "dead");
        list[i].has_watch = unit_gen_has_watch(names[i], scope, list[i].watch_path, sizeof(list[i].watch_path));
    }

    /* Batch query all units in a single systemctl show call without shell */
    char **show_argv = calloc((size_t)count + 8, sizeof(char *));
    char *output = NULL;
    if (show_argv) {
        int a_idx = 0;
        show_argv[a_idx++] = "systemctl";
        if (scope == SCOPE_USER) {
            show_argv[a_idx++] = "--user";
        }
        show_argv[a_idx++] = "show";
        for (int i = 0; i < count; i++) {
            show_argv[a_idx++] = list[i].unit_name;
        }
        show_argv[a_idx++] = "-p";
        show_argv[a_idx++] = "Id,ActiveState,SubState,MainPID,NRestarts,CPUUsageNSec,MemoryCurrent,UnitFileState";
        show_argv[a_idx] = NULL;
        output = exec_argv_output(show_argv);
        free(show_argv);
    }
    if (output) {
        char *saveptr = NULL;
        char *line = strtok_r(output, "\r\n", &saveptr);
        ProcessInfo *cur = NULL;

        while (line) {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                const char *key = line;
                const char *val = eq + 1;

                if (strcmp(key, "Id") == 0) {
                    cur = NULL;
                    for (int i = 0; i < count; i++) {
                        if (strcmp(list[i].unit_name, val) == 0) {
                            cur = &list[i];
                            break;
                        }
                    }
                } else if (cur) {
                    if (strcmp(key, "ActiveState") == 0) {
                        snprintf(cur->active_state, sizeof(cur->active_state), "%s", val);
                    } else if (strcmp(key, "SubState") == 0) {
                        snprintf(cur->sub_state, sizeof(cur->sub_state), "%s", val);
                    } else if (strcmp(key, "MainPID") == 0) {
                        cur->pid = (pid_t)atoi(val);
                    } else if (strcmp(key, "NRestarts") == 0) {
                        cur->restart_count = (uint32_t)strtoul(val, NULL, 10);
                    } else if (strcmp(key, "CPUUsageNSec") == 0 && strcmp(val, "[not set]") != 0 && strcmp(val, "18446744073709551615") != 0) {
                        uint64_t cpu = strtoull(val, NULL, 10);
                        if (cpu != (uint64_t)-1) cur->cpu_usage_nsec = cpu;
                    } else if (strcmp(key, "MemoryCurrent") == 0 && strcmp(val, "[not set]") != 0 && strcmp(val, "18446744073709551615") != 0) {
                        uint64_t m = strtoull(val, NULL, 10);
                        if (m != (uint64_t)-1) cur->memory_bytes = m;
                    } else if (strcmp(key, "UnitFileState") == 0) {
                        if (strcmp(val, "enabled") == 0) cur->enabled = true;
                    }
                }
            }
            line = strtok_r(NULL, "\r\n", &saveptr);
        }
        free(output);

        for (int i = 0; i < count; i++) {
            if (list[i].pid > 0 && list[i].memory_bytes == 0) {
                read_proc_stats(list[i].pid, &list[i].memory_bytes, &list[i].cpu_percent);
            }
        }
    } else {
        /* Fallback if batch query fails */
        for (int i = 0; i < count; i++) {
            systemd_get_unit_status(names[i], scope, &list[i]);
        }
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
