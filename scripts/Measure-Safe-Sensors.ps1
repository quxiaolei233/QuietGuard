#requires -Version 5.1
<#
.SYNOPSIS
Measures only the provenance-verified, headless QuietGuard --sensor-test child.
.DESCRIPTION
Windows only. No elevation, policy changes, production commands or process-name kills.
The caller must explicitly choose a NEW local output directory. See docs/SAFE-MEASUREMENT.md.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('All','Matrix','Disk')][string]$Mode = 'All',
    [ValidateRange(60,900)][int]$DurationSeconds = 720,
    [ValidateRange(5,120)][int]$IntervalSeconds = 60,
    [ValidateRange(1,30)][int]$SampleTimeoutSeconds = 15
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This script requires Windows. It does not emulate Windows measurements.' }
if ($Mode -eq 'Matrix' -and ($PSBoundParameters.ContainsKey('DurationSeconds') -or $PSBoundParameters.ContainsKey('IntervalSeconds'))) {
    throw 'Matrix uses exactly five 150-second runs at 30-second intervals; do not supply duration or interval.'
}
if ($DurationSeconds -lt 3 * $IntervalSeconds) { throw 'Duration must contain at least three intervals.' }

function Get-LocalPath([string]$Path, [bool]$MustExist) {
    # No UNC/device namespace, alternate data stream, shell quoting, or ambiguous trailing components.
    if ($Path -notmatch '^[A-Za-z]:\\' -or $Path.Substring(2) -match '[:"\x00-\x1f*?<>|]' -or $Path -match '[/]') {
        throw "An explicit absolute local drive path is required: $Path"
    }
    foreach ($part in $Path.Substring(3).Split('\')) {
        if ($part -eq '' -or $part -eq '.' -or $part -eq '..' -or $part -match '[ .]$') { throw "Ambiguous path component: $Path" }
    }
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Length -ge 240) { throw 'Use a shorter path (under 240 characters, including the planned report filename).' }
    $drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($full))
    if (-not $drive.IsReady -or $drive.DriveType -ne [IO.DriveType]::Fixed) { throw 'Only a ready, fixed local drive is supported.' }
    $probe = $full
    while ($probe) {
        if ([IO.File]::Exists($probe) -or [IO.Directory]::Exists($probe)) {
            if (([IO.File]::GetAttributes($probe) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse points are not supported: $probe" }
        } elseif ($MustExist -or $probe -ne $full) { throw "Path does not exist: $probe" }
        $parent = [IO.Path]::GetDirectoryName($probe)
        if ($parent -eq $probe) { break }
        $probe = $parent
    }
    return $full
}

function Write-NewJson([string]$Path, $Value) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 35) + "`r`n")
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
}

# Native calls act exclusively on the process handle returned by this script's CreateProcess.
# The job contains only that child. It is assigned while suspended, before the child can run.
# Closing the observer also closes the job, so an abandoned test cannot remain unbounded.
if ('QuietGuardSafeMeasure.Child' -as [type]) { throw 'A measurement helper is already loaded. Use a fresh PowerShell process for each invocation; an existing helper is never trusted or reused.' }
if (-not ('QuietGuardSafeMeasure.Child' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
namespace QuietGuardSafeMeasure {
 [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] internal struct Startup {
  public uint cb; public string reserved, desktop, title; public uint x,y,xsize,ysize,xchars,ychars,fill,flags;
  public ushort show,reserved2; public IntPtr reservedPtr,stdin,stdout,stderr;
 }
 [StructLayout(LayoutKind.Sequential)] internal struct ProcessInfo { public IntPtr process,thread; public uint pid,tid; }
 [StructLayout(LayoutKind.Sequential)] internal struct BasicLimits {
  public long perProcess,perJob; public uint flags; public UIntPtr minWs,maxWs; public uint active;
  public UIntPtr affinity; public uint priority,scheduling;
 }
 [StructLayout(LayoutKind.Sequential)] internal struct IoCounters { public ulong readOps,writeOps,otherOps,readBytes,writeBytes,otherBytes; }
 [StructLayout(LayoutKind.Sequential)] internal struct ExtendedLimits {
  public BasicLimits basic; public IoCounters io; public UIntPtr processMemory,jobMemory,peakProcessMemory,peakJobMemory;
 }
 internal static class Native {
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] internal static extern bool CreateProcessW(string app,StringBuilder command,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref Startup startup,out ProcessInfo info);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] internal static extern IntPtr CreateJobObjectW(IntPtr attrs,string name);
  [DllImport("kernel32.dll", SetLastError=true)] internal static extern bool SetInformationJobObject(IntPtr job,int type,ref ExtendedLimits value,uint length);
  [DllImport("kernel32.dll", SetLastError=true)] internal static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
  [DllImport("kernel32.dll", SetLastError=true)] internal static extern uint ResumeThread(IntPtr thread);
  [DllImport("kernel32.dll", SetLastError=true)] internal static extern uint WaitForSingleObject(IntPtr handle,uint milliseconds);
  [DllImport("kernel32.dll", SetLastError=true)] internal static extern bool GetExitCodeProcess(IntPtr process,out uint code);
  [DllImport("kernel32.dll", SetLastError=true)] internal static extern bool TerminateProcess(IntPtr process,uint code);
  [DllImport("kernel32.dll", SetLastError=true)] internal static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] internal static extern IntPtr OpenEventW(uint access,bool inherit,string name);
  [DllImport("kernel32.dll", SetLastError=true)] internal static extern bool SetEvent(IntPtr handle);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] internal static extern bool CreateDirectoryW(string path,IntPtr security);
 }
 public sealed class Child : IDisposable {
  private readonly object gate = new object();
  private IntPtr process,job;
  private Thread watch;
  private Stopwatch clock;
  private double deadline,grace,cancelAt;
  private bool disposed,cancelRequested,forced,eventSignaled;
  private string reason;
  private ConsoleCancelEventHandler consoleHandler;
  public uint Pid { get; private set; }
  public string EventName { get { return "Local\\QuietGuard.SensorTest." + Pid.ToString(System.Globalization.CultureInfo.InvariantCulture) + ".Cancel"; } }
  public string CancelReason { get { lock(gate) { return reason; } } }
  public bool ForcedTermination { get { lock(gate) { return forced; } } }
  public bool CancellationSignaled { get { lock(gate) { return eventSignaled; } } }
  public double ElapsedSeconds { get { return clock.Elapsed.TotalSeconds; } }
  public static void CreateNewDirectory(string path) {
   if (!Native.CreateDirectoryW(path,IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot create a NEW output directory");
  }
  private Child() { }
  public static Child Start(string executable,string arguments,int watchdogSeconds,int graceSeconds) {
   Child c = new Child();
   c.deadline=watchdogSeconds; c.grace=graceSeconds;
   c.job=Native.CreateJobObjectW(IntPtr.Zero,null);
   if(c.job==IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(),"CreateJobObject failed");
   ProcessInfo info = new ProcessInfo();
   try {
    ExtendedLimits limits=new ExtendedLimits(); limits.basic.flags=0x00002008; limits.basic.active=1; // KILL_ON_JOB_CLOSE + ACTIVE_PROCESS_LIMIT(1)
    if(!Native.SetInformationJobObject(c.job,9,ref limits,(uint)Marshal.SizeOf(typeof(ExtendedLimits)))) throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot bound test lifetime");
    Startup startup=new Startup(); startup.cb=(uint)Marshal.SizeOf(typeof(Startup));
    StringBuilder command=new StringBuilder("\""+executable+"\" "+arguments);
    if(!Native.CreateProcessW(executable,command,IntPtr.Zero,IntPtr.Zero,false,0x08000004,IntPtr.Zero,System.IO.Path.GetDirectoryName(executable),ref startup,out info)) throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot create safe test child");
    c.process=info.process; c.Pid=info.pid;
    if(!Native.AssignProcessToJobObject(c.job,c.process)) throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot assign test-only cleanup job");
    c.clock=Stopwatch.StartNew();
    c.consoleHandler=delegate(object sender,ConsoleCancelEventArgs e) { e.Cancel=true; c.RequestCancel("console-cancel"); };
    try { Console.CancelKeyPress += c.consoleHandler; } catch (InvalidOperationException) { c.consoleHandler=null; }
    c.watch=new Thread(c.Watch); c.watch.IsBackground=true; c.watch.Name="QuietGuard safe test watchdog";
    if(Native.ResumeThread(info.thread)==0xffffffff) throw new Win32Exception(Marshal.GetLastWin32Error(),"Cannot resume safe test child");
    Native.CloseHandle(info.thread); info.thread=IntPtr.Zero;
    c.watch.Start();
    return c;
   } catch {
    // Original handle only: this cannot target a pre-existing or PID-reused process.
    if(c.process!=IntPtr.Zero) { Native.TerminateProcess(c.process,125); Native.WaitForSingleObject(c.process,5000); Native.CloseHandle(c.process); c.process=IntPtr.Zero; }
    if(info.thread!=IntPtr.Zero) Native.CloseHandle(info.thread);
    if(c.consoleHandler!=null) { try { Console.CancelKeyPress-=c.consoleHandler; } catch { } }
    if(c.job!=IntPtr.Zero) Native.CloseHandle(c.job);
    throw;
   }
  }
  private bool HasExitedLocked() {
   if(disposed) return true;
   uint result=Native.WaitForSingleObject(process,0);
   if(result==0xffffffff) throw new Win32Exception(Marshal.GetLastWin32Error(),"Waiting for original child handle failed");
   return result==0;
  }
  public bool HasExited { get { lock(gate) { return HasExitedLocked(); } } }
  public uint ExitCode { get { lock(gate) {
   if(!HasExitedLocked() || disposed) throw new InvalidOperationException("Child has not exited or was disposed");
   uint code; if(!Native.GetExitCodeProcess(process,out code)) throw new Win32Exception(Marshal.GetLastWin32Error()); return code;
  } } }
  public void RequestCancel(string why) { lock(gate) {
   if(disposed || HasExitedLocked() || cancelRequested) return;
   cancelRequested=true; reason=why; cancelAt=clock.Elapsed.TotalSeconds;
  } }
  private void Watch() {
   try {
    while(true) {
     lock(gate) {
      if(disposed || HasExitedLocked()) return;
      double now=clock.Elapsed.TotalSeconds;
      if(!cancelRequested && now>=deadline) { cancelRequested=true; reason="watchdog"; cancelAt=now; }
      if(cancelRequested) {
       if(!eventSignaled) {
        // Open only this still-live child's PID-specific event; never create/spoof it.
        IntPtr ev=Native.OpenEventW(0x0002,false,EventName);
        if(ev!=IntPtr.Zero) { try { eventSignaled=Native.SetEvent(ev); } finally { Native.CloseHandle(ev); } }
       }
       if(now-cancelAt>=grace) {
        forced=true;
        if(!Native.TerminateProcess(process,125)) { reason=reason+";terminate-failed"; Native.CloseHandle(job); job=IntPtr.Zero; }
        return;
       }
      }
     }
     Thread.Sleep(200);
    }
   } catch {
    lock(gate) { if(!disposed) { reason="watchdog-error"; forced=true; if(job!=IntPtr.Zero) { Native.CloseHandle(job); job=IntPtr.Zero; } } }
   }
  }
  public void Dispose() {
   RequestCancel("observer-interrupted");
   // This also runs from the PowerShell finally block when the pipeline is stopped.
   if(watch!=null && watch.IsAlive) watch.Join((int)((grace+2)*1000));
   lock(gate) {
    if(disposed) return;
    if(!HasExitedLocked()) { forced=true; Native.TerminateProcess(process,125); Native.WaitForSingleObject(process,5000); }
    if(job!=IntPtr.Zero) { Native.CloseHandle(job); job=IntPtr.Zero; }
    if(process!=IntPtr.Zero) { Native.CloseHandle(process); process=IntPtr.Zero; }
    disposed=true;
   }
   if(consoleHandler!=null) { try { Console.CancelKeyPress-=consoleHandler; } catch { } }
   GC.SuppressFinalize(this);
  }
 }
}
'@
}

function Get-MetricNames([int]$Mask) {
    $names = @('cpu','gpu','disk','network')
    for ($i = 0; $i -lt 4; $i++) { if (($Mask -band (1 -shl $i)) -ne 0) { $names[$i] } }
}

function Get-ResourceSummary($Rows) {
    # The native report owns these readings; null stays null, including on partially failed capture.
    $result = [ordered]@{ observation_count = @($Rows).Count }
    $groups = @{
        cpu_ms='valid_cpu'; private_bytes='valid_memory'; working_set_bytes='valid_memory';
        peak_commit_bytes='valid_memory'; peak_working_set_bytes='valid_memory'; handles='valid_handles'; threads='valid_threads';
        read_operations='valid_io'; write_operations='valid_io'; other_operations='valid_io';
        read_bytes='valid_io'; write_bytes='valid_io'; other_bytes='valid_io'
    }
    foreach ($field in ($groups.Keys | Sort-Object)) {
        $valid = @($Rows | Where-Object { $_.process.($groups[$field]) -eq $true -and $null -ne $_.process.$field })
        if ($valid.Count -eq 0) {
            $result[$field] = [ordered]@{ valid_observations=0; first=$null; last=$null; max=$null; delta=$null; observed_seconds=$null }
        } else {
            $first=$valid[0]; $last=$valid[-1]
            $result[$field] = [ordered]@{
                valid_observations=$valid.Count; first=$first.process.$field; last=$last.process.$field;
                max=($valid | ForEach-Object { $_.process.$field } | Measure-Object -Maximum).Maximum;
                delta=([double]$last.process.$field - [double]$first.process.$field);
                observed_seconds=([double]$last.elapsed_ms - [double]$first.elapsed_ms)/1000.0
            }
        }
    }
    $cpu=$result['cpu_ms']
    $result['cpu_cumulative_seconds_end'] = if ($null -ne $cpu.last) { [double]$cpu.last/1000.0 } else { $null }
    $result['cpu_delta_seconds'] = if ($null -ne $cpu.delta) { [double]$cpu.delta/1000.0 } else { $null }
    $result['cpu_percent_one_logical_core'] = if ($null -ne $cpu.delta -and $cpu.delta -ge 0 -and $cpu.observed_seconds -gt 0) { 100.0 * $cpu.delta/1000.0/$cpu.observed_seconds } else { $null }
    return $result
}

function Read-RunSummary([string]$Path, $Selection, [uint32]$ExpectedPid, [int]$ExpectedExit, $Provenance) {
    if (-not [IO.File]::Exists($Path)) { throw 'Native report is missing; this run is incomplete.' }
    $file=Get-Item -LiteralPath $Path
    if ($file.Length -eq 0 -or $file.Length -gt 4MB) { throw 'Native report is empty or exceeds its bounded schema size.' }
    $report=Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    if ($report.schema_version -ne 1 -or $report.kind -cne 'quietguard.sensor-test') { throw 'Unexpected native report schema or kind.' }
    if ($report.pid -ne $ExpectedPid -or $report.cancellation_event -cne "Local\QuietGuard.SensorTest.$ExpectedPid.Cancel") { throw 'Native report child identity mismatch.' }
    if ($report.build.commit -cne $Provenance.commit -or $report.build.source -cne $Provenance.source_tree_sha256) { throw 'Native report build identity differs from the verified executable manifest.' }
    if ($report.options.mask -ne $Selection.mask -or $report.options.duration_seconds -ne $Selection.duration -or $report.options.interval_seconds -ne $Selection.interval -or $report.options.timeout_seconds -ne $SampleTimeoutSeconds) { throw 'Native report options differ from the exact requested safe run.' }
    if ($report.outcome.exit_code -ne $ExpectedExit) { throw 'Native report exit code differs from the original child process exit code.' }
    if ($report.timing.warmup_intervals -ne 2 -or $report.timing.warmup_ms -ne 2*$Selection.interval*1000) { throw 'Unexpected native warmup policy.' }
    $samples=@($report.samples)
    if ($samples.Count -gt 181 -or $samples.Count -ne $report.summary.sample_count) { throw 'Unexpected native sample count.' }
    $previousElapsed=-1.0
    $actualValidUnion=0
    foreach ($sample in $samples) {
        if (@($sample.values).Count -ne 4 -or $sample.valid_mask -lt 0 -or (($sample.valid_mask -band (-bnot $Selection.mask)) -ne 0)) { throw 'Invalid native metric vector or validity mask.' }
        if ($sample.warmup -isnot [bool] -or $sample.elapsed_ms -lt $previousElapsed -or $sample.elapsed_ms -gt $report.timing.elapsed_ms -or $sample.warmup -ne ($sample.elapsed_ms -lt $report.timing.warmup_ms)) { throw 'Native sample timing/warmup is inconsistent.' }
        $previousElapsed=[double]$sample.elapsed_ms
        $actualValidUnion=$actualValidUnion -bor [int]$sample.valid_mask
        for ($valueIndex=0; $valueIndex -lt 4; $valueIndex++) {
            $isValid=($sample.valid_mask -band (1 -shl $valueIndex)) -ne 0
            $value=$sample.values[$valueIndex]
            if ($isValid -eq ($null -eq $value)) { throw 'Native validity and null value disagree.' }
            if ($isValid -and ([double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value))) { throw 'Non-finite native sensor value.' }
        }
    }
    if ($actualValidUnion -ne $report.summary.valid_mask_union) { throw 'Native validity union does not match retained samples.' }
    $perSensor=@()
    $names=@('cpu','gpu','disk','network')
    for ($i=0; $i -lt 4; $i++) {
        if (($Selection.mask -band (1 -shl $i)) -eq 0) { continue }
        $bit=1 -shl $i
        $valid=@($samples | Where-Object { ($_.valid_mask -band $bit) -ne 0 -and $null -ne $_.values[$i] })
        $steady=@($valid | Where-Object { $_.warmup -eq $false })
        $perSensor += [ordered]@{
            sensor=$names[$i]; valid_samples=$valid.Count; steady_valid_samples=$steady.Count;
            availability=if ($steady.Count -gt 0) { 'observed-valid-in-steady-state' } else { 'no-valid-steady-state-observation' }
        }
    }
    $allRows=@([pscustomobject]@{ elapsed_ms=0; process=$report.process_start }) +
        @($samples | ForEach-Object { [pscustomobject]@{ elapsed_ms=$_.elapsed_ms; process=$_.process } }) +
        @([pscustomobject]@{ elapsed_ms=$report.timing.elapsed_ms; process=$report.process_end })
    $resourceFields=@{
        valid_cpu=@('cpu_ms'); valid_memory=@('private_bytes','working_set_bytes','peak_commit_bytes','peak_working_set_bytes');
        valid_handles=@('handles'); valid_threads=@('threads');
        valid_io=@('read_operations','write_operations','other_operations','read_bytes','write_bytes','other_bytes')
    }
    foreach ($row in $allRows) {
        foreach ($flag in $resourceFields.Keys) {
            if ($row.process.$flag -isnot [bool]) { throw 'Native resource validity flag is not a boolean.' }
            foreach ($field in $resourceFields[$flag]) {
                $value=$row.process.$field
                if ($row.process.$flag -eq ($null -eq $value)) { throw 'Native resource validity and null value disagree.' }
                if ($row.process.$flag -and ([double]$value -lt 0 -or [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value))) { throw 'Invalid native resource counter.' }
            }
        }
    }
    $steadyRows=@($samples | Where-Object { $_.warmup -eq $false } | ForEach-Object { [pscustomobject]@{ elapsed_ms=$_.elapsed_ms; process=$_.process } })
    if ($report.timing.elapsed_ms -ge $report.timing.warmup_ms) { $steadyRows += [pscustomobject]@{ elapsed_ms=$report.timing.elapsed_ms; process=$report.process_end } }
    $fullyValid=@($samples | Where-Object { $_.warmup -eq $false -and (($_.valid_mask -band $Selection.mask) -eq $Selection.mask) })
    $validAll=@($samples | Where-Object { (($_.valid_mask -band $Selection.mask) -eq $Selection.mask) })
    if ($validAll.Count -ne $report.summary.valid_samples -or $fullyValid.Count -ne $report.summary.steady_valid_samples) { throw 'Native valid-sample counts do not match retained samples.' }
    $probeValidity=[ordered]@{}
    $allProbesValid=$true; $anyProbeValid=$false
    foreach ($probe in @('valid_cpu','valid_memory','valid_handles','valid_threads','valid_io')) {
        $count=@($allRows | Where-Object { $_.process.$probe -eq $true }).Count
        $probeValidity[$probe]=[ordered]@{ valid_observations=$count; total_observations=$allRows.Count }
        if ($count -ne $allRows.Count) { $allProbesValid=$false }
        if ($count -gt 0) { $anyProbeValid=$true }
    }
    return [ordered]@{
        schema_validated=$true; native_outcome=$report.outcome;
        sensor_collection_status=if ($fullyValid.Count -gt 0) { 'steady-selected-sensors-observed' } else { 'no-fully-valid-steady-observation' };
        resource_probe_status=if ($allProbesValid) { 'complete' } elseif ($anyProbeValid) { 'partial' } else { 'unavailable' };
        resource_probe_validity=$probeValidity;
        elapsed_seconds=[double]$report.timing.elapsed_ms/1000.0;
        warmup_intervals=2; warmup_seconds=[double]$report.timing.warmup_ms/1000.0;
        selected_sensors=@(Get-MetricNames $Selection.mask); valid_sensor_union=@(Get-MetricNames ([int]$report.summary.valid_mask_union));
        sample_count=$samples.Count; warmup_sample_count=@($samples | Where-Object { $_.warmup -eq $true }).Count;
        steady_sample_count=@($samples | Where-Object { $_.warmup -eq $false }).Count;
        steady_all_selected_valid_sample_count=$fullyValid.Count; per_sensor=$perSensor;
        all_observed_resources=(Get-ResourceSummary $allRows); steady_observed_resources=(Get-ResourceSummary $steadyRows);
        resource_timing_basis='Approximate lifecycle/sample timestamp windows, not atomic CPU-read timestamps. Native resource valid_timing/capture_started_tick_ms/capture_ended_tick_ms retain capture bounds in the raw report; inspect capture duration when interpreting short-window rates.';
        resource_scope='Diagnostic child only, including its controller and provider calls. Steady resource window starts at the first retained non-warmup observation and includes orderly provider cleanup, if reached. Native final JSON write and process teardown occur after process_end and are excluded.';
        io_scope='Cumulative process I/O includes file, network and device operations. It is not physical disk I/O and does not measure provider/system/kernel work outside this process.'
    }
}

$exe=Get-LocalPath $Executable $true
if (-not [IO.File]::Exists($exe) -or [IO.Path]::GetFileName($exe) -cne 'QuietGuard.exe') { throw 'Select the exact QuietGuard.exe file from the build package.' }
$outDir=Get-LocalPath $OutputDirectory $false
if ([IO.Directory]::Exists($outDir) -or [IO.File]::Exists($outDir)) { throw 'OutputDirectory must be NEW; existing content will never be replaced.' }
$manifestPath=Get-LocalPath ([IO.Path]::Combine([IO.Path]::GetDirectoryName($exe),'PROVENANCE.json')) $true
if (-not [IO.File]::Exists($manifestPath)) { throw 'Sibling PROVENANCE.json is required.' }
$manifestLock=$null; $exeLock=$null; $child=$null; $createdOutput=$false; $completed=$false
$sessionStarted=[DateTime]::UtcNow.ToString('o')
$runs=New-Object 'Collections.Generic.List[object]'
$sessionError=$null
$selections=@([pscustomobject]@{ name='all'; metrics='cpu,gpu,disk,network'; mask=15; duration=$DurationSeconds; interval=$IntervalSeconds })
if ($Mode -eq 'Disk') {
    $selections=@([pscustomobject]@{ name='disk'; metrics='disk'; mask=4; duration=$DurationSeconds; interval=$IntervalSeconds })
}
if ($Mode -eq 'Matrix') {
    $selections=@(
        [pscustomobject]@{ name='cpu'; metrics='cpu'; mask=1; duration=150; interval=30 },
        [pscustomobject]@{ name='gpu'; metrics='gpu'; mask=2; duration=150; interval=30 },
        [pscustomobject]@{ name='disk'; metrics='disk'; mask=4; duration=150; interval=30 },
        [pscustomobject]@{ name='network'; metrics='network'; mask=8; duration=150; interval=30 },
        [pscustomobject]@{ name='all'; metrics='cpu,gpu,disk,network'; mask=15; duration=150; interval=30 }
    )
}
foreach ($selection in $selections) {
    if ([IO.Path]::Combine($outDir,"$($selection.name).native.json").Length -ge 240) { throw 'Output report path must be under 240 characters.' }
}
try {
    # Read sharing allows the loader, but denies binary/manifest modification or replacement during the run.
    $exeLock=[IO.File]::Open($exe,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $manifestLock=[IO.File]::Open($manifestPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    if ($manifestLock.Length -gt 4MB) { throw 'Provenance manifest is unexpectedly large.' }
    $reader=[IO.StreamReader]::new($manifestLock,[Text.Encoding]::UTF8,$true,4096,$true)
    try { $manifestText=$reader.ReadToEnd() } finally { $reader.Dispose() }
    $provenance=$manifestText | ConvertFrom-Json
    if ($provenance.exe_sha256 -notmatch '^[0-9a-fA-F]{64}$' -or $provenance.source_tree_sha256 -notmatch '^[0-9a-fA-F]{64}$' -or $provenance.commit -notmatch '^[0-9a-fA-F]{40}$') { throw 'Malformed provenance identity.' }
    $sha=[Security.Cryptography.SHA256]::Create()
    try { $actualHash=([BitConverter]::ToString($sha.ComputeHash($exeLock))).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
    if ($actualHash -cne ([string]$provenance.exe_sha256).ToLowerInvariant() -or $exeLock.Length -ne [long]$provenance.exe_bytes) { throw 'Executable hash or byte length does not match sibling PROVENANCE.json. Nothing was launched.' }
    [QuietGuardSafeMeasure.Child]::CreateNewDirectory($outDir)
    $createdOutput=$true
    $identity=[ordered]@{ executable=$exe; exe_sha256=$actualHash; exe_bytes=$exeLock.Length; manifest=$manifestPath; commit=$provenance.commit; source_tree_sha256=$provenance.source_tree_sha256; manifest_version=$provenance.version }
    Write-NewJson ([IO.Path]::Combine($outDir,'provenance.json')) $provenance
    Write-NewJson ([IO.Path]::Combine($outDir,'session.json')) ([ordered]@{
        schema_version=1; kind='quietguard.safe-measurement-session'; state='started'; started_utc=$sessionStarted;
        expected_runs=$selections; identity=$identity;
        completion_rule='A started session is incomplete unless final-summary.json confirms all planned runs completed.';
        cancel_instructions='Create an empty file named CANCEL in this directory, or use Ctrl+C. Do not terminate a production process.';
        scope='Safe headless diagnostic only; not an actual GUI or installed-service measurement.'
    })
    foreach ($selection in $selections) {
        if ([IO.File]::Exists([IO.Path]::Combine($outDir,'CANCEL'))) { $sessionError='Cancellation requested before the next run.'; break }
        $rawPath=[IO.Path]::Combine($outDir,"$($selection.name).native.json")
        # Values are SEPARATE arguments; only this fixed safe-mode vocabulary can be launched.
        $arguments='--sensor-test --metrics ' + $selection.metrics + ' --duration-seconds ' + $selection.duration +
            ' --interval-seconds ' + $selection.interval + ' --sample-timeout-seconds ' + $SampleTimeoutSeconds + ' --output "' + $rawPath + '"'
        $entry=[ordered]@{
            selection=$selection.name; options=$selection; timeout_seconds=$SampleTimeoutSeconds; report=$rawPath;
            state='incomplete'; started_utc=[DateTime]::UtcNow.ToString('o'); pid=$null; child_exit_code=$null;
            cancellation_reason=$null; cancellation_signaled=$false; supervisor_forced_termination=$false; error=$null; summary=$null
        }
        $runs.Add($entry)
        try {
            Write-Host ("Running {0}: {1}s, {2}s interval, first {3}s marked warmup" -f $selection.name,$selection.duration,$selection.interval,(2*$selection.interval))
            $child=[QuietGuardSafeMeasure.Child]::Start($exe,$arguments,($selection.duration+$SampleTimeoutSeconds+20),($SampleTimeoutSeconds+5))
            $entry.pid=$child.Pid
            while (-not $child.HasExited) {
                if ([IO.File]::Exists([IO.Path]::Combine($outDir,'CANCEL'))) { $child.RequestCancel('cancel-file') }
                Start-Sleep -Milliseconds 200
            }
            $entry.child_exit_code=[int]$child.ExitCode
            $entry.cancellation_reason=$child.CancelReason
            $entry.cancellation_signaled=$child.CancellationSignaled
            $entry.supervisor_forced_termination=$child.ForcedTermination
            try {
                $entry.summary=Read-RunSummary $rawPath $selection $child.Pid $entry.child_exit_code $provenance
                $native=$entry.summary.native_outcome
                if ($entry.child_exit_code -eq 0 -and $native.reason -ceq 'completed' -and $native.forced_cleanup -eq $false -and $native.provider_destroyed -eq $true -and $entry.summary.steady_all_selected_valid_sample_count -gt 0 -and $entry.summary.elapsed_seconds -ge $selection.duration -and -not $child.CancelReason -and -not $child.ForcedTermination) {
                    $entry.state='completed'
                } elseif ($child.CancelReason -or $native.reason -ceq 'cancelled' -or ($native.PSObject.Properties['initiating_reason'] -and $native.initiating_reason -ceq 'cancelled')) { $entry.state='interrupted' }
                else { $entry.state='incomplete' }
            } catch { $entry.error=$_.Exception.Message; $entry.state=if ($child.CancelReason) { 'interrupted' } else { 'incomplete' } }
        } catch {
            $entry.error=$_.Exception.Message
            $entry.state='incomplete'
            throw
        } finally {
            if ($null -ne $child) {
                if (-not $child.HasExited) { $child.RequestCancel('observer-interrupted'); $entry.state='interrupted' }
                $child.Dispose()
                $entry.cancellation_reason=$child.CancelReason
                $entry.cancellation_signaled=$child.CancellationSignaled
                $entry.supervisor_forced_termination=$child.ForcedTermination
                $child=$null
            }
            $entry.ended_utc=[DateTime]::UtcNow.ToString('o')
            Write-NewJson ([IO.Path]::Combine($outDir,"$($selection.name).summary.json")) $entry
        }
        # Provider unavailable/timeout is preserved; later selections are useful. User cancellation stops the matrix.
        if ($entry.state -eq 'interrupted' -or $entry.cancellation_reason -or $entry.supervisor_forced_termination) { break }
    }
    $completed=($runs.Count -eq $selections.Count -and @($runs | Where-Object { $_.state -ne 'completed' }).Count -eq 0)
} catch {
    $sessionError=$_.Exception.Message
} finally {
    if ($null -ne $child) { $child.Dispose(); $child=$null }
    if ($null -ne $manifestLock) { $manifestLock.Dispose(); $manifestLock=$null }
    if ($null -ne $exeLock) { $exeLock.Dispose(); $exeLock=$null }
    if ($createdOutput) {
        $interrupted=@($runs | Where-Object { $_.state -eq 'interrupted' -or $_.cancellation_reason }).Count -gt 0 -or [IO.File]::Exists([IO.Path]::Combine($outDir,'CANCEL'))
        $final=[ordered]@{
            schema_version=1; kind='quietguard.safe-measurement-summary';
            state=if ($completed) { 'completed' } elseif ($interrupted) { 'interrupted' } else { 'incomplete' };
            started_utc=$sessionStarted; ended_utc=[DateTime]::UtcNow.ToString('o'); mode=$Mode;
            state_scope='Lifecycle and valid selected-sensor collection only; inspect each resource_probe_status independently. Null resource probes remain unknown.';
            planned_run_count=$selections.Count; recorded_run_count=$runs.Count; identity=$identity; error=$sessionError; runs=@($runs.ToArray());
            interpretation=@(
                'Actual selected production metric providers run in a separate safe diagnostic process; no GUI, service, startup, saved configuration, or power action is invoked.',
                'Unavailable or invalid sensor and resource values remain null, not zero. Completion is not a resource-probe completeness or performance acceptance verdict.',
                'This test does not control a separate production instance and cannot prevent that instance from taking its own power action. Production safety must be established separately before testing.',
                'CPU percentage is one-logical-core utilization: 100 * delta CPU seconds / observed wall seconds. It can exceed 100 on concurrent work.',
                'Private and working memory, cumulative peaks, growth, handles, threads and process I/O are reported for observed windows, with warmup separated.',
                'Process I/O is not physical disk traffic. Provider/system/kernel overhead, app-off comparisons, workload slowdown, GUI and installed-service cost need separate authorized Windows WPR/WPA measurements.',
                'The observer and its output writes are separate from the diagnostic child. Child final report write and teardown occur after native process_end and are not included in those resource counters.',
                'Hash matching establishes package consistency, not publisher authenticity; verify the package source independently.'
            )
        }
        Write-NewJson ([IO.Path]::Combine($outDir,'final-summary.json')) $final
        Write-Host ("Result: {0}. Collect this entire directory: {1}" -f $final.state,$outDir)
    }
    if ($null -ne $manifestLock) { $manifestLock.Dispose() }
    if ($null -ne $exeLock) { $exeLock.Dispose() }
}
if ($sessionError) { throw $sessionError }
if (-not $completed) { throw 'Measurement is interrupted or incomplete. Preserve the entire output directory and inspect final-summary.json.' }
