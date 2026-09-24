#ifndef FIRE_DASH_LOG_VIEWER_H
#define FIRE_DASH_LOG_VIEWER_H

#include "app.h"

#define MAX_LOG_LINES 1000
#define MAX_LOG_LINE_LEN 1024

typedef struct {
    char text[MAX_LOG_LINE_LEN];
    bool is_err;
} LogLine;

typedef struct {
    LogLine lines[MAX_LOG_LINES];
    int count;
    int head;       /* Ring buffer write head */
    int scroll_offset; /* 0 = bottom / auto-scroll, >0 = scrolled up */
    char current_app[MAX_NAME_LEN];
    SystemdScope current_scope;
    char search_filter[128];
    bool auto_scroll;
    FILE *stream_pipe;
} LogViewer;

/* Initializes log viewer state */
void log_viewer_init(LogViewer *lv);

/* Cleans up active pipes or buffers */
void log_viewer_cleanup(LogViewer *lv);

/* Switches the current app whose logs are being viewed */
void log_viewer_set_app(LogViewer *lv, const char *app_name, SystemdScope scope);

/* Polls for new incoming log lines (non-blocking) */
void log_viewer_poll(LogViewer *lv);

/* Sets the search query filter string */
void log_viewer_set_filter(LogViewer *lv, const char *filter);

/* Scroll navigation */
void log_viewer_scroll_up(LogViewer *lv, int delta);
void log_viewer_scroll_down(LogViewer *lv, int delta);
void log_viewer_scroll_to_bottom(LogViewer *lv);

/* Appends a single line to the ring buffer */
void log_viewer_append(LogViewer *lv, const char *text, bool is_err);

#endif /* FIRE_DASH_LOG_VIEWER_H */
