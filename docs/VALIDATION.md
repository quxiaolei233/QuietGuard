# Validation and resource acceptance

## What passed in the Linux cloud workspace

- Fresh-source review against primary Windows API documentation
- Portable core suite: 1,132,385 assertions, not 1,132,385 distinct tests
- 10,000 bounded parser fuzz inputs and 102,400 deterministic state-machine events
- 20/20 injected unsafe defects detected by the test suite
- GCC strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer
- API-mock tests for CPU math/group rejection, PDH validity/topology/engine sums, disk maxima, network baselines/interface changes, WTS vetoes, clock gaps/suspend, selected-only acquisition and bounded resource cleanup
- Native Windows x64 PE cross-build; embedded manifest, DPI declaration, app icon, no non-system runtime DLL dependency
- Independent static integration/security review; detected issues were fixed before final archive

## Limited historical Windows checks

Earlier versions completed bounded self-test, argument rejection and startup
checks. Paused-state measurements do not establish enabled-monitor overhead.
A later 0.3 sensor test observed intermittent disk sample failures, so sensor
acceptance remained incomplete. Raw machine reports are not distributed here.

0.3.1 adds diagnostic evidence and corrects redundant rejection of the next
independently valid interval. It does not relax invalid-value handling or
establish the original failure's cause. Native Windows retesting remains pending.
See [disk diagnostics](DISK-DIAGNOSTIC.md).

## Known aggregate test failure

The safe-entry mutation test creates a temporary copy without the required
`scripts/Measure-Safe-Sensors.ps1` file. The aggregate script stops with a
missing-file error. Ordinary static contracts and independent mock tests have
passed, but the complete suite is not passing. This snapshot preserves that
existing test behavior. The application manifest also remains at 0.3.0.0 while
source and file/product versions identify 0.3.1; no product metadata was changed.

## Current 0.3.1 build: explicitly not run / not proven

- Actual Windows 11 execution, visual rendering, accessibility tree, RDP event delivery or localized PDH data
- Actual SCM installation/uninstallation, LocalService provider access, DACL/IPC negative tests, UAC cancellation and mode switching
- Native concurrent cancellation stress tests, Windows handle/GDI/heap stability
- CPU, private/working RAM, provider/kernel overhead, physical disk I/O or workload slowdown on Windows
- Actual shutdown, unsaved-app blocking and recovery after a Windows/user-aborted shutdown
- LeakSanitizer: executor tracing prevents its use; ASan was run with `detect_leaks=0`

Linux/API-mock tests are not evidence of Win32 behavior. No fabricated Windows screenshot or performance result is supplied.

## Runtime measurement plan

Use the same approved Windows 11 target, OS/build, hardware, GPU driver and power plan. First establish that the dry-run executable is the exact hash in PROVENANCE.json. Never test real shutdown on an important unattended session.

For each mode (login hidden / service with UI closed), measure CPU-only, GPU-only, disk-only, network-only and all sensors; also test UI visible and configured 60-second vs faster intervals. Run identical app-off and app-on workloads for 30–60 minutes, repeat and report spread. Include a quiet machine and real CPU/GPU/disk/network workloads. Record warmup and steady-state separately.

Read-only observer example:

```
.\scripts\Measure-QuietGuard.ps1 -ProcessId 1234 -Minutes 30 -Output .\run-all-hidden.csv
```

Record cumulative CPU seconds / elapsed seconds (one-logical-core percent), private bytes, working set, peak, handle and thread change, and process I/O deltas. Task Manager rounding to 0% is not proof. Script I/O includes file/network/device operations and cannot establish physical disk overhead. Use a short WPR/WPA A/B trace to include PDH provider/kernel CPU, hard faults, disk traffic and wakes; keep observer overhead out of the app claim.

Targets are provisional acceptance criteria, **not observations**: normal hidden monitor has no periodic application file writes, no unbounded growth, no high-resolution timers and one configured normal sample wake. Aim for <20 MiB private bytes in login mode / <15 MiB service and <0.05% average whole-machine CPU on a quiet 4+-core reference PC; measure one-core CPU too. If provider overhead exceeds the target, change the sensor path or disclose the cost rather than hiding it with memory trimming or coarse displays.

## Required native test scenarios

- 100/150/200% DPI, 1366×768 or smaller effective work area, monitor DPI move, keyboard Tab/mnemonics, scroll-to-focus, high contrast, screen-reader field names; validate Chinese glyphs and all warning text
- Start, pause, invalid Save, repeated clicks, tray close/open, Explorer restart, hidden warning visible, existing-instance activation
- Unknown/missing GPU, headless GPU/TCC/unsupported WDDM, localized OS, GPU processes starting/stopping, disk/NIC hotplug, VPN reconnect, provider stall
- Suspend/resume, clock adjustments, long scheduler stalls, counter regressions, RDP reconnect/disconnect and locked console with protection on/off
- Cancel before final sample, between result and timer, while provider stalls, during UAC install/config, after helper failure/timeout and after commit; report exact irreversible boundary
- Both startup modes never simultaneously monitor; different users/sessions, login Run-value restoration after canceled installation, corrupt config, unwritable configuration, power loss during write
- Service pipe unauthorized CONFIG, remote connection refusal, pre-created pipe spoofing, wrong process identity, malformed/oversized frames, stalled clients, config reparse/hardlink/untrusted-owner rejection
- Explicit service dry-run install, stop/remove after accepted simulated request, resource cleanup, reboot behavior and cancellation persistence semantics

## Primary sources

- CPU API/group coverage: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes
- Performance API overhead: https://learn.microsoft.com/en-us/windows/win32/perfctrs/about-performance-counters
- PDH validity: https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhcollectquerydata
- English/wildcard handling: https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw
- GPU architecture: https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/
- Process CPU/memory: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes and https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo
- Process I/O: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessiocounters
- System-level profiling: https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-recorder
- Native design/accessibility: https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/typography and https://learn.microsoft.com/en-us/windows/win32/winauto/accessibility-best-practices
- Non-forcing system shutdown: https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-initiatesystemshutdownexw
