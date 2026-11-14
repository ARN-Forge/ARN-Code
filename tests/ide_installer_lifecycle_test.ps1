[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [Parameter(Mandatory = $true)][string]$ExpectedCommit
)

# Checks the Windows IDE installer, then runs it through its whole life
# cycle, silently and for the current user only, in a fresh directory:
#   0. the installer itself (listed with 7-Zip, and its header inflated
#      directly): its data is compressed with NSIS's zlib module (Deflate),
#      not the LZMA module, and its script checks
#      for the Microsoft Visual C++ runtime DLLs and names Microsoft's
#      download, https://aka.ms/vc14/vc_redist.x64.exe;
#   1. install: arn-ide.exe, the bundled arn.exe with its OpenSSL DLLs and
#      both third-party notices files are installed, and no Visual C++
#      runtime file (a prerequisite, installed from Microsoft); the bundled
#      arn.exe prints "arn <ExpectedVersion> (<commit>)" with only System32 on
#      PATH; the uninstaller is registered for the current user;
#   2. reinstall over the existing installation: the same files, still working;
#   3. uninstall: the installation directory and the uninstall registration
#      are gone, and a file the user keeps in the IDE's data directory is not
#      touched.
# The WebView2 Runtime and the Visual C++ Redistributable are not installed
# by this test: GitHub's runners have both. (Without the Visual C++ runtime
# the installer stops before copying anything; GitHub's runners cannot show
# that case, so step 0 checks the installer's script for it instead.)

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
    $failuresBefore = $script:failures.Count
    $missing = @("arn-ide.exe", "arn.exe", "THIRD-PARTY-NOTICES.txt", "THIRD-PARTY-NOTICES-IDE.txt", "uninstall.exe") |
        Where-Object { -not (Test-Path -LiteralPath (Join-Path $dir $_)) }
    if ($missing.Count) { Fail "$Round`: missing $($missing -join ', ') in $dir"; return }
    $runtime = @(Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Name -match '^(vcruntime|msvcp|concrt|vccorlib|vcomp|vcamp|mfcm?)1[0-9]{2}' } | ForEach-Object Name)
    if ($runtime.Count) { Fail "$Round`: the Visual C++ runtime was installed: $($runtime -join ', ')"; return }
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
    if ($script:failures.Count -eq $failuresBefore) { Write-Host "ok: $Round`: arn-ide.exe $version, $line, notices and uninstaller present, no Visual C++ runtime, registered" }
}

if (Test-Path -LiteralPath $uninstallKey) { throw "ARN IDE is already installed for this user ($uninstallKey); run this test on a machine without it." }

# 0. The installer itself.
$sevenZip = (Get-Command 7z -ErrorAction SilentlyContinue).Source
if (-not $sevenZip) { throw "7-Zip (7z) is needed to inspect the installer." }
$listing = (& $sevenZip l -slt $Installer | Out-String)
$methods = @([regex]::Matches($listing, '(?m)^Method = (.+)$') | ForEach-Object { $_.Groups[1].Value.Trim() } | Sort-Object -Unique)
if (@($methods | Where-Object { $_ -match 'LZMA|BZip2|PPMD' }).Count -or -not @($methods | Where-Object { $_ -match '^Deflate' }).Count) { Fail "the installer is not compressed with zlib (Deflate): methods $($methods -join ', ')" }
else { Write-Host "ok: the installer uses NSIS's zlib compression (7-Zip: $($methods -join ', ')), not the LZMA module" }
# The installer's script, read without 7-Zip: after NSIS's first header
# (flags, 0xDEADBEEF, "NullsoftInst", header length, data length) comes one
# stream compressed with the zlib module (raw deflate, solid), which starts
# with the header's length and the header, whose strings are UTF-16. That the
# stream inflates at all also shows the LZMA module is not used.
$bytes = [IO.File]::ReadAllBytes($Installer)
$latin1 = [Text.Encoding]::GetEncoding(28591)
$signature = $latin1.GetString([byte[]](0xEF, 0xBE, 0xAD, 0xDE)) + "NullsoftInst"
$at = $latin1.GetString($bytes).IndexOf($signature, [StringComparison]::Ordinal)
$header = ""
if ($at -ge 4) {
    $headerLength = [BitConverter]::ToInt32($bytes, $at + 16)
    $stream = [IO.MemoryStream]::new($bytes, $at + 24, $bytes.Length - $at - 24)
    $inflate = [IO.Compression.DeflateStream]::new($stream, [IO.Compression.CompressionMode]::Decompress)
    try {
        $buffer = New-Object byte[] ($headerLength + 4)
        $read = 0
        while ($read -lt $buffer.Length) {
            $n = $inflate.Read($buffer, $read, $buffer.Length - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        if ($read -eq $buffer.Length -and [BitConverter]::ToInt32($buffer, 0) -eq $headerLength) {
            $header = [Text.Encoding]::Unicode.GetString($buffer, 4, $headerLength)
        }
    } catch {
        $header = ""
    } finally { $inflate.Dispose() }
}
$absent = @("https://aka.ms/vc14/vc_redist.x64.exe", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll") |
    Where-Object { $header.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -lt 0 }
if (-not $header) { Fail "the installer's header could not be read as an NSIS zlib (raw deflate) stream" }
elseif ($absent.Count) { Fail "the installer's script lacks the Visual C++ runtime check ($($absent -join ', '))" }
else { Write-Host "ok: the installer's script (zlib stream) checks for the Visual C++ runtime and names Microsoft's download" }
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
