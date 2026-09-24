#include "tui.h"
#include "dbus_systemd.h"
#include "unit_gen.h"
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

static void handle_winch(int sig) {
    (void)sig;
    g_win_resized = 1;
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

    /* Switch to alternate screen, hide cursor */
    write(STDOUT_FILENO, "\033[?1049h\033[?25l", 14);
}

static void disable_raw_mode(void) {
    /* Show cursor, restore main screen */
    write(STDOUT_FILENO, "\033[?25h\033[?1049l", 14);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
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

static void render_dashboard(TUIState *state, ProcessInfo *apps, int app_count) {
    ScreenBuffer sb;
    sb_init(&sb);

    /* Move cursor to 1,1 */
    sb_append(&sb, "\033[H", 3);

    int cols = state->cols;
    int rows = state->rows;

    /* 1. Header Banner */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    uint64_t total_mem = 0;
    int running_count = 0;
    for (int i = 0; i < app_count; i++) {
        total_mem += apps[i].memory_bytes;
        if (strcmp(apps[i].active_state, "active") == 0) {
            running_count++;
        }
    }
    char total_mem_str[32];
    format_mem(total_mem, total_mem_str, sizeof(total_mem_str));

    /* Banner Line 1 */
    sb_printf(&sb, "\033[38;5;208;1m🔥 fire-dash\033[0m \033[38;5;244mv%s\033[0m", FIRE_VERSION);
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

    /* Separator */
    sb_append(&sb, "\033[38;5;238m", 11);
    for (int i = 0; i < cols; i++) sb_append(&sb, "─", 3);
    sb_append(&sb, "\033[0m\033[K\r\n", 7);

    /* 2. Process Table */
    int table_max_rows = (rows - 8) / 2;
    if (table_max_rows < 4) table_max_rows = 4;

    /* Table Header */
    sb_printf(&sb, "\033[38;5;248;1m  %-4s %-18s %-12s %-8s %-10s %-8s %-12s\033[0m\033[K\r\n",
              "ID", "NAME", "STATUS", "PID", "MEMORY", "RESTARTS", "WATCH");

    if (app_count == 0) {
        sb_printf(&sb, "\033[38;5;244m  No applications registered. Run 'fire-dash start <script>' or create fire.config.json\033[0m\033[K\r\n");
        for (int i = 1; i < table_max_rows; i++) {
            sb_append(&sb, "\033[K\r\n", 5);
        }
    } else {
        int visible_start = 0;
        if (state->selected_idx >= table_max_rows) {
            visible_start = state->selected_idx - table_max_rows + 1;
        }

        for (int row = 0; row < table_max_rows; row++) {
            int idx = visible_start + row;
            if (idx >= app_count) {
                sb_append(&sb, "\033[K\r\n", 5);
                continue;
            }

            ProcessInfo *p = &apps[idx];
            bool is_selected = (idx == state->selected_idx);

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
                          idx, p->name, status_badge, pid_str, mem_str, p->restart_count, watch_str);
            } else {
                sb_printf(&sb, "  %-4d \033[38;5;253;1m%-18s\033[0m %-12s %-8s %-10s %-8u %-12s\033[K\r\n",
                          idx, p->name, status_badge, pid_str, mem_str, p->restart_count, watch_str);
            }
        }
    }

    /* 3. Middle Control / Status Bar */
    sb_append(&sb, "\033[38;5;238m", 11);
    for (int i = 0; i < cols; i++) sb_append(&sb, "─", 3);
    sb_append(&sb, "\033[0m\033[K\r\n", 7);

    /* Notification message if recently triggered */
    if (state->status_msg[0] && (time(NULL) - state->status_msg_time < 4)) {
        sb_printf(&sb, " \033[38;5;226;1m⚡ %s\033[0m\033[K\r\n", state->status_msg);
    } else {
        sb_printf(&sb, "\033[38;5;244m [↑/↓/j/k] Select  [s] Start  [x] Stop  [r] Restart  [d] Delete  [/] Search  [q] Quit\033[0m\033[K\r\n");
    }

    /* 4. Bottom Log Viewer Header */
    const char *selected_app_name = (app_count > 0 && state->selected_idx < app_count)
                                    ? apps[state->selected_idx].name
                                    : "none";

    sb_append(&sb, "\033[38;5;238m", 11);
    sb_printf(&sb, "╭─ \033[38;5;214;1mLogs: %s\033[0m \033[38;5;244m(%d lines)\033[0m ",
              selected_app_name, state->log_viewer.count);

    if (state->search_mode) {
        sb_printf(&sb, "\033[38;5;226;1m[Filter: %s_]\033[0m ", state->search_input);
    } else if (state->log_viewer.search_filter[0]) {
        sb_printf(&sb, "\033[38;5;51m[Filter: \"%s\"]\033[0m ", state->log_viewer.search_filter);
    }

    if (state->log_viewer.auto_scroll) {
        sb_printf(&sb, "\033[38;5;48m[auto-scroll: ON]\033[0m ");
    } else {
        sb_printf(&sb, "\033[38;5;208m[scrolled +%d]\033[0m ", state->log_viewer.scroll_offset);
    }

    for (int i = 0; i < cols - 50; i++) sb_append(&sb, "─", 3);
    sb_append(&sb, "╮\033[0m\033[K\r\n", 9);

    /* 5. Render Log Lines */
    int log_rows_avail = rows - (5 + table_max_rows + 4);
    if (log_rows_avail < 4) log_rows_avail = 4;

    LogViewer *lv = &state->log_viewer;
    if (lv->count == 0) {
        sb_printf(&sb, "\033[38;5;242m  (No logs recorded yet. Start the process or trigger actions to view journal)\033[0m\033[K\r\n");
        for (int i = 1; i < log_rows_avail; i++) {
            sb_append(&sb, "\033[K\r\n", 5);
        }
    } else {
        /* Filter and collect lines */
        int matching_indices[MAX_LOG_LINES];
        int match_count = 0;

        for (int i = 0; i < lv->count; i++) {
            int real_idx = (lv->head + i) % MAX_LOG_LINES;
            const char *text = lv->lines[real_idx].text;
            if (lv->search_filter[0]) {
                if (strcasestr(text, lv->search_filter) != NULL) {
                    matching_indices[match_count++] = real_idx;
                }
            } else {
                matching_indices[match_count++] = real_idx;
            }
        }

        if (match_count == 0) {
            sb_printf(&sb, "\033[38;5;244m  No lines match filter '%s'\033[0m\033[K\r\n", lv->search_filter);
            for (int i = 1; i < log_rows_avail; i++) {
                sb_append(&sb, "\033[K\r\n", 5);
            }
        } else {
            int end_idx = match_count - lv->scroll_offset;
            if (end_idx < 0) end_idx = 0;
            int start_idx = end_idx - log_rows_avail;
            if (start_idx < 0) start_idx = 0;

            int lines_printed = 0;
            for (int i = start_idx; i < end_idx && lines_printed < log_rows_avail; i++) {
                int real_idx = matching_indices[i];
                LogLine *ll = &lv->lines[real_idx];

                if (ll->is_err) {
                    sb_printf(&sb, "\033[38;5;196m  %.*s\033[0m\033[K\r\n", cols - 4, ll->text);
                } else {
                    sb_printf(&sb, "\033[38;5;250m  %.*s\033[0m\033[K\r\n", cols - 4, ll->text);
                }
                lines_printed++;
            }

            for (; lines_printed < log_rows_avail; lines_printed++) {
                sb_append(&sb, "\033[K\r\n", 5);
            }
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
    state.log_viewer.auto_scroll = true;
    log_viewer_init(&state.log_viewer);

    signal(SIGWINCH, handle_winch);
    enable_raw_mode();
    update_window_size(&state);

    ProcessInfo *apps = NULL;
    int app_count = 0;

    systemd_init(scope);
    systemd_list_all(scope, &apps, &app_count);

    if (app_count > 0) {
        state.selected_idx = 0;
        log_viewer_set_app(&state.log_viewer, apps[0].name, scope);
    }

    char last_selected_app[MAX_NAME_LEN] = "";
    if (app_count > 0) {
        snprintf(last_selected_app, sizeof(last_selected_app), "%s", apps[0].name);
    }

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

            if (state.selected_idx >= app_count && app_count > 0) {
                state.selected_idx = app_count - 1;
            }
        }

        /* If selected app changed, switch log viewer */
        if (app_count > 0 && state.selected_idx < app_count) {
            if (strcmp(last_selected_app, apps[state.selected_idx].name) != 0) {
                snprintf(last_selected_app, sizeof(last_selected_app), "%s", apps[state.selected_idx].name);
                log_viewer_set_app(&state.log_viewer, apps[state.selected_idx].name, scope);
            }
        }

        /* Render */
        render_dashboard(&state, apps, app_count);

        /* Wait for input with 200ms timeout */
        int poll_res = poll(&pfd, 1, 200);
        if (poll_res > 0 && (pfd.revents & POLLIN)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                if (state.search_mode) {
                    if (c == 27) { /* Escape: cancel search */
                        state.search_mode = false;
                        state.search_input[0] = '\0';
                        log_viewer_set_filter(&state.log_viewer, NULL);
                    } else if (c == '\r' || c == '\n') { /* Enter: confirm search */
                        state.search_mode = false;
                        log_viewer_set_filter(&state.log_viewer, state.search_input);
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

                if (c == 'q' || c == 'Q' || c == 3) { /* q or Ctrl+C */
                    state.should_quit = true;
                } else if (c == 27) { /* Escape sequence */
                    char seq[3];
                    if (read(STDIN_FILENO, &seq[0], 1) == 1 &&
                        read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if (seq[0] == '[') {
                            if (seq[1] == 'A') { /* Up Arrow */
                                if (state.selected_idx > 0) state.selected_idx--;
                            } else if (seq[1] == 'B') { /* Down Arrow */
                                if (state.selected_idx < app_count - 1) state.selected_idx++;
                            } else if (seq[1] == '5') { /* Page Up */
                                read(STDIN_FILENO, &seq[2], 1); /* consume ~ */
                                log_viewer_scroll_up(&state.log_viewer, 10);
                            } else if (seq[1] == '6') { /* Page Down */
                                read(STDIN_FILENO, &seq[2], 1); /* consume ~ */
                                log_viewer_scroll_down(&state.log_viewer, 10);
                            }
                        }
                    } else {
                        /* Plain ESC clears filter */
                        if (state.log_viewer.search_filter[0]) {
                            log_viewer_set_filter(&state.log_viewer, NULL);
                            set_status(&state, "Cleared search filter");
                        }
                    }
                } else if (c == 'k') {
                    if (state.selected_idx > 0) state.selected_idx--;
                } else if (c == 'j') {
                    if (state.selected_idx < app_count - 1) state.selected_idx++;
                } else if (c == '\t') {
                    state.focus = (state.focus == FOCUS_PROCESSES) ? FOCUS_LOGS : FOCUS_PROCESSES;
                } else if (c == '/') {
                    state.search_mode = true;
                    state.search_input[0] = '\0';
                } else if (c == 's') {
                    if (app_count > 0 && state.selected_idx < app_count) {
                        const char *name = apps[state.selected_idx].name;
                        set_status(&state, "Starting '%s'...", name);
                        systemd_start_unit(name, scope);
                        systemd_get_unit_status(name, scope, &apps[state.selected_idx]);
                        set_status(&state, "✔ Started '%s'", name);
                    }
                } else if (c == 'x') {
                    if (app_count > 0 && state.selected_idx < app_count) {
                        const char *name = apps[state.selected_idx].name;
                        set_status(&state, "Stopping '%s'...", name);
                        systemd_stop_unit(name, scope);
                        systemd_get_unit_status(name, scope, &apps[state.selected_idx]);
                        set_status(&state, "■ Stopped '%s'", name);
                    }
                } else if (c == 'r') {
                    if (app_count > 0 && state.selected_idx < app_count) {
                        const char *name = apps[state.selected_idx].name;
                        set_status(&state, "Restarting '%s'...", name);
                        systemd_restart_unit(name, scope);
                        systemd_get_unit_status(name, scope, &apps[state.selected_idx]);
                        set_status(&state, "⟳ Restarted '%s'", name);
                    }
                } else if (c == 'd') {
                    if (app_count > 0 && state.selected_idx < app_count) {
                        const char *name = apps[state.selected_idx].name;
                        systemd_stop_unit(name, scope);
                        systemd_disable_unit(name, scope);
                        unit_gen_delete(name, scope, NULL, 0);
                        systemd_daemon_reload(scope);
                        set_status(&state, "Deleted '%s'", name);
                        /* Refresh list immediately */
                        systemd_free_list(apps, app_count);
                        apps = NULL;
                        app_count = 0;
                        systemd_list_all(scope, &apps, &app_count);
                        if (state.selected_idx >= app_count && app_count > 0) {
                            state.selected_idx = app_count - 1;
                        }
                    }
                } else if (c == 'u') {
                    log_viewer_scroll_up(&state.log_viewer, 5);
                } else if (c == 'n') {
                    log_viewer_scroll_down(&state.log_viewer, 5);
                } else if (c == 'b') {
                    log_viewer_scroll_to_bottom(&state.log_viewer);
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
