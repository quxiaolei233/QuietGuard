#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
flags=(-Isrc -std=c17 -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow)
gcc "${flags[@]}" -O2 -DQG_TEST_MAIN src/core.c tests/core_tests.c -lm -o build/core-tests
./build/core-tests
python3 tests/mutation_check.py
python3 tests/service_gate_model.py
gcc "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DQG_TEST_MAIN src/core.c tests/core_tests.c -lm -o build/core-sanitized
ASAN_OPTIONS=detect_leaks=0 ./build/core-sanitized
gcc -std=c17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer tests/metrics_mock/test_metrics.c -lm -o build/metrics-sanitized
ASAN_OPTIONS=detect_leaks=0 ./build/metrics-sanitized

gcc "${flags[@]}" -O2 src/ui_layout.c tests/ui_layout_tests.c -o build/ui-layout-tests
./build/ui-layout-tests
gcc "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer src/ui_layout.c tests/ui_layout_tests.c -o build/ui-layout-sanitized
ASAN_OPTIONS=detect_leaks=0 ./build/ui-layout-sanitized

gcc "${flags[@]}" -O2 -DQG_SAFE_CLI_TEST_MAIN src/safe_cli.c tests/safe_cli_tests.c -o build/safe-cli-tests
./build/safe-cli-tests
gcc "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DQG_SAFE_CLI_TEST_MAIN src/safe_cli.c tests/safe_cli_tests.c -o build/safe-cli-sanitized
ASAN_OPTIONS=detect_leaks=0 ./build/safe-cli-sanitized
python3 tests/safe_entry_contract.py
python3 tests/safe_entry_contract.py --mutation-test
python3 scripts/provenance.py header
gcc "${flags[@]}" -O1 -g -pthread -Ibuild tests/sensor_mock/test_sensor.c src/core.c src/safe_cli.c -lm -o build/sensor-controller-tests
python3 tests/sensor_mock/run_tests.py
gcc "${flags[@]}" -O1 -g -pthread -Ibuild -fsanitize=address,undefined -fno-omit-frame-pointer tests/sensor_mock/test_sensor.c src/core.c src/safe_cli.c -lm -o build/sensor-controller-sanitized
QG_SENSOR_TEST_EXE="$PWD/build/sensor-controller-sanitized" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 python3 tests/sensor_mock/run_tests.py
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  for unit in safe_cli sensor_test_win metrics core; do
    x86_64-w64-mingw32-gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -Isrc -Ibuild -c "src/$unit.c" -o "build/safe-scan-$unit.o"
  done
  python3 tests/safe_entry_contract.py --nm x86_64-w64-mingw32-nm --object build/safe-scan-safe_cli.o --object build/safe-scan-sensor_test_win.o --object build/safe-scan-metrics.o --object build/safe-scan-core.o
else
  printf '%s\n' 'Windows safe-object import scan NOT RUN: crosscompiler not on PATH'
fi
if [ -n "${QG_PWSH:-}" ]; then
  python3 tests/safe_powershell/run.py --pwsh "$QG_PWSH"
else
  printf '%s\n' 'Linux PowerShell static/synthetic checks NOT RUN: set QG_PWSH to an already trusted interpreter'
fi
