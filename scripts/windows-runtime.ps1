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
# concrt, vccorlib, vcomp, vcamp, mfc). These are never part of Windows: they
# must ship with the CLI, taken from the Visual Studio redistributable folder.
function Test-VcRuntime([string]$Name) {
    return $Name -match '^(vcruntime|msvcp|concrt|vccorlib|vcomp|vcamp|mfcm?)1[0-9]{2}'
}

# Throws unless the file has a valid Authenticode signature from Microsoft.
function Assert-MicrosoftSigned([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { "" }
    if ($signature.Status -ne 'Valid' -or $subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw "$([IO.Path]::GetFileName($Path)) is not a file signed by Microsoft (signature $($signature.Status), signer '$subject')."
    }
}

# Reads ARN_MSVC_RUNTIME_DIR, the Visual Studio redistributable CRT folder that
# CMake found for the compiler, from the CMakeCache.txt of a build directory.
function Get-CMakeRuntimeDir([string]$BuildDir) {
    foreach ($candidate in @($BuildDir, (Split-Path -Parent $BuildDir))) {
        $cache = Join-Path $candidate "CMakeCache.txt"
        if (Test-Path -LiteralPath $cache) {
            $line = Select-String -LiteralPath $cache -Pattern '^ARN_MSVC_RUNTIME_DIR:[A-Z]+=(.*)$' | Select-Object -First 1
            if ($line) { return $line.Matches[0].Groups[1].Value.Trim() }
            return ""
        }
    }
    return ""
}
