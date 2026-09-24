#ifndef FIRE_DASH_DBUS_SYSTEMD_H
#define FIRE_DASH_DBUS_SYSTEMD_H

#include "app.h"

/* Initializes systemd / dbus connection */
int systemd_init(SystemdScope scope);

/* Closes connection and cleans up */
void systemd_cleanup(void);

/* Triggers systemd daemon-reload */
int systemd_daemon_reload(SystemdScope scope);

/* Unit lifecycle operations */
int systemd_start_unit(const char *name, SystemdScope scope);
int systemd_stop_unit(const char *name, SystemdScope scope);
int systemd_restart_unit(const char *name, SystemdScope scope);
int systemd_enable_unit(const char *name, SystemdScope scope);
int systemd_disable_unit(const char *name, SystemdScope scope);
int systemd_reset_failed(const char *name, SystemdScope scope);

/* Fetches live process status, PID, CPU and Memory for a single app */
int systemd_get_unit_status(const char *name, SystemdScope scope, ProcessInfo *info);

/* Lists all fire-dash managed units with their live statistics */
int systemd_list_all(SystemdScope scope, ProcessInfo **out_list, int *out_count);

/* Frees ProcessInfo list allocated by systemd_list_all */
void systemd_free_list(ProcessInfo *list, int count);

#endif /* FIRE_DASH_DBUS_SYSTEMD_H */
