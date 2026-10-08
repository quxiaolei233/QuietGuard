#!/usr/bin/env python3
"""Static source/object contracts for the isolated diagnostic entry.

These are reproducible regression checks, not a C interpreter, a complete call
analysis, or proof of native Windows runtime behavior. Portable runner mocks and
native acceptance remain separate gates. Run from any working directory:
  python3 tests/safe_entry_contract.py
Optionally check undefined imports on separately compiled safe translation units:
  python3 tests/safe_entry_contract.py --nm x86_64-w64-mingw32-nm \
    --object build/sensor-test-win.o --object build/metrics.o --object build/core.o
"""
from __future__ import annotations
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOKEN = re.compile(r'/\*.*?\*/|//[^\n]*|(?:L|u8|u|U)?"(?:\\.|[^"\\])*"|(?:L|u|U)?\'(?:\\.|[^\'\\])*\'', re.S)
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
IGNORED = {'if', 'while', 'for', 'switch', 'sizeof', '_Alignof', 'defined'}
# Persistent configuration is forbidden; pure defaults/validation/state are not.
FORBIDDEN = re.compile(
    r'^(?:qg_shutdown\w*|qg_service_\w*|qg_monitor_mutex|qg_(?:read|write)_config_file|'
    r'qg_user_config_path|qg_is_admin|qg_self_test|login_startup|has_login_startup|'
    r'start_local|pause_monitor|save_settings|service_request|service_refresh|elevate|'
    r'worker_init|worker_main|window_proc|show_main|tray_\w*|'
    r'(?:Initiate|Abort)SystemShutdown\w*|InitiateShutdown\w*|ExitWindows\w*|'
    r'SetSuspendState|SetSystemPowerState|Power\w*|AdjustTokenPrivileges|LookupPrivilegeValue\w*|'
    r'OpenProcessToken|OpenThreadToken|SetThreadToken|Impersonate\w*|'
    r'Reg\w*|CreateService\w*|OpenSCManager\w*|OpenService\w*|StartService\w*|'
    r'ControlService\w*|ChangeServiceConfig\w*|DeleteService|StartServiceCtrlDispatcher\w*|'
    r'RegisterServiceCtrlHandler\w*|SetServiceStatus|'
    r'ShellExecute\w*|WinExec|CreateProcess\w*|OpenProcess|TerminateThread|'
    r'LoadLibrary\w*|GetProcAddress|GetModuleHandle\w*|'
    r'MessageBox\w*|CreateWindow\w*|ShowWindow|FindWindow\w*|SetForegroundWindow|'
    r'RegisterClass\w*|InitCommonControls\w*|Shell_NotifyIcon\w*|PostMessage\w*|SendMessage\w*|'
    r'CopyFile\w*|MoveFile\w*|DeleteFile\w*|CreateDirectory\w*|RemoveDirectory\w*|'
    r'system|_wsystem|popen|_popen|_wpopen)$')
SAFE_QG = {
    'qg_safe_options_valid', 'qg_run_sensor_test', 'qg_defaults',
    'qg_engine_init', 'qg_engine_sample', 'qg_metrics_create',
    'qg_metrics_sample', 'qg_metrics_sample_diagnostic', 'qg_metrics_destroy',
}
checks = 0

def check(condition: bool, description: str) -> None:
    global checks
    checks += 1
    if not condition:
        raise AssertionError(description)

def stripped(source: str, keep_literals: bool = False) -> str:
    def replace(match: re.Match[str]) -> str:
        text = match.group()
        if keep_literals and not text.startswith(('//', '/*')):
            return text
        return ''.join('\n' if c == '\n' else ' ' for c in text)
    return TOKEN.sub(replace, source)

def body(source: str, name: str) -> str:
    """Extract a named C function with balanced braces after stripping literals."""
    match = re.search(r'\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', source)
    check(match is not None, f'{name} function definition must exist')
    assert match is not None
    start = match.end()
    depth = 1
    for pos in range(start, len(source)):
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if not depth:
                return source[start:pos]
    raise AssertionError(f'{name}: unterminated body')

def calls(source: str) -> set[str]:
    return set(CALL.findall(source)) - IGNORED

def entry_contract() -> None:
    main = stripped((ROOT / 'src/main.c').read_text())
    entry = body(main, 'wWinMain')
    parse_pos = entry.find('qg_safe_parse(')
    check(parse_pos >= 0, 'safe parser must run in wWinMain')
    branch = re.search(r'if\s*\(\s*safe\s*!=\s*QG_SAFE_ABSENT\s*\)\s*\{([^{}]*)\}', entry)
    check(branch is not None, 'non-absent safe result must have its own early branch')
    assert branch is not None
    check(branch.start() > parse_pos, 'safe branch must follow parsing')
    branch_text = re.sub(r'\s+', '', branch.group(1))
    check(branch_text == 'LocalFree(argv);returnsafe==QG_SAFE_VALID?qg_run_sensor_test(&safe_options):ERROR_INVALID_PARAMETER;',
          'valid runs only isolated entry; invalid returns error; both free argv and return')
    prefix_calls = calls(entry[:branch.end()])
    allowed = {'setlocale', 'CommandLineToArgvW', 'GetCommandLineW', 'qg_safe_parse', 'LocalFree', 'qg_run_sensor_test'}
    check(prefix_calls <= allowed, f'new call before safe return needs review: {sorted(prefix_calls-allowed)}')
    check(not re.search(r'\bg\s*\.', entry[:branch.end()]), 'no production global access before safe return')
    check(not re.search(r'\b(?:for|while)\s*\(', entry[:branch.start()]), 'no production argument loop before safe dispatch')
    for symbol in ('qg_self_test', 'qg_service_main', 'qg_is_admin', 'CreateMutexW', 'qg_defaults',
                   'qg_user_config_path', 'qg_read_config_file', 'qg_service_installed', 'qg_engine_init',
                   'InitCommonControlsEx', 'SetProcessDpiAwarenessContext', 'CreateWindowExW', 'worker_init'):
        pos = entry.find(symbol + '(')
        check(pos > branch.end(), f'production {symbol} must remain after terminal safe branch')
    check(entry.count('qg_run_sensor_test(') == 1, 'one isolated runner dispatch only')
    focused_disk_launcher_contract()

def focused_disk_launcher_contract() -> None:
    source = (ROOT / 'scripts/Measure-Safe-Sensors.ps1').read_text()
    check("[ValidateSet('All','Matrix','Disk')]" in source,
          'launcher mode selection remains a fixed allowlist')
    check("if ($Mode -eq 'Disk') {\n    $selections=@([pscustomobject]@{ name='disk'; metrics='disk'; mask=4; duration=$DurationSeconds; interval=$IntervalSeconds })\n}" in source,
          'focused Disk mode uses only the fixed disk sensor mask and bounded arguments')


def runner_contract() -> None:
    raw = (ROOT / 'src/sensor_test_win.c').read_text()
    source = stripped(raw)
    entry = body(source, 'qg_run_sensor_test')
    validation = re.search(r'if\s*\(\s*!qg_safe_options_valid\(o\)\s*\)\s*return\s+2\s*;', entry)
    check(validation is not None, 'runner must independently validate options before effects')
    assert validation is not None
    check(calls(entry[:validation.start()]) == set(), 'no function call before independent option validation')
    referenced = calls(source)
    identifiers = set(re.findall(r'\b[A-Za-z_]\w*\b', source))
    rejected = sorted(s for s in identifiers if FORBIDDEN.fullmatch(s))
    check(not rejected, f'forbidden direct runner symbols: {rejected}')
    unknown_qg = {s for s in identifiers if s.startswith('qg_')} - SAFE_QG
    check(not unknown_qg, f'unreviewed runner qg dependency: {sorted(unknown_qg)}')
    for required in ('qg_metrics_create', 'qg_metrics_sample_diagnostic', 'qg_metrics_destroy', 'qg_engine_sample'):
        check(required in referenced, f'runner must reuse actual {required}')
    collector = body(source, 'collect_thread')
    check(source.count('qg_metrics_sample_diagnostic(') == 1 and
          collector.count('qg_metrics_sample_diagnostic(') == 1,
          'only the existing provider owner thread requests diagnostics')
    check(re.search(r'qg_metrics_sample_diagnostic\(metrics,\s*true,\s*&c->record.sample,\s*&c->record.diagnostics\)', collector) is not None,
          'same lifecycle publishes sample and bounded diagnostics together')
    check(re.search(r'qg_engine_sample\(&c->engine,\s*&c->record.sample\)', collector) is not None,
          'only the unchanged production sample feeds the engine')
    wire_header = stripped((ROOT / 'src/qg.h').read_text())
    check('QgDiagnostics' not in wire_header and 'QgMetricDiagnostic' not in wire_header,
          'diagnostics must remain outside production sample/service wire structures')
    for unit in ('src/metrics.c', 'src/core.c', 'src/safe_cli.c'):
        dependency = stripped((ROOT / unit).read_text())
        rejected = sorted(s for s in set(re.findall(r'\b[A-Za-z_]\w*\b', dependency)) if FORBIDDEN.fullmatch(s))
        check(not rejected, f'forbidden direct symbols in safe dependency {unit}: {rejected}')
    parser_source = stripped((ROOT / 'src/safe_cli.c').read_text())
    parser_allowed = {'equal', 'decimal', 'metrics', 'upper', 'device', 'output_path',
                      'qg_safe_options_valid', 'qg_safe_parse', 'wcscmp', 'wcsncmp',
                      'wcschr', 'memcpy', 'memset'}
    parser_calls = calls(parser_source)
    check(parser_calls <= parser_allowed,
          f'parser must remain pure; new call needs review: {sorted(parser_calls-parser_allowed)}')
    # No production synchronization names or external process handle source.
    literals = stripped(raw, keep_literals=True)
    check('Local\\\\QuietGuard.SensorTest.v1' in literals, 'separate diagnostic mutex namespace')
    check('Local\\\\QuietGuard.SensorTest.%lu.Cancel' in literals, 'PID-scoped test cancellation namespace')
    check(not re.search(r'QuietGuard\.(?:UI|Monitor)|Global\\\\QuietGuard', literals), 'production singleton is not referenced')
    check('TerminateThread' not in referenced and 'OpenProcess' not in referenced, 'cannot target production thread/process')
    for match in re.finditer(r'\bTerminateProcess\s*\(([^;]+)\)\s*;', source):
        check(re.match(r'\s*GetCurrentProcess\s*\(\s*\)\s*,', match.group(1)) is not None,
              'forced termination may only use the current-process pseudo-handle')
    check('self_stop' in referenced, 'honest self-only fail-stop path exists')
    check('QG_SAFE_MAX_SAMPLES' in source, 'bounded in-memory sample cap is used')
    check('CREATE_NEW' in source, 'explicit report file must be new')
    for unsafe in ('CREATE_ALWAYS', 'OPEN_ALWAYS', 'TRUNCATE_EXISTING', 'FILE_APPEND_DATA'):
        check(unsafe not in source, f'no existing-file overwrite/append disposition {unsafe}')
    for required in ('GetProcessTimes', 'GetProcessMemoryInfo', 'GetProcessHandleCount',
                     'GetProcessIoCounters', 'CreateToolhelp32Snapshot'):
        check(required in referenced, f'process measurement includes {required}')
    check('#error QG_SENSOR_MOCK is host-test-only' in raw, 'mock seam rejects Windows production build')
    check('forced_cleanup' in raw and 'provider_destroyed' in raw, 'report distinguishes forced and orderly provider cleanup')
    check('not GUI/service performance' in raw and 'not physical disk I/O' in raw, 'report retains scope limitations')

def object_contract(objects: list[str], nm: str) -> None:
    for filename in objects:
        result = subprocess.run([nm, '-u', filename], check=True, text=True, capture_output=True)
        names = []
        for line in result.stdout.splitlines():
            fields = line.split()
            if fields:
                name = re.sub(r'^__imp_', '', fields[-1])
                name = re.sub(r'@\d+$', '', name).lstrip('_')
                names.append(name)
        check(bool(names), f'{filename}: undefined-symbol inventory must not be empty')
        bad = sorted(name for name in names if FORBIDDEN.fullmatch(name))
        check(not bad, f'{filename}: forbidden undefined object symbols: {bad}')
        print(f'  static object: {filename}, {len(names)} undefined symbols checked')

def mutation_tests() -> None:
    """Prove selected guards fail using isolated temporary source copies."""
    global ROOT, checks
    original_root, original_checks = ROOT, checks
    files = ('src/main.c', 'src/sensor_test_win.c', 'src/metrics.c', 'src/core.c', 'src/safe_cli.c', 'src/qg.h')
    changes = (
        ('production call before parse', 'src/main.c', 'QgSafeOptions safe_options;',
         'qg_service_main();QgSafeOptions safe_options;'),
        ('invalid mode falls into production', 'src/main.c', 'if(safe!=QG_SAFE_ABSENT)',
         'if(safe==QG_SAFE_VALID)'),
        ('runner calls power wrapper', 'src/sensor_test_win.c', 'mutex = CreateMutexW',
         'qg_shutdown_request(true); mutex = CreateMutexW'),
        ('runner references power callback', 'src/sensor_test_win.c', 'mutex = CreateMutexW',
         'void *power = qg_shutdown_request; mutex = CreateMutexW'),
        ('runner touches production config', 'src/sensor_test_win.c', 'mutex = CreateMutexW',
         'qg_write_config_file(0,0); mutex = CreateMutexW'),
        ('runner dynamically loads API', 'src/sensor_test_win.c', 'mutex = CreateMutexW',
         'GetProcAddress(0,0); mutex = CreateMutexW'),
        ('provider calls power API', 'src/metrics.c', 'QgMetrics *qg_metrics_create',
         'void bad(void) { ExitWindowsEx(0,0); }\nQgMetrics *qg_metrics_create'),
        ('core modifies registry', 'src/core.c', 'void qg_defaults',
         'void bad(void) { RegSetValueExW(0,0,0,0,0,0); }\nvoid qg_defaults'),
        ('existing file overwrite', 'src/sensor_test_win.c', 'CREATE_NEW', 'CREATE_ALWAYS'),
        ('production mutex namespace', 'src/sensor_test_win.c', r'Local\\QuietGuard.SensorTest.v1',
         r'Local\\QuietGuard.UI.v1'),
        ('external process termination', 'src/sensor_test_win.c',
         'TerminateProcess(GetCurrentProcess(), code)', 'TerminateProcess(other_process, code)'),
        ('options validated too late', 'src/sensor_test_win.c',
         'if (!qg_safe_options_valid(o)) return 2;', ''),
        ('runner omits diagnostic collection', 'src/sensor_test_win.c',
         'qg_metrics_sample_diagnostic(metrics, true, &c->record.sample, &c->record.diagnostics)',
         'qg_metrics_sample(metrics, true, &c->record.sample)'),
        ('diagnostics feed the decision engine', 'src/sensor_test_win.c',
         'qg_engine_sample(&c->engine, &c->record.sample)',
         'qg_engine_sample(&c->engine, (QgSample *)&c->record.diagnostics)'),
        ('diagnostics enter the wire ABI', 'src/qg.h',
         'QgSample sample;', 'QgSample sample; QgDiagnostics diagnostics;'),
    )
    try:
        for label, filename, old, new in changes:
            with tempfile.TemporaryDirectory(prefix='qg-safe-contract-') as directory:
                root = pathlib.Path(directory)
                for filename_to_copy in files:
                    target = root / filename_to_copy
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source = (original_root / filename_to_copy).read_text()
                    if filename_to_copy == filename:
                        check(old in source, f'mutation anchor absent: {label}')
                        source = source.replace(old, new, 1)
                    target.write_text(source)
                ROOT = root
                try:
                    entry_contract()
                    runner_contract()
                except AssertionError:
                    pass
                else:
                    raise AssertionError(f'static contract missed injected defect: {label}')
        print(f'  static mutations: {len(changes)}/{len(changes)} defects detected in temporary copies')
    finally:
        ROOT, checks = original_root, original_checks

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--object', action='append', default=[], help='safe translation-unit object; never the whole production EXE')
    parser.add_argument('--nm', default='nm', help='matching object-format nm tool')
    parser.add_argument('--mutation-test', action='store_true', help='also inject defects into temporary source copies')
    args = parser.parse_args()
    entry_contract()
    runner_contract()
    object_contract(args.object, args.nm)
    if args.mutation_test:
        mutation_tests()
    print(f'safe entry: {checks} static contracts passed; these are NOT native runtime proof')
    return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f'STATIC CONTRACT FAILED: {error}', file=sys.stderr)
        sys.exit(1)
