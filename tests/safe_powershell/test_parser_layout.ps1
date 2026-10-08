# Read/compile-only launcher validation. Never execute the target script entry or a Win32 method.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$LauncherPath,
    [ValidateSet('Default','5')][string]$CSharpLanguageVersion = 'Default'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([IntPtr]::Size -ne 8) { throw 'Layout expectations in this test require a 64-bit PowerShell process.' }
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($LauncherPath, [ref]$tokens, [ref]$errors)
$result = [ordered]@{
    kind='quietguard.static-powershell-validation'; synthetic_not_performance_evidence=$true;
    powershell_version=$PSVersionTable.PSVersion.ToString();
    launcher_sha256=(Get-FileHash -LiteralPath $LauncherPath -Algorithm SHA256).Hash;
    parse_error_count=@($errors).Count;
    parse_errors=@($errors | ForEach-Object { [ordered]@{message=$_.Message; line=$_.Extent.StartLineNumber; column=$_.Extent.StartColumnNumber} });
    csharp_compiled=$false; csharp_language_version=$CSharpLanguageVersion; structure_sizes=[ordered]@{}
}
if ($errors.Count -ne 0) { $result | ConvertTo-Json -Depth 8; exit 1 }
$literals = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
    $node.StringConstantType -eq [System.Management.Automation.Language.StringConstantType]::SingleQuotedHereString -and
    $node.Value.Contains('namespace QuietGuardSafeMeasure')
}, $true))
if ($literals.Count -ne 1) { throw 'Expected exactly one literal C# helper for compile-only validation.' }
# Compilation is the only use of the extracted helper text. No Child or Native method is called.
if ($CSharpLanguageVersion -eq '5') {
    $types = @(Add-Type -TypeDefinition $literals[0].Value -CompilerOptions '/langversion:5' -PassThru)
} else {
    $types = @(Add-Type -TypeDefinition $literals[0].Value -PassThru)
}
$result.csharp_compiled = $true
$assembly = $types[0].Assembly
$sizeOfType = [Runtime.InteropServices.Marshal].GetMethod('SizeOf', [type[]]@([Type]))
foreach ($item in @(@('Startup',104), @('ProcessInfo',24), @('BasicLimits',64), @('IoCounters',48), @('ExtendedLimits',144))) {
    $type = $assembly.GetType('QuietGuardSafeMeasure.' + $item[0], $true)
    # Marshal.SizeOf(Type) inspects managed layout; it does not invoke a DllImport method.
    $size = $sizeOfType.Invoke($null, @($type))
    $result.structure_sizes[$item[0]] = [ordered]@{actual=$size; expected=$item[1]; matches=($size -eq $item[1])}
    if ($size -ne $item[1]) { throw "Unexpected x64 structure size for $($item[0]): $size" }
}
$list = New-Object 'Collections.Generic.List[object]'
$drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($LauncherPath))
$result.object_construction = [ordered]@{list_type=$list.GetType().FullName; list_count=$list.Count; drive_type=$drive.GetType().FullName}
$result | ConvertTo-Json -Depth 8
