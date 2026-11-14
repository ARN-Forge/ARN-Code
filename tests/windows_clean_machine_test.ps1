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
# OpenSSL. Inside it:
#   1. checks that the machine really is clean (no Visual C++ runtime in
#      System32 and no Visual C++ Redistributable registered);
#   2. runs arn.exe from the extracted ZIP with only System32 on PATH: it must
#      print exactly "arn <ExpectedVersion> (<commit>)";
#   3. removes the packaged Visual C++ runtime DLLs from a copy and expects
#      arn.exe to fail to start, which shows the package's own runtime files
#      are what make it run;
#   4. installs the package with scripts/install.ps1 (checksum verified),
#      reinstalls it, and uninstalls it: the user PATH entry is added and
#      removed, ARN's files are removed, and a user's own file in the
#      installation directory is kept.
# Needs Docker with Windows containers (GitHub's windows-2022 runners).

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
$failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$Message) { $script:failures.Add($Message); Write-Host "FAIL: $Message" }
$system = Join-Path $env:SystemRoot "System32"
$runtime = @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_atomic_wait.dll")

# 1. A clean machine.
$present = @($runtime | Where-Object { Test-Path -LiteralPath (Join-Path $system $_) })
$registered = Test-Path "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"
if ($present.Count -or $registered) { throw "Not a clean machine: System32 has [$($present -join ', ')], Redistributable registered: $registered." }
$os = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
Write-Host "ok: clean Windows ($($os.ProductName), build $($os.CurrentBuildNumber)): no Visual C++ runtime in System32, no Redistributable registered"

function Invoke-Arn([string]$Exe) {
    $out = Join-Path $env:TEMP ([guid]::NewGuid().ToString("N"))
    $p = Start-Process -FilePath $Exe -ArgumentList "--version" -NoNewWindow -Wait -PassThru -RedirectStandardOutput $out
    $text = if (Test-Path $out) { ([IO.File]::ReadAllText($out)).Trim() } else { "" }
    return @{ Code = $p.ExitCode; Text = $text }
}

# 2. The package as published.
$pkg = "C:\t\package"
Expand-Archive -LiteralPath C:\work\arn-windows-x64.zip -DestinationPath $pkg
$env:Path = $system
$r = Invoke-Arn (Join-Path $pkg "arn.exe")
if ($r.Code -ne 0 -or $r.Text -cne $ExpectedLine) { Fail "packaged arn.exe: exit $($r.Code), '$($r.Text)', expected '$ExpectedLine'" }
else { Write-Host "ok: packaged arn.exe runs on the clean machine: $($r.Text)" }

# 3. Without the packaged Visual C++ runtime it cannot start.
$bare = "C:\t\without-runtime"
Copy-Item -LiteralPath $pkg -Destination $bare -Recurse
$removed = @($runtime | Where-Object { Test-Path -LiteralPath (Join-Path $bare $_) })
foreach ($name in $removed) { Remove-Item -LiteralPath (Join-Path $bare $name) }
if (-not $removed.Count) { Fail "the package contains none of the Visual C++ runtime DLLs" }
$r = Invoke-Arn (Join-Path $bare "arn.exe")
if ($r.Code -eq 0) { Fail "arn.exe started without the packaged Visual C++ runtime ($($removed -join ', ')): '$($r.Text)'" }
else { Write-Host ("ok: without the packaged {0} arn.exe does not start (exit 0x{1:X8})" -f ($removed -join ', '), $r.Code) }

# 4. install.ps1: install, reinstall, uninstall.
$dir = "C:\t\arn\bin"
New-Item -ItemType Directory -Path $dir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $dir "my-notes.txt"), "user data")
$common = @{ ArchivePath = "C:\work\arn-windows-x64.zip"; ChecksumsPath = "C:\work\SHA256SUMS"; InstallDir = $dir; PathScope = "User" }
function User-Path { [Environment]::GetEnvironmentVariable("Path", "User") }
function In-UserPath { @((User-Path) -split ';' | Where-Object { $_.TrimEnd('\') -ieq $dir }).Count -gt 0 }
foreach ($round in "install", "reinstall") {
    try { & C:\work\install.ps1 @common | Out-Host } catch { Fail "$round failed: $($_.Exception.Message)"; continue }
    $r = Invoke-Arn (Join-Path $dir "arn.exe")
    if ($r.Code -ne 0 -or $r.Text -cne $ExpectedLine) { Fail "$round`: installed arn.exe printed '$($r.Text)' (exit $($r.Code))" }
    elseif (-not (Test-Path (Join-Path $dir "install-manifest.json"))) { Fail "$round`: no install-manifest.json" }
    elseif (-not (In-UserPath)) { Fail "$round`: $dir is not in the user PATH '$(User-Path)'" }
    else { Write-Host "ok: $round with checksum verification: $($r.Text), user PATH updated" }
}
$bad = Join-Path $env:TEMP "BAD-SHA256SUMS"
[IO.File]::WriteAllText($bad, ("0" * 64) + "  arn-windows-x64.zip`n")
$refused = $false
try { & C:\work\install.ps1 -ArchivePath C:\work\arn-windows-x64.zip -ChecksumsPath $bad -InstallDir "C:\t\other" -PathScope None | Out-Host } catch { $refused = $_.Exception.Message -match 'Checksum verification failed' }
if (-not $refused -or (Test-Path "C:\t\other\arn.exe")) { Fail "a wrong checksum was not refused" } else { Write-Host "ok: a wrong checksum is refused and nothing is installed" }
try { & C:\work\install.ps1 -Uninstall -InstallDir $dir | Out-Host } catch { Fail "uninstall failed: $($_.Exception.Message)" }
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
