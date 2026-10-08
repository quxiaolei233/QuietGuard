# Native metrics provider

`src/metrics.c` implements the opaque `QgMetrics` ABI in `src/qg.h`. The UI worker and LocalService use exactly the same implementation. Calls must be serialized; the provider creates no threads, timers, processes, WMI sessions, logs, network connections, startup entries, or power requests. Destroy the provider before replacing it when the selection changes.

## Sample contract

- Only selected resource providers run. Session enumeration is independently optional.
- `at_ms` is `GetTickCount64()` at the beginning of the sample. Resource samples are close together, not an atomic system snapshot.
- The first sample is a baseline and is invalid. A failed, absent, malformed, non-finite, negative, out-of-range, or incomplete selected provider invalidates the **whole** sample: `valid == 0` and `discontinuity == true`. Values must never be used without checking validity.
- CPU, network, and PDH membership changes require a new baseline. Errors do not turn into zero usage. A stable subsequent measurement is required to resume eligibility.
- Tick time is compared with `QueryUnbiasedInterruptTime`, which excludes sleep/hibernation. A regression, failed clock read, zero interval, or discrepancy exceeding 100 ms resets all baselines. The tolerance accommodates clock granularity and sequential API calls. Suspends shorter than this tolerance are not guaranteed to be detected. The policy engine must additionally reject delayed/stale sampling; the provider cannot infer the caller's intended cadence.
- An active-session veto may coexist with valid resource numbers. An unknown session result sets the veto and invalidates the sample.

Microsoft documents the distinction between biased uptime and awake time in [Interrupt Time](https://learn.microsoft.com/en-us/windows/win32/sysinfo/interrupt-time).

## CPU

CPU busy percentage is `100 * (delta(kernel) + delta(user) - delta(idle)) / (delta(kernel) + delta(user))`. The kernel count already contains idle time. Zero denominators, counter regressions, arithmetic overflow, changing active processor count, and invalid ranges fail closed.

Machines with more than one active processor group are deliberately unsupported for CPU selection: the sample remains unknown. This prevents a partial group total from being mistaken for whole-machine idle. Windows documents both the idle inclusion and processor-group limitation in [GetSystemTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes).

## PDH localization, topology, and cost

GPU and disk use persistent, separate PDH queries. English counter paths are localized using `PdhAddEnglishCounterW` and `PdhGetCounterInfoW`; each sample refreshes the localized wildcard expansion, and concrete paths are added using `PdhAddCounterW`. This follows the explicit wildcard procedure in [PdhAddEnglishCounterW](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw). Passing an English wildcard straight to a formatted-array call is intentionally avoided.

Expanded paths are sorted and compared exactly with the cached membership. Order changes alone do not reset history. Membership changes rebuild that metric's query, take a baseline, and invalidate the sample. No old handles are retained when an instance disappears. Each expanded counter must return both a successful call and `PDH_CSTATUS_VALID_DATA` or `PDH_CSTATUS_NEW_DATA`; numeric values must be finite and between 0 and 100. Formatting uses `PDH_FMT_NOCAP100` so invalid per-instance values cannot silently be capped into a valid range.

Each path-list buffer is capped at 1 MiB; a metric has at most 4096 concrete counter handles. Reusable arrays have hard bounds and are freed at destruction. The normal CPU-only allocation is about 8 KiB. With both PDH providers, worst-case application-owned path buffers total 4 MiB, plus bounded counter/group arrays; PDH's own internal memory is outside those caps. Queries are cached when membership is stable. Refreshing per-instance PDH data is the main optional cost, especially on systems with thousands of GPU instances. Resource use must be measured on the target machine before claiming any particular CPU or memory budget.

The size-query/data-query pattern follows [PdhExpandWildCardPathW](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhexpandwildcardpathw). A membership race that changes the necessary buffer size, exhausted limit, or allocation failure yields unknown rather than partial data. See [PdhGetFormattedCounterValue](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcountervalue) for formatting and counter status behavior.

### GPU

Counter: `\\GPU Engine(*)\\Utilization Percentage`.

For an instance such as `pid_123_luid_0x00000000_0x00001234_phys_0_eng_1_engtype_3D`, grouping uses the adapter LUID, physical-adapter index, and engine index. The process ID is deliberately excluded from the grouping key. Per-process contributions on the same physical engine are added; the busiest resulting engine across adapters is the GPU metric. Different engines are never added together, and the maximum individual process is never used as a substitute for engine load. An aggregate above 100 is conservatively reported as 100. Unrecognized instance names or missing engine instances yield unknown.

The provider builds the grouping map on membership changes; regular aggregation is linear in the number of counters. Engine-type text is descriptive, not an identity key. The observed instance-name grammar is not treated as an eternal Windows ABI: a future incompatible provider format safely blocks eligibility pending an update.

Microsoft explains why the busiest engine is a useful headline value, and why driver support matters, in [GPUs in the Task Manager](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/). This is a resource heuristic using published Windows counters, not proof that a GPU task has finished. WDDM/driver support, counter visibility, short-lived instances between samples, and process churn affect coverage. An adapter that exposes no instances cannot be inferred to be idle; if no GPU instances exist the selected GPU metric stays unknown. This implementation does not independently enumerate every hardware adapter to certify complete driver coverage. Validate GPU readings under the actual workloads and account used to run the app. Frequent process/engine membership churn intentionally prevents a continuous-idle decision.

### Disk

Counter: `\\PhysicalDisk(*)\\% Idle Time`.

For every physical-disk instance except `_Total`, busy time is `100 - idle percentage`. The reported metric is the maximum physical-disk busy percentage, preventing one busy disk from being diluted by other idle disks. `_Total` is neither sampled nor used as a fallback. No remaining disk instance, invalid counters, or topology changes produce unknown. This avoids the different queue-related semantics of `% Disk Time`, which can exceed 100; see Microsoft's [Windows performance-counter training material](https://download.microsoft.com/download/d/2/0/d20bbbf1-2efb-4b9b-901f-cb8a49104b91/configuringwindows7_trainingkit_ch13.pdf).

Disabled counters, storage-driver limitations, and storage topology can prevent useful data. The app does not enable or reconfigure system performance counters. The provider's instance identities are not hardware serial-number identities.

## Network

`GetIfTable2` supplies 64-bit receive/transmit octet counters. The reported rate is the sum of RX + TX deltas over interfaces whose operational status is UP, divided by the actual elapsed sample time and 1024, in **KiB/s**. This is traffic volume, not link utilization percentage.

Loopback and filter interfaces are excluded. Physical-only filtering is intentionally avoided: virtual adapters, tunnels, and Tailscale interfaces can carry relevant work. Traffic passing through a tunnel and its physical interface can be counted twice; that conservative bias delays shutdown instead of hiding activity. Interface identities use `InterfaceLuid`, not row order or mutable interface index. All included interfaces, including down ones, are cached so additions, removals, type/operational/admin/media-state changes, and counter regressions trigger rebaselining. No UP interface is unknown, not zero.

The OS table is freed using `FreeMibTable` on every path. The OS allocates that temporary table before its returned count can be checked; the implementation rejects more than 4096 entries and never retains the OS allocation. App-owned snapshot arrays are bounded and reused. API details: [GetIfTable2](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-getiftable2), [MIB_IF_ROW2](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/ns-netioapi-mib_if_row2), and [FreeMibTable](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-freemibtable).

## Optional signed-in-session protection

When enabled, session enumeration checks local Windows Terminal Services state and queries the username of active, connected, connecting, or shadowing sessions. Any actual user in these states vetoes shutdown. An empty username does not count as a logged-in user. Disconnected, listening, and idle/listener states are allowed. Other states, enumeration failures, username-query failures, unexpected values, and oversized results are unknown and veto shutdown.

A locked session **still reported as connected/active** continues to veto shutdown. Locking the screen is therefore not a reliable way to clear this protection; a session reported as `WTSDisconnected` is allowed, as explicitly intended for disconnected remote sessions. Windows session state is not a measure of recent keyboard input, a remote connection's usefulness, or a workload's completion. This module does not monitor keyboard/mouse input or infer that a Tailscale connection means an interactive session exists. Microsoft documents [WTS session states](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/ne-wtsapi32-wts_connectstate_class) and [WTSQuerySessionInformationW](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsquerysessioninformationw).

Permissions matter: access to other users' session information and performance counters may differ between the interactive account and LocalService. A restricted service account can remain permanently unknown with protection enabled. The provider never elevates, changes permissions, or silently disables protection. Test both execution modes on the target Windows 11 computer; disable a protection only as an explicit user choice after considering its tradeoff.

## Validation status

The implementation cross-compiles and links as Windows x64 C17 with `-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow`; GCC static analysis (`-fanalyzer`) also passes. The checked-in Linux-hosted API-mock harness in `tests/metrics_mock/` includes the actual production source and passes AddressSanitizer and UndefinedBehaviorSanitizer checks for CPU calculations/baselines, processor-group refusal, GPU per-engine summation and membership changes, missing/invalid PDH values, per-disk maximum, NIC ordering/rates/state changes/regressions, WTS vetoes, clock errors, suspend/resume, query reuse, and matched OS query/table release counts. LeakSanitizer cannot run under this environment's tracing wrapper and was disabled. Reproduction commands are in `tests/README-metrics.md`. These mock checks and cross-compilation are not Windows runtime tests. Required target checks include English and non-English counter paths, GPU/disk instance churn, multiple active NICs and a Tailscale tunnel, adapter down/up and counter resets, missing counters, LocalService permissions, locked/connected and disconnected sessions, suspend/resume, and comparison against known busy workloads. No automatic power action is exercised by this provider.
