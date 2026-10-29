#include "updater.h"
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <errno.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

bool updater_is_newer(const char *remote, const char *local) {
    if (!remote || !local) return false;

    while (*remote == 'v' || *remote == 'V' || isspace((unsigned char)*remote)) remote++;
    while (*local == 'v' || *local == 'V' || isspace((unsigned char)*local)) local++;

    int r_maj = 0, r_min = 0, r_pat = 0;
    int l_maj = 0, l_min = 0, l_pat = 0;

    sscanf(remote, "%d.%d.%d", &r_maj, &r_min, &r_pat);
    sscanf(local, "%d.%d.%d", &l_maj, &l_min, &l_pat);

    if (r_maj > l_maj) return true;
    if (r_maj < l_maj) return false;

    if (r_min > l_min) return true;
    if (r_min < l_min) return false;

    return r_pat > l_pat;
}

void updater_get_platform(char *out_os, size_t os_len, char *out_arch, size_t arch_len) {
#if defined(__APPLE__)
    snprintf(out_os, os_len, "darwin");
#elif defined(__linux__)
    snprintf(out_os, os_len, "linux");
#else
    snprintf(out_os, os_len, "unknown");
#endif

    struct utsname un;
    if (uname(&un) == 0) {
        if (strcmp(un.machine, "x86_64") == 0 || strcmp(un.machine, "amd64") == 0) {
            snprintf(out_arch, arch_len, "amd64");
        } else if (strcmp(un.machine, "aarch64") == 0 || strcmp(un.machine, "arm64") == 0) {
            snprintf(out_arch, arch_len, "arm64");
        } else {
            snprintf(out_arch, arch_len, "%s", un.machine);
        }
    } else {
        snprintf(out_arch, arch_len, "amd64");
    }
}

int updater_get_self_path(char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return -1;

#if defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", out_path, max_len - 1);
    if (len > 0) {
        out_path[len] = '\0';
        return 0;
    }
#elif defined(__APPLE__)
    uint32_t bufsize = (uint32_t)max_len;
    char raw_path[max_len];
    if (_NSGetExecutablePath(raw_path, &bufsize) == 0) {
        char resolved[max_len];
        if (realpath(raw_path, resolved)) {
            snprintf(out_path, max_len, "%s", resolved);
        } else {
            snprintf(out_path, max_len, "%s", raw_path);
        }
        return 0;
    }
#endif

    return -1;
}

static void get_cache_path(char *out, size_t max_len) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        snprintf(out, max_len, "%s/fdash", xdg);
    } else {
        const char *home = getenv("HOME");
        if (home && *home) {
            snprintf(out, max_len, "%s/.cache/fdash", home);
        } else {
            snprintf(out, max_len, "/tmp/fdash_cache_%d", (int)getuid());
        }
    }
    mkdir(out, 0755);
    size_t len = strlen(out);
    snprintf(out + len, max_len - len, "/version_cache");
}

int updater_check_version(char *out_ver, size_t max_len, bool *out_has_update, bool force) {
    if (!out_ver || max_len == 0 || !out_has_update) return -1;
    *out_has_update = false;
    out_ver[0] = '\0';

    char cache_file[MAX_PATH_LEN];
    get_cache_path(cache_file, sizeof(cache_file));

    time_t now = time(NULL);

    /* Check cached version if not forced */
    if (!force) {
        FILE *cfp = fopen(cache_file, "r");
        if (cfp) {
            long c_time = 0;
            char cached_ver[64] = {0};
            if (fscanf(cfp, "%ld %63s", &c_time, cached_ver) == 2) {
                time_t cached_time = (time_t)c_time;
                fclose(cfp);
                if (now >= cached_time && (now - cached_time) < (6 * 3600)) {
                    snprintf(out_ver, max_len, "%s", cached_ver);
                    *out_has_update = updater_is_newer(cached_ver, FIRE_VERSION);
                    return 0;
                }
            } else {
                fclose(cfp);
            }
        }
    }

    /* Fast check: fetch raw VERSION file from main branch (uses no GitHub API quota) */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "curl -fsSL --connect-timeout 2 --max-time 3 "
             "https://raw.githubusercontent.com/%s/%s/main/VERSION 2>/dev/null",
             FIRE_REPO_OWNER, FIRE_REPO_NAME);

    FILE *pipe = popen(cmd, "r");
    char fetched_ver[64] = {0};
    if (pipe) {
        if (fgets(fetched_ver, sizeof(fetched_ver), pipe)) {
            size_t flen = strlen(fetched_ver);
            while (flen > 0 && (fetched_ver[flen - 1] == '\r' || fetched_ver[flen - 1] == '\n' || isspace((unsigned char)fetched_ver[flen - 1]))) {
                fetched_ver[--flen] = '\0';
            }
        }
        pclose(pipe);
    }

    /* Fallback: query GitHub Releases API if raw VERSION was empty */
    if (fetched_ver[0] == '\0') {
        snprintf(cmd, sizeof(cmd),
                 "curl -fsSL -H \"User-Agent: fdash\" --connect-timeout 2 --max-time 3 "
                 "https://api.github.com/repos/%s/%s/releases/latest 2>/dev/null",
                 FIRE_REPO_OWNER, FIRE_REPO_NAME);

        pipe = popen(cmd, "r");
        if (pipe) {
            char line[512];
            while (fgets(line, sizeof(line), pipe)) {
                char *tag = strstr(line, "\"tag_name\":");
                if (tag) {
                    char *quote1 = strchr(tag + 11, '"');
                    if (quote1) {
                        char *quote2 = strchr(quote1 + 1, '"');
                        if (quote2) {
                            size_t tag_len = (size_t)(quote2 - quote1 - 1);
                            if (tag_len < sizeof(fetched_ver)) {
                                strncpy(fetched_ver, quote1 + 1, tag_len);
                                fetched_ver[tag_len] = '\0';
                            }
                        }
                    }
                    break;
                }
            }
            pclose(pipe);
        }
    }

    if (fetched_ver[0] == '\0') {
        return -1;
    }

    /* Strip leading 'v' */
    const char *clean_ver = fetched_ver;
    if (*clean_ver == 'v' || *clean_ver == 'V') clean_ver++;

    snprintf(out_ver, max_len, "%s", clean_ver);

    /* Update cache file */
    FILE *cfp = fopen(cache_file, "w");
    if (cfp) {
        fprintf(cfp, "%ld %s\n", (long)now, clean_ver);
        fclose(cfp);
    }

    *out_has_update = updater_is_newer(clean_ver, FIRE_VERSION);
    return 0;
}

int updater_self_update(const char *target_version) {
    char self_path[MAX_PATH_LEN];
    if (updater_get_self_path(self_path, sizeof(self_path)) != 0) {
        fprintf(stderr, "\033[31;1mError:\033[0m Could not determine executable path of fdash.\n");
        return -1;
    }

    char os[32], arch[32];
    updater_get_platform(os, sizeof(os), arch, sizeof(arch));

    char check_ver[64];
    bool has_update = false;

    if (!target_version || !*target_version) {
        printf("🔍 Checking for updates at \033[1m%s/%s\033[0m...\n", FIRE_REPO_OWNER, FIRE_REPO_NAME);
        if (updater_check_version(check_ver, sizeof(check_ver), &has_update, true) != 0) {
            fprintf(stderr, "\033[33;1mNotice:\033[0m Could not retrieve remote version from %s/%s.\n"
                            "        Ensure release tags exist on GitHub, or check your network connection.\n",
                    FIRE_REPO_OWNER, FIRE_REPO_NAME);
            return -1;
        }

        if (!has_update) {
            printf("\033[32;1m✔ fdash is already up-to-date\033[0m (v%s)\n", FIRE_VERSION);
            return 0;
        }
        target_version = check_ver;
    }

    printf("📦 New version found: \033[38;5;214;1mv%s\033[0m (current: v%s)\n", target_version, FIRE_VERSION);
    printf("   Platform: \033[38;5;51m%s-%s\033[0m\n", os, arch);
    printf("   Target:   \033[38;5;244m%s\033[0m\n\n", self_path);

    /* Form download URL */
    char download_url[512];
    snprintf(download_url, sizeof(download_url),
             "https://github.com/%s/%s/releases/download/v%s/fdash-%s-%s",
             FIRE_REPO_OWNER, FIRE_REPO_NAME, target_version, os, arch);

    char tmp_path[MAX_PATH_LEN + 32];
    snprintf(tmp_path, sizeof(tmp_path), "%s.new.%d", self_path, (int)getpid());

    printf("⬇ Downloading update from GitHub Releases...\n");
    char dl_cmd[1024];
    snprintf(dl_cmd, sizeof(dl_cmd),
             "curl -fSL --progress-bar -o \"%s\" \"%s\"",
             tmp_path, download_url);

    int dl_status = system(dl_cmd);
    if (dl_status != 0 || access(tmp_path, F_OK) != 0) {
        /* Fallback: try latest release download */
        snprintf(download_url, sizeof(download_url),
                 "https://github.com/%s/%s/releases/latest/download/fdash-%s-%s",
                 FIRE_REPO_OWNER, FIRE_REPO_NAME, os, arch);
        snprintf(dl_cmd, sizeof(dl_cmd),
                 "curl -fSL --progress-bar -o \"%s\" \"%s\"",
                 tmp_path, download_url);
        dl_status = system(dl_cmd);
    }

    if (dl_status != 0 || access(tmp_path, F_OK) != 0) {
        unlink(tmp_path);
        fprintf(stderr, "\n\033[31;1mError:\033[0m Failed to download binary asset from:\n  %s\n", download_url);
        fprintf(stderr, "Ensure the release asset 'fdash-%s-%s' exists on GitHub.\n", os, arch);
        return -1;
    }

    /* Set executable permissions */
    chmod(tmp_path, 0755);

    /* Atomically replace current binary */
    if (rename(tmp_path, self_path) != 0) {
        if (errno == EACCES || errno == EPERM) {
            unlink(tmp_path);
            fprintf(stderr, "\n\033[31;1mPermission Denied:\033[0m Cannot write to '%s'.\n", self_path);
            fprintf(stderr, "Please re-run the update with elevated permissions:\n\n");
            fprintf(stderr, "  \033[1msudo fdash update\033[0m\n\n");
            return -1;
        } else {
            unlink(tmp_path);
            fprintf(stderr, "\n\033[31;1mError:\033[0m rename failed: %s\n", strerror(errno));
            return -1;
        }
    }

    /* Refresh version cache */
    char cache_file[MAX_PATH_LEN];
    get_cache_path(cache_file, sizeof(cache_file));
    FILE *cfp = fopen(cache_file, "w");
    if (cfp) {
        fprintf(cfp, "%ld %s\n", (long)time(NULL), target_version);
        fclose(cfp);
    }

    printf("\n\033[38;5;48;1m✔ Successfully updated fdash to v%s!\033[0m\n", target_version);
    printf("Run \033[1mfdash --version\033[0m or launch \033[1mfdash\033[0m to use the new version.\n\n");
    return 0;
}
