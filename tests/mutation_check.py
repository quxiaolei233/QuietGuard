#!/usr/bin/env python3
"""Inject deliberate defects into temporary copies to verify test sensitivity.

Never modifies production sources. Requires Python 3 and a native `cc` compiler.
Every listed mutation must compile and be rejected by qg_self_test.
"""
from pathlib import Path
import subprocess, tempfile, json, sys
root=Path(__file__).resolve().parents[1]
source=(root/'src/core.c').read_text()
mutations={
 'threshold_equality_accepted': ('s->value[i]>=e->cfg.threshold[i]', 's->value[i]>e->cfg.threshold[i]'),
 'one_valid_metric_suffices': ('(s->valid&e->cfg.mask)!=e->cfg.mask', '!(s->valid&e->cfg.mask)'),
 'nonfinite_values_accepted': ('!isfinite(s->value[i])||', ''),
 'negative_values_accepted': ('s->value[i]<0||', ''),
 'over100_percent_accepted': ('||(i<3&&s->value[i]>100.0)', ''),
 'session_veto_ignored': ('if(s->veto)', 'if(false)'),
 'clock_discontinuity_ignored': ('if(s->discontinuity ||', 'if(false ||'),
 'duplicate_timestamp_accepted': ('s->at_ms<=e->last_ms', 's->at_ms<e->last_ms'),
 'excessive_gap_ignored': ('s->at_ms-e->last_ms>expected+tolerance', 'false'),
 'idle_interval_one_ms_early': ('(uint64_t)e->cfg.idle_seconds*1000', '((uint64_t)e->cfg.idle_seconds*1000-1)'),
 'warning_one_ms_early': ('(uint64_t)e->cfg.warning_seconds*1000', '((uint64_t)e->cfg.warning_seconds*1000-1)'),
 'epoch_barrier_removed': ('e->epoch==epoch&&', ''),
 'freshness_barrier_widened': ('now-e->last_ms<=2000', 'now-e->last_ms<=2001'),
 'clock_freshness_regression': ('now>=e->last_ms&&', ''),
 'cancel_does_not_pause': ('e->cfg.enabled=0;qg_engine_reset(e,QG_PAUSED);', 'qg_engine_reset(e,QG_UNKNOWN);'),
 'reset_preserves_idle': ('e->have_last=e->have_idle=false;', 'e->have_last=false;'),
 'duplicate_config_accepted': ('k==11||(seen&(1u<<k))', 'k==11'),
 'missing_config_accepted': ('seen!=0x7ffu || ', ''),
 'embedded_nul_accepted': ('||memchr(text,0,length)', ''),
 'integer_decimal_accepted': ('!point && !integer && digit', '!point && digit'),
}
result=[]
with tempfile.TemporaryDirectory(prefix='qg-mutations-') as directory:
 d=Path(directory)
 for name,(old,new) in mutations.items():
  if source.count(old)!=1: raise SystemExit(f'Unexpected match count {name}: {source.count(old)}')
  (d/'core.c').write_text(source.replace(old,new))
  cmd=['cc','-std=c17','-O1','-DQG_TEST_MAIN','-I'+str(root/'src'),str(d/'core.c'),str(root/'tests/core_tests.c'),'-lm','-o',str(d/'test')]
  compiled=subprocess.run(cmd,capture_output=True,text=True)
  if compiled.returncode: raise SystemExit(compiled.stderr)
  run=subprocess.run([str(d/'test')],capture_output=True,text=True,timeout=15)
  result.append({'mutation':name,'detected':run.returncode!=0,'first_failure':run.stderr.splitlines()[0] if run.stderr else ''})
print(json.dumps(result,indent=2))
print(f'Detected {sum(r["detected"] for r in result)}/{len(result)} deliberate defects')
sys.exit(0 if all(r['detected'] for r in result) else 1)
