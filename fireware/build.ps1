[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs = @('build'),
    [string]$IdfProfile = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
)
$ErrorActionPreference = 'Stop'
# Query installer-provided paths without executing profile activation/eim select.
if (-not (Test-Path -LiteralPath $IdfProfile)) {
    throw "ESP-IDF environment profile not found: $IdfProfile"
}
$idfEnvironmentLines = & $IdfProfile -e 6>&1
foreach ($idfEnvironmentLine in $idfEnvironmentLines) {
    $idfEnvironmentText = $idfEnvironmentLine.ToString()
    if ($idfEnvironmentText -match '^([A-Z_][A-Z0-9_]*)=(.*)$') {
        $idfEnvironmentName = $Matches[1]
        $idfEnvironmentValue = $Matches[2]
        if ($idfEnvironmentName -eq 'SYSTEM_PATH') { continue }
        if ($idfEnvironmentName -eq 'PATH') { $env:PATH = "$idfEnvironmentValue;$env:PATH" }
        else { Set-Item -Path "Env:$idfEnvironmentName" -Value $idfEnvironmentValue }
    }
}
$env:PYTHONUTF8 = '1'
$env:IDF_CCACHE_ENABLE = '0'
$idfPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
# GCC response/spec files in this installation cannot represent Chinese paths.
# The NTFS short name references the SAME directory; no source copy or drive map.
$idfFileSystem = New-Object -ComObject Scripting.FileSystemObject
$idfProjectPath = $idfFileSystem.GetFolder($PSScriptRoot).ShortPath
if ($idfProjectPath -match '[^\x00-\x7F]') {
    throw 'An ASCII NTFS short path is required by this local toolchain. Use an ASCII project location if 8.3 names are disabled.'
}
Push-Location $idfProjectPath
try {
    & $idfPython (Join-Path $idfProjectPath 'tools\idf_short_path.py') -C $idfProjectPath -B (Join-Path $idfProjectPath 'build') @IdfArgs
    $idfExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $idfExitCode
