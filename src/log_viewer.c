#include "log_viewer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>

void log_viewer_init(LogViewer *lv) {
    if (!lv) return;
    memset(lv, 0, sizeof(*lv));
    lv->auto_scroll = true;
    lv->level_filter = LOG_LEVEL_ALL;
}

void log_viewer_cleanup(LogViewer *lv) {
    if (!lv) return;
    if (lv->stream_pipe) {
        pclose(lv->stream_pipe);
        lv->stream_pipe = NULL;
    }
}

void log_viewer_set_level_filter(LogViewer *lv, LogViewerLevel level) {
    if (!lv) return;
    lv->level_filter = level;
    lv->scroll_offset = 0;
}

const char *log_viewer_level_name(LogViewerLevel level) {
    switch (level) {
        case LOG_LEVEL_WARN_ERR: return "WARN+ERR";
        case LOG_LEVEL_ERR_ONLY: return "ERR ONLY";
        case LOG_LEVEL_ALL:
        default:
            return "ALL";
    }
}

void log_viewer_toggle_pause(LogViewer *lv) {
    if (!lv) return;
    lv->is_paused = !lv->is_paused;
    if (!lv->is_paused) {
        lv->paused_buffered_count = 0;
        lv->auto_scroll = true;
        lv->scroll_offset = 0;
    }
}

void log_viewer_add_marker(LogViewer *lv) {
    if (!lv) return;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    char marker_text[128];
    snprintf(marker_text, sizeof(marker_text), "─── [MARK: %s] ─── (checkpoint)", time_str);

    int idx = (lv->head + lv->count) % MAX_LOG_LINES;
    if (lv->count == MAX_LOG_LINES) {
        idx = lv->head;
        lv->head = (lv->head + 1) % MAX_LOG_LINES;
    } else {
        lv->count++;
    }

    LogLine *ll = &lv->lines[idx];
    memset(ll, 0, sizeof(*ll));
    snprintf(ll->text, sizeof(ll->text), "%s", marker_text);
    ll->is_marker = true;

    if (!lv->is_paused && lv->auto_scroll) {
        lv->scroll_offset = 0;
    }
}

static void extract_app_name(const char *text, char *out, size_t out_cap) {
    out[0] = '\0';
    if (!text) return;

    const char *p = strstr(text, FIRE_UNIT_PREFIX);
    if (!p) return;

    p += strlen(FIRE_UNIT_PREFIX);
    size_t i = 0;
    while (*p && *p != '.' && *p != '[' && *p != ':' && *p != ' ' && *p != '/' && i < out_cap - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
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
        idx = lv->head;
        lv->head = (lv->head + 1) % MAX_LOG_LINES;
    } else {
        lv->count++;
    }

    LogLine *ll = &lv->lines[idx];
    memset(ll, 0, sizeof(*ll));

    size_t copy_len = len < (MAX_LOG_LINE_LEN - 1) ? len : (MAX_LOG_LINE_LEN - 1);
    memcpy(ll->text, text, copy_len);
    ll->text[copy_len] = '\0';

    /* App tagging */
    if (lv->is_all_apps) {
        extract_app_name(text, ll->app_name, sizeof(ll->app_name));
        if (ll->app_name[0] == '\0') {
            snprintf(ll->app_name, sizeof(ll->app_name), "system");
        }
    } else if (lv->current_app[0]) {
        snprintf(ll->app_name, sizeof(ll->app_name), "%s", lv->current_app);
    }

    /* Auto-detect error and warning */
    if (!is_err) {
        if (strcasestr(text, "error") || strcasestr(text, "failed") ||
            strcasestr(text, "exception") || strcasestr(text, "fatal") ||
            strcasestr(text, "panic") || strcasestr(text, "errno") ||
            strstr(text, " 500 ") || strstr(text, " 502 ") ||
            strstr(text, " 503 ") || strstr(text, " 504 ")) {
            is_err = true;
        }
    }
    ll->is_err = is_err;

    if (!is_err) {
        if (strcasestr(text, "warn") || strcasestr(text, "warning") ||
            strstr(text, " 400 ") || strstr(text, " 401 ") ||
            strstr(text, " 403 ") || strstr(text, " 404 ") ||
            strstr(text, " 422 ")) {
            ll->is_warn = true;
        }
    }

    /* Pause and scroll offset management */
    if (lv->is_paused) {
        lv->paused_buffered_count++;
    } else if (lv->auto_scroll) {
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
    lv->is_paused = false;
    lv->paused_buffered_count = 0;

    if (!app_name || !*app_name || strcmp(app_name, "ALL") == 0) {
        lv->is_all_apps = true;
        snprintf(lv->current_app, sizeof(lv->current_app), "ALL");
        lv->current_scope = scope;

        /* All-apps stream using wildcard pattern */
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "journalctl %s -u '%s*.service' -n 120 --output=with-unit --no-pager 2>/dev/null",
                 scope == SCOPE_USER ? "--user" : "", FIRE_UNIT_PREFIX);

        FILE *fp = popen(cmd, "r");
        if (fp) {
            char line[MAX_LOG_LINE_LEN];
            while (fgets(line, sizeof(line), fp)) {
                log_viewer_append(lv, line, false);
            }
            pclose(fp);
        }

        snprintf(cmd, sizeof(cmd),
                 "journalctl %s -u '%s*.service' -f -n 0 --output=with-unit --no-pager 2>/dev/null",
                 scope == SCOPE_USER ? "--user" : "", FIRE_UNIT_PREFIX);

        lv->stream_pipe = popen(cmd, "r");
        if (lv->stream_pipe) {
            int fd = fileno(lv->stream_pipe);
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) {
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }
        }
        return;
    }

    lv->is_all_apps = false;
    snprintf(lv->current_app, sizeof(lv->current_app), "%s", app_name);
    lv->current_scope = scope;

    /* Single app backlog load */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "journalctl %s -u %s%s.service -n 120 --output=short-iso --no-pager 2>/dev/null",
             scope == SCOPE_USER ? "--user" : "", FIRE_UNIT_PREFIX, app_name);

    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[MAX_LOG_LINE_LEN];
        while (fgets(line, sizeof(line), fp)) {
            log_viewer_append(lv, line, false);
        }
        pclose(fp);
    }

    /* Single app live stream */
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

    char buffer[4096];
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
        if (!lv->is_paused) {
            lv->auto_scroll = true;
        }
    }
}

void log_viewer_scroll_to_bottom(LogViewer *lv) {
    if (!lv) return;
    lv->scroll_offset = 0;
    if (!lv->is_paused) {
        lv->auto_scroll = true;
    }
}

/* Syntax Highlighting & Token Colorizer */
static bool is_token_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

void log_viewer_format_colored_line(const LogLine *ll, const char *search_filter, bool show_app_tag,
                                    char *out, size_t out_cap, int max_visible_width) {
    if (!ll || !out || out_cap == 0) return;
    out[0] = '\0';

    if (max_visible_width <= 4) return;

    /* 1. Marker line rendering */
    if (ll->is_marker) {
        snprintf(out, out_cap, "\033[38;5;213;1m═══ %.*s ═══\033[0m", max_visible_width - 8, ll->text);
        return;
    }

    size_t out_len = 0;
    int visible_len = 0;

    #define APPEND_STR(s) do { \
        size_t slen = strlen(s); \
        if (out_len + slen < out_cap - 1) { \
            memcpy(out + out_len, (s), slen); \
            out_len += slen; \
            out[out_len] = '\0'; \
        } \
    } while (0)

    #define APPEND_CHAR(c) do { \
        if (out_len + 1 < out_cap - 1 && visible_len < max_visible_width) { \
            out[out_len++] = (c); \
            out[out_len] = '\0'; \
            visible_len++; \
        } \
    } while (0)

    /* 2. App Badge prefix if requested */
    if (show_app_tag && ll->app_name[0]) {
        static const int palette[] = { 39, 76, 214, 207, 45, 220, 141, 48 };
        unsigned int h = 5381;
        for (const char *p = ll->app_name; *p; p++) h = ((h << 5) + h) + (unsigned char)*p;
        int col = palette[h % 8];

        char badge[64];
        snprintf(badge, sizeof(badge), "\033[38;5;%d;1m[%-10.10s]\033[0m ", col, ll->app_name);
        APPEND_STR(badge);
        visible_len += 13;
    }

    /* Base color depending on line severity */
    const char *base_color = "\033[38;5;250m";
    if (ll->is_err) {
        base_color = "\033[38;5;203m";
    } else if (ll->is_warn) {
        base_color = "\033[38;5;222m";
    }
    APPEND_STR(base_color);

    const char *src = ll->text;
    size_t filter_len = (search_filter && *search_filter) ? strlen(search_filter) : 0;

    /* Check for leading ISO or Syslog timestamp */
    bool has_iso_ts = (isdigit((unsigned char)src[0]) && isdigit((unsigned char)src[1]) &&
                       isdigit((unsigned char)src[2]) && isdigit((unsigned char)src[3]) &&
                       src[4] == '-' && isdigit((unsigned char)src[5]));

    if (has_iso_ts) {
        /* Colorize ISO timestamp in dimmed gray */
        APPEND_STR("\033[38;5;244m");
        while (*src && *src != ' ' && visible_len < max_visible_width) {
            APPEND_CHAR(*src++);
        }
        APPEND_STR("\033[0m");
        APPEND_STR(base_color);
    }

    /* Process line word by word */
    while (*src && visible_len < max_visible_width) {
        /* Check search filter match */
        if (filter_len > 0 && strncasecmp(src, search_filter, filter_len) == 0) {
            APPEND_STR("\033[48;5;226;38;5;16;1m");
            for (size_t k = 0; k < filter_len && *src && visible_len < max_visible_width; k++) {
                APPEND_CHAR(*src++);
            }
            APPEND_STR("\033[0m");
            APPEND_STR(base_color);
            continue;
        }

        /* If start of word/token */
        if (is_token_char(*src)) {
            char word[64];
            size_t wlen = 0;
            const char *start = src;
            while (*src && is_token_char(*src) && wlen < sizeof(word) - 1) {
                word[wlen++] = *src++;
            }
            word[wlen] = '\0';

            /* Check keyword patterns */
            bool colored = false;
            if (strcmp(word, "GET") == 0 || strcmp(word, "POST") == 0 ||
                strcmp(word, "PUT") == 0 || strcmp(word, "DELETE") == 0 ||
                strcmp(word, "PATCH") == 0 || strcmp(word, "HEAD") == 0 ||
                strcmp(word, "OPTIONS") == 0) {
                APPEND_STR("\033[38;5;51;1m");
                colored = true;
            } else if (wlen == 3 && isdigit((unsigned char)word[0]) &&
                       isdigit((unsigned char)word[1]) && isdigit((unsigned char)word[2])) {
                /* HTTP Status code */
                if (word[0] == '2') {
                    APPEND_STR("\033[38;5;48;1m"); /* Green 2xx */
                    colored = true;
                } else if (word[0] == '3') {
                    APPEND_STR("\033[38;5;45;1m"); /* Cyan 3xx */
                    colored = true;
                } else if (word[0] == '4') {
                    APPEND_STR("\033[38;5;220;1m"); /* Yellow 4xx */
                    colored = true;
                } else if (word[0] == '5') {
                    APPEND_STR("\033[38;5;196;1m"); /* Red 5xx */
                    colored = true;
                }
            } else if (strcasecmp(word, "error") == 0 || strcasecmp(word, "fatal") == 0 ||
                       strcasecmp(word, "fail") == 0 || strcasecmp(word, "failed") == 0 ||
                       strcasecmp(word, "exception") == 0 || strcasecmp(word, "panic") == 0) {
                APPEND_STR("\033[38;5;196;1m"); /* Red error */
                colored = true;
            } else if (strcasecmp(word, "warn") == 0 || strcasecmp(word, "warning") == 0) {
                APPEND_STR("\033[38;5;214;1m"); /* Amber warn */
                colored = true;
            } else if (strcasecmp(word, "info") == 0) {
                APPEND_STR("\033[38;5;39m"); /* Blue info */
                colored = true;
            } else if (strcasecmp(word, "success") == 0 || strcasecmp(word, "ready") == 0 ||
                       strcasecmp(word, "online") == 0) {
                APPEND_STR("\033[38;5;48;1m"); /* Green success */
                colored = true;
            }

            /* Print word characters with potential search highlights inside */
            for (size_t i = 0; i < wlen && visible_len < max_visible_width; i++) {
                if (filter_len > 0 && strncasecmp(start + i, search_filter, filter_len) == 0) {
                    APPEND_STR("\033[48;5;226;38;5;16;1m");
                    for (size_t k = 0; k < filter_len && i < wlen && visible_len < max_visible_width; k++, i++) {
                        APPEND_CHAR(word[i]);
                    }
                    i--; /* loop counter compensation */
                    APPEND_STR("\033[0m");
                    if (colored) {
                        /* Re-apply token color */
                        if (strcmp(word, "GET") == 0 || strcmp(word, "POST") == 0) APPEND_STR("\033[38;5;51;1m");
                        else APPEND_STR(base_color);
                    } else {
                        APPEND_STR(base_color);
                    }
                    continue;
                }
                APPEND_CHAR(word[i]);
            }

            if (colored) {
                APPEND_STR("\033[0m");
                APPEND_STR(base_color);
            }
        } else {
            APPEND_CHAR(*src++);
        }
    }

    APPEND_STR("\033[0m");

    #undef APPEND_STR
    #undef APPEND_CHAR
}
