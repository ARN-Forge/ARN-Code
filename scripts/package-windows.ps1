[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [string]$ArchivePath,
    # When given, the packaged arn.exe must print exactly
    # "arn <ExpectedVersion> (<first 12 digits of ExpectedCommit>)".
    [string]$ExpectedVersion,
    [string]$ExpectedCommit,
    # Passed to verify-windows-package.ps1: THIRD-PARTY-NOTICES.txt must name
    # exactly the packaged OpenSSL version (release and CI packages).
    [switch]$RequireNoticesMatch
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "windows-runtime.ps1")
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
# Relative paths are taken from the PowerShell location, not the process
# directory that [IO.Path]::GetFullPath would use (they differ after cd).
function Get-FullPath([string]$Path) {
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}
$OutputDir = Get-FullPath $OutputDir
$executable = Join-Path $BuildDir "arn.exe"
if (-not (Test-Path -LiteralPath $executable)) { throw "arn.exe was not found in $BuildDir" }

if (Test-Path -LiteralPath $OutputDir) { Remove-Item -LiteralPath $OutputDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
Copy-Item -LiteralPath $executable -Destination (Join-Path $OutputDir "arn.exe")

# The package holds exactly the DLLs arn.exe needs, found by following its
# imports (and delay-loads) transitively:
#   - the Microsoft Visual C++ runtime is NOT packaged: it is a prerequisite
#     that users install from Microsoft (the Visual C++ Redistributable,
#     https://aka.ms/vc14/vc_redist.x64.exe), and install.ps1 checks for it;
#     a Debug build, which needs the debug runtime that no Redistributable
#     installs, cannot be packaged;
#   - other DLLs come from the build directory, where CMake deployed them
#     (OpenSSL);
#   - Windows components (System32) and API sets, including the Universal
#     CRT of Windows 10 and later, are not packaged.
$system = Join-Path $env:SystemRoot "System32"
$queue = New-Object System.Collections.Generic.Queue[string]
$queue.Enqueue($executable)
$seen = @{ "arn.exe" = $true }
while ($queue.Count) {
    $file = $queue.Dequeue()
    foreach ($import in (Get-PeImports $file)) {
        if ((Test-ApiSet $import) -or $seen.ContainsKey($import)) { continue }
        $seen[$import] = $true
        if (Test-DebugVcRuntime $import) {
            throw "$([IO.Path]::GetFileName($file)) imports $import, the debug Visual C++ runtime, which no Visual C++ Redistributable installs; package a Release build."
        } elseif (Test-VcRuntime $import) {
            continue
        } elseif (Test-Path -LiteralPath (Join-Path $BuildDir $import) -PathType Leaf) {
            $source = Join-Path $BuildDir $import
        } elseif (Test-Path -LiteralPath (Join-Path $system $import) -PathType Leaf) {
            continue
        } else {
            throw "$import, imported by $([IO.Path]::GetFileName($file)), is neither in $BuildDir nor part of Windows."
        }
        $destination = Join-Path $OutputDir ([IO.Path]::GetFileName($source))
        Copy-Item -LiteralPath $source -Destination $destination
        $queue.Enqueue($destination)
    }
}
$root = Split-Path -Parent $PSScriptRoot
Copy-Item -LiteralPath (Join-Path $root "LICENSE") -Destination $OutputDir
Copy-Item -LiteralPath (Join-Path $root "README.md") -Destination $OutputDir
Copy-Item -LiteralPath (Join-Path $root "THIRD-PARTY-NOTICES.txt") -Destination $OutputDir

# True when the error, or an exception inside it, is Windows' "file in use":
# exactly HRESULT 0x80070020 (ERROR_SHARING_VIOLATION) or 0x80070021
# (ERROR_LOCK_VIOLATION). Other codes, even with the same low 16 bits, are not.
function Test-FileInUse($ErrorRecord) {
    for ($exception = $ErrorRecord.Exception; $exception; $exception = $exception.InnerException) {
        if ($exception -is [IO.IOException] -and ('{0:X8}' -f $exception.HResult) -in @('80070020', '80070021')) {
            return $true
        }
    }
    return $false
}

# The archive is made before the packaged arn.exe runs: a program that has just
# run (and a virus scan of it) can briefly keep the file open, which made
# Compress-Archive fail. Another program can still hold a new file for a
# moment, so only that error is retried, with a warning each time.
# -ErrorAction Stop is explicit because the Archive module does not see this
# script's $ErrorActionPreference: without it a file in use is only reported,
# and the module then fails later with an unrelated "Stream was not readable".
if ($ArchivePath) {
    $ArchivePath = Get-FullPath $ArchivePath
    for ($attempt = 1; ; $attempt++) {
        Remove-Item -LiteralPath $ArchivePath -Force -ErrorAction SilentlyContinue
        try {
            Compress-Archive -Path (Join-Path $OutputDir '*') -DestinationPath $ArchivePath -ErrorAction Stop
            break
        } catch {
            if ($attempt -ge 5 -or -not (Test-FileInUse $_)) { throw }
            Write-Warning "Archive attempt $attempt of 5 failed because a file was in use; retrying: $($_.Exception.Message)"
            Start-Sleep -Milliseconds 300
        }
    }
}

# The archive counts as verified only once verify-windows-package.ps1 has
# checked its entries and dependencies and run the arn.exe extracted from it.
# Any failure before that, including an exception while starting it, removes
# the archive so it cannot pass for a release artifact; the cleanup never
# replaces the original error. Without an archive, the staged arn.exe runs.
$verified = $false
$oldPath = $env:Path
try {
    if ($ArchivePath) {
        $verifyArgs = @{ ArchivePath = $ArchivePath; RequireNoticesMatch = $RequireNoticesMatch }
        if ($ExpectedVersion -or $ExpectedCommit) {
            $verifyArgs.ExpectedVersion = $ExpectedVersion
            $verifyArgs.ExpectedCommit = $ExpectedCommit
        }
        $version = & (Join-Path $PSScriptRoot "verify-windows-package.ps1") @verifyArgs
    } else {
        $env:Path = Join-Path $env:SystemRoot "System32"
        $version = & (Join-Path $OutputDir "arn.exe") --version
        if ($LASTEXITCODE -ne 0 -or $version -notmatch '^arn [0-9]+\.[0-9]+\.[0-9]+') {
            throw "Packaged arn.exe failed with only Windows System32 on PATH."
        }
    }
    $verified = $true
} finally {
    $env:Path = $oldPath
    if ($ArchivePath -and -not $verified) {
        Remove-Item -LiteralPath $ArchivePath -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $ArchivePath) {
            Write-Warning "Could not remove the unverified archive $ArchivePath; do not use it."
        }
    }
}
Write-Host "Packaged $version with $(@(Get-ChildItem $OutputDir -Filter '*.dll').Count) runtime DLL(s)."
