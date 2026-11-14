[CmdletBinding()]
param(
    [string]$ZipPath,
    [string]$Directory,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [Parameter(Mandatory = $true)][string]$ExpectedCommit,
    # Fail unless THIRD-PARTY-NOTICES.txt names exactly the OpenSSL version of
    # the packaged OpenSSL DLLs (release and CI packages); otherwise a warning.
    [switch]$RequireNoticesMatch
)

# Verifies the Windows IDE packages before they are published:
#   -ZipPath   the portable arn-ide-windows-x64.zip: flat entries, no
#              duplicates, exactly arn-ide.exe, arn.exe, LICENSE, README.md,
#              IDE-README.md, THIRD-PARTY-NOTICES.txt,
#              THIRD-PARTY-NOTICES-IDE.txt and DLLs;
#   -Directory the NSIS installer payload extracted with 7-Zip: the same
#              top-level files except LICENSE, README.md and IDE-README.md
#              must be present (the installer's own files, such as
#              $PLUGINSDIR, may be there too).
# For both:
#   - every required file is non-empty;
#   - every DLL that arn.exe or arn-ide.exe imports, directly or through a
#     packaged DLL, is packaged, a Windows component in System32, or an API
#     set; the Visual C++ runtime must be packaged and signed by Microsoft;
#     no packaged DLL is unused;
#   - THIRD-PARTY-NOTICES.txt names the version of the packaged OpenSSL DLLs
#     (enforced with -RequireNoticesMatch, otherwise a warning);
#   - arn.exe --version, run with only System32 on PATH, prints exactly
#     "arn <ExpectedVersion> (<first 12 digits of ExpectedCommit>)", so a
#     "-dirty" or "unknown" build is rejected;
#   - arn-ide.exe's version resource names ProductName "ARN IDE" and
#     ProductVersion and FileVersion <ExpectedVersion>, so the IDE and the CLI
#     it bundles have the same version.
# Throws on any problem; the temporary directory is always removed.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem
. (Join-Path $PSScriptRoot "windows-runtime.ps1")

if ([bool]$ZipPath -eq [bool]$Directory) { throw "Pass exactly one of -ZipPath and -Directory." }
if ($ExpectedVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw "-ExpectedVersion must be X.Y.Z, got '$ExpectedVersion'." }
$commit = $ExpectedCommit.ToLowerInvariant()
if ($commit -notmatch '^[0-9a-f]{12,40}$') { throw "-ExpectedCommit must be 12-40 hexadecimal digits, got '$ExpectedCommit'." }
$expectedLine = "arn $ExpectedVersion ($($commit.Substring(0, 12)))"

$notices = @('THIRD-PARTY-NOTICES.txt', 'THIRD-PARTY-NOTICES-IDE.txt')
$zipOnly = @('LICENSE', 'README.md', 'IDE-README.md')
$required = @('arn-ide.exe', 'arn.exe') + $notices
$problems = New-Object System.Collections.Generic.List[string]
$extractDir = $null
$oldPath = $env:Path
try {
    if ($ZipPath) {
        $ZipPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ZipPath)
        if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) { throw "Archive '$ZipPath' not found." }
        $zip = [IO.Compression.ZipFile]::OpenRead($ZipPath)
        try { $entries = @($zip.Entries | ForEach-Object { $_.FullName }) } finally { $zip.Dispose() }
        $seen = @{}
        foreach ($entry in $entries) {
            $key = $entry.ToLowerInvariant()
            if ($seen.ContainsKey($key)) { $problems.Add("duplicate entry '$entry'"); continue }
            $seen[$key] = $true
            if ($entry -match '[\\/]' -or $entry -match '^\.\.?$' -or $entry -match ':' -or $entry -eq '') {
                $problems.Add("entry '$entry' is not a plain file name (no directories, '..' or rooted paths)")
            } elseif ($entry -cnotin ($required + $zipOnly) -and $entry -notmatch '^[^\\/:]+\.dll$') {
                $problems.Add("unexpected entry '$entry'")
            }
        }
        foreach ($name in ($required + $zipOnly)) {
            if ($entries -cnotcontains $name) { $problems.Add("missing '$name'") }
        }
        if ($problems.Count) { throw "Invalid IDE package ${ZipPath}:`n  - $($problems -join "`n  - ")" }
        $extractDir = Join-Path ([IO.Path]::GetTempPath()) ("arn-ide-verify-" + [guid]::NewGuid().ToString("N"))
        [IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $extractDir)
        $root = $extractDir
        $what = $ZipPath
        $mustExist = $required + $zipOnly
    } else {
        $root = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Directory)
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw "Directory '$root' not found." }
        $what = $root
        $mustExist = $required
        foreach ($name in $mustExist) {
            if (-not (Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf)) { $problems.Add("missing '$name'") }
        }
        if ($problems.Count) { throw "Invalid IDE payload ${root}:`n  - $($problems -join "`n  - ")" }
    }
    foreach ($name in $mustExist) {
        if ((Get-Item -LiteralPath (Join-Path $root $name)).Length -eq 0) { $problems.Add("'$name' is empty") }
    }
    if ($problems.Count) { throw "Invalid IDE package ${what}:`n  - $($problems -join "`n  - ")" }

    # DLL closure of arn.exe and arn-ide.exe over the top-level DLLs.
    $system = Join-Path $env:SystemRoot "System32"
    $packagedDlls = @{}
    foreach ($file in @(Get-ChildItem -LiteralPath $root -File -Filter '*.dll')) { $packagedDlls[$file.Name.ToLowerInvariant()] = $file.Name }
    $needed = @{}
    $missing = New-Object System.Collections.Generic.List[string]
    $queue = New-Object System.Collections.Generic.Queue[string]
    $queue.Enqueue((Join-Path $root 'arn.exe'))
    $queue.Enqueue((Join-Path $root 'arn-ide.exe'))
    $visited = @{}
    while ($queue.Count) {
        $file = $queue.Dequeue()
        $fileName = [IO.Path]::GetFileName($file)
        if ($visited.ContainsKey($fileName.ToLowerInvariant())) { continue }
        $visited[$fileName.ToLowerInvariant()] = $true
        try { $imports = Get-PeImports $file } catch { throw "Cannot read the imports of ${fileName}; it is not a valid x64 executable: $($_.Exception.Message)" }
        foreach ($import in $imports) {
            if (Test-ApiSet $import) { continue }
            if ($packagedDlls.ContainsKey($import)) {
                $needed[$import] = $true
                $queue.Enqueue((Join-Path $root $packagedDlls[$import]))
            } elseif (Test-VcRuntime $import) {
                $missing.Add("$import (Visual C++ runtime, imported by $fileName)")
            } elseif (-not (Test-Path -LiteralPath (Join-Path $system $import))) {
                $missing.Add("$import (imported by $fileName)")
            }
        }
    }
    if ($missing.Count) { throw "Imports that are not packaged and not part of Windows: $($missing -join ', ')." }
    $unneeded = @($packagedDlls.Keys | Where-Object { -not $needed.ContainsKey($_) } | ForEach-Object { $packagedDlls[$_] })
    if ($unneeded.Count) { throw "Packaged DLLs that neither arn.exe nor arn-ide.exe uses: $($unneeded -join ', ')." }
    foreach ($dll in @($packagedDlls.Keys | Where-Object { Test-VcRuntime $_ })) { Assert-MicrosoftSigned (Join-Path $root $packagedDlls[$dll]) }
    $openSsl = @($packagedDlls.Keys | Where-Object { Test-OpenSslDll $_ } | ForEach-Object { Join-Path $root $packagedDlls[$_] })
    if ($openSsl.Count) {
        $noticesProblem = Get-OpenSslNoticesProblem (Join-Path $root "THIRD-PARTY-NOTICES.txt") $openSsl
        if ($noticesProblem -and $RequireNoticesMatch) { throw "$noticesProblem." }
        if ($noticesProblem) { Write-Warning "$noticesProblem; a release package must not be built this way." }
    }

    # The bundled CLI: exact version and commit, with only Windows on PATH.
    $env:Path = $system
    Push-Location -LiteralPath $root
    try {
        $output = & (Join-Path $root 'arn.exe') --version
        $exitCode = $LASTEXITCODE
    } catch {
        throw "The packaged arn.exe could not be started: $($_.Exception.Message)"
    } finally {
        Pop-Location
        $env:Path = $oldPath
    }
    $line = (@($output) -join "`n").Trim()
    if ($exitCode -ne 0) { throw "The packaged arn.exe --version failed with exit code ${exitCode}: $line" }
    if ($line -cne $expectedLine) { throw "The packaged arn.exe --version printed '$line', expected '$expectedLine'." }

    # The IDE executable's own version.
    $info = (Get-Item -LiteralPath (Join-Path $root 'arn-ide.exe')).VersionInfo
    if ($info.ProductName -cne 'ARN IDE' -or $info.ProductVersion -cne $ExpectedVersion -or $info.FileVersion -cne $ExpectedVersion) {
        throw "arn-ide.exe has ProductName '$($info.ProductName)', ProductVersion '$($info.ProductVersion)' and FileVersion '$($info.FileVersion)', expected 'ARN IDE' and $ExpectedVersion."
    }
    Write-Host "Verified $what`: arn-ide.exe $($info.ProductVersion), $line, $($packagedDlls.Count) DLL(s), third-party notices present."
} finally {
    $env:Path = $oldPath
    if ($extractDir -and (Test-Path -LiteralPath $extractDir)) { Remove-Item -LiteralPath $extractDir -Recurse -Force }
}
