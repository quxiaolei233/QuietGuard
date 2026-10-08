# Headless sensor-test runtime and evidence

This is a bounded diagnostic harness, not an installed monitor, a GUI activation
mechanism, or a measurement of the GUI/service executable's ordinary overhead.
The cloud implementation and tests below do **not** establish native Windows
runtime compatibility or measured Windows resource results. Native execution
still needs the user's separate authorization.

## Entry and isolation

`main.c` dispatches a valid `--sensor-test` before production UI, persistent
configuration, service, startup or production singleton initialization. The
strict portable parser accepts only diagnostic options. The runner revalidates
the options, never loads or writes application configuration, and uses:

- singleton `Local\QuietGuard.SensorTest.v1`
- manual-reset cancellation event
  `Local\QuietGuard.SensorTest.<decimal child PID>.Cancel`

The named objects have the process's default security descriptor. The launcher
runs under the same user; it opens only the exact event for the child it created.
A pre-existing cancellation object is rejected. The namespace is separate from
production monitoring, so an active or paused production instance can coexist.
Console Ctrl+C/Break/Close, if a console already exists, signals the same event;
the runner never attaches/allocates a console or creates a window. The shared
handler pointer and in-flight handler counter use interlocked access and the
handler is removed before its event is closed.

The provider entry points are `qg_metrics_create`,
`qg_metrics_sample_diagnostic(..., true, ...)` and `qg_metrics_destroy`. The
diagnostic sampler is the same implementation used by `qg_metrics_sample`, whose
normal wrapper passes no diagnostic output. The existing owner thread captures
the sample and a separate fixed-size diagnostic record together. The normal WTS
session-veto work therefore remains included. The shared pure engine is fed each
completed sample, but no engine outcome can invoke a power action. This entry
has no call to the shutdown wrapper, even with a dry-run argument, nor to service,
startup, config-file or GUI APIs.

Diagnostics are outside `QgSample` and the service wire structures. The ordinary
monitor does not allocate raw-counter diagnostic storage, make extra raw-counter
calls, or serialize reports. Raw disk queries occur only when this safe entry
explicitly requests diagnostics. There are no diagnostic background workers,
ambient logs, periodic output or additional shutdown paths.

## Schedule, worker ownership and bounded shutdown

Defaults: 720-second run, 60-second sampling interval, 15-second operation timeout.
Accepted bounds: duration 60–900 seconds; interval 5–120 seconds; duration at least
three intervals; timeout 1–30 seconds. One immediate baseline is followed by
interval slots strictly before the duration deadline. Missed slots are skipped;
there is no overlap or catch-up burst. At most 181 record slots are allocated.
Samples with elapsed time less than two complete intervals are marked `warmup`.

One persistent provider thread owns the actual collector object from create
through destroy. The controller signals one request at a time and reads a
published sample only after the completion event. Each create/sample wait uses
its own timeout and the earlier overall duration deadline. Cancellation interrupts
those waits immediately. Provider creation's timeout starts after the initial
resource probe; that probe does not consume a separate create-time allowance.

On completion, cancellation or error, the controller requests that this worker
stop and call the actual provider destroy routine. It waits up to two seconds
for this orderly cleanup. A stalled create/sample/destroy call retains all state
and handles it could touch. The report says `forced_cleanup`, sets
`forced_cleanup: true`, and preserves the initiating reason. Only this diagnostic
process is terminated with `TerminateProcess(GetCurrentProcess(), ...)` after a
bounded report attempt. No thread is killed independently and no other PID is
opened or terminated. OS process teardown is **not** represented as orderly
provider destruction.

Initial/final resource probes run on separate bounded probe threads with private
buffers. A completed result is copied only after thread exit. A blocked probe is
cancelled where possible and given the same two-second grace. Unfinished probe
buffers are not read or freed; the test explicitly reports forced cleanup and
ends its own process. Initial probes observe the cancellation event. End probes
are attempted after provider cleanup; a provider already requiring forced
cleanup skips further probes, leaving unavailable end values null.

Cancellation APIs cannot promise that every driver will complete a pending call.
See [Microsoft: canceling pending I/O](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations).
Process termination skips ordinary DLL cleanup; see
[Microsoft: terminating a process](https://learn.microsoft.com/en-us/windows/win32/procthread/terminating-a-process).
A process waiting in a pathological kernel/driver operation may still take time
to disappear despite a termination request. The harness never claims otherwise.

## Output and local-path safety

An explicit output must be a new canonical local drive path accepted by the
parser. The runner normalizes all accepted `/` separators to `\` once and uses
that same bounded path for parent validation and file creation. It first rejects
remote drive roots. It then opens and holds every ancestor, including the root,
with `FILE_READ_ATTRIBUTES`, `FILE_SHARE_READ` only, `OPEN_REPARSE_POINT` and
`BACKUP_SEMANTICS`. It rejects non-directories, reparse points and nonlocal resolved
paths. Holding these handles denies writable/deletable ancestor replacement
while `CREATE_NEW` reserves the report file. The final handle is checked again.
No existing output can be overwritten. Conservative rejection can occur if an
ancestor already has an incompatible writable handle; use another local folder.

This entire open/check/zero-byte-write preflight runs in a bounded worker before
any provider is created: five seconds plus two seconds of cancellation grace.
Failure is a nonzero process exit, possibly with an empty reserved file. If no
path is supplied, only an existing writable stdout handle is duplicated; an
invalid/missing stdout is an error. This mode does not invent a console or log
location.

The report is buffered in 181 records and a 1-MiB bounded JSON buffer. The original
512-KiB cap cannot hold a maximum-length run with the additive diagnostics. A
portable bound test serializes a maximally wide record, including the two fully
escaped bounded PDH names and raw counters, and checks that 181 such records plus
8 KiB for root fields fit the fixed cap. Each name has a 96-wide-character storage
limit; malformed surrogates/code points become replacement characters in JSON.
It is serialized once at the end. The single final report operation may require more
than one `WriteFile` to handle short writes; there is no periodic disk log.
Writing is isolated on a worker and bounded to five seconds plus a two-second
cancellation grace. A full pipe or blocked device cannot leave the controller
waiting forever; unresolved output blocks cause self-termination. A write failure
can leave an incomplete file; the process's exit code is authoritative. The JSON
cannot promise that its own eventual write succeeds. There is no durability/fsync
guarantee, and interruption or power loss may leave a partial file.

## JSON schema, version 1

Root fields:

- `schema_version: 1`, `kind: "quietguard.sensor-test"`, `pid`
- `build: {version, commit, source}` from generated build identity
- `options: {mask, duration_seconds, interval_seconds, timeout_seconds}`
- `cancellation_event` with the exact PID event name above
- `outcome: {reason, initiating_reason, exit_code, forced_cleanup,
  provider_destroyed, provider_stage, sample_in_flight_at_stop,
  console_cancel_handler}`
- `timing: {started_tick_ms, ended_tick_ms, elapsed_ms,
  started_utc_filetime, ended_utc_filetime, warmup_intervals: 2, warmup_ms}`
- `summary: {sample_count, valid_samples, steady_valid_samples, valid_mask_union}`
- `process_start`, `process_end`, `samples`, and a human-readable `scope`

Provider stages are 0 idle/unstarted, 1 create, 2 sample/self-probe, 3 destroy,
4 destroyed. `provider_destroyed` means the actual destroy routine returned. It
is false when creation never produced a provider, and can be true for a later
telemetry/output error. `sample_in_flight_at_stop` describes stop initiation; an
in-flight sample can finish during grace but is deliberately not published late.

Each sample has `index`, `elapsed_ms`, `warmup`, `at_ms`, `valid_mask`, `values`,
`veto`, `discontinuity`, `collector_ms`, `phase`, `process` and the additive
`diagnostics` object. Values are exactly
four entries in CPU-percent, GPU-percent, disk-busy-percent, network-KiB/s order.
An unavailable/unselected value is JSON null. `collector_ms` measures the actual
metrics call, including requested diagnostic raw queries and session veto, before
self-resource instrumentation. Phase
is the shared `QgPhase` numeric enum and is observational only.

Validity is the **actual production provider's** result, not a synthetic sensor
reading. Production currently invalidates the entire combined snapshot if any
selected sensor, session enumeration or clock continuity check fails. Thus a
missing GPU can produce `valid_mask: 0` for an all-sensor run. The harness does
not reconstruct partial values from untrusted underlying numbers. Diagnostic
evidence now identifies the independent provider outcomes before that aggregate
clear; it never restores the production `valid_mask` or authorizes idle. A run with no fully valid
selected-mask sample outside warmup exits 9; no unknown is converted to zero.

### Additive provider diagnostics

`diagnostics` belongs to the same completed sample and contains:

- `provider_valid_mask`: per-provider valid bits before aggregate clock/session/
  selected-mask rejection; this is evidence only, not an engine input
- `clock`: `start_ok`, `end_ok`, numeric `status` and `reason_code`,
  `continuous_before`, `continuous_after`, `elapsed_tick_ms`, `elapsed_awake_ms`,
  `collection_tick_ms`, `collection_awake_ms`
- `sessions`: `checked`, `ok`, numeric `status` and `reason_code`; successful
  enumeration can still produce the ordinary `veto: true`
- `metrics`: four entries in CPU, GPU, disk, network order, including disabled
  entries, with `metric`, `stage`, `stage_code`, `api_status`, `counter_status`,
  `item_count`, zero-based `item_index`, `has_formatted`, `formatted_value`,
  `has_uncapped`, `uncapped_value`, `uncapped_api_status`,
  `uncapped_counter_status`, `topology_changed`, `name_truncated`, `instance`,
  `raw_previous` and `raw_current`

Stage names are `disabled`, `ok`, `baseline`, `cpu_groups`, `cpu_query`,
`cpu_counters`, `pdh_open`, `pdh_localize`, `pdh_info_size`, `pdh_info`, `pdh_path`,
`pdh_expand_size`, `pdh_expand`, `pdh_parse`, `pdh_empty`, `limit`, `pdh_duplicate`,
`pdh_add`, `pdh_collect`, `pdh_format`, `pdh_cstatus`, `pdh_range`, `pdh_aggregate`,
`net_query`, `net_topology`, `net_counters`, `net_empty`, `net_range`, `allocation`.
Their numeric codes are the zero-based order above. Unknown future enum values
retain their number and serialize as `unknown`. Numeric Windows/PDH status values
are unsigned 32-bit values. A zero status alone is not proof of success: use the
stage, availability flags and clock/session reason codes together. Unattempted
or non-applicable fields retain zero/false/empty defaults.

Clock reason codes are 0 success, 1 baseline, 2 start API failure, 3 start clock
regression, 4 excessive paired-clock query delay, 5 between-sample sleep/gap,
6 end API failure, 7 end clock regression and 8 within-collection sleep/gap.
Session reason codes are 0 success, 1 enumeration API failure, 2 invalid
enumeration shape, 3 username API failure, 4 invalid username shape and 5 unknown
session state. The timing deltas are zero when no usable comparison was formed;
always interpret them alongside the corresponding success and continuity flags.

For PDH, the recorded instance is a representative selected counter on success
or the first failing counter, not an exhaustive hardware inventory. A truncated
name is explicitly marked. Disk `formatted_value`/`uncapped_value` are the
underlying **% Idle Time**, not the disk-busy value in the production `values`
array. GPU diagnostics describe a concrete engine contribution, not necessarily
the final aggregate. The ordinary formatted query already uses
`PDH_FMT_NOCAP100`; `uncapped_value` records that same result rather than a second
measurement. A finite out-of-range result is retained here even when production
rejects it. The existing rejection policy for a concrete value above 100 or below
0 is unchanged. No diagnostic reading is clamped into validity.

Each raw object contains `present`, `api_status`, `counter_status`,
`counter_type`, signed 64-bit `first`/`second`, unsigned FILETIME `timestamp`, and
`multi_count`. `present` requires both API success and a VALID/NEW counter status.
When absent, counter numbers/timestamp/multicount are null while statuses remain
visible. Raw collection is currently disk-only; CPU/GPU/network raw objects are
absent. A missing previous raw record may mean baseline/rebuild or an earlier raw
failure. Raw-query failure cannot turn an otherwise valid formatted sample into
an invalid one. Formatted numbers are null when absent or non-finite;
`has_formatted`/`has_uncapped` distinguish an absent result from a returned
non-finite result. JSON never contains NaN or Infinity tokens. Consumers that use
IEEE-754 doubles for integers should retain original integer text when comparing
64-bit raw counters; their exact values can exceed that format's integer range.

These fields support the next separately authorized Windows reproduction of
intermittent disk-invalid samples. They do not establish a root cause, prove that
the issue is fixed, or replace native Windows evidence. The existing null
readings, fail-closed aggregate behavior and schema version 1 remain unchanged.

Each process-resource object contains validity flags `valid_cpu`, `valid_memory`,
`valid_handles`, `valid_threads`, `valid_io`, and `valid_timing`. Fields are:

- `cpu_ms`: cumulative process user plus kernel CPU time; not a utilization percent
- `private_bytes`, `working_set_bytes`, `peak_commit_bytes`, `peak_working_set_bytes`
- `handles`, `threads`
- `read_operations`, `write_operations`, `other_operations`
- `read_bytes`, `write_bytes`, `other_bytes`
- `capture_started_tick_ms`, `capture_ended_tick_ms`: the resource probe window

Unavailable groups have null values, with false validity flags. These APIs are
sampled sequentially, not atomically. CPU-rate derivations across samples are
approximate and should account for the recorded probe windows, rather than imply
that CPU and metrics were measured at an identical instant. `peak_commit_bytes`
is `PeakPagefileUsage`; it is **not** a private-bytes peak. A sampled private peak
can be computed as max of valid `private_bytes`, with that limitation explicit.
See [Microsoft: process memory counters](https://learn.microsoft.com/en-us/windows/win32/memory/memory-performance-information).

Initial resources are captured before provider creation; final resources are
captured after orderly provider cleanup and before report serialization/writing.
The process snapshots include harness, thread, session-veto and instrumentation
costs. Process I/O is cumulative process attribution, not a physical-disk or
whole-system/provider trace. Provider-owned or asynchronous system activity may
fall elsewhere. Use the separate WPR/WPA plan for disk/device attribution.
Wall-clock gaps and suspension remain visible in timestamps/discontinuities;
a successful exit does not mean every interval was valid or uninterrupted.

## Exit codes and outcome interpretation

| Code | Meaning |
| ---: | --- |
| 0 | Completed and at least one steady selected-mask-valid sample |
| 2 | Invalid diagnostic arguments/options |
| 3 | Another diagnostic instance holds the separate singleton |
| 4 | Output preflight failed/timed out |
| 5 | Setup/controller/telemetry failure |
| 6 | Provider allocation/create failed |
| 7 | Provider operation timed out but orderly cleanup completed |
| 8 | Cancelled with orderly cleanup |
| 9 | No steady fully selected-mask-valid sample |
| 10 | Forced self-process cleanup required |
| 11 | Final report serialization/write failure or unresolved output block |

Ordinary reasons are `completed`, `cancelled`, `provider_create_failed`,
`provider_timeout`, `controller_error`, `telemetry_error`, `no_valid_samples`, and
`sample_limit` (defensive guard). A forced cleanup always has reason
`forced_cleanup` and keeps its original cause as `initiating_reason`, which can
be `completed` when the duration expired before a sample/destroy operation could
finish. Early setup/output errors can occur before a report is possible. A report
with successful collection but an eventual write failure may contain the earlier
collection code; always also inspect the actual process exit and JSON integrity.

## Reproducible cloud tests

`tests/sensor_mock/test_sensor.c` includes the actual `sensor_test_win.c`, links
the actual `core.c` and strict parser, and substitutes only the Windows API and
provider boundaries. Real host pthreads execute the unmodified controller and
worker procedures. A 100x test clock makes ordinary 60-second cases fast; the
maximum-duration case uses a 1000x clock. These are bounded concurrency/lifecycle
checks, **not** a Windows kernel emulator or native performance test.

Build and run from the repository root after generating build identity:

```sh
python3 scripts/provenance.py header
gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow \
  -O1 -g -pthread -Ibuild -Isrc tests/sensor_mock/test_sensor.c \
  src/core.c src/safe_cli.c -lm -o build/sensor-controller-tests
python3 tests/sensor_mock/run_tests.py
```

The suite covers normal/max-duration runs; exact warmup and null semantics;
partial/all-invalid sensors; unknown resources; separate provider/probe timeouts;
cancellation in idle/sample/probe; create/sample/destroy/probe hangs; timeout with
normal eventual cleanup; duration expiration during a call; blocked output open,
preflight, stdout and final write; unavailable stdout; no-overwrite preservation;
remote/reparse/mixed-separator parent rejection; and singleton isolation.
Additional cases verify that independent provider evidence survives an invalid
aggregate; disk range/raw details; clock and session error reasons; disabled
metrics; non-finite values; Unicode, malformed and nonterminated bounded names;
and maximum-run reports larger than the old cap. The same serializer also checks
the worst-case fixed report bound and fails closed on buffer exhaustion. Normal
return paths assert no owned mock handle leak. Destroy asserts no concurrent
sample. Forced self-termination asserts the process target is exactly self and
never frees an active collector. API-mocked metrics coverage lives separately in
`tests/metrics_mock`; it exercises the real provider implementation.
