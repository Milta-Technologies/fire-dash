#ifndef FIRE_DASH_UPDATER_H
#define FIRE_DASH_UPDATER_H

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define FIRE_REPO_OWNER "Milta-Technologies"
#define FIRE_REPO_NAME  "fire-dash"

/* Compares semantic versions (e.g. "1.1.0" > "1.0.0"). Returns true if remote is newer. */
bool updater_is_newer(const char *remote, const char *local);

/* Checks for updates against GitHub releases. Cached for 6 hours unless force is true.
 * Returns 0 on successful check (with *out_has_update set), -1 on error. */
int updater_check_version(char *out_ver, size_t max_len, bool *out_has_update, bool force);

/* Resolves the absolute path to the current running binary */
int updater_get_self_path(char *out_path, size_t max_len);

/* Detects current OS (linux/darwin) and architecture (amd64/arm64) */
void updater_get_platform(char *out_os, size_t os_len, char *out_arch, size_t arch_len);

/* Performs self-update by downloading the binary for the host OS/arch and atomically replacing self */
int updater_self_update(const char *target_version);

#endif /* FIRE_DASH_UPDATER_H */
