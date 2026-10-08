#!/usr/bin/env python3
"""Execute the actual diagnostic controller against pthread Win32/provider mocks."""
import json, os, pathlib, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
EXE = pathlib.Path(os.environ.get('QG_SENSOR_TEST_EXE', str(ROOT / 'build/sensor-controller-tests')))
CASES = {
 'normal': (0, 'completed'), 'resources_unknown': (0, 'completed'),
 'slow_probe_and_create': (0, 'completed'), 'maximum_run': (0, 'completed'), 'stdout_normal': (0, 'completed'),
 'invalid': (2, None), 'singleton': (3, None), 'output_open_error': (4, None), 'stdout_missing': (4, None), 'existing_output': (4, None),
 'remote_output': (4, None), 'reparse_output': (4, None), 'mixed_reparse_output': (4, None), 'output_open_hang': (4, None), 'preflight_error': (4, None),
 'thread_failure': (5, 'controller_error'), 'probe_recover': (5, 'telemetry_error'), 'cancel_probe': (8, 'cancelled'), 'create_error': (6, 'provider_create_failed'),
 'timeout_recover': (7, 'provider_timeout'), 'cancel_idle': (8, 'cancelled'),
 'cancel_recover': (8, 'cancelled'), 'no_valid': (9, 'no_valid_samples'),
 'partial': (9, 'no_valid_samples'), 'timeout_hang': (10, 'forced_cleanup'),
 'cancel_hang': (10, 'forced_cleanup'), 'create_hang': (10, 'forced_cleanup'),
 'destroy_hang': (10, 'forced_cleanup'), 'duration_deadline': (10, 'forced_cleanup'), 'stdout_hang': (11, None), 'output_write_error': (11, None),
 'output_hang': (11, None), 'preflight_hang': (4, None),
 'initial_probe_hang': (10, 'forced_cleanup'), 'final_probe_hang': (10, 'forced_cleanup'),
 'diagnostic_cpu_only': (0, 'completed'),
 'diagnostic_disk_failure': (9, 'no_valid_samples'),
 'diagnostic_clock_failure': (9, 'no_valid_samples'),
 'diagnostic_session_failure': (9, 'no_valid_samples'),
 'diagnostic_edges': (9, 'no_valid_samples'),
}
def reject_nonfinite(token):
 raise AssertionError(f'Non-JSON numeric token: {token}')

with tempfile.TemporaryDirectory(prefix='qg-sensor-tests-') as directory:
 for case, (exit_code, reason) in CASES.items():
  report_path = pathlib.Path(directory) / f'{case}.json'
  if case == 'existing_output': report_path.write_text('preserve this existing content')
  run = subprocess.run([str(EXE), case], env={**os.environ, 'QG_MOCK_REPORT': str(report_path)}, timeout=4)
  assert run.returncode == exit_code, (case, run.returncode, exit_code)
  if case == 'existing_output':
   assert report_path.read_text() == 'preserve this existing content'
   continue
  if reason is None:
   assert not report_path.exists() or report_path.stat().st_size == 0, case
   continue
  assert report_path.stat().st_size < 1024 * 1024, case
  report = json.loads(report_path.read_text(), parse_constant=reject_nonfinite)
  assert report['schema_version'] == 1 and report['kind'] == 'quietguard.sensor-test'
  assert report['outcome']['reason'] == reason, (case, report['outcome'])
  assert report['outcome']['exit_code'] == exit_code, case
  assert report['outcome']['forced_cleanup'] == (exit_code == 10), case
  assert report['outcome']['provider_destroyed'] == ((exit_code not in (5,6,10) and case != 'cancel_probe') or case == 'final_probe_hang'), case
  assert len(report['samples']) <= 181
  assert report['summary']['sample_count'] == len(report['samples'])
  assert report['cancellation_event'] == r'Local\QuietGuard.SensorTest.4321.Cancel'
  if case in ('normal','resources_unknown'):
   assert report['summary']['steady_valid_samples'] > 0
   assert 59000 <= report['timing']['elapsed_ms'] <= 65000, report['timing']
   assert report['samples'][0]['warmup'] and not report['samples'][-1]['warmup']
  if case == 'maximum_run':
   assert 150 <= len(report['samples']) <= 181
   assert 899000 <= report['timing']['elapsed_ms'] <= 905000
   assert report_path.stat().st_size > 512 * 1024
  if case == 'duration_deadline':
   assert report['outcome']['initiating_reason'] == 'completed'
   assert report['outcome']['sample_in_flight_at_stop']
   assert 61000 <= report['timing']['elapsed_ms'] <= 65000
  for sample in report['samples']:
   assert sample['warmup'] == (sample['elapsed_ms'] < 10000)
   assert sample['veto']
   for index, value in enumerate(sample['values']):
    assert (value is not None) == bool(sample['valid_mask'] & (1 << index))
   diagnostic = sample['diagnostics']
   metrics = diagnostic['metrics']
   assert [m['metric'] for m in metrics] == ['cpu', 'gpu', 'disk', 'network']
   assert len(metrics) == 4
   assert set(diagnostic) == {'provider_valid_mask', 'clock', 'sessions', 'metrics'}
   for metric in metrics:
    assert isinstance(metric['stage'], str) and isinstance(metric['stage_code'], int)
    for key in ('api_status', 'counter_status', 'uncapped_api_status', 'uncapped_counter_status'):
     assert 0 <= metric[key] <= 2**32 - 1
    for name in ('raw_previous', 'raw_current'):
     raw = metric[name]
     for key in ('first', 'second', 'timestamp', 'multi_count'):
      assert (raw[key] is not None) == raw['present']
   if case != 'maximum_run':
    assert diagnostic['clock']['start_ok'] is True
    assert diagnostic['clock']['end_ok'] == (case != 'diagnostic_clock_failure')
    assert diagnostic['clock']['status'] == (5 if case == 'diagnostic_clock_failure' else 0)
    assert diagnostic['clock']['reason_code'] == (6 if case == 'diagnostic_clock_failure' else 0)
    assert diagnostic['sessions'] == {'checked': True, 'ok': case != 'diagnostic_session_failure',
                                     'status': 5 if case == 'diagnostic_session_failure' else 0,
                                     'reason_code': 3 if case == 'diagnostic_session_failure' else 0}
   if case.startswith('diagnostic_') and case != 'diagnostic_cpu_only':
    assert sample['valid_mask'] == 0 and sample['values'] == [None] * 4
    assert diagnostic['provider_valid_mask'] == (11 if case in ('diagnostic_disk_failure', 'diagnostic_edges') else 15)
   if case == 'diagnostic_disk_failure':
    assert [m['stage'] for m in metrics] == ['ok', 'ok', 'pdh_range', 'ok']
    disk = metrics[2]
    assert disk['formatted_value'] == disk['uncapped_value'] == 100.25
    assert disk['item_count'] == 3 and disk['item_index'] == 1 and disk['instance'] == '0 C:'
    assert disk['raw_previous']['first'] == -(2**63)
    assert disk['raw_previous']['second'] == 2**63 - 1
    assert disk['raw_previous']['timestamp'] == 2**64 - 1
   if case == 'diagnostic_cpu_only':
    assert all(m['stage'] == 'disabled' for m in metrics[1:])
    assert sample['values'][1:] == [None, None, None]
   if case == 'diagnostic_edges':
    assert metrics[1]['stage'] == 'unknown' and metrics[1]['stage_code'] == 2**32 - 1
    assert metrics[1]['instance'] == 'A' * 96 and metrics[1]['name_truncated'] is True
    disk = metrics[2]
    assert disk['instance'] == '"\\\n中😀�X�😀�'
    assert disk['has_formatted'] and disk['has_uncapped']
    assert disk['formatted_value'] is None and disk['uncapped_value'] is None
    assert disk['raw_current']['present'] is False
    assert disk['raw_current']['api_status'] == 0xc0000bc6
   if case == 'resources_unknown':
    for flag in ('cpu','memory','handles','threads','io'):
     assert sample['process']['valid_'+flag] is False
    assert sample['process']['cpu_ms'] is None
    assert sample['process']['private_bytes'] is None
    assert sample['process']['threads'] is None
   if case == 'partial': assert sample['values'] == [1.5, None, None, None]
  if case in ('normal','resources_unknown'):
   # At most one completed sample per slot; no catch-up burst or overlap.
   slots=[s['elapsed_ms']//5000 for s in report['samples']]
   assert len(set(slots)) == len(slots), slots
 print(f'PASS: {len(CASES)} actual-runner controller/provider/cleanup/output cases')
