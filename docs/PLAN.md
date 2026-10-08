# QuietGuard: fresh native Windows implementation plan

Initial empty source milestone: f0ac805. No previous application source was reused.

## Decisions before implementation

- C17 + Win32: mature native APIs, no managed runtime/WebView/browser, no telemetry, no polling subprocesses.
- One shared fail-closed decision core and strict bounded configuration parser. Both login and boot-service modes use it.
- Per-user UI runs metrics on one sleeping worker and service IPC on a separate sleeping worker so a blocked provider cannot block service Cancel. No UI animation or hidden redraw timer.
- Optional service runs as LocalService; settings UI is on demand. Authenticated local pipe is bounded; configuration requires elevated administrators. Installation is opt-in, admin-protected binary/config paths, no installation during development.
- Modes exclude each other using one protected named monitor gate. Login mode has no monitoring before first login after boot. Service mode works before login but cannot show ordinary session-0 windows.
- Default active-session veto protects connected console/RDP work. A locked but still connected session also vetoes. Disconnected sessions may become eligible. Enumeration errors are unknown and veto.
- All selected metrics must be valid and below thresholds. CPU is average busy across supported system processors; GPU is busiest engine after summing process contributions; disk is busiest physical disk active time; network is total receive+send KiB/s across up non-loopback interfaces (virtual adapters may double-count conservatively).
- Defaults: 180 min idle, 60 s sample, 60 s app-owned warning. Any busy/unknown/veto resets the entire interval. Suspend, resume, restart and excessive gaps reset history. No progress persisted.
- There is no mathematical proof of continuous resource inactivity between samples. Interval averages can hide brief bursts; this limitation is visible and documented.
- Shutdown is local, non-forcing, only after fresh final metrics and epoch/cancel barrier. OS request acceptance is not proof of completion; there is no force fallback.
- Release executable contains safe self-tests and dry-run mode; no power or startup mutation during cloud testing.

## Acceptance gates

1. Portable logic/config tests, adversarial traces, sanitizer checks, reproducible seed.
2. Independent safety/static review including service IPC, ACLs, resource ownership and cancellation.
3. Strict x64 cross-compile and import/binary size/hash provenance, sources committed before archive.
4. Native Windows 11 test gate (NOT available on Linux): GUI at 100/150/200% DPI, keyboard/high contrast, actual metrics and topology failures, RDP/suspend, install/uninstall/mode switch, safe dry-run cancellation, and CPU/private bytes/handles over time. Only an explicitly authorized real shutdown can validate power-off end to end.
5. Targets, not measured results: hidden monitor CPU under 0.05% average on a quiet 4+core reference PC, private bytes under 20 MiB login / 15 MiB service, no steadily rising handles, at most one normal sampling wake per configured interval. Measure PDH GPU overhead separately.

Sources and observed test outcomes will be recorded separately.
