# All inputs are artificial fixtures. These are helper contract tests, never performance measurements.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$LauncherPath,
 [Parameter(Mandatory=$true)][string]$FixtureDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$source=$LauncherPath
$tokens=$null;$errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count) { throw 'Source must parse before testing selected pure/read-only functions' }
foreach($name in @('Get-MetricNames','Get-ResourceSummary','Read-RunSummary')) {
 $functions=@($ast.FindAll({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true))
 if($functions.Count -ne 1) { throw "Expected one $name definition" }
 # Only three manually reviewed pure/read-only functions, never the target entrypoint or Win32 helper.
 . ([scriptblock]::Create($functions[0].Extent.Text))
}
$SampleTimeoutSeconds=15
$provenance=[pscustomobject]@{commit=('a'*40);source_tree_sha256=('b'*64)}
$manifest=Get-Content -Raw (Join-Path $FixtureDirectory 'manifest.json')|ConvertFrom-Json
$results=@();$failed=0
foreach($case in $manifest) {
 $mask=if($case.PSObject.Properties['mask']) {$case.mask} else {15}
 $selection=[pscustomobject]@{mask=$mask;duration=720;interval=60}
 $summary=$null;$message=$null
 try {$summary=Read-RunSummary (Join-Path $FixtureDirectory ($case.name+'.json')) $selection 12345 0 $provenance} catch {$message=$_.Exception.Message}
 $passed=if($case.expect -eq 'reject') {$null -ne $message} else {$null -eq $message}
 if($passed -and $case.expect -eq 'accept') {
  $passed=$summary.resource_probe_status -eq $case.resource -and $summary.sensor_collection_status -eq $case.sensor
  if($case.PSObject.Properties['cpu_percent']) {$passed=$passed -and [Math]::Abs($summary.all_observed_resources.cpu_percent_one_logical_core-$case.cpu_percent) -lt 1e-8 -and [Math]::Abs($summary.steady_observed_resources.cpu_percent_one_logical_core-$case.cpu_percent) -lt 1e-8}
  if($case.PSObject.Properties['cpu_null']) {$passed=$passed -and $null -eq $summary.all_observed_resources.cpu_percent_one_logical_core -and $null -eq $summary.steady_observed_resources.cpu_percent_one_logical_core -and $summary.all_observed_resources.cpu_ms.valid_observations -eq 0}
  if($case.PSObject.Properties['cpu_count']) {$passed=$passed -and $summary.all_observed_resources.cpu_ms.valid_observations -eq $case.cpu_count}
  if($case.PSObject.Properties['cpu_observed_seconds']) {$passed=$passed -and $summary.all_observed_resources.cpu_ms.observed_seconds -eq $case.cpu_observed_seconds}
  if($case.PSObject.Properties['cpu_percentage_null']) {$passed=$passed -and $null -eq $summary.all_observed_resources.cpu_percent_one_logical_core -and $null -eq $summary.steady_observed_resources.cpu_percent_one_logical_core}
  if($case.PSObject.Properties['steady_observation_count']) {$passed=$passed -and $summary.steady_observed_resources.observation_count -eq $case.steady_observation_count -and $null -eq $summary.steady_observed_resources.cpu_ms.last}
  if(-not $passed){$message='Unexpected resource/sensor status or calculated CPU value'}
 }
 if(-not $passed){$failed++}
 $results += [ordered]@{name=$case.name;expected=$case.expect;passed=$passed;message=$message}
}
$output=[ordered]@{kind='quietguard.synthetic-helper-validation'; synthetic_not_performance_evidence=$true; launcher_sha256=(Get-FileHash $source -Algorithm SHA256).Hash;total=$results.Count;passed=$results.Count-$failed;failed=$failed;cases=$results}
$output | ConvertTo-Json -Depth 8
if($failed){exit 1}
