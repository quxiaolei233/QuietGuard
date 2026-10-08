#!/usr/bin/env python3
"""Linux-only static/managed tests of the Windows measurement launcher.

Starts only the explicitly supplied PowerShell interpreter on this Linux host.
Never runs Measure-Safe-Sensors.ps1 as an entry point or any Windows executable.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_checks(pwsh: Path, launcher: Path, output: Path) -> dict:
    expected_hash = sha256(launcher)
    env = os.environ.copy()
    for variable, directory in (
        ("HOME", "home"), ("XDG_CACHE_HOME", "cache"),
        ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"), ("TMPDIR", "tmp"),
    ):
        value = output / directory
        value.mkdir()
        env[variable] = str(value)
    env.update(POWERSHELL_TELEMETRY_OPTOUT="1", POWERSHELL_UPDATECHECK="Off", NO_COLOR="1", TERM="dumb")
    fixtures = output / "fixtures"
    subprocess.run(
        [sys.executable, str(HERE / "make_fixtures.py"), "--output", str(fixtures)],
        check=True, env=env,
    )
    checks = []
    commands = [
        ("parser-layout-default", "test_parser_layout.ps1", []),
        ("parser-layout-csharp5", "test_parser_layout.ps1", ["-CSharpLanguageVersion", "5"]),
        ("synthetic-helpers", "test_synthetic_helpers.ps1", ["-FixtureDirectory", str(fixtures)]),
    ]
    for label, harness, options in commands:
        # -File is ALWAYS one of our test harnesses, never the target launcher.
        command = [str(pwsh), "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
                   str(HERE / harness), "-LauncherPath", str(launcher), *options]
        completed = subprocess.run(command, env=env, text=True, capture_output=True, timeout=120)
        (output / (label + ".stdout.txt")).write_text(completed.stdout, encoding="utf-8")
        (output / (label + ".stderr.txt")).write_text(completed.stderr, encoding="utf-8")
        if completed.returncode:
            raise RuntimeError(f"{label} failed ({completed.returncode}):\n{completed.stdout}\n{completed.stderr}")
        result = json.loads(completed.stdout)
        if result["launcher_sha256"].lower() != expected_hash or sha256(launcher) != expected_hash:
            raise RuntimeError("Launcher changed during validation; rerun on a stable source file")
        if result.get("failed", 0) or result.get("parse_error_count", 0):
            raise RuntimeError(f"{label} reported failed checks")
        (output / (label + ".json")).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        checks.append({"name": label, "result": result})
    report = {
        "kind": "quietguard.linux-static-validation",
        "synthetic_not_performance_evidence": True,
        "launcher": str(launcher), "launcher_sha256": expected_hash,
        "interpreter": str(pwsh), "platform": sys.platform, "checks": checks,
        "scope": "ParseFile, literal C# compilation, managed structure sizes and three AST-extracted read-only/pure helpers only. No target entry, P/Invoke, Windows executable, shutdown, startup or installation action.",
    }
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pwsh", required=True, type=Path, help="Path to an already trusted official Linux x64 PowerShell interpreter")
    parser.add_argument("--launcher", type=Path, default=ROOT / "scripts/Measure-Safe-Sensors.ps1")
    parser.add_argument("--output-dir", type=Path, help="New directory for JSON results and generated SYNTHETIC fixtures; otherwise use temporary storage")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("This runner is deliberately Linux-only; it is not Windows runtime authorization")
    pwsh = args.pwsh.resolve(strict=True)
    launcher = args.launcher.resolve(strict=True)
    if not pwsh.is_file() or not launcher.is_file():
        parser.error("--pwsh and --launcher must identify existing files")
    try:
        if args.output_dir:
            output = args.output_dir.resolve()
            output.mkdir(parents=True, exist_ok=False)
            report = run_checks(pwsh, launcher, output)
            print(f"Evidence directory: {output}")
        else:
            with tempfile.TemporaryDirectory(prefix="quietguard-synthetic-pwsh-") as temporary:
                report = run_checks(pwsh, launcher, Path(temporary))
        print(f"PASS: source SHA-256 {report['launcher_sha256']}")
        print("PASS: zero parse errors; C# default and language-version-5 compilation; five x64 layouts")
        synthetic = report["checks"][-1]["result"]
        print(f"PASS: {synthetic['passed']}/{synthetic['total']} SYNTHETIC helper cases; no performance evidence")
        print("Windows PowerShell 5.1 and Win32 runtime behavior remain untested")
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
