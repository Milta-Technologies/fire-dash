#include "tui.h"
#include "dbus_systemd.h"
#include "unit_gen.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <poll.h>
#include <ctype.h>
#include <stdarg.h>

static struct termios orig_termios;
static volatile sig_atomic_t g_win_resized = 0;
static bool g_raw_enabled = false;

static void handle_winch(int sig) {
    (void)sig;
    g_win_resized = 1;
}

static void disable_raw_mode(void) {
    if (!g_raw_enabled) return;
    /* Disable mouse tracking, show cursor, restore main screen */
    write(STDOUT_FILENO, "\033[?1006l\033[?1000l\033[?25h\033[?1049l", 28);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    g_raw_enabled = false;
}

static void handle_sig_exit(int sig) {
    (void)sig;
    disable_raw_mode();
    _exit(0);
}

static void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    /* Switch to alternate screen, hide cursor, enable SGR mouse tracking */
    write(STDOUT_FILENO, "\033[?1049h\033[?25l\033[?1000h\033[?1006h", 28);
    g_raw_enabled = true;
    atexit(disable_raw_mode);
}

static void update_window_size(TUIState *state) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        state->rows = ws.ws_row;
        state->cols = ws.ws_col;
    } else {
        state->rows = 24;
        state->cols = 80;
    }
}

/* Dynamic string buffer for single-write rendering */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ScreenBuffer;

static void sb_init(ScreenBuffer *sb) {
    sb->cap = 32768;
    sb->data = malloc(sb->cap);
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

static void sb_free(ScreenBuffer *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void sb_append(ScreenBuffer *sb, const char *s, size_t slen) {
    if (!sb->data) return;
    if (sb->len + slen + 1 >= sb->cap) {
        while (sb->len + slen + 1 >= sb->cap) {
            sb->cap *= 2;
        }
        char *new_data = realloc(sb->data, sb->cap);
        if (!new_data) return;
        sb->data = new_data;
    }
    memcpy(sb->data + sb->len, s, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
}

static void sb_printf(ScreenBuffer *sb, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_append(sb, tmp, (size_t)n);
    }
}

static void format_mem(uint64_t bytes, char *out, size_t max_len) {
    if (bytes == 0) {
        snprintf(out, max_len, "0 B");
    } else if (bytes < 1024) {
        snprintf(out, max_len, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, max_len, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        snprintf(out, max_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, max_len, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

static void set_status(TUIState *state, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->status_msg, sizeof(state->status_msg), fmt, ap);
    va_end(ap);
    state->status_msg_time = time(NULL);
}

static bool copy_to_clipboard(const char *text) {
    if (!text || !*text) return false;
    FILE *fp = popen("pbcopy 2>/dev/null || wl-copy 2>/dev/null || xclip -selection clipboard 2>/dev/null || xsel --clipboard --input 2>/dev/null", "w");
    if (!fp) return false;
    fputs(text, fp);
    pclose(fp);
    return true;
}

static char *format_json_if_present(const char *text) {
    if (!text) return NULL;
    const char *start = strchr(text, '{');
    const char *end = strrchr(text, '}');
    if (!start || !end || end <= start) return NULL;

    size_t len = (size_t)(end - start + 1);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, start, len);
    copy[len] = '\0';

    cJSON *json = cJSON_Parse(copy);
    free(copy);
    if (!json) return NULL;

    char *printed = cJSON_Print(json);
    cJSON_Delete(json);
    return printed;
}

/* Detail Inspector Full-screen Overlay */
static void render_detail_inspector(ScreenBuffer *sb, TUIState *state, LogLine *ll, int match_idx, int match_total) {
    int cols = state->cols;
    int rows = state->rows;

    sb_append(sb, "\033[H", 3);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    /* Row 1: Header */
    sb_printf(sb, "\033[38;5;208;1m🔥 fdash\033[0m \033[38;5;244mv%s\033[0m", FIRE_VERSION);
    sb_printf(sb, "  \033[38;5;240m│\033[0m  \033[48;5;236;38;5;220;1m 🔍 LOG DETAIL INSPECTOR \033[0m");
    sb_printf(sb, "  \033[38;5;240m│\033[0m  Line \033[38;5;51;1m#%d\033[0m of \033[38;5;244m%d\033[0m", match_idx + 1, match_total);

    int cur_len = 54 + 12;
    int pad = cols - cur_len - (int)strlen(time_str);
    if (pad > 0) {
        for (int i = 0; i < pad; i++) sb_append(sb, " ", 1);
    }
    sb_printf(sb, "\033[38;5;244m%s\033[0m\033[K\r\n", time_str);

    /* Row 2: Separator */
    sb_append(sb, "\033[38;5;238m", 11);
    for (int i = 0; i < cols; i++) sb_append(sb, "─", 3);
    sb_append(sb, "\033[0m\033[K\r\n", 7);

    /* Row 3: Metadata bar */
    const char *sev_badge = "\033[38;5;48;1m● INFO / OK\033[0m";
    if (ll->is_err) {
        sev_badge = "\033[38;5;196;1m● ERROR / FATAL\033[0m";
    } else if (ll->is_warn) {
        sev_badge = "\033[38;5;220;1m▲ WARNING\033[0m";
    } else if (ll->is_marker) {
        sev_badge = "\033[38;5;213;1m❖ CHECKPOINT MARKER\033[0m";
    }

    static const int palette[] = { 39, 76, 214, 207, 45, 220, 141, 48 };
    unsigned int h = 5381;
    for (const char *p = ll->app_name; *p; p++) h = ((h << 5) + h) + (unsigned char)*p;
    int app_col = palette[h % 8];

    sb_printf(sb, "  App: \033[38;5;%d;1m[%s]\033[0m   Severity: %s   Length: \033[38;5;244m%lu chars\033[0m\033[K\r\n",
              app_col, ll->app_name[0] ? ll->app_name : "system", sev_badge, (unsigned long)strlen(ll->text));

    /* Row 4: Separator */
    sb_append(sb, "\033[38;5;238m", 11);
    for (int i = 0; i < cols; i++) sb_append(sb, "─", 3);
    sb_append(sb, "\033[0m\033[K\r\n", 7);

    /* Content Area */
    int content_rows = rows - 7;
    if (content_rows < 6) content_rows = 6;
    int printed_rows = 0;

    sb_printf(sb, " \033[38;5;248;1m[RAW LOG PAYLOAD]\033[0m\033[K\r\n");
    printed_rows++;

    /* Wrap raw text */
    const char *raw = ll->text;
    size_t raw_len = strlen(raw);
    size_t offset = 0;
    int wrap_width = cols - 6;
    if (wrap_width < 20) wrap_width = 20;

    while (offset < raw_len && printed_rows < content_rows - 4) {
        size_t chunk = raw_len - offset;
        if ((int)chunk > wrap_width) chunk = wrap_width;

        const char *color = ll->is_err ? "\033[38;5;203m" : (ll->is_warn ? "\033[38;5;222m" : "\033[38;5;253m");
        sb_printf(sb, "   %s%.*s\033[0m\033[K\r\n", color, (int)chunk, raw + offset);
        offset += chunk;
        printed_rows++;
    }

    /* Formatted JSON check */
    char *pretty_json = format_json_if_present(ll->text);
    if (pretty_json && printed_rows < content_rows - 2) {
        sb_append(sb, "\r\n", 2);
        printed_rows++;
        sb_printf(sb, " \033[38;5;51;1m[PARSED JSON STRUCTURE]\033[0m\033[K\r\n");
        printed_rows++;

        char *line = strtok(pretty_json, "\r\n");
        while (line && printed_rows < content_rows) {
            sb_printf(sb, "   \033[38;5;141m%.*s\033[0m\033[K\r\n", cols - 6, line);
            printed_rows++;
            line = strtok(NULL, "\r\n");
        }
        cJSON_free(pretty_json);
    }

    for (; printed_rows < content_rows; printed_rows++) {
        sb_append(sb, "\033[K\r\n", 5);
    }

    /* Footer separator */
    sb_append(sb, "\033[38;5;238m", 11);
    for (int i = 0; i < cols; i++) sb_append(sb, "─", 3);
    sb_append(sb, "\033[0m\033[K\r\n", 7);

    /* Footer actions */
    if (state->status_msg[0] && (time(NULL) - state->status_msg_time < 4)) {
        sb_printf(sb, " \033[38;5;226;1m⚡ %s\033[0m\033[K", state->status_msg);
    } else {
        sb_printf(sb, " \033[38;5;244m[m] Insert Marker Below  [c] Copy to Clipboard  [↑/k] Prev Line  [↓/j] Next Line  [Esc/Enter/q] Close\033[0m\033[K");
    }
}

static void render_dashboard(TUIState *state, ProcessInfo *apps, int app_count) {
    ScreenBuffer sb;
    sb_init(&sb);

    int cols = state->cols;
    int rows = state->rows;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    uint64_t total_mem = 0;
    int running_count = 0;
    uint32_t total_restarts = 0;
    bool any_watch = false;
    for (int i = 0; i < app_count; i++) {
        total_mem += apps[i].memory_bytes;
        total_restarts += apps[i].restart_count;
        if (apps[i].has_watch) any_watch = true;
        if (strcmp(apps[i].active_state, "active") == 0) {
            running_count++;
        }
    }
    char total_mem_str[32];
    format_mem(total_mem, total_mem_str, sizeof(total_mem_str));

    int total_items = app_count + 1;
    LogViewer *lv = &state->log_viewer;

    /* Collect filtered line indices */
    int matching_indices[MAX_LOG_LINES];
    int match_count = 0;

    for (int i = 0; i < lv->count; i++) {
        int real_idx = (lv->head + i) % MAX_LOG_LINES;
        LogLine *ll = &lv->lines[real_idx];

        if (ll->is_marker) {
            matching_indices[match_count++] = real_idx;
            continue;
        }

        if (lv->level_filter == LOG_LEVEL_ERR_ONLY && !ll->is_err) {
            continue;
        }
        if (lv->level_filter == LOG_LEVEL_WARN_ERR && !ll->is_err && !ll->is_warn) {
            continue;
        }

        if (lv->search_filter[0]) {
            if (strcasestr(ll->text, lv->search_filter) == NULL &&
                strcasestr(ll->app_name, lv->search_filter) == NULL) {
                continue;
            }
        }

        matching_indices[match_count++] = real_idx;
    }

    /* If Log Detail Inspector is open */
    if (state->show_detail && state->selected_log_idx >= 0 && state->selected_log_idx < match_count) {
        render_detail_inspector(&sb, state, &lv->lines[matching_indices[state->selected_log_idx]], state->selected_log_idx, match_count);
        write(STDOUT_FILENO, sb.data, sb.len);
        sb_free(&sb);
        return;
    }

    /* Move cursor to 1,1 */
    sb_append(&sb, "\033[H", 3);

    int log_rows_avail = 4;

    if (state->fullscreen_logs) {
        /* FULLSCREEN LOGS VIEW */
        sb_printf(&sb, "\033[38;5;208;1m🔥 fdash\033[0m \033[38;5;244mv%s\033[0m", FIRE_VERSION);
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  Scope: \033[38;5;51;1m%s\033[0m",
                  state->scope == SCOPE_SYSTEM ? "SYSTEM" : "USER");
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  Online: \033[38;5;48;1m%d/%d\033[0m", running_count, app_count);
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  RAM: \033[38;5;220;1m%s\033[0m", total_mem_str);
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  \033[48;5;236;38;5;226;1m 🖥 FULLSCREEN LOGS \033[0m");

        int cur_len = 58 + (int)strlen(total_mem_str);
        int pad = cols - cur_len - (int)strlen(time_str);
        if (pad > 0) {
            for (int i = 0; i < pad; i++) sb_append(&sb, " ", 1);
        }
        sb_printf(&sb, "\033[38;5;244m%s\033[0m\033[K\r\n", time_str);

        if (state->status_msg[0] && (time(NULL) - state->status_msg_time < 4)) {
            sb_printf(&sb, " \033[38;5;226;1m⚡ %s\033[0m\033[K\r\n", state->status_msg);
        } else if (state->selected_log_idx >= 0) {
            sb_printf(&sb, " \033[38;5;220;1m[Line #%d Selected]\033[0m \033[38;5;244m[Enter/o] Details  [m] Mark Below  [c] Copy  [Esc] Unselect  [↑/↓] Navigate\033[0m\033[K\r\n", state->selected_log_idx + 1);
        } else {
            sb_printf(&sb, "\033[38;5;244m [↑/↓/Click] Select Log  [f/Esc] Split View  [l] Level: %-8s  [Space] Pause  [m] Mark  [/] Search  [q] Quit\033[0m\033[K\r\n",
                      log_viewer_level_name(state->log_viewer.level_filter));
        }

        log_rows_avail = rows - 4;
        if (log_rows_avail < 4) log_rows_avail = 4;
    } else {
        /* 50/50 SPLIT DASHBOARD VIEW */
        sb_printf(&sb, "\033[38;5;208;1m🔥 fdash\033[0m \033[38;5;244mv%s\033[0m", FIRE_VERSION);
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  Scope: \033[38;5;51;1m%s\033[0m",
                  state->scope == SCOPE_SYSTEM ? "SYSTEM (/etc)" : "USER (~/.config)");
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  Online: \033[38;5;48;1m%d/%d\033[0m", running_count, app_count);
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  RAM: \033[38;5;220;1m%s\033[0m", total_mem_str);

        int header_text_len = 55 + (int)strlen(total_mem_str);
        int pad = cols - header_text_len - (int)strlen(time_str);
        if (pad > 0) {
            for (int i = 0; i < pad; i++) sb_append(&sb, " ", 1);
        }
        sb_printf(&sb, "\033[38;5;244m%s\033[0m\033[K\r\n", time_str);

        sb_append(&sb, "\033[38;5;238m", 11);
        for (int i = 0; i < cols; i++) sb_append(&sb, "─", 3);
        sb_append(&sb, "\033[0m\033[K\r\n", 7);

        int table_max_rows = (rows - 8) / 2;
        if (table_max_rows < 4) table_max_rows = 4;

        const char *apps_header_tag = (state->focus == FOCUS_PROCESSES)
                                      ? "\033[38;5;48;1m[ACTIVE FOCUS]\033[0m"
                                      : "\033[38;5;240m[Tab to focus]\033[0m";
        sb_printf(&sb, "\033[38;5;248;1m  %-4s %-18s %-12s %-8s %-10s %-8s %-12s\033[0m %s\033[K\r\n",
                  "ID", "NAME", "STATUS", "PID", "MEMORY", "RESTARTS", "WATCH", apps_header_tag);

        int visible_start = 0;
        if (state->selected_idx >= table_max_rows) {
            visible_start = state->selected_idx - table_max_rows + 1;
        }

        for (int row = 0; row < table_max_rows; row++) {
            int item_idx = visible_start + row;
            if (item_idx >= total_items) {
                sb_append(&sb, "\033[K\r\n", 5);
                continue;
            }

            if (item_idx == 0) {
                bool is_selected = (state->selected_idx == 0 && state->focus == FOCUS_PROCESSES);
                char all_status[64];
                if (running_count > 0) {
                    snprintf(all_status, sizeof(all_status), "\033[38;5;48;1m● active (%d)\033[0m", running_count);
                } else {
                    snprintf(all_status, sizeof(all_status), "\033[38;5;244m■ stopped\033[0m");
                }

                char count_str[16];
                snprintf(count_str, sizeof(count_str), "%u", total_restarts);

                char watch_str[32];
                if (any_watch) {
                    snprintf(watch_str, sizeof(watch_str), "\033[38;5;51m✓ active\033[0m");
                } else {
                    snprintf(watch_str, sizeof(watch_str), "\033[38;5;240m-\033[0m");
                }

                if (is_selected) {
                    sb_printf(&sb, "\033[48;5;236;38;5;214;1m► *    %-18s\033[0m %-12s \033[38;5;220m%-8s\033[0m %-10s %-8s %-12s\033[K\r\n",
                              "[ALL APPS]", all_status, "-", total_mem_str, count_str, watch_str);
                } else {
                    sb_printf(&sb, "  *    \033[38;5;51;1m%-18s\033[0m %-12s %-8s %-10s %-8s %-12s\033[K\r\n",
                              "[ALL APPS]", all_status, "-", total_mem_str, count_str, watch_str);
                }
            } else {
                int app_idx = item_idx - 1;
                ProcessInfo *p = &apps[app_idx];
                bool is_selected = (state->selected_idx == item_idx && state->focus == FOCUS_PROCESSES);

                char mem_str[32];
                format_mem(p->memory_bytes, mem_str, sizeof(mem_str));

                char status_badge[64];
                if (strcmp(p->active_state, "active") == 0) {
                    snprintf(status_badge, sizeof(status_badge), "\033[38;5;48;1m● active\033[0m");
                } else if (strcmp(p->active_state, "failed") == 0) {
                    snprintf(status_badge, sizeof(status_badge), "\033[38;5;196;1m▲ failed\033[0m");
                } else {
                    snprintf(status_badge, sizeof(status_badge), "\033[38;5;244m■ stopped\033[0m");
                }

                char pid_str[16];
                if (p->pid > 0) {
                    snprintf(pid_str, sizeof(pid_str), "%d", p->pid);
                } else {
                    snprintf(pid_str, sizeof(pid_str), "-");
                }

                char watch_str[32];
                if (p->has_watch) {
                    snprintf(watch_str, sizeof(watch_str), "\033[38;5;51m✓ active\033[0m");
                } else {
                    snprintf(watch_str, sizeof(watch_str), "\033[38;5;240m-\033[0m");
                }

                if (is_selected) {
                    sb_printf(&sb, "\033[48;5;236;38;5;214;1m► %-4d %-18s\033[0m %-12s \033[38;5;220m%-8s\033[0m %-10s %-8u %-12s\033[K\r\n",
                              app_idx, p->name, status_badge, pid_str, mem_str, p->restart_count, watch_str);
                } else {
                    sb_printf(&sb, "  %-4d \033[38;5;253;1m%-18s\033[0m %-12s %-8s %-10s %-8u %-12s\033[K\r\n",
                              app_idx, p->name, status_badge, pid_str, mem_str, p->restart_count, watch_str);
                }
            }
        }

        sb_append(&sb, "\033[38;5;238m", 11);
        for (int i = 0; i < cols; i++) sb_append(&sb, "─", 3);
        sb_append(&sb, "\033[0m\033[K\r\n", 7);

        if (state->status_msg[0] && (time(NULL) - state->status_msg_time < 4)) {
            sb_printf(&sb, " \033[38;5;226;1m⚡ %s\033[0m\033[K\r\n", state->status_msg);
        } else if (state->selected_log_idx >= 0) {
            sb_printf(&sb, " \033[38;5;220;1m[Line #%d Selected]\033[0m \033[38;5;244m[Enter/o] Details  [m] Mark Below  [c] Copy  [Esc] Unselect  [↑/↓] Navigate\033[0m\033[K\r\n", state->selected_log_idx + 1);
        } else {
            const char *focus_hint = (state->focus == FOCUS_LOGS)
                                     ? "\033[38;5;214;1m[Tab] Focus: LOGS (↑/↓/Wheel select)\033[0m"
                                     : "\033[38;5;48;1m[Tab] Focus: APPS (↑/↓ select)\033[0m";
            sb_printf(&sb, " %s  \033[38;5;244m[a] All-Apps  [f/Enter] Fullscreen  [l] Level  [/] Search  [Space] Pause  [m] Mark  [q] Quit\033[0m\033[K\r\n", focus_hint);
        }

        log_rows_avail = rows - (5 + table_max_rows + 4);
        if (log_rows_avail < 4) log_rows_avail = 4;
    }

    /* 4. Log Viewer Header */
    const char *selected_view_name = (state->selected_idx == 0 || state->log_viewer.is_all_apps)
                                     ? "ALL APPLICATIONS"
                                     : ((app_count > 0 && state->selected_idx <= app_count)
                                         ? apps[state->selected_idx - 1].name
                                         : "none");

    sb_append(&sb, "\033[38;5;238m╭─ ", 15);
    sb_printf(&sb, "\033[38;5;214;1mLogs: %s\033[0m \033[38;5;244m(%d lines)\033[0m ",
              selected_view_name, state->log_viewer.count);

    if (!state->fullscreen_logs) {
        if (state->focus == FOCUS_LOGS) {
            sb_printf(&sb, "\033[48;5;214;38;5;16;1m[FOCUS: LOGS]\033[0m ");
        }
    }

    if (state->log_viewer.level_filter == LOG_LEVEL_ERR_ONLY) {
        sb_printf(&sb, "\033[38;5;196;1m[Level: ERR ONLY]\033[0m ");
    } else if (state->log_viewer.level_filter == LOG_LEVEL_WARN_ERR) {
        sb_printf(&sb, "\033[38;5;220;1m[Level: WARN+ERR]\033[0m ");
    } else {
        sb_printf(&sb, "\033[38;5;244m[Level: ALL]\033[0m ");
    }

    if (state->search_mode) {
        sb_printf(&sb, "\033[38;5;226;1m[Search: %s_]\033[0m ", state->search_input);
    } else if (state->log_viewer.search_filter[0]) {
        sb_printf(&sb, "\033[38;5;51m[Filter: \"%s\"]\033[0m ", state->log_viewer.search_filter);
    }

    if (state->log_viewer.is_paused) {
        sb_printf(&sb, "\033[48;5;208;38;5;16;1m[PAUSED +%d]\033[0m ", state->log_viewer.paused_buffered_count);
    } else if (state->log_viewer.auto_scroll) {
        sb_printf(&sb, "\033[38;5;48m[auto-scroll: ON]\033[0m ");
    } else {
        sb_printf(&sb, "\033[38;5;208m[scrolled +%d]\033[0m ", state->log_viewer.scroll_offset);
    }

    for (int i = 0; i < cols - 70; i++) sb_append(&sb, "─", 3);
    sb_append(&sb, "╮\033[0m\033[K\r\n", 9);

    /* 5. Render Log Lines */
    if (lv->count == 0) {
        sb_printf(&sb, "\033[38;5;242m  (No logs recorded yet. Start the process or trigger actions to view journal)\033[0m\033[K\r\n");
        for (int i = 1; i < log_rows_avail; i++) {
            sb_append(&sb, "\033[K\r\n", 5);
        }
    } else if (match_count == 0) {
        sb_printf(&sb, "\033[38;5;244m  No lines match active filter [level=%s, search='%s']\033[0m\033[K\r\n",
                  log_viewer_level_name(lv->level_filter), lv->search_filter);
        for (int i = 1; i < log_rows_avail; i++) {
            sb_append(&sb, "\033[K\r\n", 5);
        }
    } else {
        int end_idx = match_count - lv->scroll_offset;
        if (end_idx < 0) end_idx = 0;
        int start_idx = end_idx - log_rows_avail;
        if (start_idx < 0) start_idx = 0;

        int lines_printed = 0;
        bool show_tag = lv->is_all_apps || (state->selected_idx == 0);

        for (int i = start_idx; i < end_idx && lines_printed < log_rows_avail; i++) {
            int real_idx = matching_indices[i];
            LogLine *ll = &lv->lines[real_idx];
            bool is_selected_line = (state->selected_log_idx == i);

            char formatted[2048];
            int avail_w = cols - (is_selected_line ? 6 : 4);
            log_viewer_format_colored_line(ll, lv->search_filter, show_tag, formatted, sizeof(formatted), avail_w);

            if (is_selected_line) {
                sb_printf(&sb, "\033[48;5;237;38;5;220;1m► \033[0m\033[48;5;236m%s\033[0m\033[K\r\n", formatted);
            } else {
                sb_printf(&sb, "  %s\033[K\r\n", formatted);
            }
            lines_printed++;
        }

        for (; lines_printed < log_rows_avail; lines_printed++) {
            sb_append(&sb, "\033[K\r\n", 5);
        }
    }

    /* Bottom border */
    sb_append(&sb, "\033[38;5;238m╰", 14);
    for (int i = 0; i < cols - 2; i++) sb_append(&sb, "─", 3);
    sb_append(&sb, "╯\033[0m\033[K", 9);

    /* Single write flush */
    write(STDOUT_FILENO, sb.data, sb.len);
    sb_free(&sb);
}

int tui_run(SystemdScope scope) {
    TUIState state;
    memset(&state, 0, sizeof(state));
    state.scope = scope;
    state.focus = FOCUS_PROCESSES;
    state.selected_idx = 0; /* Default to [ALL APPS] */
    state.last_app_idx = 1;
    state.fullscreen_logs = false;
    state.selected_log_idx = -1; /* None selected by default (following live stream) */
    state.show_detail = false;
    state.detail_scroll = 0;
    state.log_viewer.auto_scroll = true;
    log_viewer_init(&state.log_viewer);

    signal(SIGWINCH, handle_winch);
    signal(SIGINT, handle_sig_exit);
    signal(SIGTERM, handle_sig_exit);
    enable_raw_mode();
    update_window_size(&state);

    ProcessInfo *apps = NULL;
    int app_count = 0;

    systemd_init(scope);
    systemd_list_all(scope, &apps, &app_count);

    log_viewer_set_app(&state.log_viewer, "ALL", scope);
    char last_target[MAX_NAME_LEN] = "ALL";

    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    uint64_t frame_count = 0;

    while (!state.should_quit) {
        if (g_win_resized) {
            g_win_resized = 0;
            update_window_size(&state);
        }

        /* Check log pipe output */
        log_viewer_poll(&state.log_viewer);

        /* Refresh app list & process info every ~1 second (5 frames @ 200ms) */
        frame_count++;
        if (frame_count % 5 == 0) {
            systemd_free_list(apps, app_count);
            apps = NULL;
            app_count = 0;
            systemd_list_all(scope, &apps, &app_count);

            int total_items = app_count + 1;
            if (state.selected_idx >= total_items && total_items > 0) {
                state.selected_idx = total_items - 1;
            }
        }

        /* If selected app changed, switch log viewer stream cleanly */
        const char *current_target = (state.selected_idx == 0)
                                     ? "ALL"
                                     : ((app_count > 0 && state.selected_idx <= app_count)
                                         ? apps[state.selected_idx - 1].name
                                         : "ALL");

        if (strcmp(last_target, current_target) != 0) {
            snprintf(last_target, sizeof(last_target), "%s", current_target);
            state.selected_log_idx = -1; /* Reset selected line on stream switch */
            if (state.selected_idx == 0) {
                log_viewer_set_app(&state.log_viewer, "ALL", scope);
                set_status(&state, "Switched to unified [ALL APPS] log stream");
            } else {
                log_viewer_set_app(&state.log_viewer, apps[state.selected_idx - 1].name, scope);
                set_status(&state, "Switched to '%s' log stream", apps[state.selected_idx - 1].name);
            }
        }

        /* Render */
        render_dashboard(&state, apps, app_count);

        /* Wait for input with 200ms timeout */
        int poll_res = poll(&pfd, 1, 200);
        if (poll_res > 0 && (pfd.revents & POLLIN)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                int total_items = app_count + 1;
                LogViewer *lv = &state.log_viewer;

                /* Compute matching indices for current frame */
                int matching_indices[MAX_LOG_LINES];
                int match_count = 0;
                for (int i = 0; i < lv->count; i++) {
                    int real_idx = (lv->head + i) % MAX_LOG_LINES;
                    LogLine *ll = &lv->lines[real_idx];

                    if (ll->is_marker) {
                        matching_indices[match_count++] = real_idx;
                        continue;
                    }
                    if (lv->level_filter == LOG_LEVEL_ERR_ONLY && !ll->is_err) continue;
                    if (lv->level_filter == LOG_LEVEL_WARN_ERR && !ll->is_err && !ll->is_warn) continue;
                    if (lv->search_filter[0]) {
                        if (strcasestr(ll->text, lv->search_filter) == NULL &&
                            strcasestr(ll->app_name, lv->search_filter) == NULL) {
                            continue;
                        }
                    }
                    matching_indices[match_count++] = real_idx;
                }

                /* 1. If Log Detail Inspector is currently open */
                if (state.show_detail) {
                    if (c == 27 || c == 'q' || c == 'Q' || c == '\r' || c == '\n' || c == 'o') {
                        state.show_detail = false;
                        set_status(&state, "Closed Log Inspector");
                    } else if (c == 'c' || c == 'C') {
                        if (state.selected_log_idx >= 0 && state.selected_log_idx < match_count) {
                            int r_idx = matching_indices[state.selected_log_idx];
                            copy_to_clipboard(lv->lines[r_idx].text);
                            set_status(&state, "✔ Copied log payload to clipboard");
                        }
                    } else if (c == 'm' || c == 'M') {
                        if (state.selected_log_idx >= 0 && state.selected_log_idx < match_count) {
                            int r_idx = matching_indices[state.selected_log_idx];
                            log_viewer_insert_marker_after(lv, r_idx);
                            set_status(&state, "✔ Inserted checkpoint marker below line #%d", state.selected_log_idx + 1);
                        }
                    } else if (c == 'k') {
                        if (state.selected_log_idx > 0) state.selected_log_idx--;
                    } else if (c == 'j') {
                        if (state.selected_log_idx < match_count - 1) state.selected_log_idx++;
                    }
                    continue;
                }

                /* 2. If in search input mode */
                if (state.search_mode) {
                    if (c == 27) { /* Escape */
                        state.search_mode = false;
                        state.search_input[0] = '\0';
                        log_viewer_set_filter(&state.log_viewer, NULL);
                        set_status(&state, "Search cancelled");
                    } else if (c == '\r' || c == '\n') { /* Enter */
                        state.search_mode = false;
                        log_viewer_set_filter(&state.log_viewer, state.search_input);
                        set_status(&state, "Applied search: '%s'", state.search_input);
                    } else if (c == 127 || c == 8) { /* Backspace */
                        size_t slen = strlen(state.search_input);
                        if (slen > 0) {
                            state.search_input[slen - 1] = '\0';
                            log_viewer_set_filter(&state.log_viewer, state.search_input);
                        }
                    } else if (isprint((unsigned char)c)) {
                        size_t slen = strlen(state.search_input);
                        if (slen < sizeof(state.search_input) - 1) {
                            state.search_input[slen] = c;
                            state.search_input[slen + 1] = '\0';
                            log_viewer_set_filter(&state.log_viewer, state.search_input);
                        }
                    }
                    continue;
                }

                /* 3. Normal Dashboard Navigation */
                bool is_log_mode = (state.fullscreen_logs || state.focus == FOCUS_LOGS);

                if (c == 'q' || c == 'Q' || c == 3) {
                    state.should_quit = true;
                } else if (c == 'f') {
                    state.fullscreen_logs = !state.fullscreen_logs;
                    set_status(&state, state.fullscreen_logs ? "Entered Fullscreen Log View" : "Exited Fullscreen Log View");
                } else if (c == '\r' || c == '\n' || c == 'o') {
                    if (state.selected_log_idx >= 0 && state.selected_log_idx < match_count) {
                        /* Open detail inspector on selected line */
                        state.show_detail = true;
                        state.detail_scroll = 0;
                    } else {
                        /* Toggle fullscreen */
                        state.fullscreen_logs = !state.fullscreen_logs;
                        set_status(&state, state.fullscreen_logs ? "Entered Fullscreen Log View" : "Exited Fullscreen Log View");
                    }
                } else if (c == 'c' || c == 'C') {
                    if (state.selected_log_idx >= 0 && state.selected_log_idx < match_count) {
                        int r_idx = matching_indices[state.selected_log_idx];
                        copy_to_clipboard(lv->lines[r_idx].text);
                        set_status(&state, "✔ Copied selected log line to clipboard");
                    }
                } else if (c == ' ') {
                    log_viewer_toggle_pause(&state.log_viewer);
                    if (state.log_viewer.is_paused) {
                        set_status(&state, "Log stream PAUSED. Press [Space] to resume auto-scroll");
                    } else {
                        set_status(&state, "Log stream RESUMED (auto-scroll ON)");
                    }
                } else if (c == 'm' || c == 'M') {
                    if (state.selected_log_idx >= 0 && state.selected_log_idx < match_count) {
                        int r_idx = matching_indices[state.selected_log_idx];
                        log_viewer_insert_marker_after(&state.log_viewer, r_idx);
                        set_status(&state, "✔ Inserted checkpoint marker below line #%d", state.selected_log_idx + 1);
                    } else {
                        log_viewer_add_marker(&state.log_viewer);
                        set_status(&state, "Inserted visual checkpoint marker");
                    }
                } else if (c == 'l' || c == 'L') {
                    state.log_viewer.level_filter = (state.log_viewer.level_filter + 1) % 3;
                    state.selected_log_idx = -1;
                    set_status(&state, "Log Triage Filter: %s", log_viewer_level_name(state.log_viewer.level_filter));
                } else if (c == 'a' || c == 'A') {
                    if (state.selected_idx == 0) {
                        state.selected_idx = (state.last_app_idx > 0 && state.last_app_idx < total_items)
                                             ? state.last_app_idx
                                             : (app_count > 0 ? 1 : 0);
                    } else {
                        state.last_app_idx = state.selected_idx;
                        state.selected_idx = 0;
                    }
                } else if (c == 27) { /* Escape sequence or single Esc */
                    char seq[3];
                    if (read(STDIN_FILENO, &seq[0], 1) == 1 &&
                        read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if (seq[0] == '[') {
                            if (seq[1] == '<') {
                                /* SGR Mouse reporting: \033[<btn;col;rowM or m */
                                char mbuf[64];
                                int mlen = 0;
                                char mc;
                                while (mlen < (int)sizeof(mbuf) - 1) {
                                    if (read(STDIN_FILENO, &mc, 1) != 1) break;
                                    mbuf[mlen++] = mc;
                                    if (mc == 'M' || mc == 'm') break;
                                }
                                mbuf[mlen] = '\0';

                                int btn = 0, m_col = 0, m_row = 0;
                                char m_type = mc;
                                sscanf(mbuf, "%d;%d;%d%c", &btn, &m_col, &m_row, &m_type);

                                if (btn == 64) {
                                    /* Wheel Up */
                                    log_viewer_scroll_up(&state.log_viewer, 3);
                                } else if (btn == 65) {
                                    /* Wheel Down */
                                    log_viewer_scroll_down(&state.log_viewer, 3);
                                } else if (btn == 0 && m_type == 'M') {
                                    /* Left Click */
                                    int table_max_rows = (state.rows - 8) / 2;
                                    if (table_max_rows < 4) table_max_rows = 4;

                                    int log_start_row = state.fullscreen_logs ? 4 : (6 + table_max_rows);
                                    int log_avail = state.fullscreen_logs ? (state.rows - 4) : (state.rows - (5 + table_max_rows + 4));
                                    if (log_avail < 4) log_avail = 4;

                                    if (m_row >= log_start_row && m_row < log_start_row + log_avail) {
                                        /* Clicked inside log viewport */
                                        int end_idx = match_count - lv->scroll_offset;
                                        if (end_idx < 0) end_idx = 0;
                                        int start_idx = end_idx - log_avail;
                                        if (start_idx < 0) start_idx = 0;

                                        int clicked_match = start_idx + (m_row - log_start_row);
                                        if (clicked_match >= 0 && clicked_match < match_count && clicked_match < end_idx) {
                                            if (state.selected_log_idx == clicked_match) {
                                                /* Clicked already selected line: open detail inspector */
                                                state.show_detail = true;
                                                state.detail_scroll = 0;
                                            } else {
                                                state.selected_log_idx = clicked_match;
                                                state.focus = FOCUS_LOGS;
                                                state.log_viewer.auto_scroll = false;
                                                set_status(&state, "Selected line #%d. Press [Enter/o] for details, [m] to mark below.", clicked_match + 1);
                                            }
                                        }
                                    } else if (!state.fullscreen_logs && m_row >= 4 && m_row < 4 + table_max_rows) {
                                        /* Clicked inside process table */
                                        int visible_start = 0;
                                        if (state.selected_idx >= table_max_rows) {
                                            visible_start = state.selected_idx - table_max_rows + 1;
                                        }
                                        int clicked_item = visible_start + (m_row - 4);
                                        if (clicked_item < total_items) {
                                            state.selected_idx = clicked_item;
                                            state.focus = FOCUS_PROCESSES;
                                            state.selected_log_idx = -1;
                                        }
                                    }
                                }
                            } else if (seq[1] == 'A') { /* Up Arrow */
                                if (is_log_mode) {
                                    if (state.selected_log_idx == -1) {
                                        state.selected_log_idx = (match_count > 0) ? (match_count - 1) : -1;
                                    } else if (state.selected_log_idx > 0) {
                                        state.selected_log_idx--;
                                    }
                                    state.log_viewer.auto_scroll = false;

                                    int log_avail = state.fullscreen_logs ? (state.rows - 4) : 4;
                                    int end_idx = match_count - lv->scroll_offset;
                                    int start_idx = end_idx - log_avail;
                                    if (state.selected_log_idx < start_idx) {
                                        log_viewer_scroll_up(&state.log_viewer, start_idx - state.selected_log_idx);
                                    }
                                } else {
                                    if (state.selected_idx > 0) state.selected_idx--;
                                }
                            } else if (seq[1] == 'B') { /* Down Arrow */
                                if (is_log_mode) {
                                    if (state.selected_log_idx >= 0) {
                                        if (state.selected_log_idx < match_count - 1) {
                                            state.selected_log_idx++;
                                            int end_idx = match_count - lv->scroll_offset;
                                            if (state.selected_log_idx >= end_idx) {
                                                log_viewer_scroll_down(&state.log_viewer, state.selected_log_idx - end_idx + 1);
                                            }
                                        } else {
                                            state.selected_log_idx = -1;
                                            state.log_viewer.auto_scroll = true;
                                            state.log_viewer.scroll_offset = 0;
                                        }
                                    }
                                } else {
                                    if (state.selected_idx < total_items - 1) state.selected_idx++;
                                }
                            } else if (seq[1] == '5') { /* Page Up */
                                read(STDIN_FILENO, &seq[2], 1);
                                log_viewer_scroll_up(&state.log_viewer, 10);
                            } else if (seq[1] == '6') { /* Page Down */
                                read(STDIN_FILENO, &seq[2], 1);
                                log_viewer_scroll_down(&state.log_viewer, 10);
                            }
                        } else if (seq[0] == 'O') {
                            if (seq[1] == 'A') {
                                if (is_log_mode) {
                                    if (state.selected_log_idx == -1) state.selected_log_idx = (match_count > 0) ? (match_count - 1) : -1;
                                    else if (state.selected_log_idx > 0) state.selected_log_idx--;
                                } else if (state.selected_idx > 0) state.selected_idx--;
                            } else if (seq[1] == 'B') {
                                if (is_log_mode) {
                                    if (state.selected_log_idx >= 0 && state.selected_log_idx < match_count - 1) state.selected_log_idx++;
                                    else { state.selected_log_idx = -1; state.log_viewer.auto_scroll = true; }
                                } else if (state.selected_idx < total_items - 1) state.selected_idx++;
                            }
                        }
                    } else {
                        /* Plain ESC key */
                        if (state.selected_log_idx >= 0) {
                            state.selected_log_idx = -1;
                            state.log_viewer.auto_scroll = true;
                            set_status(&state, "Unselected line (live auto-scroll resumed)");
                        } else if (state.fullscreen_logs) {
                            state.fullscreen_logs = false;
                            set_status(&state, "Exited Fullscreen Log View");
                        } else if (state.log_viewer.search_filter[0]) {
                            log_viewer_set_filter(&state.log_viewer, NULL);
                            set_status(&state, "Cleared search filter");
                        } else if (state.log_viewer.level_filter != LOG_LEVEL_ALL) {
                            log_viewer_set_level_filter(&state.log_viewer, LOG_LEVEL_ALL);
                            set_status(&state, "Reset level filter to ALL");
                        }
                    }
                } else if (c == 'k') {
                    if (is_log_mode) {
                        if (state.selected_log_idx == -1) state.selected_log_idx = (match_count > 0) ? (match_count - 1) : -1;
                        else if (state.selected_log_idx > 0) state.selected_log_idx--;
                        state.log_viewer.auto_scroll = false;
                    } else {
                        if (state.selected_idx > 0) state.selected_idx--;
                    }
                } else if (c == 'j') {
                    if (is_log_mode) {
                        if (state.selected_log_idx >= 0 && state.selected_log_idx < match_count - 1) state.selected_log_idx++;
                        else { state.selected_log_idx = -1; state.log_viewer.auto_scroll = true; }
                    } else {
                        if (state.selected_idx < total_items - 1) state.selected_idx++;
                    }
                } else if (c == '\t') {
                    if (!state.fullscreen_logs) {
                        state.focus = (state.focus == FOCUS_PROCESSES) ? FOCUS_LOGS : FOCUS_PROCESSES;
                        if (state.focus == FOCUS_LOGS) {
                            set_status(&state, "Focus: LOGS (↑/↓/Click to select line, Tab to switch)");
                        } else {
                            state.selected_log_idx = -1;
                            set_status(&state, "Focus: PROCESSES (↑/↓ to select app, Tab to switch)");
                        }
                    }
                } else if (c == '/') {
                    state.search_mode = true;
                    state.search_input[0] = '\0';
                } else if (c == 'u') {
                    log_viewer_scroll_up(&state.log_viewer, 5);
                } else if (c == 'd' || c == 'n') {
                    log_viewer_scroll_down(&state.log_viewer, 5);
                } else if (c == 'b' || c == 'G') {
                    state.selected_log_idx = -1;
                    log_viewer_scroll_to_bottom(&state.log_viewer);
                } else if (c == 'g') {
                    log_viewer_scroll_up(&state.log_viewer, state.log_viewer.count);
                    state.selected_log_idx = 0;
                } else if (c == 's') {
                    if (state.selected_idx == 0) {
                        set_status(&state, "Starting all %d registered services...", app_count);
                        for (int i = 0; i < app_count; i++) {
                            systemd_start_unit(apps[i].name, scope);
                            systemd_get_unit_status(apps[i].name, scope, &apps[i]);
                        }
                        set_status(&state, "✔ Started all %d services", app_count);
                    } else if (state.selected_idx <= app_count) {
                        const char *name = apps[state.selected_idx - 1].name;
                        set_status(&state, "Starting '%s'...", name);
                        systemd_start_unit(name, scope);
                        systemd_get_unit_status(name, scope, &apps[state.selected_idx - 1]);
                        set_status(&state, "✔ Started '%s'", name);
                    }
                } else if (c == 'x') {
                    if (state.selected_idx == 0) {
                        set_status(&state, "Stopping all %d registered services...", app_count);
                        for (int i = 0; i < app_count; i++) {
                            systemd_stop_unit(apps[i].name, scope);
                            systemd_get_unit_status(apps[i].name, scope, &apps[i]);
                        }
                        set_status(&state, "■ Stopped all %d services", app_count);
                    } else if (state.selected_idx <= app_count) {
                        const char *name = apps[state.selected_idx - 1].name;
                        set_status(&state, "Stopping '%s'...", name);
                        systemd_stop_unit(name, scope);
                        systemd_get_unit_status(name, scope, &apps[state.selected_idx - 1]);
                        set_status(&state, "■ Stopped '%s'", name);
                    }
                } else if (c == 'r') {
                    if (state.selected_idx == 0) {
                        set_status(&state, "Restarting all %d registered services...", app_count);
                        for (int i = 0; i < app_count; i++) {
                            systemd_restart_unit(apps[i].name, scope);
                            systemd_get_unit_status(apps[i].name, scope, &apps[i]);
                        }
                        set_status(&state, "⟳ Restarted all %d services", app_count);
                    } else if (state.selected_idx <= app_count) {
                        const char *name = apps[state.selected_idx - 1].name;
                        set_status(&state, "Restarting '%s'...", name);
                        systemd_restart_unit(name, scope);
                        systemd_get_unit_status(name, scope, &apps[state.selected_idx - 1]);
                        set_status(&state, "⟳ Restarted '%s'", name);
                    }
                } else if (c == 'd') {
                    if (state.selected_idx > 0 && state.selected_idx <= app_count) {
                        const char *name = apps[state.selected_idx - 1].name;
                        systemd_stop_unit(name, scope);
                        systemd_disable_unit(name, scope);
                        unit_gen_delete(name, scope, NULL, 0);
                        systemd_daemon_reload(scope);
                        set_status(&state, "Deleted '%s'", name);
                        systemd_free_list(apps, app_count);
                        apps = NULL;
                        app_count = 0;
                        systemd_list_all(scope, &apps, &app_count);
                        total_items = app_count + 1;
                        if (state.selected_idx >= total_items && total_items > 0) {
                            state.selected_idx = total_items - 1;
                        }
                    }
                }
            }
        }
    }

    log_viewer_cleanup(&state.log_viewer);
    systemd_free_list(apps, app_count);
    systemd_cleanup();
    disable_raw_mode();
    return 0;
}
