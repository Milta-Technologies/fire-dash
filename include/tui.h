#ifndef FIRE_DASH_TUI_H
#define FIRE_DASH_TUI_H

#include "app.h"
#include "log_viewer.h"

typedef enum {
    FOCUS_PROCESSES = 0,
    FOCUS_LOGS = 1
} TUIFocus;

typedef struct {
    int rows;
    int cols;
    int selected_idx;       /* 0 = [ALL APPS], 1..N = apps[0..N-1] */
    int last_app_idx;       /* Last non-zero selected app index */
    bool fullscreen_logs;   /* Toggle between 50/50 split and fullscreen logs */
    TUIFocus focus;
    bool search_mode;
    char search_input[128];
    char status_msg[128];
    time_t status_msg_time;
    SystemdScope scope;
    LogViewer log_viewer;
    bool should_quit;
} TUIState;

/* Starts the interactive TUI event loop */
int tui_run(SystemdScope scope);

#endif /* FIRE_DASH_TUI_H */
