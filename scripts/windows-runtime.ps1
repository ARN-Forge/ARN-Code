# Shared by package-windows.ps1 and verify-windows-package.ps1 (dot-sourced).
# Reads PE imports and recognises the Microsoft Visual C++ runtime, so that
# packaging and verification derive the CLI package from the same rules.

# Lower-case names of the DLLs a PE32+ (x64) image imports, including
# delay-loaded ones. Throws when the file is not an x64 PE32+ image.
function Get-PeImports([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $name = [IO.Path]::GetFileName($Path)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "$name is not a Windows executable." }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -lt 0 -or $pe + 0x108 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) {
        throw "$name is not a valid PE image."
    }
    $machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
    if ($machine -ne 0x8664) { throw ("$name is not an x64 image (machine 0x{0:X4})." -f $machine) }
    $optional = $pe + 24
    if ([BitConverter]::ToUInt16($bytes, $optional) -ne 0x20B) { throw "$name is not a PE32+ image." }
    $count = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $sections = $optional + [BitConverter]::ToUInt16($bytes, $pe + 20)
    $toOffset = {
        param([uint32]$rva)
        for ($i = 0; $i -lt $count; $i++) {
            $s = $sections + $i * 40
            $va = [BitConverter]::ToUInt32($bytes, $s + 12)
            $size = [Math]::Max([BitConverter]::ToUInt32($bytes, $s + 8), [BitConverter]::ToUInt32($bytes, $s + 16))
            if ($rva -ge $va -and $rva -lt $va + $size) { return [BitConverter]::ToUInt32($bytes, $s + 20) + $rva - $va }
        }
        throw "$name has an invalid import table."
    }
    $names = New-Object System.Collections.Generic.List[string]
    # Data directory 1: import table (20-byte descriptors, name RVA at +12).
    # Data directory 13: delay-load table (32-byte descriptors, name RVA at +4).
    foreach ($directory in @(@(1, 20, 12), @(13, 32, 4))) {
        $rva = [BitConverter]::ToUInt32($bytes, $optional + 112 + $directory[0] * 8)
        if ($rva -eq 0) { continue }
        for ($at = & $toOffset $rva; [BitConverter]::ToUInt32($bytes, $at + $directory[2]) -ne 0; $at += $directory[1]) {
            $start = & $toOffset ([BitConverter]::ToUInt32($bytes, $at + $directory[2]))
            $end = [Array]::IndexOf($bytes, [byte]0, [int]$start)
            $names.Add([Text.Encoding]::ASCII.GetString($bytes, $start, $end - $start).ToLowerInvariant())
        }
    }
    return , $names.ToArray()
}

# API sets (api-ms-*, ext-ms-*) are resolved by Windows itself; the Universal
# CRT behind api-ms-win-crt-* is part of Windows 10 and later.
function Test-ApiSet([string]$Name) {
    return $Name -match '^(api|ext)-ms-'
}

# The Visual C++ runtime family (vcruntime, msvcp and its dot libraries,
# concrt, vccorlib, vcomp, vcamp, mfc). It is not part of Windows and ARN Code
# does not ship it: it is a prerequisite that users install from Microsoft,
# the Microsoft Visual C++ Redistributable ($VcRedistUrl).
function Test-VcRuntime([string]$Name) {
    return $Name -match '^(vcruntime|msvcp|concrt|vccorlib|vcomp|vcamp|mfcm?)1[0-9]{2}'
}

# The debug Visual C++ runtime (vcruntime140d.dll, msvcp140d.dll, ...) and the
# debug Universal CRT: no Redistributable installs them.
function Test-DebugVcRuntime([string]$Name) {
    return $Name -match '^((vcruntime|msvcp|concrt|vccorlib|vcomp|vcamp|mfcm?)1[0-9]{2}(_[0-9a-z_]+)?d|ucrtbased)\.dll$'
}

# Microsoft's permanent link to the latest supported Visual C++ v14
# Redistributable for x64 (Visual Studio 2017-2026):
# https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist
$VcRedistUrl = "https://aka.ms/vc14/vc_redist.x64.exe"

# True for the OpenSSL DLLs of a Windows build (libcrypto-3-x64.dll, libssl-4-x64.dll, ...).
function Test-OpenSslDll([string]$Name) {
    return $Name -match '^lib(crypto|ssl)-[0-9]+(-x64)?\.dll$'
}

# Compares the OpenSSL version that THIRD-PARTY-NOTICES.txt names with the
# packaged OpenSSL DLLs. The notices reproduce the copyright notice and
# license text of one exact OpenSSL release ("... of OpenSSL X.Y.Z"); a
# build with another OpenSSL would ship notices for a different version.
# Returns a description of the mismatch, or $null when they agree.
function Get-OpenSslNoticesProblem([string]$NoticesPath, [string[]]$DllPaths) {
    $text = [IO.File]::ReadAllText($NoticesPath)
    $named = @([regex]::Matches($text, '\bOpenSSL ([0-9]+\.[0-9]+\.[0-9]+)\b') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
    $packaged = @($DllPaths | ForEach-Object {
        $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($_).ProductVersion
        if ($version) { $version.Trim() } else { "(no version) $([IO.Path]::GetFileName($_))" }
    } | Sort-Object -Unique)
    if ($named.Count -eq 1 -and $packaged.Count -eq 1 -and $named[0] -eq $packaged[0]) { return $null }
    return "THIRD-PARTY-NOTICES.txt names OpenSSL $(if ($named.Count) { $named -join ', ' } else { '(no X.Y.Z version)' }), but the packaged OpenSSL DLLs are version $($packaged -join ', ')"
}
