#ifndef FIRE_DASH_APP_H
#define FIRE_DASH_APP_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

#define FIRE_VERSION "1.0.0"
#define FIRE_UNIT_PREFIX "fire-"
#define MAX_NAME_LEN 64
#define MAX_PATH_LEN 1024
#define MAX_CMD_LEN 2048
#define MAX_ENV_VARS 128

typedef enum {
    SCOPE_USER = 0,
    SCOPE_SYSTEM = 1
} SystemdScope;

typedef struct {
    char key[128];
    char value[1024];
} EnvVar;

typedef struct {
    char name[MAX_NAME_LEN];
    char script[MAX_CMD_LEN];
    char cwd[MAX_PATH_LEN];
    EnvVar envs[MAX_ENV_VARS];
    int env_count;
    char watch[MAX_PATH_LEN];
    int restart_sec;
    SystemdScope scope;
} AppService;

typedef struct {
    char name[MAX_NAME_LEN];
    char unit_name[MAX_NAME_LEN + 16];
    char active_state[32];   /* "active", "inactive", "failed", "activating", etc. */
    char sub_state[32];      /* "running", "dead", "failed", "exited", etc. */
    pid_t pid;
    uint64_t memory_bytes;
    double cpu_percent;
    uint64_t cpu_usage_nsec;
    uint64_t prev_cpu_usage_nsec;
    uint64_t prev_timestamp_nsec;
    time_t start_time;
    uint32_t restart_count;
    bool has_watch;
    char watch_path[MAX_PATH_LEN];
    bool enabled;
    SystemdScope scope;
} ProcessInfo;

#endif /* FIRE_DASH_APP_H */
