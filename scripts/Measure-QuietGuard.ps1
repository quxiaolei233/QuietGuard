# Read-only observer. Does not start QuietGuard, change startup, or shut down Windows.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$ProcessId,
    [ValidateRange(1,1440)][int]$Minutes = 30,
    [ValidateRange(1,60)][int]$IntervalSeconds = 5,
    [string]$Output = '.\QuietGuard-measurement.csv'
)
$ErrorActionPreference = 'Stop'
if (-not ('QuietGuardObserver.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace QuietGuardObserver {
 [StructLayout(LayoutKind.Sequential)] public struct Io {
  public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes;
 }
 public static class Native {
  [DllImport("kernel32.dll",SetLastError=true)] public static extern bool GetProcessIoCounters(IntPtr h, out Io counters);
 }
}
'@
}
$p = [Diagnostics.Process]::GetProcessById($ProcessId)
$started = $p.StartTime
$watch = [Diagnostics.Stopwatch]::StartNew()
$rows = [Collections.Generic.List[object]]::new()
while ($watch.Elapsed.TotalMinutes -lt $Minutes) {
    $p.Refresh()
    if ($p.HasExited -or $p.StartTime -ne $started) { throw 'Target process exited or was replaced; measurement is incomplete.' }
    $io = New-Object QuietGuardObserver.Io
    if (-not [QuietGuardObserver.Native]::GetProcessIoCounters($p.Handle,[ref]$io)) {
        throw "I/O counters unavailable. Windows error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()). Service observation may require an elevated observer."
    }
    $rows.Add([pscustomobject]@{
        ElapsedSeconds=[math]::Round($watch.Elapsed.TotalSeconds,3)
        CpuSeconds=$p.TotalProcessorTime.TotalSeconds
        PrivateBytes=$p.PrivateMemorySize64
        WorkingSetBytes=$p.WorkingSet64
        PeakWorkingSetBytes=$p.PeakWorkingSet64
        Handles=$p.HandleCount
        Threads=$p.Threads.Count
        ReadOperations=$io.ReadOperations
        WriteOperations=$io.WriteOperations
        OtherOperations=$io.OtherOperations
        ReadBytes=$io.ReadBytes
        WriteBytes=$io.WriteBytes
        OtherBytes=$io.OtherBytes
    })
    Start-Sleep -Seconds $IntervalSeconds
}
$rows | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath $Output
$first=$rows[0]; $last=$rows[$rows.Count-1]
$elapsed=$last.ElapsedSeconds-$first.ElapsedSeconds
if ($elapsed -le 0) { throw 'Insufficient elapsed time.' }
$cpu=$last.CpuSeconds-$first.CpuSeconds
[pscustomobject]@{
    Status='Observed; compare matching app-off and workload runs separately'
    ProcessId=$ProcessId
    DurationSeconds=$elapsed
    CpuSeconds=$cpu
    CpuPercentOneLogicalCore=100*$cpu/$elapsed
    PrivateMiBStart=$first.PrivateBytes/1MB
    PrivateMiBEnd=$last.PrivateBytes/1MB
    PrivateMiBMax=($rows.PrivateBytes|Measure-Object -Maximum).Maximum/1MB
    WorkingSetMiBMax=($rows.WorkingSetBytes|Measure-Object -Maximum).Maximum/1MB
    HandleChange=$last.Handles-$first.Handles
    ThreadChange=$last.Threads-$first.Threads
    ReadBytes=$last.ReadBytes-$first.ReadBytes
    WriteBytes=$last.WriteBytes-$first.WriteBytes
    OtherBytes=$last.OtherBytes-$first.OtherBytes
    Csv=(Resolve-Path -LiteralPath $Output).Path
    Important='I/O includes file/network/device activity; these counters do not establish physical disk or provider/kernel overhead. Use WPR/WPA A/B traces too.'
} | Format-List
