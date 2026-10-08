# 0.3.1: targeted disk failure diagnosis and recovery correction

## What is established

Limited earlier Windows testing observed intermittent aggregate-invalid disk
samples without sufficient evidence to establish the cause. Raw machine reports
are not included here. Version 0.3.1 has not been retested on Windows.

Source review found that one formatted-value/API/CStatus/range rejection forcibly
cleared the collector baseline even after `PdhCollectQueryData` succeeded. That
made the next sample unknown without asking PDH whether its new interval was
valid. 0.3.1 removes this redundant recovery penalty. A rejected interval remains
unknown and resets the engine; the next independently validated interval starts
a new idle period, with **no credit for the rejected interval**. Query creation,
topology changes, failed collection and clock discontinuity still rebaseline.

This is an established source-level recovery correction. It is **not** a claim
that the original native failure was caused by range overshoot or is resolved.

## What deliberately remains strict

Each concrete physical disk must have a successful format API result, valid or
new-data CStatus, and finite uncapped idle percentage in [0,100]. One bad disk
invalidates the aggregate. Missing data, negative/NaN/infinite/over-100 readings
never become zero activity. The UI metric is maximum physical-disk active-time
percentage = max(100 − per-disk % Idle Time), excluding `_Total`. It is neither
bytes/sec nor a guarantee that blocked/queued work is absent.

PDH's documented default caps at100, while `PDH_FMT_NOCAP100` disables that cap.
Blindly removing this flag could hide an arbitrarily large invalid idle reading
as zero busy. We found no primary-source justification for a universal safe
overshoot tolerance, so the shutdown rule is not relaxed. PhysicalDisk idle
metadata identifies a precision timer, not an inverse timer; our code performs
the 100-minus transformation itself.

## Evidence added to the existing safe entry

Each record includes independent provider-valid mask (before aggregate clearing),
per-metric failure stage and API/CStatus, item count/index, bounded instance name,
uncapped/formatted value, and disk current/previous raw numerator/base values,
counter type and timestamp. It also records separate clock continuity/reason and
WTS query success/reason. API output CStatus not written is represented by
UINT32_MAX; auxiliary raw read failure does not authorize or invalidate the
ordinary formatted result. No raw values are accepted as shutdown inputs.

The representative successful disk is the busiest one; on failure it is the
first failing concrete disk in sorted path order. This is bounded evidence,
not an unbounded log of every drive. Previous raw evidence is reset with a new
query/topology and retains identity by the same sorted concrete path. Names are
bounded at95 UTF-16 units; truncation is explicit. The report may contain local
drive letters/labels; review it before sharing outside this task.

Ordinary monitoring still calls the same wrapper with a null diagnostics pointer:
no new raw-counter calls, diagnostic buffers, periodic writes or GUI refreshes.
The safe entry alone allocates bounded raw cache and increases report capacity
to1MiB to hold maximum-duration diagnostic records; this is test overhead, not
ordinary monitoring memory. Provider APIs remain under the existing worker
timeout and bounded self-process cleanup.

## Next authorized Windows check (not performed here)

Use the exact packaged binary and sibling PROVENANCE.json. Confirm any production
monitor independently paused/stopped; this harness does not change that instance.
Use a new local output directory and a fresh PowerShell process. A 120-second
focused run with10-second intervals is enough to repeat the previously failing
sampling pattern while leaving diagnostic evidence. If no anomaly recurs, that
means only this observation window passed; it does not establish the old cause.

```powershell
$root = 'C:\path\to\QuietGuard'
$out = Join-Path $env:LOCALAPPDATA ('Temp\QG-disk-' + [Guid]::NewGuid().ToString('N'))
powershell.exe -NoProfile -File "$root\scripts\Measure-Safe-Sensors.ps1" -Executable "$root\QuietGuard.exe" -OutputDirectory "$out" -Mode Disk -DurationSeconds 120 -IntervalSeconds 10
```

Read disk.native.json, not only exit0 or the summary. For each invalid aggregate,
inspect diagnostics.metrics[2].stage, API/CStatus and raw values, then clock and
sessions. An active-session veto=true can be legitimate while sensors remain
valid; an unknown session query is separately marked. No real shutdown, service
installation or startup modification belongs to this check.

## Primary references

- [PDH formatted values: status, default cap and two collected values](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcountervalue)
- [PDH counter status semantics and retry behavior](https://learn.microsoft.com/en-us/windows/win32/perfctrs/checking-pdh-interface-return-values)
- [Raw counter return value, type and separate CStatus](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetrawcountervalue)
- [PhysicalDisk metadata, including PercentIdleTime CounterType](https://learn.microsoft.com/en-us/previous-versions/aa394308(v=vs.85))
- [Counter formulas, including precision timers](https://learn.microsoft.com/en-us/windows/win32/perfctrs/calculating-counter-values)
- [English wildcard localization procedure](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw)
