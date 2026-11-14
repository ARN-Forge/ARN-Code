param(
    [Parameter(Mandatory = $true)][string]$FixtureDir,
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$Version
)

# Tests scripts/verify-ide-package.ps1 with portable ZIPs and installer
# payload directories built from stand-in executables
# (tests/ide_package_fixture.cpp): ide_fixture_current prints
# "arn <Version> (0123456789ab)" and has an "ARN IDE" <Version> version
# resource; ide_fixture_dirty prints a "-dirty" commit; ide_fixture_old has
# version 0.0.1. Valid packages pass; a wrong version or commit, a dirty
# build, an older IDE, missing or empty files, an unexpected or nested entry
# and a missing Visual C++ runtime DLL are rejected.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $repo "scripts/windows-runtime.ps1")
$verify = Join-Path $repo "scripts/verify-ide-package.ps1"

$current = Join-Path $FixtureDir "ide_fixture_current.exe"
$dirty = Join-Path $FixtureDir "ide_fixture_dirty.exe"
$old = Join-Path $FixtureDir "ide_fixture_old.exe"
foreach ($file in @($current, $dirty, $old)) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Fixture '$file' not found; build the ide_fixture_* targets." }
}
# Get-PeImports returns its list as one object; assigning it unrolls it.
$fixtureImports = Get-PeImports $current
if (@($fixtureImports | Where-Object { $_ -match '^(vcruntime140(_1)?d|msvcp140d|ucrtbased)' }).Count) {
    Write-Host "skip: the fixtures are a Debug build; its debug Visual C++ runtime cannot be redistributed"
    exit 77
}
$runtimeDir = Get-CMakeRuntimeDir (Resolve-Path -LiteralPath $BuildDir).Path
if (-not $runtimeDir -or -not (Test-Path -LiteralPath (Join-Path $runtimeDir "vcruntime140.dll"))) {
    throw "ARN_MSVC_RUNTIME_DIR is not set or not found ('$runtimeDir'); configure with MSVC."
}
$crt = @($fixtureImports | Where-Object { Test-VcRuntime $_ })
if (-not $crt.Count) { throw "The fixture imports no Visual C++ runtime DLL; the missing-runtime case would test nothing." }

$commit = "0123456789abcdef0123456789abcdef01234567"
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$root = Join-Path $tempBase ("arn-ide-package-test-" + [guid]::NewGuid().ToString("N"))
$failures = New-Object System.Collections.Generic.List[string]
$cases = 0

# A valid package directory: name -> source file (or text for plain files).
function New-Layout([string]$Dir, [hashtable]$Changes = @{}) {
    $files = [ordered]@{
        'arn.exe' = $current; 'arn-ide.exe' = $current
        'LICENSE' = 'text:MIT License'; 'README.md' = 'text:# ARN'; 'IDE-README.md' = 'text:# ARN IDE'
        'THIRD-PARTY-NOTICES.txt' = 'text:ARN Code - third-party notices'
        'THIRD-PARTY-NOTICES-IDE.txt' = 'text:ARN IDE for Windows - third-party notices'
    }
    foreach ($dll in $crt) { $files[$dll] = Join-Path $runtimeDir $dll }
    foreach ($key in $Changes.Keys) { if ($null -eq $Changes[$key]) { $files.Remove($key) } else { $files[$key] = $Changes[$key] } }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    foreach ($name in $files.Keys) {
        $target = Join-Path $Dir $name
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        $source = $files[$name]
        if ($source.StartsWith('text:')) { [IO.File]::WriteAllText($target, $source.Substring(5)) }
        elseif ($source -eq 'empty') { [IO.File]::WriteAllBytes($target, [byte[]]@()) }
        else { Copy-Item -LiteralPath $source -Destination $target }
    }
    return $Dir
}

# Zips every file below $Dir with its relative path as the entry name.
function New-Zip([string]$Dir, [string]$Zip) {
    $archive = [IO.Compression.ZipFile]::Open($Zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in @(Get-ChildItem -LiteralPath $Dir -Recurse -File)) {
            $entry = $file.FullName.Substring($Dir.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry)
        }
    } finally { $archive.Dispose() }
    return $Zip
}

function Test-Case([string]$Description, [hashtable]$Arguments, [bool]$ShouldPass, [string]$Fragment) {
    $script:cases++
    $message = ""
    $passed = $true
    try { & $verify @Arguments | Out-Null } catch { $passed = $false; $message = $_.Exception.Message }
    if ($passed -ne $ShouldPass) {
        $failures.Add("${Description}: expected $(if ($ShouldPass) { 'success' } else { 'failure' }), got $(if ($passed) { 'success' } else { "failure: $message" })")
    } elseif (-not $ShouldPass -and $message.IndexOf($Fragment, [StringComparison]::Ordinal) -lt 0) {
        $failures.Add("${Description}: message lacks '$Fragment': $message")
    } else {
        Write-Host "ok: $Description"
    }
}

$n = 0
function Zip-Case([string]$Description, [hashtable]$Changes, [bool]$ShouldPass, [string]$Fragment,
                  [string]$ExpectedVersion = $Version, [string]$ExpectedCommit = $commit) {
    $script:n++
    $dir = New-Layout (Join-Path $root "zip$script:n") $Changes
    $zip = New-Zip $dir (Join-Path $root "zip$script:n.zip")
    Test-Case $Description @{ ZipPath = $zip; ExpectedVersion = $ExpectedVersion; ExpectedCommit = $ExpectedCommit } $ShouldPass $Fragment
}

try {
    New-Item -ItemType Directory -Path $root | Out-Null

    Zip-Case "valid portable ZIP" @{} $true ""
    Zip-Case "wrong expected version" @{} $false "expected 'arn 9.9.9 (0123456789ab)'" -ExpectedVersion "9.9.9"
    Zip-Case "wrong expected commit" @{} $false "expected 'arn $Version (ffffffffffff)'" -ExpectedCommit "ffffffffffffffffffffffffffffffffffffffff"
    Zip-Case "dirty arn.exe build" @{ 'arn.exe' = $dirty } $false "(0123456789ab-dirty)"
    Zip-Case "IDE with an older version resource" @{ 'arn-ide.exe' = $old } $false "ProductVersion '0.0.1'"
    Zip-Case "missing arn-ide.exe" @{ 'arn-ide.exe' = $null } $false "missing 'arn-ide.exe'"
    Zip-Case "missing arn.exe" @{ 'arn.exe' = $null } $false "missing 'arn.exe'"
    Zip-Case "missing IDE third-party notices" @{ 'THIRD-PARTY-NOTICES-IDE.txt' = $null } $false "missing 'THIRD-PARTY-NOTICES-IDE.txt'"
    Zip-Case "missing CLI third-party notices" @{ 'THIRD-PARTY-NOTICES.txt' = $null } $false "missing 'THIRD-PARTY-NOTICES.txt'"
    Zip-Case "empty third-party notices" @{ 'THIRD-PARTY-NOTICES.txt' = 'empty' } $false "'THIRD-PARTY-NOTICES.txt' is empty"
    Zip-Case "missing Visual C++ runtime" @{ $crt[0] = $null } $false "$($crt[0]) (Visual C++ runtime"
    Zip-Case "unexpected file" @{ 'notes.txt' = 'text:extra' } $false "unexpected entry 'notes.txt'"
    Zip-Case "nested entry" @{ 'sub/arn.exe' = $current } $false "entry 'sub/arn.exe' is not a plain file name"
    Zip-Case "unused DLL" @{ 'unused.dll' = (Join-Path $runtimeDir $crt[0]) } $false "Packaged DLLs that neither arn.exe nor arn-ide.exe uses: unused.dll"
    Zip-Case "invalid expected commit" @{} $false "-ExpectedCommit must be 12-40 hexadecimal digits" -ExpectedCommit "abc"

    # Installer payload directories (7-Zip extraction of the NSIS setup).
    $payload = New-Layout (Join-Path $root "payload") @{ 'LICENSE' = $null; 'README.md' = $null; 'IDE-README.md' = $null
                                                          'uninstall.exe' = $current; '$PLUGINSDIR/System.dll' = 'text:plugin' }
    Test-Case "valid installer payload" @{ Directory = $payload; ExpectedVersion = $Version; ExpectedCommit = $commit } $true ""
    $bad = New-Layout (Join-Path $root "payload-bad") @{ 'arn.exe' = $null }
    Test-Case "installer payload without arn.exe" @{ Directory = $bad; ExpectedVersion = $Version; ExpectedCommit = $commit } $false "missing 'arn.exe'"
    $dirtyPayload = New-Layout (Join-Path $root "payload-dirty") @{ 'arn.exe' = $dirty }
    Test-Case "installer payload with a dirty build" @{ Directory = $dirtyPayload; ExpectedVersion = $Version; ExpectedCommit = $commit } $false "-dirty)"
} finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}

Write-Host "$cases cases, $($failures.Count) failure(s)"
foreach ($failure in $failures) { Write-Host "FAIL: $failure" }
if ($failures.Count) { exit 1 }
exit 0
