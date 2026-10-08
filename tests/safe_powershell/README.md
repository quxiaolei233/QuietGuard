# Linux-only PowerShell launcher contract tests

These are **static checks and synthetic contract tests**, not Windows measurements. Every fixture number, PID, path, build identity and elapsed duration is invented. Never use them as performance, resource-budget or Windows-lifecycle evidence.

## Prerequisites and run

Use Linux x64, Python 3.9+, and an already trusted official Linux x64 PowerShell interpreter. The suite was checked with PowerShell 7.6.6. This directory contains no interpreter or installer and does not download or install one. See [INTERPRETER-PROVENANCE.md](INTERPRETER-PROVENANCE.md) for the exact official archive and its verified hash.

From the repository root, supply your **actual** official interpreter path:

```sh
python3 tests/safe_powershell/run.py \
  --pwsh /absolute/path/to/powershell/pwsh
```

To retain machine-readable evidence, add a **new** output directory under a location you are authorized to write:

```sh
python3 tests/safe_powershell/run.py \
  --pwsh /absolute/path/to/powershell/pwsh \
  --output-dir /tmp/quietguard-pwsh-validation-new
```

The default target is `scripts/Measure-Safe-Sensors.ps1` in this checkout. `--launcher /absolute/path/to/script.ps1` selects another source file **for parsing, never script-entry execution**. The runner checks the source hash before, during and after its three separate interpreter invocations so a changing target fails validation. Do not run concurrently with launcher edits.

The runner creates isolated HOME/XDG/TMPDIR directories within its temporary/output directory and disables PowerShell telemetry and update checking. It does not alter execution policy, install software, change system settings or access a user computer. Generated output/fixtures are outside the source directory. Existing output directories are rejected.

## What runs

1. `test_parser_layout.ps1` calls `Parser.ParseFile`, extracts the one literal embedded `QuietGuardSafeMeasure` C# helper and compiles that literal with `Add-Type`. It inspects managed structure sizes with `Marshal.SizeOf(Type)` and checks ordinary managed object construction
2. A **fresh interpreter process** repeats the first check using C# `/langversion:5`; this catches newer C# syntax incompatible with the intended older compiler language level
3. `test_synthetic_helpers.ps1` AST-extracts only `Get-MetricNames`, `Get-ResourceSummary` and `Read-RunSummary`. Their definitions and read-only/pure bodies are the only target PowerShell code executed. These helpers read locally generated artificial JSON and compute summaries

**Never dot-source or execute the entire launcher in this suite. Never call any `Child` or `Native` helper method. Never invoke a Windows executable or a P/Invoke declaration.** The runner deliberately refuses non-Linux platforms. It starts only the explicitly supplied Linux PowerShell interpreter and the Python fixture generator.

If launcher code changes, review extracted helper bodies before running: those three functions must remain pure computation or local fixture reads. This test is not a sandbox for arbitrary modified source. C# compilation/layout reflection is not permission to invoke the embedded process launcher.

Expected managed x64 layouts are Startup 104, ProcessInfo 24, BasicLimits 64, IoCounters 48 and ExtendedLimits 144 bytes. These match the intended Windows x64 declarations but do not exercise the Windows ABI.

## Synthetic cases

`make_fixtures.py` is the canonical fixture source. It generates 31 individually named JSON cases, each stamped `SYNTHETIC CONTRACT TEST`, plus the expected-result manifest. Keeping the generator rather than dozens of mostly duplicated JSON files makes changes reviewable.

Eight accepted cases cover complete/partial/unavailable resource probes; CPU-only selection with null unselected sensors; unavailable sensors; a partial CPU observation window; a single CPU observation with unknown percentage; and empty samples/steady observations. Twenty-three rejected cases cover child/build/options/exit mismatches, mask/null/non-finite sensor errors, sample-count and warmup/timing errors, and malformed resource flags/fields.

Accepted fixtures indicate only that the summary helper accepts that synthetic schema. They do **not** mean a measurement session completed: full wrapper lifecycle completion is deliberately not executed.

## Evidence and limits

With `--output-dir`, `results.json` records the exact launcher hash, interpreter version, parse results, compilation/layout checks and each synthetic case result. Individual stdout/stderr and JSON results remain available for diagnosis. Generated files are test evidence only; no measured resource results are included.

A passing Linux PowerShell 7 run does not prove Windows PowerShell 5.1 parsing/execution, Windows API availability, process/job/event behavior, cancellation, host-failure cleanup, provider availability, actual GUI/service cost or performance. C# language-version-5 compilation is a language check, not the .NET Framework compiler/runtime. Those require a separately authorized Windows session and the intended runtime.
