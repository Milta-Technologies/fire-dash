# 🔥 fdash (fire-dash)

> **The ultra-lightweight terminal process dashboard & manager on top of `systemd`.**  
> Written in pure C (ANSI C99) with zero external TUI dependencies. Binary size: **< 100 KB**.  
> Primary command: **`fdash`** (alias: `fire-dash`).

---

## ⚡ Why fdash?

`pm2` is friendly, but it brings along a massive Node.js runtime, high idle memory consumption (50–100 MB+), and custom daemon failure modes. `systemd` is the rock-solid, kernel-level standard on modern Linux, but raw `systemctl` lacks an interactive terminal dashboard, quick developer commands, ecosystem JSON configs, and unified live log streaming.

**`fdash` gives you the best of both worlds:**
1. **Rock-Solid Foundation:** Every process is a true native `systemd` service (`fire-<name>.service`).
2. **Ultra-Lightweight C Binary:** Consumes virtually **0 MB** RAM at idle and builds into a single self-contained ~90 KB executable.
3. **PM2-like Developer Ergonomics:** Simple commands like `fdash start`, `stop`, `restart`, `delete`, `logs`, `save`.
4. **Live Split-Pane TUI Dashboard:** Running `fdash` with no arguments opens an interactive terminal UI with live CPU/RAM metrics, process list, and real-time journal log tailing with search.
5. **Native Watch Mode via Systemd `.path` Units:** File watch triggers auto-restart natively through systemd without keeping extra Node.js/Python watcher daemons alive in memory!
6. **Multi-App Config:** Define all your services in a `fire.config.json` (or `ecosystem.config.json`) and start them in one command.

---

## 🖥️ Interactive TUI Dashboard

Running `fdash` without arguments (or typing `fdash monit`) opens the split-pane dashboard:

```text
🔥 fdash v1.0.0  │  Scope: USER (~/.config)  │  Online: 3/3  │  RAM: 142.4 MB          00:15:32
───────────────────────────────────────────────────────────────────────────────────────────────────
  ID   NAME               STATUS       PID      MEMORY     RESTARTS WATCH       
  ──── ────────────────── ──────────── ──────── ────────── ──────── ────────────
► 0    api-service        ● active     14208    48.2 MB    0        ✓ active    
  1    worker-queue       ● active     14219    68.5 MB    1        -           
  2    metrics-agent      ● active     14234    25.7 MB    0        ✓ active    
───────────────────────────────────────────────────────────────────────────────────────────────────
 [↑/↓/j/k] Select  [s] Start  [x] Stop  [r] Restart  [d] Delete  [/] Search  [q] Quit
╭─ Logs: api-service (100 lines) [Filter: ""] [auto-scroll: ON] ───────────────────────────────────╮
  2026-09-25T00:14:22+0000 server [14208]: [INFO] Database connection established
  2026-09-25T00:14:23+0000 server [14208]: [INFO] HTTP server listening on port 3000
  2026-09-25T00:14:25+0000 server [14208]: [GET] /healthcheck 200 OK (1.2ms)
╰──────────────────────────────────────────────────────────────────────────────────────────────────╯
```

### Dashboard Shortcuts:
- `↑` / `↓` or `k` / `j`: Navigate applications in table (or navigate/select log lines when in log mode)
- `Mouse Click`: Click any log line to select it (or click an app to switch logs)
- `Enter` or `o`: Open **Log Detail Inspector** for selected log line (or toggle Fullscreen if no log is selected)
- `m`: Insert a visual checkpoint marker (`─── [MARK: HH:MM:SS] ───`) **directly below the selected log line** (or at bottom if none selected)
- `c`: Copy selected log payload to clipboard (`pbcopy` / `xclip` / `wl-copy`)
- `a`: Quick-toggle between active single-app logs and unified `[ALL APPS]` stream
- `f`: Toggle **Fullscreen Log View** (expands logs to full terminal height)
- `Tab`: Switch focus between Process Table and Live Logs
- `l`: Cycle log triage filter level: `ALL` ➔ `WARN+ERR` ➔ `ERR ONLY`
- `/`: Interactive log search with instant keyword highlighting (`Esc` to cancel, `Enter` to commit)
- `Space`: **Pause / Resume** live auto-scroll (buffers incoming logs in background)
- `Trackpad / Mouse Wheel`: Smoothly scroll through logs up/down
- `PageUp` / `PageDown` or `u` / `d`: Scroll through logs
- `g`: Jump to oldest log line in buffer
- `b` or `G`: Jump to bottom / resume auto-scroll
- `s`: Start selected application (or all applications if `[ALL APPS]` is selected)
- `x`: Stop selected application (or all applications if `[ALL APPS]` is selected)
- `r`: Restart selected application (or all applications if `[ALL APPS]` is selected)
- `d`: Delete service unit and unregister from systemd
- `Esc`: Unselect log line, close Inspector, exit fullscreen, or clear search filter
- `q` or `Ctrl+C`: Exit dashboard

---

## 🚀 Installation & Building

### Requirements
- Linux with `systemd` (`systemctl` & `journalctl`)
- Standard C compiler (`gcc` or `clang`) and `make`
- Optional: `libsystemd-dev` for native D-Bus integration (auto-detected; falls back cleanly to direct systemctl interface if absent)

### Quick Build
```bash
git clone https://github.com/the-synomics-project/fire-dash.git
cd fire-dash
make
sudo make install
```

This installs **`fdash`** (and symlinks `fire-dash` alias) to `/usr/local/bin/`.

---

## 🛠️ CLI Commands & Usage

### 1. Starting an App
```bash
# Start a script with auto-derived name
fdash start app.js

# Custom name and working directory
fdash start "node server.js" --name api --cwd /var/www/api

# Enable watch mode (creates fire-api.path watching the folder)
fdash start "python3 bot.py" --name discord-bot --watch ./src

# Target system scope (/etc/systemd/system) instead of user scope
sudo fdash start "node prod.js" --name prod-api --system
```

### 2. Multi-App Config (`fire.config.json`)
Create a `fire.config.json` in your project root:
```json
{
  "apps": [
    {
      "name": "api-service",
      "script": "npm run build && npm start",
      "cwd": "./server",
      "env": {
        "NODE_ENV": "production",
        "PORT": "3000"
      },
      "watch": "./server/src",
      "restart_sec": 2
    },
    {
      "name": "worker-queue",
      "script": "python3 -m worker.main",
      "cwd": "./worker",
      "restart_sec": 3
    }
  ]
}
```

Start all apps at once:
```bash
fdash start
# or specify file:
fdash start fire.config.json
```

### 3. Process Control
```bash
# Stop a service or all services
fdash stop api-service
fdash stop all

# Restart a service or all services
fdash restart api-service
fdash restart all

# Delete / unregister services
fdash delete api-service
fdash delete all
```

### 4. Viewing Status & Logs
```bash
# Non-interactive CLI table (ideal for scripts and CI)
fdash list
fdash ls

# Stream logs in terminal
fdash logs api-service -f
fdash logs -n 100
```

### 5. Persist Across Reboots
```bash
fdash save
```
Enables all currently registered `fire-*` units with systemd so they boot automatically on system restart.

---

## 📐 Architecture

- **`src/tui.c`**: ANSI escape sequences with raw termios. Double-buffered single-write frame pipeline. Zero dependencies on ncurses.
- **`src/unit_gen.c`**: Generates standard INI `fire-<name>.service` and `fire-<name>.path` units in `~/.config/systemd/user/` or `/etc/systemd/system/`. Injects host `$PATH`, `$HOME`, and `$USER`.
- **`src/dbus_systemd.c`**: Direct D-Bus (`sd-bus`) communication with fallback CLI wrapper, querying unit active states, restart counters, and `/proc/<pid>/` memory & CPU metrics.
- **`src/log_viewer.c`**: Ring-buffered journal log streamer with non-blocking pipes, search indexing, and auto-scroll handling.
- **`src/config.c` & `src/cJSON.c`**: Embedded lightweight JSON parser for config files.

---

## ⚖️ License
MIT License. Free and open source.
