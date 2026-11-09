[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [string]$ArchivePath
)

$ErrorActionPreference = "Stop"
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$executable = Join-Path $BuildDir "arn.exe"
if (-not (Test-Path -LiteralPath $executable)) { throw "arn.exe was not found in $BuildDir" }

if (Test-Path -LiteralPath $OutputDir) { Remove-Item -LiteralPath $OutputDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
Copy-Item -LiteralPath $executable -Destination (Join-Path $OutputDir "arn.exe")
foreach ($dll in @(Get-ChildItem -LiteralPath $BuildDir -Filter '*.dll' -File)) {
    Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $OutputDir $dll.Name)
}
$root = Split-Path -Parent $PSScriptRoot
Copy-Item -LiteralPath (Join-Path $root "LICENSE") -Destination $OutputDir
Copy-Item -LiteralPath (Join-Path $root "README.md") -Destination $OutputDir

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
    $ArchivePath = [IO.Path]::GetFullPath($ArchivePath)
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

# The archive counts as verified only once the packaged arn.exe has run and
# printed its version. Any failure before that, including an exception while
# starting it, removes the archive so it cannot pass for a release artifact;
# the cleanup never replaces the original error.
$verified = $false
$oldPath = $env:Path
try {
    $env:Path = Join-Path $env:SystemRoot "System32"
    $version = & (Join-Path $OutputDir "arn.exe") --version
    if ($LASTEXITCODE -ne 0 -or $version -notmatch '^arn [0-9]+\.[0-9]+\.[0-9]+') {
        throw "Packaged arn.exe failed with only Windows System32 on PATH."
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
