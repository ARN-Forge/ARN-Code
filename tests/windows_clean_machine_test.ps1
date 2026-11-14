[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ArchivePath,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [Parameter(Mandatory = $true)][string]$ExpectedCommit,
    # A Windows image without the Visual C++ Redistributable. Its build must
    # match the host's for process isolation (windows-2022 runners: ltsc2022).
    [string]$Image = "mcr.microsoft.com/windows/servercore:ltsc2022"
)

# Runs the Windows CLI package on a clean Windows: a Windows Server Core
# container, which has no Visual C++ runtime, no developer tools and no
# OpenSSL. The package does not ship the Microsoft Visual C++ runtime: it is a
# prerequisite that users install from Microsoft. Inside the container:
#   1. checks that the machine really is clean (no Visual C++ runtime in
#      System32 and no Visual C++ Redistributable registered);
#   2. checks that the package contains no Visual C++ runtime file, and that
#      its arn.exe cannot start without the runtime;
#   3. runs scripts/install.ps1: it must stop with Microsoft's download link
#      and change nothing;
#   4. as a user would, installs the Visual C++ Redistributable from
#      Microsoft's link (the test does this, not ARN Code; the download must
#      be signed by Microsoft);
#   5. runs the packaged arn.exe ("arn <ExpectedVersion> (<commit>)"), then
#      installs the package with install.ps1 (checksum verified), reinstalls
#      it, refuses a wrong checksum, and uninstalls it: the user PATH entry is
#      added and removed, ARN's files are removed, and a user's own file in
#      the installation directory is kept. No step of ARN Code needs
#      administrator rights; only the Redistributable's installer does.
# Needs Docker with Windows containers (GitHub's windows-2022 runners) and
# network access from the container to Microsoft's download.

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$ArchivePath = (Resolve-Path -LiteralPath $ArchivePath).Path
$commit = $ExpectedCommit.ToLowerInvariant()
if ($ExpectedVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw "-ExpectedVersion must be X.Y.Z." }
if ($commit -notmatch '^[0-9a-f]{12,40}$') { throw "-ExpectedCommit must be 12-40 hexadecimal digits." }
$expectedLine = "arn $ExpectedVersion ($($commit.Substring(0, 12)))"

$work = Join-Path ([IO.Path]::GetTempPath()) ("arn-clean-machine-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    Copy-Item -LiteralPath $ArchivePath -Destination (Join-Path $work "arn-windows-x64.zip")
    Copy-Item -LiteralPath (Join-Path $repo "scripts/install.ps1") -Destination (Join-Path $work "install.ps1")
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ArchivePath).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText((Join-Path $work "SHA256SUMS"), "$hash  arn-windows-x64.zip`n")

    $inner = @'
param([string]$ExpectedLine)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$Message) { $script:failures.Add($Message); Write-Host "FAIL: $Message" }
$system = Join-Path $env:SystemRoot "System32"
$runtime = @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_atomic_wait.dll")
$redistUrl = "https://aka.ms/vc14/vc_redist.x64.exe"

function Invoke-Arn([string]$Exe) {
    $out = Join-Path $env:TEMP ([guid]::NewGuid().ToString("N"))
    $p = Start-Process -FilePath $Exe -ArgumentList "--version" -NoNewWindow -Wait -PassThru -RedirectStandardOutput $out
    $text = if (Test-Path $out) { ([IO.File]::ReadAllText($out)).Trim() } else { "" }
    return @{ Code = $p.ExitCode; Text = $text }
}
function Invoke-Installer([hashtable]$Arguments) {
    try { & C:\work\install.ps1 @Arguments | Out-Host; return $null } catch { return $_.Exception.Message }
}

# 1. A clean machine.
$present = @($runtime | Where-Object { Test-Path -LiteralPath (Join-Path $system $_) })
$registered = (Test-Path "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64") -or
              (Test-Path "HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64")
if ($present.Count -or $registered) { throw "Not a clean machine: System32 has [$($present -join ', ')], Redistributable registered: $registered." }
$os = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
Write-Host "ok: clean Windows ($($os.ProductName), build $($os.CurrentBuildNumber)): no Visual C++ runtime in System32, no Redistributable registered"

# 2. The package: no Visual C++ runtime inside, and arn.exe needs it.
$pkg = "C:\t\package"
Expand-Archive -LiteralPath C:\work\arn-windows-x64.zip -DestinationPath $pkg
$shipped = @(Get-ChildItem -LiteralPath $pkg -File | Where-Object { $_.Name -match '^(vcruntime|msvcp|concrt|vccorlib|vcomp|vcamp|mfcm?)1[0-9]{2}' } | ForEach-Object Name)
if ($shipped.Count) { Fail "the package contains the Visual C++ runtime: $($shipped -join ', ')" }
else { Write-Host "ok: the package contains no Visual C++ runtime file ($(@(Get-ChildItem -LiteralPath $pkg -File | ForEach-Object Name) -join ', '))" }
$env:Path = $system
$r = Invoke-Arn (Join-Path $pkg "arn.exe")
if ($r.Code -eq 0) { Fail "arn.exe started without the Visual C++ runtime: '$($r.Text)'" }
else { Write-Host ("ok: without the Visual C++ runtime the packaged arn.exe does not start (exit 0x{0:X8})" -f $r.Code) }

# 3. install.ps1 without the runtime: Microsoft's link, nothing changed.
$dir = "C:\t\arn\bin"
$common = @{ ArchivePath = "C:\work\arn-windows-x64.zip"; ChecksumsPath = "C:\work\SHA256SUMS"; InstallDir = $dir; PathScope = "User" }
function User-Path { [Environment]::GetEnvironmentVariable("Path", "User") }
function In-UserPath { @((User-Path) -split ';' | Where-Object { $_.TrimEnd('\') -ieq $dir }).Count -gt 0 }
$message = Invoke-Installer $common
if (-not $message -or -not $message.Contains($redistUrl) -or -not $message.Contains("ARN Code was not installed.")) { Fail "install.ps1 without the runtime: '$message'" }
elseif ((Test-Path -LiteralPath $dir) -or (In-UserPath)) { Fail "install.ps1 without the runtime changed $dir or the user PATH" }
else { Write-Host "ok: without the runtime install.ps1 stops and changes nothing: $($message -replace "`r?`n", ' ')" }

# 4. The user installs the Visual C++ Redistributable from Microsoft.
$redist = Join-Path $env:TEMP "vc_redist.x64.exe"
Invoke-WebRequest -UseBasicParsing -Uri $redistUrl -OutFile $redist
$signature = Get-AuthenticodeSignature -LiteralPath $redist
if ($signature.Status -ne "Valid" -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    throw "The download from $redistUrl is not signed by Microsoft ($($signature.Status), $($signature.SignerCertificate.Subject))."
}
$p = Start-Process -FilePath $redist -ArgumentList "/install", "/quiet", "/norestart" -Wait -PassThru
if ($p.ExitCode -notin 0, 3010) { throw "The Visual C++ Redistributable did not install (exit $($p.ExitCode))." }
$version = (Get-Item -LiteralPath (Join-Path $system "msvcp140.dll")).VersionInfo.FileVersion
Write-Host "ok: the Visual C++ Redistributable from Microsoft is installed (msvcp140.dll $version)"

# 5. With the runtime: run, install, reinstall, wrong checksum, uninstall.
$r = Invoke-Arn (Join-Path $pkg "arn.exe")
if ($r.Code -ne 0 -or $r.Text -cne $ExpectedLine) { Fail "packaged arn.exe: exit $($r.Code), '$($r.Text)', expected '$ExpectedLine'" }
else { Write-Host "ok: the packaged arn.exe runs: $($r.Text)" }
New-Item -ItemType Directory -Path $dir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $dir "my-notes.txt"), "user data")
foreach ($round in "install", "reinstall") {
    $message = Invoke-Installer $common
    if ($message) { Fail "$round failed: $message"; continue }
    $r = Invoke-Arn (Join-Path $dir "arn.exe")
    if ($r.Code -ne 0 -or $r.Text -cne $ExpectedLine) { Fail "$round`: installed arn.exe printed '$($r.Text)' (exit $($r.Code))" }
    elseif (-not (Test-Path (Join-Path $dir "install-manifest.json"))) { Fail "$round`: no install-manifest.json" }
    elseif (-not (In-UserPath)) { Fail "$round`: $dir is not in the user PATH '$(User-Path)'" }
    else { Write-Host "ok: $round with checksum verification: $($r.Text), user PATH updated" }
}
$bad = Join-Path $env:TEMP "BAD-SHA256SUMS"
[IO.File]::WriteAllText($bad, ("0" * 64) + "  arn-windows-x64.zip`n")
$message = Invoke-Installer @{ ArchivePath = "C:\work\arn-windows-x64.zip"; ChecksumsPath = $bad; InstallDir = "C:\t\other"; PathScope = "None" }
if (-not $message -or $message -notmatch 'Checksum verification failed' -or (Test-Path "C:\t\other\arn.exe")) { Fail "a wrong checksum was not refused: '$message'" }
else { Write-Host "ok: a wrong checksum is refused and nothing is installed" }
$message = Invoke-Installer @{ Uninstall = $true; InstallDir = $dir }
if ($message) { Fail "uninstall failed: $message" }
$left = @(Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne "my-notes.txt" } | ForEach-Object { $_.Name })
if ($left.Count) { Fail "uninstall left: $($left -join ', ')" }
elseif (-not (Test-Path (Join-Path $dir "my-notes.txt"))) { Fail "uninstall removed the user's own file" }
elseif (In-UserPath) { Fail "uninstall left $dir in the user PATH" }
else { Write-Host "ok: uninstall removed ARN's files and the PATH entry and kept the user's file" }

Write-Host "$($failures.Count) failure(s)"
if ($failures.Count) { exit 1 }
'@
    [IO.File]::WriteAllText((Join-Path $work "inner.ps1"), $inner)

    docker pull $Image | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "docker pull $Image failed." }
    docker run --rm --isolation process -v "${work}:C:\work" $Image `
        powershell -NoProfile -ExecutionPolicy Bypass -File C:\work\inner.ps1 -ExpectedLine $expectedLine
    if ($LASTEXITCODE -ne 0) { throw "The clean-machine test failed in the container (exit $LASTEXITCODE)." }
    Write-Host "Clean-machine test passed."
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
