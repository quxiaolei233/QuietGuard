# Safe headless sensor measurements

This runner exercises the actual production metric providers and pure decision core in a separate, headless diagnostic process. It cannot request shutdown, save production configuration, enable startup, install/control a service, or operate the GUI. It uses a separate singleton and does not touch an existing QuietGuard instance.

**These are diagnostic-process measurements.** They do not establish GUI/service overhead, physical disk traffic, provider/kernel costs outside the process, workload slowdown, or whether the production application meets its resource targets. A previously measured paused GUI is not evidence for active sensor costs. See [SAFE-SENSOR-PLAN.md](SAFE-SENSOR-PLAN.md) and [VALIDATION.md](VALIDATION.md).

## Preconditions and authorization

- Use an explicitly authorized Windows x64 test machine/session. This feature has not been executed on Windows merely because Linux portable tests or a cross-build pass
- Keep `QuietGuard.exe` and its exact build's `PROVENANCE.json` together. The launcher requires the case-sensitive filename `QuietGuard.exe`, SHA-256 and byte length matching the sibling manifest, and a plausible commit/source identity
- Trust the package source independently. A matching self-supplied manifest proves package consistency, not a publisher signature
- Use a fresh Windows PowerShell 5.1 or PowerShell 7 process for each invocation; the script refuses to reuse an already loaded measurement helper. Use an execution policy that already permits this script. The launcher does not modify execution policy or request elevation. If policy blocks the script, stop and ask the authorized administrator for the approved method; do not bypass policy
- Supply explicit absolute paths on a ready, fixed local drive. UNC/device paths, alternate data streams, reparse points/symlinks, ambiguous path components and existing output directories are rejected. Choose a short output path; each final native filename must be under 240 characters
- The output directory must not exist, and its parent must already exist. The launcher creates it without overwriting anything. It holds read-only, non-delete/non-write-sharing handles on the verified executable and manifest until all created children stop
- Keep other people/tools from changing the package/output directories during the run. File handles protect verified file objects, but ancestor directories are checked rather than locked; this trusted-directory assumption excludes concurrent directory rename/replacement races. This script is not a sandbox for hostile binaries or a substitute for filesystem access controls
- The isolated test cannot prevent a separate production QuietGuard instance from taking its own power action. Before a future authorized test, the tester must independently establish that production is paused/safe under its own authorization. This script leaves it untouched and never quietly pauses, stops or reconfigures it. Record its state/workload for comparisons

## Focused disk diagnosis (0.3.1)

Use `-Mode Disk -DurationSeconds 120 -IntervalSeconds 10` with the same verified
launcher, explicit new output path and authorization. This runs only disk
providers and saves `disk.native.json`. Its additive diagnostics preserve PDH
stage/status/uncapped value/raw evidence plus independent clock/session results.
See [DISK-DIAGNOSTIC.md](DISK-DIAGNOSTIC.md) for the complete command and limits.
The default remains all sensors; Matrix remains unchanged.

## Default: about twelve minutes, all sensors

From an already authorized PowerShell console, substitute your actual extracted package and a **new** directory below an existing parent:

```powershell
powershell.exe -NoProfile -File 'C:\QuietGuard\scripts\Measure-Safe-Sensors.ps1' `
  -Executable 'C:\QuietGuard\dist\QuietGuard.exe' `
  -OutputDirectory 'C:\QuietGuard-results\all-2026-10-03'
```

Default selected sensors are CPU, GPU, disk and network. Duration is 720 seconds, sampling interval 60 seconds, and the per-provider-operation timeout 15 seconds. The first two intervals (120 seconds) are explicitly warmup; roughly 600 seconds remain. Startup and cleanup can add time. The script does not generate workloads or keep the computer awake. Sleep, suspend, scheduling delays, provider availability and discontinuities must remain visible in the evidence.

A bounded custom all-sensor run is possible:

```powershell
powershell.exe -NoProfile -File 'C:\QuietGuard\scripts\Measure-Safe-Sensors.ps1' `
  -Executable 'C:\QuietGuard\dist\QuietGuard.exe' `
  -OutputDirectory 'C:\QuietGuard-results\all-custom' `
  -DurationSeconds 300 -IntervalSeconds 30 -SampleTimeoutSeconds 15
```

Bounds are duration 60–900 seconds, interval 5–120 seconds, duration at least three intervals, and timeout 1–30 seconds. These are diagnostic options only; they are never written into production configuration.

## Optional: five-selection matrix, about twelve and a half minutes

```powershell
powershell.exe -NoProfile -File 'C:\QuietGuard\scripts\Measure-Safe-Sensors.ps1' `
  -Executable 'C:\QuietGuard\dist\QuietGuard.exe' `
  -OutputDirectory 'C:\QuietGuard-results\matrix-2026-10-03' `
  -Mode Matrix
```

Order: CPU, GPU, disk, network, then all four. Each gets its own new process and report, with 150 seconds duration and 30-second intervals. The first 60 seconds are warmup, leaving roughly 90 seconds per selection. Nominal sampling time is 750 seconds total; startup/cleanup add time. Do not combine Matrix with duration/interval overrides. Timeout can still be set within its bounds.

This short sequential matrix is a **screening** exercise, not a randomized or controlled benchmark. Workload, caches, system state and temporal drift may differ between selections. Missing GPU support, hotplug, counter resets, suspend and timeouts are findings, not zeros. The matrix can continue after a sensor/provider failure so remaining selections still produce evidence; a user cancellation or watchdog cancellation stops it.

## Exact native interface

The script builds only these separate argument tokens; there are no additional-argument, production-mode or shell-command parameters:

```text
C:\QuietGuard\dist\QuietGuard.exe --sensor-test --metrics cpu,gpu,disk,network --duration-seconds 720 --interval-seconds 60 --sample-timeout-seconds 15 --output C:\QuietGuard-results\one-new-report.json
```

Quote the output path when needed. Do not use `--duration-seconds720`, equals-style forms, repeated options, or mix in production/service/startup/shutdown/self-test/build-info arguments. The safe parser rejects invalid/mixed modes before production initialization. For a single selected sensor, use `cpu`, `gpu`, `disk`, or `network` as the metrics value. The wrapper exposes the fixed All/Matrix plans to reduce mistakes.

If the optional `.ps1` is simply unavailable, a separately authorized tester can use the exact direct executable interface above after independently verifying that full executable path's SHA-256 and byte length against its sibling `PROVENANCE.json`. The native runner produces its own resource/validity report; the wrapper's extra job/watchdog, plan summaries and `CANCEL` file control are absent. This is an independently authorized executable test path, not a workaround for a policy denial: if script execution was denied, stop and obtain the administrator's approved method rather than routing around that denial. Do not start a different filename or a copy found through PATH.

The native runner reserves the explicit output file with create-new semantics before starting providers, retains a bounded in-memory report and writes final JSON once. A preflight/setup failure may leave no JSON or an empty reserved file; neither is successful evidence. Ordinary application runs gain no diagnostic log.

## Cancellation without mouse automation

The preferred unattended cancellation signal is a new empty file named `CANCEL` in that run's output directory. From a second PowerShell console:

```powershell
New-Item -ItemType File -Path 'C:\QuietGuard-results\matrix-2026-10-03\CANCEL'
```

The launcher checks it every 200 milliseconds, requests cancellation for its current child and starts no later selections. This is a local script control file, not a production app command. Ctrl+C is also supported in a normal console; behavior of GUI/script hosts differs. The script's `finally` cleanup is a second protection if the pipeline is interrupted.

For each child, the launcher:

1. Creates exactly the verified executable with fixed `--sensor-test` arguments, initially suspended and with no inherited handles
2. Assigns it to a new unnamed job limited to **one process**, with kill-on-job-close, before resuming it. It refuses the run if this containment fails
3. Retains the **original process handle**. Its watchdog can signal only `Local\QuietGuard.SensorTest.<that child PID>.Cancel` while that child is still alive; it never creates/spoofs the event
4. On cancellation, allows up to `sample timeout + 5` seconds for native cleanup. If still running, it terminates only its original test-process handle. The PID is not reopened and cannot be used to accidentally target a reused PID
5. Independently requests watchdog cancellation if the child exceeds `duration + sample timeout + 20` seconds. Including grace, the supervisor's outer bound is approximately `duration + 2 × timeout + 25` seconds, plus process scheduling/OS termination delay

The job contains no existing production process and its one-process limit prevents child process creation. Closing the PowerShell host closes its job handle, bounding an abandoned test even when no final JSON can be written. An abrupt host/OS failure can interrupt serialization; preserve partial files. Never use a process-name kill, a PID supplied by another process, or a real-instance command as a substitute.

## Collecting and interpreting results

Collect the **entire output directory**, without removing partial/error files. It contains:

| File | Meaning |
| --- | --- |
| `session.json` | Initial plan, source/executable identity, and explicit incomplete-until-final rule |
| `provenance.json` | Copy of the build manifest read under lock |
| `<selection>.native.json` | Original native report, untouched by the observer; may be absent, empty or incomplete on failure |
| `<selection>.summary.json` | Native identity/options validation, exit/cancellation status, per-sensor validity, and resource summaries |
| `final-summary.json` | Machine-readable session state, every attempted run and limitations |
| `CANCEL` | Optional user cancellation request, if used |

All result files use UTF-8. No upload, archive, installation or external sharing happens. The output may reveal local paths, PIDs and system activity; share it only with the intended reviewer.

A simple read-only collection check:

```powershell
$result = Get-Content -Raw -LiteralPath 'C:\QuietGuard-results\matrix-2026-10-03\final-summary.json' | ConvertFrom-Json
$result.state
$result.runs | Select-Object selection, state, child_exit_code, cancellation_reason
```

`completed` requires every planned run, an identity/options-matched native report, native exit code zero and `completed` reason, orderly provider cleanup, no cancellation/forced termination, and at least one steady-state sample where **every selected sensor** was valid. This establishes lifecycle/selected-sensor collection only. Each run has a separate `resource_probe_status` (`complete`, `partial`, `unavailable`) plus per-probe validity counts; even a completed sensor run can have unknown CPU/memory/I/O probes. It is not a resource-budget pass. Any missing final summary means the session is **incomplete**. `interrupted`/`incomplete` are never silently promoted to success. The script throws a terminating error for either; when invoked as `powershell.exe -File ...`, the caller can also retain the nonzero exit status.

### Native report schema version 1

- `kind`: `quietguard.sensor-test`; `pid`; `cancellation_event`
- `build`: `version`, `commit`, `source`. The observer compares commit/source with the verified manifest; the preview manifest version can differ in suffix from the embedded app version
- `options`: `mask`, `duration_seconds`, `interval_seconds`, `timeout_seconds`; metric bits CPU=1, GPU=2, disk=4, network=8
- `outcome`: `reason`, `initiating_reason`, `exit_code`, `forced_cleanup`, `provider_destroyed` (additional native controller-stage fields may also be retained)
- `timing`: `started_tick_ms`, `ended_tick_ms`, `elapsed_ms`, `warmup_intervals` (=2), `warmup_ms`
- `summary`: `sample_count`, `valid_samples`, `steady_valid_samples`, `valid_mask_union`
- `process_start`, `process_end`, and `samples[].process`: `valid_timing`, `capture_started_tick_ms`, `capture_ended_tick_ms` expose the resource capture window (null times when invalid); validity booleans `valid_cpu`, `valid_memory`, `valid_handles`, `valid_threads`, `valid_io`; corresponding `cpu_ms`, `private_bytes`, `working_set_bytes`, `peak_commit_bytes`, `peak_working_set_bytes`, `handles`, `threads`, and cumulative `read_operations`, `write_operations`, `other_operations`, `read_bytes`, `write_bytes`, `other_bytes`. Invalid groups have null values
- `samples`: up to 181 records with `index`, `elapsed_ms`, `warmup`, `at_ms`, `valid_mask`, `values` (CPU/GPU/disk/network in that order, null when invalid), `veto`, `discontinuity`, `collector_ms`, `phase`, and `process`

Native exit codes: 0 completed; 2 invalid arguments; 3 test singleton busy; 4 output preflight failed; 5 setup/controller/telemetry error; 6 provider creation failed; 7 provider timeout; 8 cancelled; 9 no steady sample valid for all selected sensors; 10 forced cleanup; 11 final output failure. Forced cleanup uses reason `forced_cleanup`, preserves its initiating reason in `initiating_reason`, and sets `forced_cleanup`. The supervisor uses code 125 only when it must terminate its own child. No-report and exit/report mismatches remain incomplete regardless of exit code.

### Resource windows and units

- `all_observed_resources` spans native start, retained sample resource observations and native end; `steady_observed_resources` begins at the **first retained non-warmup observation**, not a fabricated exact warmup-boundary reading, and includes orderly provider cleanup if reached
- For each resource, retain the valid observation count, first, last, maximum, delta and that resource's observed seconds. Missing groups stay null; different availability can create different windows
- Probe capture and its associated lifecycle/sample timestamp are not atomic; instrumenting and scheduling can skew short-window rates. The summary uses approximate lifecycle/sample windows, while raw `valid_timing` and `capture_started_tick_ms`/`capture_ended_tick_ms` preserve each probe capture bound. Inspect those bounds and avoid inferring precise sub-interval CPU rates.
- CPU seconds are cumulative native process CPU milliseconds divided by 1000. One-logical-core utilization is `100 × delta CPU seconds / observed wall seconds`, and may exceed 100 with concurrent work. This is not whole-machine percent
- Private bytes, working set and high-water commit/working-set bytes are byte counts. `peak_commit_bytes` is Windows `PeakPagefileUsage`, not peak private bytes. The sampled private peak is `private_bytes.max`, which can miss between-sample peaks. Delta is growth over the stated observed window. Peaks are cumulative process high-water values and may include warmup even in the steady summary; they are not a reset steady-only high-water mark
- Handles and threads show observed counts/maximum/growth. Negative memory/count deltas can represent cleanup; they are not automatically measurement errors
- Process I/O includes file/network/device operations. These counters do **not** establish physical disk I/O or work performed by provider services/kernel components outside the process
- The native end capture occurs before final JSON serialization and process teardown. Those costs are excluded from the native resource window. The PowerShell observer, its C# helper, watchdog polling and output serialization also have their own overhead, which is not included in the child figures
- Warmup, selected/valid sensors, vetoes and discontinuities remain in the native evidence. A sample validity union does not mean all sensors were valid together. The per-sensor and all-selected steady counts answer different questions

Use separately authorized, matched app-off/app-on WPR/WPA traces and longer repeated GUI/service tests to establish full-system cost and production acceptance. Record Windows build, hardware, GPU/driver, power plan, workload and other running monitor state with those tests. This script does not invent that context or silently collect unrelated personal/system information.

## Validation boundary

The script and its embedded Windows helper must be syntax-checked on the intended PowerShell version and exercised on authorized Windows before relying on their runtime behavior. Linux source checks and portable safe-parser/lifecycle tests can catch interface mistakes; they cannot prove PowerShell parsing, Win32 job/handle/event behavior, provider availability, Ctrl+C behavior, cleanup under process/host failure, or measured Windows performance. No actual Windows execution is performed by the Linux checks.

### Cloud-only checks recorded for this implementation

On 2026-10-03, PowerShell 7.6.6 on Linux parsed the script with zero errors; its embedded C# compiled, including with C# language-version 5, without calling Windows APIs. Generic list and drive-info object constructions also succeeded. Native-structure sizes on Linux x64 were Startup=104, ProcessInfo=24, BasicLimits=64, IoCounters=48, ExtendedLimits=144 bytes. Sixteen additional source-invariant checks covered safe-only arguments, new-only output, no policy/elevation/production commands or PID reopening, original-handle termination, assign-before-resume and the one-process job limit. These checks do not validate PowerShell 5.1 parsing or any actual Windows runtime behavior.

The Linux synthetic helper harness also passed 31 cases: valid/partial/unavailable resource captures, null preservation, both-window CPU arithmetic, single-sensor selection and unavailable sensors, partial CPU observation windows, a single observation (unknown CPU percent), zero samples/empty steady windows, and malformed identity/options/exit/count/mask/timing/value/resource reports. These tests use invented fixtures to validate summary logic and are not measured sensor/performance results.
