param([Parameter(Mandatory = $true)][string]$BuildDir)

# Tests scripts/package-windows.ps1 and scripts/verify-windows-package.ps1 with
# ZIPs built in a temporary directory from the real arn.exe, its DLLs and the
# Visual C++ runtime from Visual Studio's redistributable folder.
# Exits 77 (skipped) for a Debug build: its debug runtime cannot be packaged.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $repo "scripts/windows-runtime.ps1")
$verifyScript = Join-Path $repo "scripts/verify-windows-package.ps1"
$packageScript = Join-Path $repo "scripts/package-windows.ps1"
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$exe = Join-Path $BuildDir "arn.exe"
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$root = Join-Path $tempBase ("arn-package-test-" + [guid]::NewGuid().ToString("N"))
$failures = New-Object System.Collections.Generic.List[string]
$cases = 0

if (@(Get-PeImports $exe | Where-Object { $_ -match '^(vcruntime140(_1)?d|msvcp140d)' }).Count) {
    Write-Host "skip: arn.exe is a Debug build; its debug Visual C++ runtime cannot be redistributed"
    exit 77
}
$runtimeDir = Get-CMakeRuntimeDir $BuildDir
if (-not $runtimeDir -or -not (Test-Path -LiteralPath (Join-Path $runtimeDir "vcruntime140.dll"))) {
    throw "ARN_MSVC_RUNTIME_DIR is not set or not found ('$runtimeDir'); configure with MSVC."
}

$line = & $exe --version
if ($line -notmatch '^arn ([0-9]+\.[0-9]+\.[0-9]+) \(([0-9a-f]{12}(-dirty)?|unknown)\)$') {
    throw "Unexpected arn --version output '$line'."
}
$version = $Matches[1]
$commit = $Matches[2]

# The Visual C++ runtime files arn.exe needs, worked out here independently
# of package-windows.ps1 by following imports through the build directory and
# the redistributable folder.
$expectedRuntime = @{}
$queue = New-Object System.Collections.Generic.Queue[string]
$queue.Enqueue($exe)
$visited = @{}
while ($queue.Count) {
    $file = $queue.Dequeue()
    foreach ($import in (Get-PeImports $file)) {
        if ((Test-ApiSet $import) -or $visited.ContainsKey($import)) { continue }
        $visited[$import] = $true
        if (Test-VcRuntime $import) {
            $expectedRuntime[$import] = $true
            $queue.Enqueue((Join-Path $runtimeDir $import))
        } elseif (Test-Path -LiteralPath (Join-Path $BuildDir $import)) {
            $queue.Enqueue((Join-Path $BuildDir $import))
        }
    }
}

# Writes a ZIP whose entries are (name, source file or text) pairs, in order;
# a name may repeat.
function New-Zip([string]$Path, [object[]]$Entries) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew)
    try {
        $zip = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($entry in $Entries) {
                $item = $zip.CreateEntry($entry[0])
                $writer = $item.Open()
                try {
                    if (Test-Path -LiteralPath $entry[1] -PathType Leaf) {
                        $bytes = [IO.File]::ReadAllBytes($entry[1])
                    } else {
                        $bytes = [Text.Encoding]::UTF8.GetBytes([string]$entry[1])
                    }
                    $writer.Write($bytes, 0, $bytes.Length)
                } finally { $writer.Dispose() }
            }
        } finally { $zip.Dispose() }
    } finally { $stream.Dispose() }
}

# The output of a child PowerShell as one line of text. PowerShell 7 shows an
# error in its "concise view": the message is wrapped to the window width,
# with a "|" gutter at the start of each continuation line. The gutter and
# the line breaks are taken out, so a message reads the same as in Windows
# PowerShell 5.1, whatever the width.
function ConvertTo-PlainText([object[]]$Lines) {
    return ((@($Lines | ForEach-Object { "$_" }) -join " ") -replace '\s+\|\s+', ' ') -replace '\s+', ' '
}

# Runs verify-windows-package.ps1 in a child PowerShell so its failure cannot
# end this test; expects success, or failure whose message has every fragment.
function Test-Verify([string]$Name, [string]$Zip, [bool]$ShouldPass, [string[]]$Fragments, [string[]]$Extra = @()) {
    $script:cases++
    $shell = (Get-Process -Id $PID).Path
    # Windows PowerShell 5.1 turns a native program's redirected stderr into
    # errors; with "Stop" the first one would end this test instead.
    $ErrorActionPreference = "Continue"
    $output = & $shell -NoProfile -ExecutionPolicy Bypass -File $verifyScript -ArchivePath $Zip @Extra 2>&1 |
        ForEach-Object { "$_" }
    $passed = $LASTEXITCODE -eq 0
    $ErrorActionPreference = "Stop"
    $text = ConvertTo-PlainText $output
    if ($ShouldPass -and -not $passed) {
        $failures.Add("${Name}: expected success, got: $text")
        return
    }
    if (-not $ShouldPass -and $passed) {
        $failures.Add("${Name}: expected failure, but verification passed: $text")
        return
    }
    foreach ($fragment in @($Fragments | Where-Object { $_ })) {
        if ($text.IndexOf($fragment, [StringComparison]::Ordinal) -lt 0) {
            $failures.Add("${Name}: output lacks '$fragment': $text")
            return
        }
    }
    Write-Host "ok: $Name"
}

function Copy-Modified([string]$Source, [string]$Destination, [int]$Offset) {
    $bytes = [IO.File]::ReadAllBytes($Source)
    $bytes[$Offset] = $bytes[$Offset] -bxor 0xFF
    [IO.File]::WriteAllBytes($Destination, $bytes)
}

try {
    New-Item -ItemType Directory -Path $root | Out-Null

    # Valid package built by package-windows.ps1, then verified on its own.
    $script:cases++
    $packageDir = Join-Path $root "package"
    $packageZip = Join-Path $root "package.zip"
    & $packageScript -BuildDir $BuildDir -OutputDir $packageDir -ArchivePath $packageZip | Out-Host
    if (-not (Test-Path -LiteralPath $packageZip)) { $failures.Add("package-windows.ps1 did not leave a verified archive") }
    else { Write-Host "ok: package-windows.ps1 creates and verifies a package" }

    # Every file of the valid package, as (name, path) pairs for the variants.
    $standard = @(Get-ChildItem -LiteralPath $packageDir -File | Sort-Object Name | ForEach-Object { , @($_.Name, $_.FullName) })
    $packagedNames = @($standard | ForEach-Object { $_[0].ToLowerInvariant() })

    # The package ships exactly the Visual C++ runtime that arn.exe needs.
    $script:cases++
    $packagedRuntime = @($packagedNames | Where-Object { Test-VcRuntime $_ } | Sort-Object)
    $wantedRuntime = @($expectedRuntime.Keys | Sort-Object)
    if (-not $wantedRuntime.Count -or ($packagedRuntime -join ',') -ne ($wantedRuntime -join ',')) {
        $failures.Add("packaged Visual C++ runtime [$($packagedRuntime -join ', ')] differs from the needed [$($wantedRuntime -join ', ')]")
    } elseif ($packagedNames -notcontains 'vcruntime140.dll') {
        $failures.Add("vcruntime140.dll is not packaged")
    } else {
        Write-Host "ok: the package has exactly the needed Visual C++ runtime: $($packagedRuntime -join ', ')"
    }
    $script:cases++
    $sourceMismatch = @($packagedRuntime | Where-Object {
        (Get-FileHash -LiteralPath (Join-Path $packageDir $_)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $runtimeDir $_)).Hash
    })
    if ($sourceMismatch.Count) { $failures.Add("packaged runtime differs from the redistributable folder: $($sourceMismatch -join ', ')") }
    else { Write-Host "ok: the packaged runtime files are the redistributable folder's files" }

    Test-Verify "valid package without expectations" $packageZip $true @(
        "Extracted arn.exe: arn $version",
        # Transitive dependency: libcrypto through libssl.
        "libcrypto-4-x64.dll (imported by libssl-4-x64.dll)",
        # Loaded by Windows from the extracted directory, with only System32 on PATH.
        "loaded from the package: vcruntime140.dll",
        "loaded from the package: libssl-4-x64.dll",
        "vcruntime140.dll $((Get-Item -LiteralPath (Join-Path $runtimeDir 'vcruntime140.dll')).VersionInfo.FileVersion), signed by Microsoft")
    if ($commit -match '^[0-9a-f]{12}$') {
        Test-Verify "valid package with exact version and commit" $packageZip $true @() `
            @('-ExpectedVersion', $version, '-ExpectedCommit', $commit)
    } else {
        Write-Host "skip: exact version and commit (this build is '$commit')"
    }
    Test-Verify "wrong version" $packageZip $false @("expected 'arn 9.9.9 (0123456789ab)'") `
        @('-ExpectedVersion', '9.9.9', '-ExpectedCommit', '0123456789abcdef')
    Test-Verify "wrong commit" $packageZip $false @("expected 'arn $version (ffffffffffff)'") `
        @('-ExpectedVersion', $version, '-ExpectedCommit', 'ffffffffffffffffffffffffffffffffffffffff')
    Test-Verify "commit without version" $packageZip $false @("Pass both -ExpectedVersion and -ExpectedCommit") `
        @('-ExpectedCommit', 'ffffffffffffffffffffffffffffffffffffffff')

    # Relative paths follow the PowerShell location, not the process directory.
    $script:cases++
    $relativeDir = Join-Path $root "relative"
    New-Item -ItemType Directory -Path $relativeDir | Out-Null
    $processDir = [Environment]::CurrentDirectory
    Push-Location -LiteralPath $relativeDir
    try {
        & $packageScript -BuildDir $BuildDir -OutputDir "rel-package" -ArchivePath "rel.zip" | Out-Null
    } finally { Pop-Location }
    if (-not (Test-Path -LiteralPath (Join-Path $relativeDir "rel.zip"))) {
        $failures.Add("relative -ArchivePath was not created in the PowerShell location")
    } elseif ((Test-Path -LiteralPath (Join-Path $processDir "rel.zip")) -and
              ($processDir.TrimEnd('\') -ne $relativeDir)) {
        $failures.Add("relative -ArchivePath was also written to the process directory $processDir")
    } else {
        Write-Host "ok: relative paths follow the PowerShell location"
    }

    # package-windows.ps1 deletes an archive that fails verification.
    $script:cases++
    $rejectedZip = Join-Path $root "rejected.zip"
    $shell = (Get-Process -Id $PID).Path
    $ErrorActionPreference = "Continue"
    & $shell -NoProfile -ExecutionPolicy Bypass -File $packageScript -BuildDir $BuildDir `
        -OutputDir (Join-Path $root "rejected") -ArchivePath $rejectedZip `
        -ExpectedVersion 9.9.9 -ExpectedCommit 0123456789abcdef 2>&1 | Out-Null
    $rejectedExit = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($rejectedExit -eq 0) { $failures.Add("package-windows.ps1 accepted a wrong expected version") }
    elseif (Test-Path -LiteralPath $rejectedZip) { $failures.Add("package-windows.ps1 left an unverified archive") }
    else { Write-Host "ok: package-windows.ps1 removes an archive that fails verification" }

    # package-windows.ps1 refuses a runtime folder without the needed files.
    $script:cases++
    $emptyRuntime = Join-Path $root "empty-runtime"
    New-Item -ItemType Directory -Path $emptyRuntime | Out-Null
    $ErrorActionPreference = "Continue"
    $text = ConvertTo-PlainText (& $shell -NoProfile -ExecutionPolicy Bypass -File $packageScript -BuildDir $BuildDir `
        -OutputDir (Join-Path $root "no-runtime") -ArchivePath (Join-Path $root "no-runtime.zip") `
        -RuntimeDir $emptyRuntime 2>&1)
    $noRuntimeExit = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($noRuntimeExit -eq 0 -or $text -notmatch 'is not in the Visual C\+\+ redistributable folder') {
        $failures.Add("package-windows.ps1 with an empty runtime folder: exit $noRuntimeExit, $text")
    } elseif (Test-Path -LiteralPath (Join-Path $root "no-runtime.zip")) {
        $failures.Add("package-windows.ps1 left an archive without the runtime")
    } else {
        Write-Host "ok: package-windows.ps1 requires the redistributable Visual C++ runtime"
    }

    # Malformed packages.
    $zip = Join-Path $root "missing.zip"
    New-Zip $zip @($standard | Where-Object { $_[0] -ne 'LICENSE' })
    Test-Verify "missing LICENSE" $zip $false @("missing 'LICENSE'")

    $zip = Join-Path $root "extra.zip"
    New-Zip $zip ($standard + , @('notes.txt', 'extra'))
    Test-Verify "unexpected file" $zip $false @("unexpected entry 'notes.txt'")

    $zip = Join-Path $root "extra-dll.zip"
    New-Zip $zip ($standard + , @('unrelated.dll', (Join-Path $packageDir 'libssl-4-x64.dll')))
    Test-Verify "unexpected DLL" $zip $false @("Packaged DLLs that arn.exe does not use: unrelated.dll")

    $zip = Join-Path $root "unused-runtime.zip"
    New-Zip $zip ($standard + , @('concrt140.dll', (Join-Path $runtimeDir 'concrt140.dll')))
    Test-Verify "unused Visual C++ runtime DLL" $zip $false @("Packaged DLLs that arn.exe does not use: concrt140.dll")

    $zip = Join-Path $root "duplicate.zip"
    New-Zip $zip ($standard + , @('LICENSE', 'second copy'))
    Test-Verify "duplicate entry" $zip $false @("duplicate entry 'LICENSE'")

    $zip = Join-Path $root "traversal.zip"
    New-Zip $zip ($standard + , @('../evil.txt', 'evil'))
    Test-Verify "path traversal entry" $zip $false @("entry '../evil.txt' is not a plain file name")

    $zip = Join-Path $root "subdirectory.zip"
    New-Zip $zip ($standard + , @('bin/arn.exe', $exe))
    Test-Verify "entry in a subdirectory" $zip $false @("entry 'bin/arn.exe' is not a plain file name")

    $zip = Join-Path $root "rooted.zip"
    New-Zip $zip ($standard + , @('C:/evil.txt', 'evil'))
    Test-Verify "rooted entry" $zip $false @("entry 'C:/evil.txt' is not a plain file name")

    $zip = Join-Path $root "missing-runtime.zip"
    New-Zip $zip @($standard | Where-Object { $_[0] -ne 'vcruntime140_1.dll' })
    Test-Verify "missing Visual C++ runtime DLL" $zip $false @("vcruntime140_1.dll (Visual C++ runtime, imported by arn.exe)")

    $zip = Join-Path $root "missing-openssl.zip"
    New-Zip $zip @($standard | Where-Object { $_[0] -ne 'libcrypto-4-x64.dll' })
    Test-Verify "missing transitive dependency" $zip $false @(
        "libcrypto-4-x64.dll (imported by libssl-4-x64.dll)", "not packaged and not part of Windows")

    # Wrong architecture: the official x86 build of the same runtime file.
    $x86Runtime = Join-Path ($runtimeDir -replace '[\\/]x64[\\/]', '\x86\') "vcruntime140.dll"
    $wrongArch = Join-Path $root "vcruntime140-x86.dll"
    if (Test-Path -LiteralPath $x86Runtime) {
        Copy-Item -LiteralPath $x86Runtime -Destination $wrongArch
    } else {
        # No x86 redistributable installed: mark an x64 copy as i386 instead.
        $bytes = [IO.File]::ReadAllBytes((Join-Path $runtimeDir "vcruntime140.dll"))
        $pe = [BitConverter]::ToInt32($bytes, 0x3C)
        $bytes[$pe + 4] = 0x4C
        $bytes[$pe + 5] = 0x01
        [IO.File]::WriteAllBytes($wrongArch, $bytes)
    }
    $zip = Join-Path $root "wrong-arch.zip"
    New-Zip $zip (@($standard | Where-Object { $_[0] -ne 'vcruntime140.dll' }) + , @('vcruntime140.dll', $wrongArch))
    Test-Verify "Visual C++ runtime of the wrong architecture" $zip $false @("vcruntime140.dll is not an x64 image")

    # A runtime file that is no longer Microsoft's (one byte changed).
    $tampered = Join-Path $root "vcruntime140_1-tampered.dll"
    Copy-Modified (Join-Path $runtimeDir "vcruntime140_1.dll") $tampered 0x1000
    $zip = Join-Path $root "tampered-runtime.zip"
    New-Zip $zip (@($standard | Where-Object { $_[0] -ne 'vcruntime140_1.dll' }) + , @('vcruntime140_1.dll', $tampered))
    Test-Verify "modified Visual C++ runtime DLL" $zip $false @("vcruntime140_1.dll is not a file signed by Microsoft")

    # An arn.exe that cannot run.
    $truncated = Join-Path $root "truncated-arn.exe"
    [IO.File]::WriteAllBytes($truncated, [IO.File]::ReadAllBytes($exe)[0..1023])
    $zip = Join-Path $root "broken-exe.zip"
    New-Zip $zip (@(, @('arn.exe', $truncated)) + @($standard | Where-Object { $_[0] -ne 'arn.exe' }))
    Test-Verify "arn.exe that is not a valid executable" $zip $false @("Cannot read the imports of arn.exe")

    $zip = Join-Path $root "not-a-zip.zip"
    Set-Content -LiteralPath $zip -Value "not a zip"
    Test-Verify "file that is not a ZIP" $zip $false @()

    # Verification leaves no extraction directories behind.
    $leftovers = @(Get-ChildItem -LiteralPath $tempBase -Directory -Filter 'arn-package-verify-*' |
        Where-Object { $_.LastWriteTime -gt (Get-Item -LiteralPath $root).CreationTime })
    $script:cases++
    if ($leftovers.Count) { $failures.Add("verification left directories: $($leftovers.Name -join ', ')") }
    else { Write-Host "ok: no extraction directories left behind" }
} finally {
    if ($root.StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "$cases cases, $($failures.Count) failure(s)"
if ($failures.Count) {
    $failures | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
