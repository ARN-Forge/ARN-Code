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

$oldPath = $env:Path
try {
    $env:Path = Join-Path $env:SystemRoot "System32"
    $version = & (Join-Path $OutputDir "arn.exe") --version
    if ($LASTEXITCODE -ne 0 -or $version -notmatch '^arn [0-9]+\.[0-9]+\.[0-9]+') {
        throw "Packaged arn.exe failed with only Windows System32 on PATH."
    }
} finally {
    $env:Path = $oldPath
}

if ($ArchivePath) {
    $ArchivePath = [IO.Path]::GetFullPath($ArchivePath)
    Remove-Item -LiteralPath $ArchivePath -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $OutputDir '*') -DestinationPath $ArchivePath
}
Write-Host "Packaged $version with $(@(Get-ChildItem $OutputDir -Filter '*.dll').Count) runtime DLL(s)."
