# Optional boot service: implementation and validation

## Modes and operating model

Login startup is the default installation-free mode. Boot service mode is an explicit elevated opt-in; the UI disables its login startup registration and releases its monitoring gate before invoking the installer. Merely opening service settings does not start another monitor. The fixed `Global\QuietGuard.Monitor.v1` kernel object excludes concurrent monitors across processes, accounts and sessions. While a service is installed, the UI acts as a client rather than another sampling engine.

The optional `QuietGuard` service uses `SERVICE_WIN32_OWN_PROCESS`, automatic boot startup, and `NT AUTHORITY\LocalService`, not LocalSystem. The same executable dispatches `--service` through the Service Control Manager. LocalService has no user desktop; there is no session-0 warning window. Service warning/cancel status is available in the settings UI, which need not stay open. Default active-session veto prevents shutdown while a console or RDP session is connected, including a locked connected session; the metrics layer treats session enumeration errors as a veto. Disabling that veto is an explicit configuration choice that reduces protection.

Every process start begins a new idle interval. Enabled monitoring collects one sample per configured interval, or every two seconds (every one second if configured) during the app-owned warning. Unknown, busy, session-vetoed, discontinuous or late data resets progress through the shared core. Power/session events invalidate in-flight samples and reset the interval. A paused monitor waits for a configuration/stop event without sampling. There is no busy-wait monitoring loop.

The sample that reaches the end of the warning is the final fresh sample. The core must permit commit with its current epoch and a sample no older than two seconds. The service rechecks stop, cancellation, configuration and session/power generations, then atomically changes a single shared commit latch from uninhibited to committed. This successful compare-and-exchange is the explicit irreversible submission boundary immediately before the power call. Cancel, configuration, stop, and session/power-reset operations compete on that same latch, so there is no check-then-call cancellation gap. Metrics collection happens outside the state lock so Cancel can pause an in-flight sampling operation. Cancel that wins the latch prevents commit even if its engine-lock update has not yet run. If commit wins, CANCEL and CONFIG return `ERROR_SHUTDOWN_IN_PROGRESS` and never rewrite the state to paused; this remains true for the tiny interval between the atomic boundary and entry into the power API. Success keeps the committed latch for the service lifetime, including dry-run; restart is required for another dry-run cycle. If the power API reports failure, the engine stays faulted/disabled and an administrator may explicitly reconfigure it. Manual service Stop and removal remain allowed after commit, so the monitor can be removed even if Windows or an application later aborts shutdown. Stopping/removing the monitor does not retract an already committed OS request; CANCEL still reports too-late. An OS shutdown notification also stops the service normally.

Non-admin Cancel disables the engine in memory. It cannot enable monitoring, alter thresholds or write persistent settings. This pause lasts until an administrator applies a new configuration or the service restarts. A restarted service reads the saved enabled setting and begins a completely new idle interval; no progress is persisted. A failed shutdown request pauses in `QG_FAULT` without retry or forcing. Success means the OS accepted a request, not that power-off completed.

## Fixed protected installation

Paths come from Windows system-known folders, never request-supplied paths or environment variables:

- `%ProgramFiles%\QuietGuard\QuietGuard.exe`: Administrators and SYSTEM have full access; LocalService and authenticated users have read/execute
- `%ProgramData%\QuietGuard\service.ini`: Administrators and SYSTEM have full access; LocalService has read access

New directories/files have explicit protected DACLs and the Administrators group as owner. Existing targets must be administrator/SYSTEM-owned, non-reparse objects with no untrusted writable ACEs. Files with multiple hardlinks are rejected. Unsafe pre-existing paths are errors, not repaired or recursively deleted. Directory/path security outside those dedicated folders remains the Windows administrator's responsibility.

The installer streams its own running binary into a `CREATE_NEW` temporary file with the final protected ACL, flushes it, then atomically moves it to the fixed executable destination. It deliberately does not use `CopyFile`, which could transiently copy a user-writable source ACL. No arbitrary executable, command or destination is accepted. Config writes similarly use bounded formatting, an explicitly protected new temporary file, flush and atomic replacement. Temp collision attempts and file sizes are bounded.

Installation refuses an existing service registration, creates only the fixed service, starts it and waits up to ten seconds for `RUNNING`. A failed start leaves the service and files available for diagnosis and explicit removal; installation is not falsely reported successful. Removal requests a clean stop, waits up to ten seconds, then deletes only the fixed service registration. Executable and settings files remain for safe manual cleanup/reinstall. It never recursively deletes a directory, force-terminates a service, changes machine shutdown rights or adds recovery commands.

## Local IPC and privilege boundary

Endpoint: `\\.\pipe\QuietGuard.v1`. One fixed-size request and reply plus a one-byte receipt acknowledgment are permitted per connection. The ABI is private to the same release; `QG_WIRE_MAGIC` and exact message sizes are checked. There are no variable-length strings, paths, shells, subprocess requests, memory allocation controlled by a client, or arbitrary commands.

- `STATUS`: reads a snapshot, including fault state
- `CANCEL`: safe in-memory pause, permitted to local authenticated users
- `CONFIG`: requires an impersonated elevated administrator token and validated bounded configuration; the caller's admin token writes the fixed config, then the service reverts before applying it

LocalService itself cannot write the config. Starting a config update pauses the current engine before persistence; a failed update leaves it paused. Both administrator membership and token elevation are checked, so a normal UAC-filtered administrator process cannot configure it. Failed impersonation denies the action; failed reversion stops the service worker instead of continuing service work under the client identity.

The pipe has an explicit protected DACL, `PIPE_REJECT_REMOTE_CLIENTS`, `FILE_FLAG_FIRST_PIPE_INSTANCE`, and a single instance held for the service lifetime. Authenticated users get only read/write-data, read/write-attributes and synchronization rights. In particular they do **not** get `FILE_APPEND_DATA`, which aliases `FILE_CREATE_PIPE_INSTANCE` for pipes. Ordinary `GENERIC_WRITE` would inadvertently grant that right and is not used. The UI verifies the pipe server PID against the currently running SCM service PID before transmitting anything. Pipe-name squatting or a conflicting monitor fails service startup closed; a same-machine denial of service is possible, but it cannot arm another monitor or authorize shutdown.

Connect/read/write operations use overlapped I/O and a stop event. Inactive clients are disconnected after a two-second I/O deadline. The one-byte ACK lets a legitimate client consume its response without any unbounded pipe `FlushFileBuffers` call. Stop cancels pending pipe I/O before joining the worker. Windows completion is awaited before releasing each `OVERLAPPED` object. This relies on the Windows named-pipe cancellation contract; there is no forced-thread termination. A single pipe worker bounds resource usage, so a hostile local client can delay another client's access by its timeout; it cannot increase worker count.

## Shutdown and dry-run seam

Microsoft documents `SE_SHUTDOWN_NAME` among LocalService's initially disabled privileges. The platform function temporarily enables it and checks for privilege assignment failure, requests local shutdown with `InitiateSystemShutdownExW(NULL, ..., 0, FALSE, FALSE, reason)`, and restores the previous privilege state. This is local, non-rebooting and non-forcing. There is no `shutdown.exe`, forced fallback, remote target, or adjustment of user rights.

`--service-dry-run` goes through the same service dispatcher, config, metrics, warning, final sample, and cancellation/epoch barrier. Only the final `qg_shutdown_request(true)` seam differs; it returns without invoking any power API. On an explicitly authorized disposable Windows test host, an elevated `--install-service-dry-run --config <prepared-config-file>` installs the fixed service with that dry-run argument. `--service-dry-run` alone means dispatch as a service, while ordinary `--dry-run` never installs or changes startup state. The dry-run install still changes startup/service state and therefore needs explicit authorization; none was performed during development.

## Verification status and required native gate

Passed here: strict x64 MinGW compilation of `src/service.c` with C17, `-Wall -Wextra -Werror -Wconversion`; `python3 tests/service_gate_model.py` checks all 10 ordered interleavings of cancel-CAS/pause and validate/commit-CAS/power-call, finding no accepted cancellation followed by a power call or false paused state; it also checks all 10 Stop/submission interleavings and permits Stop on either side of commit. The model does not execute Windows atomics or the production service. Source-level review is not proof of Windows ACL, SCM, shutdown or race behavior. This Linux environment did not install/start a service, change startup settings, run a native executable, or request shutdown.

Native Windows 11 testing remains mandatory before claiming release validation:

1. Explicitly authorized dry-run install/remove: verify LocalService identity, quoted fixed image path, auto-start choice, protected owner/DACLs, successful start and clean stop. Repeat with disabled config and service-start failure
2. Standard user: STATUS succeeds; CANCEL immediately pauses; CONFIG fails. UAC-filtered admin CONFIG fails; elevated admin CONFIG succeeds and survives restart. Inspect final file ACL after every update
3. Another account/session: monitor exclusion works. A precreated pipe/gate fails closed. Remote pipe connections fail. Extra pipe instance creation by an ordinary user fails. Verify client rejection of a spoofed endpoint
4. Short, oversized, invalid-magic, unknown-command and invalid-config packets are rejected. Connect-without-writing, partial requests and response-not-read cases time out without leaked threads/handles. Stop the service during each blocked I/O phase
5. During warning and a deliberately slow sample, repeatedly race Cancel, Config, Stop, session changes and suspend/resume against final dry-run commit. Confirm stale samples never commit. Explicitly place Cancel immediately before and after the atomic commit boundary: the former must prevent submission; the latter must return `ERROR_SHUTDOWN_IN_PROGRESS`, never a successful pause. Verify the UI reports reply errors instead of assuming transport success means cancellation success. Default connected-session veto, RDP disconnect/reconnect, locked session and enumeration failures need real-machine coverage
6. Test rejected precreated user-owned directories, file/directory symlinks/junctions, hardlinks, temp-name collisions, write failures and target replacement failures. Verify safe errors and no unexpected deletion
7. Measure CPU/private bytes/handles and sampling wakeups over time. Provider hangs are outside the IPC cancellation guarantee; native testing must establish actual provider behavior. Test native removal timeouts rather than assuming the ten-second installer wait guarantees all Windows providers return
8. A real end-to-end shutdown, privilege adjustment and interaction with unsaved applications requires separate explicit authorization. Dry-run and cross-compilation do not validate power-off

## Primary references

- [LocalService account and privileges](https://learn.microsoft.com/en-us/windows/win32/services/localservice-account)
- [Named pipe security and access rights](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)
- [CreateNamedPipeW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createnamedpipew)
- [GetNamedPipeServerProcessId](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getnamedpipeserverprocessid)
- [CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)
- [InitiateSystemShutdownExW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-initiatesystemshutdownexw)
