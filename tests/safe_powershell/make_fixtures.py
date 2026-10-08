"""Generate wholly synthetic contract-test data; never performance evidence.

Each case is written only to the explicit fixture directory provided by the test runner.
"""
from pathlib import Path
import argparse
import json, copy
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args=parser.parse_args()
out=args.output
out.mkdir(parents=True, exist_ok=False)
fields={'valid_cpu':['cpu_ms'],'valid_memory':['private_bytes','working_set_bytes','peak_commit_bytes','peak_working_set_bytes'],'valid_handles':['handles'],'valid_threads':['threads'],'valid_io':['read_operations','write_operations','other_operations','read_bytes','write_bytes','other_bytes']}
def proc(t):
    return {**{k:True for k in fields}, 'cpu_ms':float(t), 'private_bytes':1000+t, 'working_set_bytes':2000+t, 'peak_commit_bytes':3000+t, 'peak_working_set_bytes':4000+t, 'handles':10,'threads':2, 'read_operations':t,'write_operations':t,'other_operations':t,'read_bytes':t*10,'write_bytes':t*10,'other_bytes':t*10}
base={'_fixture_notice':'SYNTHETIC CONTRACT TEST: invented values; never performance evidence','schema_version':1,'kind':'quietguard.sensor-test','pid':12345,'cancellation_event':r'Local\QuietGuard.SensorTest.12345.Cancel','build':{'version':'0.1','commit':'a'*40,'source':'b'*64},'options':{'mask':15,'duration_seconds':720,'interval_seconds':60,'timeout_seconds':15},'outcome':{'reason':'completed','exit_code':0,'forced_cleanup':False,'provider_destroyed':True},'timing':{'started_tick_ms':1000,'ended_tick_ms':721000,'elapsed_ms':720000,'warmup_intervals':2,'warmup_ms':120000},'summary':{'sample_count':12,'valid_samples':12,'steady_valid_samples':11,'valid_mask_union':15},'process_start':proc(0),'process_end':proc(720),'samples':[{'index':i,'elapsed_ms':t*1000,'warmup':t<120,'at_ms':1000+t*1000,'valid_mask':15,'values':[0.0,1.0,2.0,3.0],'veto':False,'discontinuity':False,'collector_ms':1,'phase':'collected','process':proc(t)} for i,t in enumerate(range(60,721,60))]}
manifest=[]
def save(name, obj, expect='accept', **expected):
    (out/(name+'.json')).write_text(json.dumps(obj))
    manifest.append({'name':name,'expect':expect, **expected})
def mutate(name, fn, **args):
    x=copy.deepcopy(base); fn(x); save(name,x,**args)
def set_group_null(p, group):
    p[group]=False
    for f in fields[group]: p[f]=None
def all_procs(x): return [x['process_start'],*[s['process'] for s in x['samples']],x['process_end']]
save('complete',base,resource='complete',sensor='steady-selected-sensors-observed',cpu_percent=0.1)
mutate('resources_unavailable',lambda x:[set_group_null(p,g) for p in all_procs(x) for g in fields],resource='unavailable',sensor='steady-selected-sensors-observed',cpu_null=True)
mutate('partial_memory',lambda x:set_group_null(x['samples'][1]['process'],'valid_memory'),resource='partial',sensor='steady-selected-sensors-observed',cpu_percent=0.1)
def single(x):
    x['options']['mask']=1; x['summary']['valid_mask_union']=1
    for s in x['samples']: s['valid_mask']=1; s['values']=[0.0,None,None,None]
mutate('only_cpu_selected',single,resource='complete',sensor='steady-selected-sensors-observed',mask=1,cpu_percent=0.1)
def unavailable(x):
    x['summary'].update(valid_samples=0,steady_valid_samples=0,valid_mask_union=0)
    for s in x['samples']:s['valid_mask']=0;s['values']=[None]*4
mutate('sensors_unavailable',unavailable,resource='complete',sensor='no-fully-valid-steady-observation',cpu_percent=0.1)
for name,fn in {
    'pid_mismatch':lambda x:x.update(pid=12346),
    'event_mismatch':lambda x:x.update(cancellation_event='Local.Wrong'),
    'commit_mismatch':lambda x:x['build'].update(commit='c'*40),
    'source_mismatch':lambda x:x['build'].update(source='c'*64),
    'options_mismatch':lambda x:x['options'].update(duration_seconds=721),
    'exit_mismatch':lambda x:x['outcome'].update(exit_code=8),
    'valid_sensor_null':lambda x:x['samples'][0]['values'].__setitem__(0,None),
    'invalid_sensor_numeric':lambda x:x['samples'][0].update(valid_mask=14),
    'sensor_nan':lambda x:x['samples'][0]['values'].__setitem__(0,'NaN'),
    'sensor_infinity':lambda x:x['samples'][0]['values'].__setitem__(0,'Infinity'),
    'mask_negative':lambda x:x['samples'][0].update(valid_mask=-1),
    'mask_unselected':lambda x:x['samples'][0].update(valid_mask=31),
    'union_mismatch':lambda x:x['summary'].update(valid_mask_union=7),
    'count_mismatch':lambda x:x['summary'].update(sample_count=13),
    'valid_count_mismatch':lambda x:x['summary'].update(valid_samples=11),
    'steady_count_mismatch':lambda x:x['summary'].update(steady_valid_samples=10),
    'nonmonotonic_timing':lambda x:x['samples'][1].update(elapsed_ms=0),
    'wrong_warmup':lambda x:x['samples'][0].update(warmup=False),
    'valid_resource_null':lambda x:x['process_start'].update(cpu_ms=None),
    'invalid_resource_numeric':lambda x:x['process_start'].update(valid_cpu=False),
    'resource_flag_string':lambda x:x['process_start'].update(valid_cpu='true'),
    'resource_negative':lambda x:x['process_start'].update(cpu_ms=-1),
    'resource_missing_property':lambda x:x['process_start'].pop('cpu_ms'),
}.items(): mutate(name,fn,expect='reject')


# Partial and empty windows must not invent resource readings or observation duration.
def cpu_null(pr):
    pr.update(valid_cpu=False, cpu_ms=None)
x=copy.deepcopy(base)
for pr in (x['process_start'], x['process_end'], x['samples'][0]['process'], x['samples'][-1]['process']):
    cpu_null(pr)
save('partial_cpu_window',x,resource='partial',sensor='steady-selected-sensors-observed',cpu_percent=0.1,cpu_count=10,cpu_observed_seconds=540)
x=copy.deepcopy(base)
for pr in all_procs(x):
    cpu_null(pr)
x['samples'][1]['process'].update(valid_cpu=True,cpu_ms=120.0)
save('single_cpu_observation',x,resource='partial',sensor='steady-selected-sensors-observed',cpu_count=1,cpu_observed_seconds=0,cpu_percentage_null=True)
x=copy.deepcopy(base)
x['samples']=[]
x['timing'].update(ended_tick_ms=1000,elapsed_ms=0)
x['process_end']=copy.deepcopy(x['process_start'])
x['summary'].update(sample_count=0,valid_samples=0,steady_valid_samples=0,valid_mask_union=0)
save('zero_samples',x,resource='complete',sensor='no-fully-valid-steady-observation',cpu_percentage_null=True,steady_observation_count=0)
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Wrote',len(manifest),'SYNTHETIC contract cases; these are not performance evidence')
