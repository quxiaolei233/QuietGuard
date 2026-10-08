# Native metrics provider tests

The host-side deterministic harness includes `../../src/metrics.c` directly. It never tests a copied implementation. `QG_METRICS_MOCK` selects the semantic API stubs in `metrics_mock/win32_stubs.h`; production Windows builds cannot select this seam. The public sample/configuration ABI comes from the real `src/qg.h`.

Run from the repository root on Linux with GCC:

```sh
mkdir -p build
cc -std=c17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  tests/metrics_mock/test_metrics.c -o build/metrics_mock_tests
./build/metrics_mock_tests
```

AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cc -std=c17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/metrics_mock/test_metrics.c -o build/metrics_mock_sanitized
ASAN_OPTIONS=detect_leaks=0 ./build/metrics_mock_sanitized
```

The cloud execution wrapper uses tracing, which prevents LeakSanitizer from operating. The command above disables only leak detection; address and undefined-behavior checks stay enabled. On a normal compatible host, also run with `ASAN_OPTIONS=detect_leaks=1`. Query open/close and OS-table allocate/free counts are asserted by the harness independently.

Windows x64 compile and static analysis (replace the compiler path with your installed MinGW-w64 compiler):

```sh
x86_64-w64-mingw32-gcc -std=c17 -O2 -Wall -Wextra -Wpedantic \
  -Werror -Wconversion -Wshadow -fanalyzer \
  -c src/metrics.c -o build/metrics.o
```

The normal Windows application links with `-lpdh -liphlpapi -lwtsapi32` for this module. Do not define `QG_METRICS_MOCK` when cross-compiling the application.

## Coverage

- Initial baselines, null/invalid inputs, bounded allocation refusal, finite-value checks, and GPU-key parser overflow
- CPU busy calculation including kernel idle time, counter regression, group/processor-count changes, API failure, and recovery
- No calls into unselected resource or session providers
- GPU process summation on the same physical engine, maximum across different engines, conservative aggregate capping, topology changes, query reuse, failed collection, absent instances, invalid per-item status, NaN, and recovery
- All-or-nothing validity when one selected resource is unavailable
- Physical-disk maximum busy percentage, exclusion of `_Total`, and no aggregate fallback
- RX+TX KiB/s, physical plus virtual interfaces, loopback exclusion, stable LUID ordering, status changes, byte-counter regression, API failure, recovery, and table release
- Active/connected user veto, empty username, disconnected sessions, failed username query, unknown session state, and explicit option disablement
- Awake-time comparison, sleep between samples, and sleep inside a provider call

## Limits

These deterministic mocks test application logic. They do not emulate Windows drivers, PDH localization, actual permissions, WTS races, operating-system ABI layout, performance cost, or actual clock granularity. In particular, host `wchar_t` and mock structures are not Windows ABI replicas. MinGW compilation checks the real Windows headers separately.

Real target validation remains necessary on Windows 11, including non-English counter expansion, LocalService permissions, physical GPU coverage, GPU/disk churn, multiple adapters/Tailscale, remote/locked/disconnected sessions, suspend/resume, and comparison against known busy workloads. See `docs/METRICS.md` for semantics and limitations. These tests perform no shutdown, registration, network connection, or startup action.
