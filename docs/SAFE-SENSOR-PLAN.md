# Safe headless sensor measurement plan

This feature is additive to the 0.2 interface. It addresses a verified testability gap: a paused GUI process cannot establish the resource cost of active monitoring, and the available Windows GUI automation could not operate the Start button.

## Safety boundary

- `--sensor-test` is parsed before any production UI/config/service/startup initialization
- Only metrics selection, bounded duration/interval/provider timeout, and an explicit output path are accepted. Any mixed production/service/install/shutdown/self-test/build-info mode is rejected
- The diagnostic runner reuses the actual production metric create/sample/destroy functions and pure decision core. Its call graph contains no power, shutdown privilege, startup, service-control, persistent-config or UI actions, including no call to the dry-run power wrapper
- It uses a separate test namespace. It neither activates nor pauses an existing production instance
- Default duration720 seconds, interval60 seconds; bounds60–900 seconds,5–120 seconds and at least three sampling intervals. Provider timeout1–30 seconds, default15. Samples are bounded to181 records
- Normal completion/cancellation asks the sampling worker to finish and release providers. A provider that never returns cannot be safely destroyed underneath its thread; the diagnostic run must report timeout/forced cleanup honestly and terminate only its own test process after a short grace. Never terminate a production process or pretend forced cleanup was orderly
- No continuous test-file writes. The bounded in-memory report is written once at completion to a newly created explicit local output file, or an existing stdout handle. Ordinary application runs gain no log

## Measurement and reporting

Measure process CPU time, private/working memory, handle/thread counts and cumulative process I/O. Retain actual sample validity, timestamps, veto and discontinuity indicators. Mark warmup separately; never convert unavailable sensors to zero. Output clearly labels the diagnostic process, not GUI or installed-service performance. Process I/O is not a physical-disk/provider-system measurement; the original WPR/WPA requirement remains.

A PowerShell launcher will offer a roughly12-minute all-sensor run and an optional roughly12.5-minute five-run selection matrix. It launches only this exact safe mode, verifies the binary path/hash, never changes execution policy/startup/service settings, and cancels/stops only the child process it created. Windows execution remains subject to a later explicit test authorization.

## Gates before handoff

Strict portable argument tests, malformed/mixed-dangerous option matrix, simulated timeout/cancel/error/cleanup tests, source/link reachability checks, strict Windows cross-build and repeatable hashes, preserved 0.2 UI behavior. The new diagnostic entry itself is not native-validated merely because older `--self-test` ran on Windows.
