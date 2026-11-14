[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ArchivePath,
    [string]$ExpectedVersion,
    [string]$ExpectedCommit,
    # Fail unless THIRD-PARTY-NOTICES.txt names exactly the OpenSSL version
    # of the packaged OpenSSL DLLs (release and CI packages). Without it a
    # mismatch is only reported, so that builds with a local OpenSSL can run
    # the tests.
    [switch]$RequireNoticesMatch
)

# Verifies the Windows CLI ZIP:
#   - entries are flat file names, without duplicates, directories, rooted
#     paths or "..": arn.exe, LICENSE, README.md, THIRD-PARTY-NOTICES.txt and
#     DLLs only;
#   - every packaged DLL is x64 and imported, directly or through another
#     packaged DLL, by arn.exe (nothing unrelated is shipped);
#   - every import is packaged, a Windows component in System32, an API set
#     (the Universal CRT is part of Windows 10 and later), or the Microsoft
#     Visual C++ runtime, which is a prerequisite that users install from
#     Microsoft: it must NOT be packaged, and a debug runtime is refused;
#   - THIRD-PARTY-NOTICES.txt names the version of the packaged OpenSSL DLLs
#     (enforced with -RequireNoticesMatch, otherwise a warning);
#   - the archive extracted into a fresh temporary directory runs, with only
#     System32 on PATH: `arn.exe --version` prints exactly
#     "arn <ExpectedVersion> (<first 12 digits of ExpectedCommit>)", or any
#     "arn X.Y.Z" line when no expectation is given;
#   - `arn.exe --server` starts there, and every packaged DLL it loads is
#     loaded from the extracted directory, not from System32 or PATH.
# Writes the version line to the pipeline; throws on any problem. The
# temporary directory is always removed.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem
. (Join-Path $PSScriptRoot "windows-runtime.ps1")

if ([bool]$ExpectedVersion -ne [bool]$ExpectedCommit) {
    throw "Pass both -ExpectedVersion and -ExpectedCommit, or neither."
}
$expectedLine = $null
if ($ExpectedVersion) {
    if ($ExpectedVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
        throw "-ExpectedVersion must be X.Y.Z, got '$ExpectedVersion'."
    }
    $commit = $ExpectedCommit.ToLowerInvariant()
    if ($commit -notmatch '^[0-9a-f]{12,40}$') {
        throw "-ExpectedCommit must be 12-40 hexadecimal digits, got '$ExpectedCommit'."
    }
    $expectedLine = "arn $ExpectedVersion ($($commit.Substring(0, 12)))"
}

# Relative to the PowerShell location, not the process directory.
$ArchivePath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ArchivePath)
if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) { throw "Archive '$ArchivePath' not found." }

# 1. Entry names.
$zip = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
try { $entries = @($zip.Entries | ForEach-Object { $_.FullName }) } finally { $zip.Dispose() }
$problems = New-Object System.Collections.Generic.List[string]
$seen = @{}
foreach ($entry in $entries) {
    $key = $entry.ToLowerInvariant()
    if ($seen.ContainsKey($key)) { $problems.Add("duplicate entry '$entry'"); continue }
    $seen[$key] = $true
    if ($entry -match '[\\/]' -or $entry -match '^\.\.?$' -or $entry -match ':' -or $entry -eq '') {
        $problems.Add("entry '$entry' is not a plain file name (no directories, '..' or rooted paths)")
    } elseif ($entry -notin @('arn.exe', 'LICENSE', 'README.md', 'THIRD-PARTY-NOTICES.txt') -and $entry -notmatch '^[^\\/:]+\.dll$') {
        $problems.Add("unexpected entry '$entry'")
    }
}
foreach ($required in @('arn.exe', 'LICENSE', 'README.md', 'THIRD-PARTY-NOTICES.txt')) {
    if ($entries -cnotcontains $required) { $problems.Add("missing '$required'") }
}
if ($problems.Count) { throw "Invalid package ${ArchivePath}:`n  - $($problems -join "`n  - ")" }

$extractDir = Join-Path ([IO.Path]::GetTempPath()) ("arn-package-verify-" + [guid]::NewGuid().ToString("N"))
$oldPath = $env:Path
try {
    # 2. Extract into a fresh directory (names were checked above).
    [IO.Compression.ZipFile]::ExtractToDirectory($ArchivePath, $extractDir)
    $exe = Join-Path $extractDir "arn.exe"

    # 3. Dependencies.
    $system = Join-Path $env:SystemRoot "System32"
    $packagedDlls = @{}
    foreach ($entry in @($entries | Where-Object { $_ -like '*.dll' })) { $packagedDlls[$entry.ToLowerInvariant()] = $entry }
    $packagedRuntime = @($packagedDlls.Keys | Where-Object { (Test-VcRuntime $_) -or (Test-DebugVcRuntime $_) } | ForEach-Object { $packagedDlls[$_] })
    if ($packagedRuntime.Count) {
        throw "The package contains the Microsoft Visual C++ runtime ($($packagedRuntime -join ', ')); it must not be shipped: users install the Visual C++ Redistributable from Microsoft ($VcRedistUrl)."
    }
    $needed = @{}
    $queue = New-Object System.Collections.Generic.Queue[string]
    $queue.Enqueue($exe)
    $visited = @{}
    $report = New-Object System.Collections.Generic.List[string]
    $missing = New-Object System.Collections.Generic.List[string]
    while ($queue.Count) {
        $file = $queue.Dequeue()
        $fileName = [IO.Path]::GetFileName($file)
        if ($visited.ContainsKey($fileName.ToLowerInvariant())) { continue }
        $visited[$fileName.ToLowerInvariant()] = $true
        try {
            $imports = Get-PeImports $file
        } catch {
            throw "Cannot read the imports of ${fileName}; it is not a valid x64 executable: $($_.Exception.Message)"
        }
        foreach ($import in $imports) {
            if (Test-ApiSet $import) {
                continue
            } elseif (Test-DebugVcRuntime $import) {
                $missing.Add("$import (the debug Visual C++ runtime, which no Redistributable installs; imported by $fileName)")
            } elseif (Test-VcRuntime $import) {
                $report.Add(("  {0,-15} {1} (imported by {2})" -f "prerequisite:", $import, $fileName))
            } elseif ($packagedDlls.ContainsKey($import)) {
                $needed[$import] = $true
                $report.Add(("  {0,-15} {1} (imported by {2})" -f "packaged:", $import, $fileName))
                $queue.Enqueue((Join-Path $extractDir $packagedDlls[$import]))
            } elseif (Test-Path -LiteralPath (Join-Path $system $import)) {
                $report.Add(("  {0,-15} {1} (imported by {2})" -f "Windows:", $import, $fileName))
            } else {
                $missing.Add("$import (imported by $fileName)")
            }
        }
    }
    Write-Host "Dependencies of the packaged arn.exe:"
    $report | Sort-Object -Unique | ForEach-Object { Write-Host $_ }
    $unneeded = @($packagedDlls.Keys | Where-Object { -not $needed.ContainsKey($_) } | ForEach-Object { $packagedDlls[$_] })
    if ($unneeded.Count) { throw "Packaged DLLs that arn.exe does not use: $($unneeded -join ', ')." }
    if ($missing.Count) { throw "Imports that are not packaged and not part of Windows: $($missing -join ', ')." }
    $openSsl = @($packagedDlls.Keys | Where-Object { Test-OpenSslDll $_ } | ForEach-Object { Join-Path $extractDir $packagedDlls[$_] })
    if ($openSsl.Count) {
        $noticesProblem = Get-OpenSslNoticesProblem (Join-Path $extractDir "THIRD-PARTY-NOTICES.txt") $openSsl
        if ($noticesProblem -and $RequireNoticesMatch) { throw "$noticesProblem." }
        if ($noticesProblem) { Write-Warning "$noticesProblem; a release package must not be built this way." }
        else { Write-Host "  THIRD-PARTY-NOTICES.txt names the packaged OpenSSL version" }
    }

    # 4. Run the extracted executable with only Windows on PATH.
    $env:Path = $system
    Push-Location -LiteralPath $extractDir
    try {
        # stdout only: in Windows PowerShell 5.1, redirecting a native
        # program's stderr turns it into errors that would mask the exit code.
        $output = & $exe --version
        $exitCode = $LASTEXITCODE
    } catch {
        throw "The extracted arn.exe could not be started: $($_.Exception.Message)"
    } finally {
        Pop-Location
    }
    $line = (@($output) -join "`n").Trim()
    if ($exitCode -ne 0) { throw "The extracted arn.exe --version failed with exit code ${exitCode}: $line" }
    if ($expectedLine) {
        if ($line -cne $expectedLine) { throw "The extracted arn.exe --version printed '$line', expected '$expectedLine'." }
    } elseif ($line -notmatch '^arn [0-9]+\.[0-9]+\.[0-9]+') {
        throw "The extracted arn.exe --version printed '$line', not an arn version."
    }
    Write-Host "Extracted arn.exe: $line"

    # 5. Which files Windows actually loads: start the server mode, which
    # announces itself and then waits for input, list its modules, then end
    # it by closing its input.
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $exe
    $start.Arguments = "--server"
    $start.WorkingDirectory = $extractDir
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.CreateNoWindow = $true
    $start.EnvironmentVariables["PATH"] = $system
    $server = [Diagnostics.Process]::Start($start)
    try {
        $readyTask = $server.StandardOutput.ReadLineAsync()
        if (-not $readyTask.Wait(15000)) { throw "The extracted arn.exe --server did not start within 15 s." }
        if ($readyTask.Result -notmatch '"type"\s*:\s*"ready"') {
            throw "The extracted arn.exe --server printed '$($readyTask.Result)' instead of its ready message."
        }
        $loaded = @{}
        foreach ($module in $server.Modules) { $loaded[$module.ModuleName.ToLowerInvariant()] = $module.FileName }
    } finally {
        $server.StandardInput.Close()
        if (-not $server.WaitForExit(10000)) { $server.Kill(); throw "The extracted arn.exe --server did not exit after its input closed." }
    }
    if ($server.ExitCode -ne 0) { throw "The extracted arn.exe --server exited with code $($server.ExitCode)." }
    $wrongPlace = New-Object System.Collections.Generic.List[string]
    foreach ($dll in $packagedDlls.Keys) {
        if (-not $loaded.ContainsKey($dll)) { continue }
        $from = [IO.Path]::GetDirectoryName($loaded[$dll])
        if (-not [string]::Equals($from.TrimEnd('\'), $extractDir.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) {
            $wrongPlace.Add("$dll from $from")
        } else {
            Write-Host "  loaded from the package: $($packagedDlls[$dll])"
        }
    }
    if ($wrongPlace.Count) { throw "Packaged DLLs were loaded from elsewhere: $($wrongPlace -join ', ')." }
    Write-Output $line
} finally {
    $env:Path = $oldPath
    Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue
}
