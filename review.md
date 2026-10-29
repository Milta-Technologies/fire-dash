
What it is: fdash is a small command-line tool and terminal dashboard written in C (about 3,300 lines of its own code). It replaces pm2 by generating and controlling systemd services named fire-<name>.

Verdict: not ready to ship. Readiness is about 4/10. The code is cleanly structured and builds without warnings, and the few existing tests pass. But the main watch feature likely doesn't work, the dashboard has user-visible bugs, and there are real security problems. There is no sign it has ever run on a Linux machine with systemd: the author's commits come from a MacBook, there's no CI, and the systemd, dashboard and CLI code have no tests.

One gap in the report: the Fable agent was stopped twice by a safety filter partway through the security table. Its full write-up covers the summary, the check results and the first three security findings, and ends there. The code-quality, readiness and roadmap sections below are my summary of the agent's other findings. I re-checked four of its headline findings directly in the cloned code (src/main.c:408-414, src/tui.c:29, tui.c:983 vs 1036, src/unit_gen.c:153,188), and they are as described.

Checks the agent ran (on macOS; no systemd machine was available)

┌────────────────────────────────────────────────────────┬─────────────────────────────────────────────────────────────────────────┐
│                         Check                          │                                 Result                                  │
├────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────┤
│ make with strict warnings                              │ Passes with 0 warnings. The binary is 111 KB, although the README       │
│                                                        │ claims "~90 KB".                                                        │
├────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────┤
│ make test                                              │ Passes, but there are only 3 tests.                                     │
├────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────┤
│ Tests under memory/undefined-behaviour sanitizers, and │ Both clean.                                                             │
│  the clang static analyzer                             │                                                                         │
├────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────┤
│ Full CLI under the memory sanitizer, run with more     │ Fails: a stack buffer overflow at main.c:414.                           │
│ than 64 arguments                                      │                                                                         │
├────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────┤
│ Test coverage                                          │ 56% of lines and 37% of branches in the 3 modules that have tests.      │
│                                                        │ main.c, tui.c and dbus_systemd.c have no tests at all.                  │
├────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────┤
│ Secrets scan of the files and full git history         │ Nothing found.                                                          │
├────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────┤
│ Bundled JSON library (cJSON 1.7.18)                    │ Current, and it includes the fix for CVE-2024-31755.                    │
├────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────┤
│ fdash start on a machine without systemd               │ Printed OK / TRIGGERED anyway, so failures are reported as success.     │
└────────────────────────────────────────────────────────┴─────────────────────────────────────────────────────────────────────────┘

Security findings

┌──────────┬────────────────────────────────────────────────────────────────────────────────────┬──────────────────────────────────┐
│ Severity │                                      Finding                                       │              Where               │
├──────────┼────────────────────────────────────────────────────────────────────────────────────┼──────────────────────────────────┤
│          │ Services created with --system run as root. They have no User= setting and no      │                                  │
│ High     │ sandboxing, and they copy in the invoking user's $PATH and $HOME. The README       │ src/unit_gen.c:95-150            │
│          │ recommends this mode for production.                                               │                                  │
├──────────┼────────────────────────────────────────────────────────────────────────────────────┼──────────────────────────────────┤
│          │ Values from the config's env block (typically database passwords and API keys) are │                                  │
│ High     │  written in plain text into service files that everyone on the machine can read    │ unit_gen.c:116-124,153           │
│          │ (permissions 0644). With --system these files live in /etc/systemd/system.         │                                  │
├──────────┼────────────────────────────────────────────────────────────────────────────────────┼──────────────────────────────────┤
│          │ Command injection: app names go straight into shell commands without checking. A   │ main.c:373-386, dbus_systemd.c   │
│ Medium   │ name-validation function exists (is_valid_name), but it only runs when unit files  │ (9 places), log_viewer.c (4      │
│          │ are written or deleted. The stop, restart, delete, logs and save commands never    │ places)                          │
│          │ apply it. The agent confirmed this on logs and stop.                               │                                  │
├──────────┼────────────────────────────────────────────────────────────────────────────────────┼──────────────────────────────────┤
│ Medium   │ Stack buffer overflow: a fixed 64-slot array is filled from the command-line       │ main.c:408-414                   │
│          │ arguments with no size check.                                                      │                                  │
├──────────┼────────────────────────────────────────────────────────────────────────────────────┼──────────────────────────────────┤
│          │ Running fdash start with no arguments picks up fire.config.json from the current   │                                  │
│ Context  │ directory. In a cloned project, that registers the project's script as a           │ src/config.c:9-24                │
│          │ permanent, auto-restarting service without asking.                                 │                                  │
└──────────┴────────────────────────────────────────────────────────────────────────────────────┴──────────────────────────────────┘

Bugs that block release

1. Watch mode probably doesn't restart anything. The generated watch unit (unit_gen.c:188) only starts the service, and starting a service that is already running does nothing. The watch is also not recursive, so watch: "./src" misses files in src/lib/. This follows from how systemd path units work, but it hasn't been tested on a real systemd machine.
2. The dashboard's d (delete) key never fires. An earlier branch at tui.c:983 already handles d as scroll-down, so the delete code at tui.c:1036 is dead.
3. The terminal is left broken on exit. 12 write() calls in tui.c pass the wrong byte count for their text. For example, tui.c:29 passes 35 for a 38-byte sequence, which cuts off the "leave alternate screen" code. The agent confirmed this by capturing a real run.
4. Quoting is broken in the generated service. A script containing double quotes produces ExecStart=/bin/bash -lc "node -e "console.log('hi')"".
5. Failures are ignored. The results of system() calls are discarded (for example main.c:276-277), so the CLI reports OK even when the command failed.

Code quality

- Good: clear separation into modules (unit generation, config, systemd calls, log viewer, dashboard). No compiler warnings, no leftover TODOs, the bundled library is up to date, and the license is handled correctly.
- Weak:
  - The code runs systemctl and journalctl by building shell command strings instead of passing argument arrays or using systemd's own API (sd-bus).
  - Byte counts for text are typed by hand throughout tui.c.
  - tui.c is one 1,063-line file, and its keypress handling is a long if/else chain with conflicting keys.
  - There are 13 minor sign-conversion warnings under stricter compiler flags.
  - Coverage is thin, and the riskiest code has none.

Product readiness

- Missing: CI, release tags or builds, packages (.deb, Homebrew, etc.), and any testing against a real systemd machine.
- The README is inaccurate in places:
  - its binary-size claim is wrong;
  - it mentions D-Bus and a CPU column that don't match the code;
  - its clone URL points to a different GitHub organisation.
- Error handling is misleading: the tool claims success when systemd isn't present.

Roadmap

P0: before any release
- Check every name with is_valid_name() as soon as it enters from the command line or dashboard. Replace system()/popen() with fork+execvp using an argument list. (M)
- Put a size limit on filtered_args. (S)
- Replace hand-typed byte counts with sizeof(lit)-1 or a macro. (S)
- Fix the d key conflict. (S)
- Check every system() / systemctl result and report failures honestly. (S)
- Rework watch mode: have it trigger a oneshot unit that runs systemctl restart, and handle subdirectories. (M)
- For --system: require a User=, add basic sandboxing settings (NoNewPrivileges, ProtectSystem, PrivateTmp), stop copying the user's PATH/HOME, and use multi-user.target. (M)
- Move secrets into a separate EnvironmentFile= with permissions 0600. (S)
- Fix the quoting in ExecStart. (S)

P1: soon after
- Set up CI on Linux: build with the sanitizers and run an integration test against real systemd in a container. (M/L)
- Add tests for main.c, tui.c and dbus_systemd.c. (M)
- Ask for confirmation before loading a fire.config.json found in the current directory. (S)
- Fix the README. (S)

P2: nice to have
- Use sd-bus directly when libsystemd is available. (L)
- Split tui.c into smaller pieces and use a key-binding table. (M)
- Release packaging and versioned tags. (M)
