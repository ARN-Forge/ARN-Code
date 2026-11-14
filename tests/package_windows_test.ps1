param([Parameter(Mandatory = $true)][string]$BuildDir)

# Tests scripts/package-windows.ps1 and scripts/verify-windows-package.ps1 with
# ZIPs built in a temporary directory from the real arn.exe and its DLLs. The
# Microsoft Visual C++ runtime is a prerequisite that users install from
# Microsoft: a package must not contain it.
# Exits 77 (skipped) for a Debug build: it needs the debug runtime, which no
# Redistributable installs.

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
    Write-Host "skip: arn.exe is a Debug build; it needs the debug Visual C++ runtime, which no Redistributable installs"
    exit 77
}
# This machine's Visual C++ runtime (the Redistributable), used only to build
# packages that wrongly contain it.
$system = Join-Path $env:SystemRoot "System32"
if (-not (Test-Path -LiteralPath (Join-Path $system "vcruntime140.dll"))) {
    throw "This machine has no Visual C++ runtime in System32; install the Visual C++ Redistributable to run arn.exe."
}

$line = & $exe --version
if ($line -notmatch '^arn ([0-9]+\.[0-9]+\.[0-9]+) \(([0-9a-f]{12}(-dirty)?|unknown)\)$') {
    throw "Unexpected arn --version output '$line'."
}
$version = $Matches[1]
$commit = $Matches[2]

# The Visual C++ runtime files arn.exe and its packaged DLLs need, worked out
# here independently of package-windows.ps1 by following imports through the
# build directory. They are prerequisites, not package contents.
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
# with a "|" gutter at the start of each continuation line, and it may be
# coloured with ANSI escape sequences (as on GitHub's runners). The colours,
# the gutter and the line breaks are taken out, so a message reads the same as
# in Windows PowerShell 5.1, whatever the width and the terminal.
function ConvertTo-PlainText([object[]]$Lines) {
    $text = (@($Lines | ForEach-Object { "$_" }) -join " ") -replace '\x1b\[[0-9;?]*[ -/]*[@-~]', ''
    return ($text -replace '\s+\|\s+', ' ') -replace '\s+', ' '
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
    # The OpenSSL DLLs as this build names them (libcrypto-3-x64.dll with
    # OpenSSL 3, libcrypto-4-x64.dll with OpenSSL 4, ...).
    $cryptoDll = @($standard | ForEach-Object { $_[0] } | Where-Object { $_ -match '^libcrypto-[0-9]+(-x64)?\.dll$' })
    $sslDll = @($standard | ForEach-Object { $_[0] } | Where-Object { $_ -match '^libssl-[0-9]+(-x64)?\.dll$' })
    if ($cryptoDll.Count -ne 1 -or $sslDll.Count -ne 1) {
        throw "Expected exactly one libcrypto and one libssl DLL in the package, found: $($packagedNames -join ', ')."
    }
    $cryptoDll = $cryptoDll[0]
    $sslDll = $sslDll[0]

    # The package contains no Visual C++ runtime file; arn.exe needs it.
    $script:cases++
    $packagedRuntime = @($packagedNames | Where-Object { (Test-VcRuntime $_) -or (Test-DebugVcRuntime $_) } | Sort-Object)
    if ($packagedRuntime.Count) {
        $failures.Add("the package contains the Visual C++ runtime: $($packagedRuntime -join ', ')")
    } elseif (-not $expectedRuntime.ContainsKey('vcruntime140.dll')) {
        $failures.Add("arn.exe does not import vcruntime140.dll; the prerequisite cases would test nothing")
    } else {
        Write-Host "ok: the package contains no Visual C++ runtime file (arn.exe needs $(@($expectedRuntime.Keys | Sort-Object) -join ', ') as a prerequisite)"
    }

    Test-Verify "valid package without expectations" $packageZip $true @(
        "Extracted arn.exe: arn $version",
        # Transitive dependency: libcrypto through libssl.
        "$cryptoDll (imported by $sslDll)",
        # The Visual C++ runtime is a prerequisite, reported as such.
        "prerequisite:", "vcruntime140.dll (imported by arn.exe)",
        # Loaded by Windows from the extracted directory, with only System32 on PATH.
        "loaded from the package: $sslDll")
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

    # THIRD-PARTY-NOTICES.txt must name the packaged OpenSSL version. The
    # repository's notices describe the release build's OpenSSL, which a local
    # build may not use, so the cases below rewrite the version they name.
    $openSslVersion = (Get-Item -LiteralPath (Join-Path $packageDir $cryptoDll)).VersionInfo.ProductVersion.Trim()
    $noticesText = [IO.File]::ReadAllText((Join-Path $packageDir 'THIRD-PARTY-NOTICES.txt'))
    function New-NoticesZip([string]$Name, [string]$Version) {
        $notices = Join-Path $root "$Name-THIRD-PARTY-NOTICES.txt"
        [IO.File]::WriteAllText($notices, [regex]::Replace($noticesText, '\bOpenSSL [0-9]+\.[0-9]+\.[0-9]+\b', "OpenSSL $Version"))
        $zip = Join-Path $root "$Name.zip"
        New-Zip $zip @($standard | ForEach-Object { if ($_[0] -eq 'THIRD-PARTY-NOTICES.txt') { , @($_[0], $notices) } else { , $_ } })
        return $zip
    }
    $matchingZip = New-NoticesZip "notices-matching" $openSslVersion
    Test-Verify "notices naming the packaged OpenSSL version" $matchingZip $true `
        @("THIRD-PARTY-NOTICES.txt names the packaged OpenSSL version") @('-RequireNoticesMatch')
    $otherZip = New-NoticesZip "notices-other-version" "9.8.7"
    Test-Verify "notices naming another OpenSSL version" $otherZip $false `
        @("THIRD-PARTY-NOTICES.txt names OpenSSL 9.8.7, but the packaged OpenSSL DLLs are version $openSslVersion") @('-RequireNoticesMatch')
    Test-Verify "another OpenSSL version without -RequireNoticesMatch only warns" $otherZip $true `
        @("a release package must not be built this way")
    $twoVersions = Join-Path $root "notices-two-versions-THIRD-PARTY-NOTICES.txt"
    [IO.File]::WriteAllText($twoVersions, $noticesText + "`nOpenSSL $openSslVersion`nOpenSSL 9.8.7`n")
    $twoZip = Join-Path $root "notices-two-versions.zip"
    New-Zip $twoZip @($standard | ForEach-Object { if ($_[0] -eq 'THIRD-PARTY-NOTICES.txt') { , @($_[0], $twoVersions) } else { , $_ } })
    Test-Verify "notices naming two OpenSSL versions" $twoZip $false @("but the packaged OpenSSL DLLs are version") @('-RequireNoticesMatch')

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

    # package-windows.ps1 never packages the Visual C++ runtime, even when the
    # build directory holds a copy of it.
    $script:cases++
    $buildCopy = Join-Path $root "build-with-runtime"
    New-Item -ItemType Directory -Path $buildCopy | Out-Null
    Copy-Item -LiteralPath $exe -Destination $buildCopy
    foreach ($dll in @(Get-ChildItem -LiteralPath $BuildDir -File -Filter '*.dll')) { Copy-Item -LiteralPath $dll.FullName -Destination $buildCopy }
    foreach ($dll in @($expectedRuntime.Keys)) { Copy-Item -LiteralPath (Join-Path $system $dll) -Destination $buildCopy -Force }
    $copyPackage = Join-Path $root "package-from-copy"
    & $packageScript -BuildDir $buildCopy -OutputDir $copyPackage -ArchivePath (Join-Path $root "package-from-copy.zip") | Out-Null
    $copied = @(Get-ChildItem -LiteralPath $copyPackage -File | Where-Object { Test-VcRuntime $_.Name } | ForEach-Object Name)
    if ($copied.Count) { $failures.Add("package-windows.ps1 packaged the Visual C++ runtime from the build directory: $($copied -join ', ')") }
    else { Write-Host "ok: package-windows.ps1 leaves the Visual C++ runtime out, even when the build directory has it" }

    # Malformed packages.
    $zip = Join-Path $root "missing.zip"
    New-Zip $zip @($standard | Where-Object { $_[0] -ne 'LICENSE' })
    Test-Verify "missing LICENSE" $zip $false @("missing 'LICENSE'")

    $zip = Join-Path $root "missing-notices.zip"
    New-Zip $zip @($standard | Where-Object { $_[0] -ne 'THIRD-PARTY-NOTICES.txt' })
    Test-Verify "missing third-party notices" $zip $false @("missing 'THIRD-PARTY-NOTICES.txt'")

    $zip = Join-Path $root "extra.zip"
    New-Zip $zip ($standard + , @('notes.txt', 'extra'))
    Test-Verify "unexpected file" $zip $false @("unexpected entry 'notes.txt'")

    $zip = Join-Path $root "extra-dll.zip"
    New-Zip $zip ($standard + , @('unrelated.dll', (Join-Path $packageDir $sslDll)))
    Test-Verify "unexpected DLL" $zip $false @("Packaged DLLs that arn.exe does not use: unrelated.dll")

    $zip = Join-Path $root "with-runtime.zip"
    New-Zip $zip ($standard + , @('vcruntime140.dll', (Join-Path $system 'vcruntime140.dll')))
    Test-Verify "package containing the Visual C++ runtime" $zip $false `
        @("The package contains the Microsoft Visual C++ runtime (vcruntime140.dll); it must not be shipped", "https://aka.ms/vc14/vc_redist.x64.exe")

    $zip = Join-Path $root "with-debug-runtime.zip"
    New-Zip $zip ($standard + , @('vcruntime140d.dll', 'not a real DLL'))
    Test-Verify "package containing the debug Visual C++ runtime" $zip $false `
        @("The package contains the Microsoft Visual C++ runtime (vcruntime140d.dll)")

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

    $zip = Join-Path $root "missing-openssl.zip"
    New-Zip $zip @($standard | Where-Object { $_[0] -ne $cryptoDll })
    Test-Verify "missing transitive dependency" $zip $false @(
        "$cryptoDll (imported by $sslDll)", "not packaged and not part of Windows")

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
