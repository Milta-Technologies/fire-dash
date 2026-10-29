#include "tui.h"
#include "dbus_systemd.h"
#include "unit_gen.h"
#include "cJSON.h"
#include "updater.h"
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

static TUIState *g_tui_state = NULL;

static void disable_raw_mode(void) {
    if (!g_raw_enabled) return;
    /* Ensure mouse tracking is disabled, show cursor, re-enable auto-wrap, restore main screen */
    WRITE_LIT(STDOUT_FILENO, "\033[?1000l\033[?1002l\033[?1006l\033[?7h\033[?25h\033[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    g_raw_enabled = false;
}

static void handle_sig_exit(int sig) {
    (void)sig;
    if (g_tui_state && g_tui_state->log_viewer.stream_pid > 0) {
        pid_t pid = g_tui_state->log_viewer.stream_pid;
        kill(pid, SIGTERM);
        int status;
        for (int i = 0; i < 10; i++) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || r < 0) break;
            usleep(5000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, WNOHANG);
    }
    disable_raw_mode();
    _exit(0);
}

static void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cflag |= (tcflag_t)(CS8);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    /* Switch to alternate screen, hide cursor, disable auto-wrap, ensure mouse tracking is DISABLED */
    WRITE_LIT(STDOUT_FILENO, "\033[?1049h\033[?25l\033[?7l\033[?1000l\033[?1002l\033[?1006l");
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
    if (sb->data) {
        free(sb->data);
        sb->data = NULL;
    }
    sb->len = 0;
    sb->cap = 0;
}

static void sb_reset(ScreenBuffer *sb) {
    if (!sb->data) {
        sb_init(sb);
    } else {
        sb->len = 0;
        sb->data[0] = '\0';
    }
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

static void set_status(TUIState *state, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->status_msg, sizeof(state->status_msg), fmt, ap);
    va_end(ap);
    state->status_msg_time = time(NULL);
}

static void osc52_copy(const char *text) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(text);
    if (len > 100000) return; /* safety cap for escape sequence */
    WRITE_LIT(STDOUT_FILENO, "\033]52;c;");
    for (size_t i = 0; i < len; i += 3) {
        uint32_t oct_a = (unsigned char)text[i];
        uint32_t oct_b = (i + 1 < len) ? (unsigned char)text[i + 1] : 0;
        uint32_t oct_c = (i + 2 < len) ? (unsigned char)text[i + 2] : 0;
        uint32_t triple = (oct_a << 16) | (oct_b << 8) | oct_c;
        char out[4];
        out[0] = b64[(triple >> 18) & 0x3F];
        out[1] = b64[(triple >> 12) & 0x3F];
        out[2] = (i + 1 < len) ? b64[(triple >> 6) & 0x3F] : '=';
        out[3] = (i + 2 < len) ? b64[triple & 0x3F] : '=';
        write(STDOUT_FILENO, out, 4);
    }
    WRITE_LIT(STDOUT_FILENO, "\007");
}

static bool copy_to_clipboard(const char *text) {
    if (!text || !*text) return false;
    bool ok = false;
    FILE *fp = popen("pbcopy 2>/dev/null || wl-copy 2>/dev/null || xclip -selection clipboard 2>/dev/null || xsel --clipboard --input 2>/dev/null", "w");
    if (fp) {
        fputs(text, fp);
        if (pclose(fp) == 0) ok = true;
    }
    osc52_copy(text);
    return ok || true;
}

static bool copy_marked_logs(TUIState *state) {
    LogViewer *lv = &state->log_viewer;
    if (state->match_count == 0) {
        set_status(state, "No logs available to copy");
        return false;
    }

    int marker_k[MAX_LOG_LINES];
    int marker_count = 0;
    for (int k = 0; k < state->match_count; k++) {
        int r_idx = state->matching_indices[k];
        if (lv->lines[r_idx].is_marker) {
            marker_k[marker_count++] = k;
        }
    }

    if (marker_count == 0) {
        if (state->selected_log_idx >= 0 && state->selected_log_idx < state->match_count) {
            int r_idx = state->matching_indices[state->selected_log_idx];
            copy_to_clipboard(lv->lines[r_idx].text);
            set_status(state, "✔ Copied selected log line to clipboard");
            return true;
        } else {
            set_status(state, "No markers set. Press [m] to add a marker, or select a line.");
            return false;
        }
    }

    int start_k = 0;
    int end_k = 0;
    if (marker_count == 1) {
        start_k = marker_k[0] + 1;
        end_k = state->match_count - 1;
    } else {
        start_k = marker_k[marker_count - 2] + 1;
        end_k = marker_k[marker_count - 1] - 1;
    }

    if (start_k > end_k) {
        set_status(state, "No log lines found %s to copy",
                   marker_count == 1 ? "below marker" : "between markers");
        return false;
    }

    size_t total_len = 0;
    int line_count = 0;
    for (int k = start_k; k <= end_k; k++) {
        int r_idx = state->matching_indices[k];
        if (lv->lines[r_idx].is_marker) continue;
        total_len += strlen(lv->lines[r_idx].text) + 1;
        line_count++;
    }

    if (line_count == 0) {
        set_status(state, "No log lines found %s to copy",
                   marker_count == 1 ? "below marker" : "between markers");
        return false;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        set_status(state, "Memory allocation failed");
        return false;
    }

    size_t cur = 0;
    for (int k = start_k; k <= end_k; k++) {
        int r_idx = state->matching_indices[k];
        if (lv->lines[r_idx].is_marker) continue;
        const char *txt = lv->lines[r_idx].text;
        size_t tlen = strlen(txt);
        memcpy(buf + cur, txt, tlen);
        cur += tlen;
        buf[cur++] = '\n';
    }
    buf[cur] = '\0';

    copy_to_clipboard(buf);
    free(buf);

    if (marker_count == 1) {
        set_status(state, "✔ Copied %d log lines (below marker) to clipboard", line_count);
    } else {
        set_status(state, "✔ Copied %d log lines (between markers) to clipboard", line_count);
    }
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

    sb_append(sb, "\033[?2026h\033[H\033[?25l", 17);

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

        char *saveptr = NULL;
        char *line = strtok_r(pretty_json, "\r\n", &saveptr);
        while (line && printed_rows < content_rows) {
            sb_printf(sb, "   \033[38;5;141m%.*s\033[0m\033[K\r\n", cols - 6, line);
            printed_rows++;
            line = strtok_r(NULL, "\r\n", &saveptr);
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
        if (cols >= 100) {
            sb_printf(sb, " \033[38;5;244m[m] Insert Marker Below  [c] Copy to Clipboard  [↑/k] Prev Line  [↓/j] Next Line  [Esc/Enter/q] Close\033[0m\033[K");
        } else {
            sb_printf(sb, " \033[38;5;244m[m] Mark Below  [c] Copy  [↑/k] Prev  [↓/j] Next  [Esc/q] Close\033[0m\033[K");
        }
    }
    sb_append(sb, "\033[?2026l", 8);
}

static void update_matching_indices(TUIState *state) {
    LogViewer *lv = &state->log_viewer;
    if (!lv->is_dirty) return;

    state->match_count = 0;
    for (int i = 0; i < lv->count; i++) {
        int real_idx = (lv->head + i) % MAX_LOG_LINES;
        LogLine *ll = &lv->lines[real_idx];

        if (ll->is_marker) {
            state->matching_indices[state->match_count++] = real_idx;
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

        state->matching_indices[state->match_count++] = real_idx;
    }
    lv->is_dirty = false;
}

static ScreenBuffer g_screen_buf = {0};

static void render_dashboard(TUIState *state, ProcessInfo *apps, int app_count) {
#define sb g_screen_buf
    sb_reset(&sb);

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

    /* Collect filtered line indices using cache */
    update_matching_indices(state);
    int *matching_indices = state->matching_indices;
    int match_count = state->match_count;

    /* If Log Detail Inspector is open */
    if (state->show_detail && state->selected_log_idx >= 0 && state->selected_log_idx < match_count) {
        render_detail_inspector(&sb, state, &lv->lines[matching_indices[state->selected_log_idx]], state->selected_log_idx, match_count);
        write(STDOUT_FILENO, sb.data, sb.len);
        return;
    }

    /* Synchronized update begin, move cursor to 1,1 and hide cursor */
    sb_append(&sb, "\033[?2026h\033[H\033[?25l", 17);

    int log_rows_avail = 4;

    if (state->fullscreen_logs) {
        /* FULLSCREEN LOGS VIEW */
        sb_printf(&sb, "\033[38;5;208;1m🔥 fdash\033[0m \033[38;5;244mv%s\033[0m", FIRE_VERSION);
        int badge_len = 0;
        if (state->has_update) {
            sb_printf(&sb, " \033[48;5;220;38;5;16;1m ✨ v%s AVAILABLE [U] \033[0m", state->update_version);
            badge_len = 20 + (int)strlen(state->update_version);
        }
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  Scope: \033[38;5;51;1m%s\033[0m",
                  state->scope == SCOPE_SYSTEM ? "SYSTEM" : "USER");
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  Online: \033[38;5;48;1m%d/%d\033[0m", running_count, app_count);
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  RAM: \033[38;5;220;1m%s\033[0m", total_mem_str);
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  \033[48;5;236;38;5;226;1m 🖥 FULLSCREEN LOGS \033[0m");

        int cur_len = 58 + (int)strlen(total_mem_str) + badge_len;
        int pad = cols - cur_len - (int)strlen(time_str);
        if (pad > 0) {
            for (int i = 0; i < pad; i++) sb_append(&sb, " ", 1);
        }
        sb_printf(&sb, "\033[38;5;244m%s\033[0m\033[K\r\n", time_str);

        int marker_count = 0;
        for (int k = 0; k < match_count; k++) {
            if (lv->lines[matching_indices[k]].is_marker) marker_count++;
        }

        if (state->confirm_delete) {
            sb_printf(&sb, " \033[48;5;196;38;5;231;1m ⚠ CONFIRM DELETE \033[0m \033[38;5;196;1mDelete service '%s'?\033[0m \033[38;5;220;1m[y] Confirm  [n/Esc] Cancel\033[0m\033[K\r\n", state->confirm_delete_name);
        } else if (state->confirm_update) {
            sb_printf(&sb, " \033[48;5;220;38;5;16;1m 🚀 UPGRADE FDASH \033[0m \033[38;5;220;1mUpgrade to v%s now?\033[0m \033[38;5;48;1m[y] Confirm\033[0m  \033[38;5;244m[n/Esc] Cancel\033[0m\033[K\r\n", state->update_version);
        } else if (state->status_msg[0] && (time(NULL) - state->status_msg_time < 4)) {
            sb_printf(&sb, " \033[38;5;226;1m⚡ %s\033[0m\033[K\r\n", state->status_msg);
        } else if (state->selected_log_idx >= 0) {
            sb_printf(&sb, " \033[38;5;220;1m[Line #%d Selected]\033[0m \033[38;5;244m[Enter/o] Details  [m] Mark Below  [c] %s  [Esc] Unselect  [↑/↓] Navigate\033[0m\033[K\r\n",
                      state->selected_log_idx + 1, marker_count > 0 ? "Copy Marked" : "Copy Line");
        } else {
            const char *upg_hint = state->has_update ? "  \033[38;5;220;1m[U] Upgrade\033[0m" : "";
            if (marker_count > 0) {
                if (cols >= 100) {
                    sb_printf(&sb, "\033[38;5;244m [↑/↓/j/k] Select Log  \033[38;5;51;1m[c] Copy Marked (%d)\033[0m\033[38;5;244m  [M] Clear  [f/Esc] Split  [l] Level: %-8s  [Space] Pause  [m] Mark%s  [q] Quit\033[0m\033[K\r\n",
                              marker_count, log_viewer_level_name(state->log_viewer.level_filter), upg_hint);
                } else {
                    sb_printf(&sb, "\033[38;5;244m \033[38;5;51;1m[c] Copy (%d)\033[0m\033[38;5;244m  [M] Clear  [f/Esc] Split  [m] Mark%s  [q] Quit\033[0m\033[K\r\n",
                              marker_count, upg_hint);
                }
            } else {
                if (cols >= 95) {
                    sb_printf(&sb, "\033[38;5;244m [↑/↓/j/k] Select Log  [f/Esc] Split View  [l] Level: %-8s  [Space] Pause  [m] Mark  [/] Search%s  [q] Quit\033[0m\033[K\r\n",
                              log_viewer_level_name(state->log_viewer.level_filter), upg_hint);
                } else {
                    sb_printf(&sb, "\033[38;5;244m [f/Esc] Split  [l] Level: %-8s  [m] Mark  [/] Search%s  [q] Quit\033[0m\033[K\r\n",
                              log_viewer_level_name(state->log_viewer.level_filter), upg_hint);
                }
            }
        }

        log_rows_avail = rows - 5;
        if (log_rows_avail < 4) log_rows_avail = 4;
    } else {
        /* 50/50 SPLIT DASHBOARD VIEW */
        sb_printf(&sb, "\033[38;5;208;1m🔥 fdash\033[0m \033[38;5;244mv%s\033[0m", FIRE_VERSION);
        int badge_len = 0;
        if (state->has_update) {
            sb_printf(&sb, " \033[48;5;220;38;5;16;1m ✨ v%s AVAILABLE [U] \033[0m", state->update_version);
            badge_len = 20 + (int)strlen(state->update_version);
        }
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  Scope: \033[38;5;51;1m%s\033[0m",
                  state->scope == SCOPE_SYSTEM ? "SYSTEM (/etc)" : "USER (~/.config)");
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  Online: \033[38;5;48;1m%d/%d\033[0m", running_count, app_count);
        sb_printf(&sb, "  \033[38;5;240m│\033[0m  RAM: \033[38;5;220;1m%s\033[0m", total_mem_str);

        int header_text_len = 55 + (int)strlen(total_mem_str) + badge_len;
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

        const char *apps_header_tag = "";
        if (cols >= 95) {
            apps_header_tag = (state->focus == FOCUS_PROCESSES)
                              ? "\033[38;5;48;1m[ACTIVE FOCUS]\033[0m"
                              : "\033[38;5;240m[Tab to focus]\033[0m";
        } else if (cols >= 86) {
            apps_header_tag = (state->focus == FOCUS_PROCESSES)
                              ? "\033[38;5;48;1m[ACTIVE]\033[0m"
                              : "\033[38;5;240m[Tab]\033[0m";
        }
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

        int marker_count = 0;
        for (int k = 0; k < match_count; k++) {
            if (lv->lines[matching_indices[k]].is_marker) marker_count++;
        }

        if (state->confirm_delete) {
            sb_printf(&sb, " \033[48;5;196;38;5;231;1m ⚠ CONFIRM DELETE \033[0m \033[38;5;196;1mDelete service '%s'?\033[0m \033[38;5;220;1m[y] Confirm  [n/Esc] Cancel\033[0m\033[K\r\n", state->confirm_delete_name);
        } else if (state->confirm_update) {
            sb_printf(&sb, " \033[48;5;220;38;5;16;1m 🚀 UPGRADE FDASH \033[0m \033[38;5;220;1mUpgrade to v%s now?\033[0m \033[38;5;48;1m[y] Confirm\033[0m  \033[38;5;244m[n/Esc] Cancel\033[0m\033[K\r\n", state->update_version);
        } else if (state->status_msg[0] && (time(NULL) - state->status_msg_time < 4)) {
            sb_printf(&sb, " \033[38;5;226;1m⚡ %s\033[0m\033[K\r\n", state->status_msg);
        } else if (state->selected_log_idx >= 0) {
            sb_printf(&sb, " \033[38;5;220;1m[Line #%d Selected]\033[0m \033[38;5;244m[Enter/o] Details  [m] Mark Below  [c] %s  [Esc] Unselect  [↑/↓] Navigate\033[0m\033[K\r\n",
                      state->selected_log_idx + 1, marker_count > 0 ? "Copy Marked" : "Copy Line");
        } else {
            const char *focus_hint = (state->focus == FOCUS_LOGS)
                                     ? ((cols >= 105) ? "\033[38;5;214;1m[Tab] Focus: LOGS (↑/↓/j/k navigate)\033[0m" : "\033[38;5;214;1m[Tab] LOGS\033[0m")
                                     : ((cols >= 105) ? "\033[38;5;48;1m[Tab] Focus: APPS (↑/↓/j/k select)\033[0m" : "\033[38;5;48;1m[Tab] APPS\033[0m");
            const char *upg_hint = state->has_update ? "  \033[38;5;220;1m[U] Upgrade\033[0m" : "";
            if (marker_count > 0) {
                if (cols >= 100) {
                    sb_printf(&sb, " %s  \033[38;5;51;1m[c] Copy Marked (%d)\033[0m\033[38;5;244m  [M] Clear  [m] Mark  [Space] Pause  [a] All  [f] Full%s  [q] Quit\033[0m\033[K\r\n", focus_hint, marker_count, upg_hint);
                } else {
                    sb_printf(&sb, " %s  \033[38;5;51;1m[c] Copy (%d)\033[0m\033[38;5;244m  [M] Clear  [f] Full%s  [q] Quit\033[0m\033[K\r\n", focus_hint, marker_count, upg_hint);
                }
            } else {
                if (cols >= 110) {
                    sb_printf(&sb, " %s  \033[38;5;244m[a] All-Apps  [f/Enter] Fullscreen  [l] Level  [/] Search  [Space] Pause  [m] Mark%s  [q] Quit\033[0m\033[K\r\n", focus_hint, upg_hint);
                } else if (cols >= 85) {
                    sb_printf(&sb, " %s  \033[38;5;244m[a] All  [f] Full  [l] Level  [/] Search  [m] Mark%s  [q] Quit\033[0m\033[K\r\n", focus_hint, upg_hint);
                } else {
                    sb_printf(&sb, " %s  \033[38;5;244m[f] Full  [m] Mark  [/] Find%s  [q] Quit\033[0m\033[K\r\n", focus_hint, upg_hint);
                }
            }
        }

        log_rows_avail = rows - (table_max_rows + 8);
        if (log_rows_avail < 4) log_rows_avail = 4;
    }

    /* 4. Log Viewer Header */
    const char *selected_view_name = (state->selected_idx == 0 || state->log_viewer.is_all_apps)
                                     ? "ALL APPLICATIONS"
                                     : ((app_count > 0 && state->selected_idx <= app_count)
                                         ? apps[state->selected_idx - 1].name
                                         : "none");

    sb_append(&sb, "\033[38;5;238m╭─ ", 18);
    int vis_w = 3; /* "╭─ " */

    char title_buf[128];
    int title_len = snprintf(title_buf, sizeof(title_buf), "Logs: %s (%d lines) ",
                             selected_view_name, state->log_viewer.count);
    sb_printf(&sb, "\033[38;5;214;1mLogs: %s\033[0m \033[38;5;244m(%d lines)\033[0m ",
              selected_view_name, state->log_viewer.count);
    vis_w += title_len;

    if (!state->fullscreen_logs && state->focus == FOCUS_LOGS) {
        sb_printf(&sb, "\033[48;5;214;38;5;16;1m[FOCUS: LOGS]\033[0m ");
        vis_w += 14;
    }

    if (state->log_viewer.level_filter == LOG_LEVEL_ERR_ONLY) {
        sb_printf(&sb, "\033[38;5;196;1m[Level: ERR ONLY]\033[0m ");
        vis_w += 18;
    } else if (state->log_viewer.level_filter == LOG_LEVEL_WARN_ERR) {
        sb_printf(&sb, "\033[38;5;220;1m[Level: WARN+ERR]\033[0m ");
        vis_w += 18;
    } else if (cols >= 90) {
        sb_printf(&sb, "\033[38;5;244m[Level: ALL]\033[0m ");
        vis_w += 13;
    }

    if (state->search_mode) {
        char sbuf[64];
        int sl = snprintf(sbuf, sizeof(sbuf), "[Search: %s_] ", state->search_input);
        sb_printf(&sb, "\033[38;5;226;1m%s\033[0m", sbuf);
        vis_w += sl;
    } else if (state->log_viewer.search_filter[0]) {
        char fbuf[64];
        int fl = snprintf(fbuf, sizeof(fbuf), "[Filter: \"%s\"] ", state->log_viewer.search_filter);
        sb_printf(&sb, "\033[38;5;51m%s\033[0m", fbuf);
        vis_w += fl;
    }

    if (state->log_viewer.is_paused) {
        char pbuf[32];
        int pl = snprintf(pbuf, sizeof(pbuf), "[PAUSED +%d] ", state->log_viewer.paused_buffered_count);
        sb_printf(&sb, "\033[48;5;208;38;5;16;1m%s\033[0m", pbuf);
        vis_w += pl;
    } else if (state->log_viewer.auto_scroll) {
        if (cols >= 80) {
            sb_printf(&sb, "\033[38;5;48m[auto-scroll: ON]\033[0m ");
            vis_w += 18;
        }
    } else {
        char sbuf[32];
        int sl = snprintf(sbuf, sizeof(sbuf), "[scrolled +%d] ", state->log_viewer.scroll_offset);
        sb_printf(&sb, "\033[38;5;208m%s\033[0m", sbuf);
        vis_w += sl;
    }

    sb_append(&sb, "\033[38;5;238m", 11);
    int fill = (cols - 1) - vis_w;
    if (fill > 0) {
        for (int i = 0; i < fill; i++) sb_append(&sb, "─", 3);
    }
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

    /* End synchronized update */
    sb_append(&sb, "\033[?2026l", 8);

    /* Single write flush */
    write(STDOUT_FILENO, sb.data, sb.len);
}
#undef sb

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

    g_tui_state = &state;
    signal(SIGWINCH, handle_winch);
    signal(SIGINT, handle_sig_exit);
    signal(SIGTERM, handle_sig_exit);
    signal(SIGHUP, handle_sig_exit);
    enable_raw_mode();
    update_window_size(&state);

    /* Check for background update check (cached for 6 hrs) */
    updater_check_version(state.update_version, sizeof(state.update_version), &state.has_update, false);

    ProcessInfo *apps = NULL;
    int app_count = 0;

    systemd_init(scope);
    systemd_list_all(scope, &apps, &app_count);

    log_viewer_set_app(&state.log_viewer, "ALL", scope);
    char last_target[MAX_NAME_LEN] = "ALL";

    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    bool needs_render = true;
    time_t last_systemd_refresh = 0;
    time_t last_render_sec = 0;

    while (!state.should_quit) {
        if (g_win_resized) {
            g_win_resized = 0;
            update_window_size(&state);
            needs_render = true;
        }

        /* Check log pipe output */
        bool had_dirty = state.log_viewer.is_dirty;
        log_viewer_poll(&state.log_viewer);
        if (state.log_viewer.is_dirty && !had_dirty) {
            needs_render = true;
        }

        time_t now = time(NULL);

        /* Refresh app list & process info every ~1 second */
        if (now - last_systemd_refresh >= 1) {
            last_systemd_refresh = now;
            systemd_free_list(apps, app_count);
            apps = NULL;
            app_count = 0;
            systemd_list_all(scope, &apps, &app_count);

            int total_items = app_count + 1;
            if (state.selected_idx >= total_items && total_items > 0) {
                state.selected_idx = total_items - 1;
            }
            needs_render = true;
        }

        /* Expire status message after 4 seconds */
        if (state.status_msg[0] && (now - state.status_msg_time >= 4)) {
            state.status_msg[0] = '\0';
            needs_render = true;
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
            needs_render = true;
        }

        /* Render if state is dirty or at least once per second for the clock update */
        if (needs_render || (now != last_render_sec)) {
            render_dashboard(&state, apps, app_count);
            last_render_sec = now;
            needs_render = false;
        }

        /* Wait for input with 100ms timeout for snappy responsiveness */
        int poll_res = poll(&pfd, 1, 100);
        if (poll_res > 0 && (pfd.revents & POLLIN)) {
            needs_render = true;
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                int total_items = app_count + 1;
                LogViewer *lv = &state.log_viewer;

                /* Compute matching indices for current frame using cache */
                update_matching_indices(&state);
                int *matching_indices = state.matching_indices;
                int match_count = state.match_count;

                /* 0a. If Update Confirmation is active */
                if (state.confirm_update) {
                    if (c == 'y' || c == 'Y') {
                        state.confirm_update = false;
                        disable_raw_mode();
                        printf("\033[2J\033[H\033[?25h");
                        printf("🔥 Upgrading fdash to v%s...\n", state.update_version);
                        fflush(stdout);

                        int res = updater_self_update(state.update_version);
                        if (res == 0) {
                            printf("\n✔ Update to v%s completed successfully! Restarting fdash...\n", state.update_version);
                            sleep(1);
                            char self_path[PATH_MAX];
                            if (updater_get_self_path(self_path, sizeof(self_path)) == 0) {
                                execl(self_path, self_path, (char *)NULL);
                            }
                            exit(0);
                        } else {
                            printf("\n❌ Update failed. Press Enter to return to dashboard...");
                            char tmp[16];
                            if (fgets(tmp, sizeof(tmp), stdin)) {}
                            enable_raw_mode();
                            set_status(&state, "Update to v%s failed", state.update_version);
                        }
                    } else if (c == 'n' || c == 'N' || c == 27 || c == 'q' || c == 'Q') {
                        state.confirm_update = false;
                        set_status(&state, "Cancelled upgrade");
                    }
                    continue;
                }

                /* 0b. If Delete Confirmation is active */
                if (state.confirm_delete) {
                    if (c == 'y' || c == 'Y') {
                        state.confirm_delete = false;
                        char name[MAX_NAME_LEN];
                        snprintf(name, sizeof(name), "%s", state.confirm_delete_name);
                        systemd_stop_unit(name, scope);
                        systemd_disable_unit(name, scope);
                        unit_gen_delete(name, scope, NULL, 0);
                        systemd_daemon_reload(scope);
                        set_status(&state, "✔ Permanently deleted service '%s'", name);
                        systemd_free_list(apps, app_count);
                        apps = NULL;
                        app_count = 0;
                        systemd_list_all(scope, &apps, &app_count);
                        total_items = app_count + 1;
                        if (state.selected_idx >= total_items && total_items > 0) {
                            state.selected_idx = total_items - 1;
                        }
                    } else if (c == 'n' || c == 'N' || c == 27 || c == 'q' || c == 'Q') {
                        state.confirm_delete = false;
                        state.confirm_delete_name[0] = '\0';
                        set_status(&state, "Cancelled deletion");
                    }
                    continue;
                }

                /* 1. If Log Detail Inspector is currently open */
                if (state.show_detail) {
                    if (c == 27) {
                        char seq[3];
                        if (read(STDIN_FILENO, &seq[0], 1) == 1 &&
                            read(STDIN_FILENO, &seq[1], 1) == 1) {
                            if (seq[0] == '[' || seq[0] == 'O') {
                                if (seq[1] == 'A') { /* Up Arrow */
                                    if (state.selected_log_idx > 0) state.selected_log_idx--;
                                    continue;
                                } else if (seq[1] == 'B') { /* Down Arrow */
                                    if (state.selected_log_idx < match_count - 1) state.selected_log_idx++;
                                    continue;
                                }
                            }
                        }
                        state.show_detail = false;
                        set_status(&state, "Closed Log Inspector");
                    } else if (c == 'q' || c == 'Q' || c == '\r' || c == '\n' || c == 'o') {
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
                    copy_marked_logs(&state);
                } else if (c == ' ') {
                    log_viewer_toggle_pause(&state.log_viewer);
                    if (state.log_viewer.is_paused) {
                        set_status(&state, "Log stream PAUSED. Press [Space] to resume auto-scroll");
                    } else {
                        set_status(&state, "Log stream RESUMED (auto-scroll ON)");
                    }
                } else if (c == 'm') {
                    if (state.selected_log_idx >= 0 && state.selected_log_idx < match_count) {
                        int r_idx = matching_indices[state.selected_log_idx];
                        log_viewer_insert_marker_after(&state.log_viewer, r_idx);
                        set_status(&state, "✔ Inserted checkpoint marker below line #%d", state.selected_log_idx + 1);
                    } else {
                        log_viewer_add_marker(&state.log_viewer);
                        set_status(&state, "✔ Inserted checkpoint marker (press [c] to copy logs below)");
                    }
                } else if (c == 'M') {
                    int cleared = log_viewer_clear_markers(&state.log_viewer);
                    if (cleared > 0) {
                        set_status(&state, "✔ Cleared %d checkpoint marker%s", cleared, cleared > 1 ? "s" : "");
                    } else {
                        set_status(&state, "No checkpoint markers to clear");
                    }
                } else if (c == 'l' || c == 'L') {
                    log_viewer_set_level_filter(&state.log_viewer, (state.log_viewer.level_filter + 1) % 3);
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
                            if (seq[1] == 'A') { /* Up Arrow */
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
                            } else if (seq[1] == 'C') { /* Right Arrow -> Switch focus to Logs */
                                if (!state.fullscreen_logs) {
                                    state.focus = FOCUS_LOGS;
                                    set_status(&state, "Focus: LOGS (↑/↓/j/k to select line, Tab to switch)");
                                }
                            } else if (seq[1] == 'D') { /* Left Arrow -> Switch focus to Processes */
                                if (!state.fullscreen_logs) {
                                    state.focus = FOCUS_PROCESSES;
                                    state.selected_log_idx = -1;
                                    set_status(&state, "Focus: PROCESSES (↑/↓/j/k to select app, Tab to switch)");
                                }
                            } else if (seq[1] == '5') { /* Page Up */
                                read(STDIN_FILENO, &seq[2], 1); /* consume ~ */
                                log_viewer_scroll_up(&state.log_viewer, 10);
                            } else if (seq[1] == '6') { /* Page Down */
                                read(STDIN_FILENO, &seq[2], 1); /* consume ~ */
                                log_viewer_scroll_down(&state.log_viewer, 10);
                            } else if (seq[1] == 'H' || seq[1] == '1' || seq[1] == '7') { /* Home */
                                if (seq[1] != 'H') read(STDIN_FILENO, &seq[2], 1); /* consume ~ */
                                if (is_log_mode) {
                                    log_viewer_scroll_up(&state.log_viewer, state.log_viewer.count);
                                    state.selected_log_idx = 0;
                                } else {
                                    state.selected_idx = 0;
                                }
                            } else if (seq[1] == 'F' || seq[1] == '4' || seq[1] == '8') { /* End */
                                if (seq[1] != 'F') read(STDIN_FILENO, &seq[2], 1); /* consume ~ */
                                if (is_log_mode) {
                                    state.selected_log_idx = -1;
                                    log_viewer_scroll_to_bottom(&state.log_viewer);
                                } else {
                                    state.selected_idx = total_items - 1;
                                }
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
                            } else if (seq[1] == 'C') {
                                if (!state.fullscreen_logs) {
                                    state.focus = FOCUS_LOGS;
                                    set_status(&state, "Focus: LOGS");
                                }
                            } else if (seq[1] == 'D') {
                                if (!state.fullscreen_logs) {
                                    state.focus = FOCUS_PROCESSES;
                                    state.selected_log_idx = -1;
                                    set_status(&state, "Focus: PROCESSES");
                                }
                            } else if (seq[1] == 'H') {
                                if (is_log_mode) {
                                    log_viewer_scroll_up(&state.log_viewer, state.log_viewer.count);
                                    state.selected_log_idx = 0;
                                } else {
                                    state.selected_idx = 0;
                                }
                            } else if (seq[1] == 'F') {
                                if (is_log_mode) {
                                    state.selected_log_idx = -1;
                                    log_viewer_scroll_to_bottom(&state.log_viewer);
                                } else {
                                    state.selected_idx = total_items - 1;
                                }
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
                            set_status(&state, "Focus: LOGS (↑/↓/j/k to select line, Tab to switch)");
                        } else {
                            state.selected_log_idx = -1;
                            set_status(&state, "Focus: PROCESSES (↑/↓/j/k to select app, Tab to switch)");
                        }
                    }
                } else if (c == '/') {
                    state.search_mode = true;
                    state.search_input[0] = '\0';
                } else if (c == 'u') {
                    if (is_log_mode) {
                        log_viewer_scroll_up(&state.log_viewer, 5);
                    } else if (state.has_update) {
                        state.confirm_update = true;
                        set_status(&state, "🚀 Upgrade to v%s? Press [y] to confirm, [n/Esc] to cancel", state.update_version);
                    }
                } else if (c == 'U') {
                    if (state.has_update) {
                        state.confirm_update = true;
                        set_status(&state, "🚀 Upgrade to v%s? Press [y] to confirm, [n/Esc] to cancel", state.update_version);
                    } else {
                        set_status(&state, "Checking for updates...");
                        render_dashboard(&state, apps, app_count);
                        updater_check_version(state.update_version, sizeof(state.update_version), &state.has_update, true);
                        if (state.has_update) {
                            state.confirm_update = true;
                            set_status(&state, "✨ New version v%s available! Press [y] to confirm upgrade", state.update_version);
                        } else {
                            set_status(&state, "✔ Already on the latest version (v%s)", FIRE_VERSION);
                        }
                    }
                } else if (c == 'n') {
                    if (is_log_mode) {
                        log_viewer_scroll_down(&state.log_viewer, 5);
                    }
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
                    if (is_log_mode) {
                        log_viewer_scroll_down(&state.log_viewer, 5);
                    } else if (state.selected_idx > 0 && state.selected_idx <= app_count) {
                        const char *name = apps[state.selected_idx - 1].name;
                        state.confirm_delete = true;
                        snprintf(state.confirm_delete_name, sizeof(state.confirm_delete_name), "%s", name);
                        set_status(&state, "⚠ Delete '%s'? Press [y] to confirm, [n/Esc] to cancel", name);
                    }
                }
            }
        }
    }

    log_viewer_cleanup(&state.log_viewer);
    systemd_free_list(apps, app_count);
    systemd_cleanup();
    sb_free(&g_screen_buf);
    g_tui_state = NULL;
    disable_raw_mode();
    return 0;
}
