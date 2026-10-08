#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${CC:=x86_64-w64-mingw32-gcc}"
: "${WINDRES:=x86_64-w64-mingw32-windres}"
: "${OBJDUMP:=x86_64-w64-mingw32-objdump}"
mkdir -p build dist
python3 scripts/provenance.py header
"$WINDRES" -I resources resources/app.rc -O coff -o build/resources.o
"$CC" -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong \
  -ffunction-sections -fdata-sections -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -Ibuild -Isrc \
  src/core.c src/safe_cli.c src/sensor_test_win.c src/platform.c src/metrics.c src/service.c src/ui_layout.c src/main.c tests/core_tests.c tests/safe_cli_tests.c build/resources.o \
  -municode -mwindows -s -static -static-libgcc -Wl,--gc-sections,--dynamicbase,--nxcompat,--high-entropy-va,--tsaware,--no-insert-timestamp \
  -lcomctl32 -lshell32 -ladvapi32 -lgdi32 -luser32 -lpdh -liphlpapi -lwtsapi32 -lole32 -lpsapi \
  -o dist/QuietGuard.exe
"$OBJDUMP" -p dist/QuietGuard.exe > build/pe-inspection.txt
python3 scripts/provenance.py manifest
