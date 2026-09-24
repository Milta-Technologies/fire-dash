#include "log_viewer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

void log_viewer_init(LogViewer *lv) {
    if (!lv) return;
    memset(lv, 0, sizeof(*lv));
    lv->auto_scroll = true;
}

void log_viewer_cleanup(LogViewer *lv) {
    if (!lv) return;
    if (lv->stream_pipe) {
        pclose(lv->stream_pipe);
        lv->stream_pipe = NULL;
    }
}

void log_viewer_append(LogViewer *lv, const char *text, bool is_err) {
    if (!lv || !text) return;

    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == '\n')) {
        len--;
    }
    if (len == 0) return;

    int idx = (lv->head + lv->count) % MAX_LOG_LINES;
    if (lv->count == MAX_LOG_LINES) {
        /* Overwrite oldest */
        idx = lv->head;
        lv->head = (lv->head + 1) % MAX_LOG_LINES;
    } else {
        lv->count++;
    }

    size_t copy_len = len < (MAX_LOG_LINE_LEN - 1) ? len : (MAX_LOG_LINE_LEN - 1);
    memcpy(lv->lines[idx].text, text, copy_len);
    lv->lines[idx].text[copy_len] = '\0';

    /* Auto-detect error strings if is_err is false */
    if (!is_err) {
        if (strcasestr(text, "error") || strcasestr(text, "failed") ||
            strcasestr(text, "exception") || strcasestr(text, "fatal") ||
            strcasestr(text, "panic") || strcasestr(text, "warn")) {
            is_err = true;
        }
    }
    lv->lines[idx].is_err = is_err;

    if (lv->auto_scroll) {
        lv->scroll_offset = 0;
    }
}

void log_viewer_set_app(LogViewer *lv, const char *app_name, SystemdScope scope) {
    if (!lv) return;

    log_viewer_cleanup(lv);

    lv->count = 0;
    lv->head = 0;
    lv->scroll_offset = 0;
    lv->auto_scroll = true;

    if (!app_name || !*app_name) {
        lv->current_app[0] = '\0';
        return;
    }

    snprintf(lv->current_app, sizeof(lv->current_app), "%s", app_name);
    lv->current_scope = scope;

    /* 1. Initial backlog load */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "journalctl %s -u %s%s.service -n 100 --output=short-iso --no-pager 2>/dev/null",
             scope == SCOPE_USER ? "--user" : "", FIRE_UNIT_PREFIX, app_name);

    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[MAX_LOG_LINE_LEN];
        while (fgets(line, sizeof(line), fp)) {
            log_viewer_append(lv, line, false);
        }
        pclose(fp);
    }

    /* 2. Open non-blocking live stream */
    snprintf(cmd, sizeof(cmd),
             "journalctl %s -u %s%s.service -f -n 0 --output=short-iso --no-pager 2>/dev/null",
             scope == SCOPE_USER ? "--user" : "", FIRE_UNIT_PREFIX, app_name);

    lv->stream_pipe = popen(cmd, "r");
    if (lv->stream_pipe) {
        int fd = fileno(lv->stream_pipe);
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
}

void log_viewer_poll(LogViewer *lv) {
    if (!lv || !lv->stream_pipe) return;

    char buffer[2048];
    ssize_t bytes_read;
    int fd = fileno(lv->stream_pipe);

    while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        char *line = strtok(buffer, "\r\n");
        while (line) {
            log_viewer_append(lv, line, false);
            line = strtok(NULL, "\r\n");
        }
    }
}

void log_viewer_set_filter(LogViewer *lv, const char *filter) {
    if (!lv) return;
    if (filter) {
        snprintf(lv->search_filter, sizeof(lv->search_filter), "%s", filter);
    } else {
        lv->search_filter[0] = '\0';
    }
    lv->scroll_offset = 0;
}

void log_viewer_scroll_up(LogViewer *lv, int delta) {
    if (!lv || lv->count == 0) return;
    lv->auto_scroll = false;
    lv->scroll_offset += delta;
    if (lv->scroll_offset >= lv->count) {
        lv->scroll_offset = lv->count - 1;
    }
}

void log_viewer_scroll_down(LogViewer *lv, int delta) {
    if (!lv) return;
    lv->scroll_offset -= delta;
    if (lv->scroll_offset <= 0) {
        lv->scroll_offset = 0;
        lv->auto_scroll = true;
    }
}

void log_viewer_scroll_to_bottom(LogViewer *lv) {
    if (!lv) return;
    lv->scroll_offset = 0;
    lv->auto_scroll = true;
}
