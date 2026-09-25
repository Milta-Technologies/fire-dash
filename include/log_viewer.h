#ifndef FIRE_DASH_LOG_VIEWER_H
#define FIRE_DASH_LOG_VIEWER_H

#include "app.h"

#define MAX_LOG_LINES 2000
#define MAX_LOG_LINE_LEN 1024

typedef enum {
    LOG_LEVEL_ALL = 0,
    LOG_LEVEL_WARN_ERR = 1,
    LOG_LEVEL_ERR_ONLY = 2
} LogViewerLevel;

typedef struct {
    char text[MAX_LOG_LINE_LEN];
    char app_name[MAX_NAME_LEN];
    bool is_err;
    bool is_warn;
    bool is_marker;
} LogLine;

typedef struct {
    LogLine lines[MAX_LOG_LINES];
    int count;
    int head;       /* Ring buffer write head */
    int scroll_offset; /* 0 = bottom / auto-scroll, >0 = scrolled up */
    char current_app[MAX_NAME_LEN];
    bool is_all_apps;
    SystemdScope current_scope;
    char search_filter[128];
    LogViewerLevel level_filter;
    bool auto_scroll;
    bool is_paused;
    int paused_buffered_count;
    FILE *stream_pipe;
} LogViewer;

/* Initializes log viewer state */
void log_viewer_init(LogViewer *lv);

/* Cleans up active pipes or buffers */
void log_viewer_cleanup(LogViewer *lv);

/* Switches the current app whose logs are being viewed ("ALL" or NULL for all apps) */
void log_viewer_set_app(LogViewer *lv, const char *app_name, SystemdScope scope);

/* Polls for new incoming log lines (non-blocking) */
void log_viewer_poll(LogViewer *lv);

/* Sets the search query filter string */
void log_viewer_set_filter(LogViewer *lv, const char *filter);

/* Sets the log triage level filter */
void log_viewer_set_level_filter(LogViewer *lv, LogViewerLevel level);
const char *log_viewer_level_name(LogViewerLevel level);

/* Stream controls */
void log_viewer_toggle_pause(LogViewer *lv);
void log_viewer_add_marker(LogViewer *lv);

/* Scroll navigation */
void log_viewer_scroll_up(LogViewer *lv, int delta);
void log_viewer_scroll_down(LogViewer *lv, int delta);
void log_viewer_scroll_to_bottom(LogViewer *lv);

/* Appends a single line to the ring buffer */
void log_viewer_append(LogViewer *lv, const char *text, bool is_err);

/* Syntax highlighting & formatting: formats a LogLine with colors and search highlight into out */
void log_viewer_format_colored_line(const LogLine *ll, const char *search_filter, bool show_app_tag,
                                    char *out, size_t out_cap, int max_visible_width);

#endif /* FIRE_DASH_LOG_VIEWER_H */
