# Portable decision-core safety tests

`core_tests.c` exposes `int qg_self_test(void)`. It has a `main` only when
`QG_TEST_MAIN` is defined. The same code can be linked into the Windows release
for its `--self-test` mode. It uses no Windows APIs, shutdown calls, timers,
startup registration, network, file writes, or system-setting changes.

## Run locally

From the repository root:

```sh
mkdir -p build
cc -std=c17 -Wall -Wextra -Wpedantic -Werror -DQG_TEST_MAIN -Isrc \
  src/core.c tests/core_tests.c -lm -o build/core-tests
./build/core-tests

cc -std=c17 -g -O1 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -DQG_TEST_MAIN -Isrc \
  src/core.c tests/core_tests.c -lm -o build/core-tests-sanitize
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./build/core-tests-sanitize

python3 tests/mutation_check.py
```

Leak detection is disabled above because the current cloud executor runs under
ptrace, which LeakSanitizer does not support. This is a test-environment
limitation, not a leak-check pass. The core and these tests do not allocate heap
memory. AddressSanitizer and UndefinedBehaviorSanitizer passed with that setting.

## Coverage

- Every nonempty selection of the four metrics: all selected values must be
  valid and strictly below threshold. Exact equality is busy. Unknown and NaN
  values in disabled metrics do not block. Network is not a percentage.
- Missing baseline/provider errors, NaN, infinities, negative readings,
  impossible percentage readings, active-session veto, and discontinuities
- Full idle and warning durations, exact 1 ms boundaries, zero timestamps,
  duplicate/regressing timestamps, sleep-sized gaps, normal scheduling slack,
  increased warning sampling rate, and unsigned-clock rollover safety
- Cancellation before/after the final sample, repeated cancellation, unchanged
  and changed configuration, invalid configuration, terminal requested state,
  stale epochs, and final sample freshness at 1,999/2,000/2,001 ms
- Parser required fields, all omitted/duplicated/unknown keys, all byte-prefix
  truncations, every embedded-NUL position, CRLF/LF, bounded non-NUL-terminated
  spans, oversize input, BOM/comments/whitespace, signs, exponents, fractional
  integers, malformed decimals, numerical bounds and output-buffer boundaries
- 3,000 valid configuration round trips, 256 shuffled-key records, 10,000
  deterministic parser fuzz inputs, and 102,400 model-based timeline events
- 20 deliberately broken core variants must fail the suite. Mutations are
  compiled in temporary directories and production files remain unchanged.

Reproducible PRNG seeds: parser `17b21acd`, temporal model `51afe207`.
The observed native result on GCC 14.2.0 was **1,132,385 checks passed**, with
**20/20 injected defects detected**. Mutation testing exposed a missing test for
clock wrap in the freshness predicate; that test was added and the existing
production predicate passed it. No behavioral changes to `core.c` were needed.

These tests establish properties of the portable parser/decision state machine.
They do not verify real Windows metrics, GUI/worker races, service ACLs/IPC,
installation, suspend notifications, actual idle resource use, or shutdown.
Those require the separate Windows acceptance gates in `docs/PLAN.md`.
