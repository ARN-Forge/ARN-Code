[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [Parameter(Mandatory = $true)][string]$ExpectedCommit
)

# Runs the Windows IDE installer through its whole life cycle, silently and
# for the current user only, in a fresh directory:
#   1. install: arn-ide.exe, the bundled arn.exe with its DLLs and both
#      third-party notices files are installed; the bundled arn.exe prints
#      "arn <ExpectedVersion> (<commit>)" with only System32 on PATH; the
#      uninstaller is registered for the current user;
#   2. reinstall over the existing installation: the same files, still working;
#   3. uninstall: the installation directory and the uninstall registration
#      are gone, and a file the user keeps in the IDE's data directory is not
#      touched.
# The WebView2 Runtime is not installed by this test: GitHub's runners have it.

$ErrorActionPreference = "Stop"
$Installer = (Resolve-Path -LiteralPath $Installer).Path
$expectedLine = "arn $ExpectedVersion ($($ExpectedCommit.ToLowerInvariant().Substring(0, 12)))"
$dir = Join-Path ([IO.Path]::GetTempPath()) ("arn-ide-lifecycle-" + [guid]::NewGuid().ToString("N"))
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ARN IDE"
$dataDir = Join-Path $env:APPDATA "com.arnecto.ide"
$userFile = Join-Path $dataDir ("lifecycle-test-" + [guid]::NewGuid().ToString("N") + ".txt")
$failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$Message) { $script:failures.Add($Message); Write-Host "FAIL: $Message" }

function Invoke-Silently([string]$Exe, [string[]]$Arguments) {
    $p = Start-Process -FilePath $Exe -ArgumentList $Arguments -Wait -PassThru
    return $p.ExitCode
}
function Test-Installation([string]$Round) {
    $missing = @("arn-ide.exe", "arn.exe", "THIRD-PARTY-NOTICES.txt", "THIRD-PARTY-NOTICES-IDE.txt", "uninstall.exe",
                 "vcruntime140.dll", "msvcp140.dll") | Where-Object { -not (Test-Path -LiteralPath (Join-Path $dir $_)) }
    if ($missing.Count) { Fail "$Round`: missing $($missing -join ', ') in $dir"; return }
    $version = (Get-Item -LiteralPath (Join-Path $dir "arn-ide.exe")).VersionInfo.ProductVersion
    if ($version -ne $ExpectedVersion) { Fail "$Round`: arn-ide.exe is version '$version', expected $ExpectedVersion" }
    $oldPath = $env:Path
    try {
        $env:Path = Join-Path $env:SystemRoot "System32"
        $line = (& (Join-Path $dir "arn.exe") --version | Out-String).Trim()
    } finally { $env:Path = $oldPath }
    if ($line -cne $expectedLine) { Fail "$Round`: installed arn.exe printed '$line', expected '$expectedLine'" }
    $registered = Get-ItemProperty -LiteralPath $uninstallKey -ErrorAction SilentlyContinue
    if (-not $registered -or $registered.DisplayVersion -ne $ExpectedVersion) { Fail "$Round`: no uninstall registration for version $ExpectedVersion under $uninstallKey" }
    if (-not $script:failures.Count) { Write-Host "ok: $Round`: arn-ide.exe $version, $line, notices and uninstaller present, registered" }
}

if (Test-Path -LiteralPath $uninstallKey) { throw "ARN IDE is already installed for this user ($uninstallKey); run this test on a machine without it." }
try {
    New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
    [IO.File]::WriteAllText($userFile, "user data")

    foreach ($round in "install", "reinstall") {
        $code = Invoke-Silently $Installer @("/S", "/D=$dir")
        if ($code -ne 0) { Fail "$round`: the installer exited with $code"; continue }
        Test-Installation $round
    }

    $uninstaller = Join-Path $dir "uninstall.exe"
    if (Test-Path -LiteralPath $uninstaller) {
        $code = Invoke-Silently $uninstaller @("/S")
        if ($code -ne 0) { Fail "uninstall: the uninstaller exited with $code" }
        # The NSIS uninstaller runs a copy of itself and returns at once.
        $deadline = (Get-Date).AddSeconds(60)
        while ((Test-Path -LiteralPath (Join-Path $dir "arn-ide.exe")) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
        $left = @(Get-ChildItem -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($dir.Length + 1) })
        if ($left.Count) { Fail "uninstall left: $($left -join ', ')" }
        elseif (Test-Path -LiteralPath $uninstallKey) { Fail "uninstall left the registration $uninstallKey" }
        elseif (-not (Test-Path -LiteralPath $userFile)) { Fail "uninstall removed the user's file in $dataDir" }
        else { Write-Host "ok: uninstall removed the installation and its registration and kept the user's data" }
    }
} finally {
    Remove-Item -LiteralPath $userFile -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath (Join-Path $dir "uninstall.exe")) { Start-Process (Join-Path $dir "uninstall.exe") "/S" -Wait -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "$($failures.Count) failure(s)"
if ($failures.Count) { exit 1 }
