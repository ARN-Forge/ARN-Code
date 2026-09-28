[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Arn\bin"),
    [string]$Repository = "ARN-Forge/ARN-Code",
    [string]$ArchivePath = $env:ARN_INSTALL_ARCHIVE,
    [ValidateSet("User", "Process", "None")]
    [string]$PathScope = "User"
)

$ErrorActionPreference = "Stop"

if ($Repository -notmatch "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$") {
    throw "Repository must have the form owner/name."
}

$InstallDir = [IO.Path]::GetFullPath($InstallDir)
$temporaryDir = Join-Path ([IO.Path]::GetTempPath()) ("arn-install-" + [guid]::NewGuid().ToString("N"))
$temporaryZip = Join-Path $temporaryDir "arn-windows-x64.zip"
$extractDir = Join-Path $temporaryDir "payload"
$headers = @{ "User-Agent" = "ARN-Code-Installer" }

function Add-ArnToPath([string]$Directory, [string]$Scope) {
    if ($Scope -eq "None") { return }

    if ($Scope -eq "User") {
        $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
        $entries = @($userPath -split ';' | Where-Object { $_ })
        $otherEntries = @($entries | Where-Object {
            -not [string]::Equals($_.TrimEnd('\'), $Directory.TrimEnd('\'),
                                 [StringComparison]::OrdinalIgnoreCase)
        })
        if ($otherEntries.Count -ne $entries.Count -or
            $entries.Count -eq 0 -or $entries[0] -ne $Directory) {
            [Environment]::SetEnvironmentVariable("Path", (@($Directory) + $otherEntries) -join ';', "User")
            Write-Host "Added $Directory to your user PATH."
        }
    }

    $processEntries = @($env:Path -split ';' | Where-Object { $_ })
    $otherProcessEntries = @($processEntries | Where-Object {
        -not [string]::Equals($_.TrimEnd('\'), $Directory.TrimEnd('\'),
                             [StringComparison]::OrdinalIgnoreCase)
    })
    $env:Path = (@($Directory) + $otherProcessEntries) -join ';'
}

try {
    New-Item -ItemType Directory -Path $temporaryDir, $extractDir -Force | Out-Null
    if ($ArchivePath) {
        $resolvedArchive = (Resolve-Path -LiteralPath $ArchivePath).Path
        Copy-Item -LiteralPath $resolvedArchive -Destination $temporaryZip
        Write-Host "Installing ARN Code from local package..."
    } else {
        $release = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$Repository/releases/latest"
        $asset = $release.assets | Where-Object { $_.name -eq "arn-windows-x64.zip" } | Select-Object -First 1
        if (-not $asset) { throw "The latest release does not contain arn-windows-x64.zip." }
        Write-Host "Downloading ARN Code $($release.tag_name)..."
        Invoke-WebRequest -Headers $headers -Uri $asset.browser_download_url -OutFile $temporaryZip
    }

    Expand-Archive -LiteralPath $temporaryZip -DestinationPath $extractDir -Force
    $payloadRoot = $extractDir
    if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot "arn.exe"))) {
        $children = @(Get-ChildItem -LiteralPath $extractDir -Directory)
        if ($children.Count -eq 1 -and (Test-Path -LiteralPath (Join-Path $children[0].FullName "arn.exe"))) {
            $payloadRoot = $children[0].FullName
        }
    }
    $stagedExecutable = Join-Path $payloadRoot "arn.exe"
    if (-not (Test-Path -LiteralPath $stagedExecutable)) {
        throw "The package does not contain arn.exe."
    }

    $version = & $stagedExecutable --version
    if ($LASTEXITCODE -ne 0 -or $version -notmatch '^arn [0-9]+\.[0-9]+\.[0-9]+') {
        throw "The downloaded ARN executable failed its version check."
    }

    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    $manifestPath = Join-Path $InstallDir "install-manifest.json"
    if (Test-Path -LiteralPath $manifestPath) {
        foreach ($name in @(Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json)) {
            if ($name -is [string] -and [IO.Path]::GetFileName($name) -eq $name) {
                Remove-Item -LiteralPath (Join-Path $InstallDir $name) -Force -ErrorAction SilentlyContinue
            }
        }
    }

    $files = @(Get-ChildItem -LiteralPath $payloadRoot -File)
    foreach ($file in $files) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $InstallDir $file.Name) -Force
    }
    (@($files | ForEach-Object Name) + @("install-manifest.json")) | ConvertTo-Json |
        Set-Content -LiteralPath $manifestPath -Encoding utf8

    Add-ArnToPath $InstallDir $PathScope
    $installedExecutable = Join-Path $InstallDir "arn.exe"
    $installedVersion = & $installedExecutable --version
    if ($LASTEXITCODE -ne 0 -or $installedVersion -ne $version) {
        throw "ARN was copied but failed its installed version check."
    }
    if ($PathScope -ne "None") {
        $discovered = Get-Command arn -CommandType Application -ErrorAction SilentlyContinue
        if (-not $discovered -or
            -not [string]::Equals([IO.Path]::GetFullPath($discovered.Source), $installedExecutable,
                                  [StringComparison]::OrdinalIgnoreCase)) {
            throw "ARN was installed, but command discovery did not resolve to the new installation."
        }
    }

    Write-Host "$installedVersion installed to $InstallDir"
    if ($PathScope -eq "User") { Write-Host "Open a new terminal, then run: arn" }
    else { Write-Host "Run: arn" }
} finally {
    Remove-Item -LiteralPath $temporaryDir -Recurse -Force -ErrorAction SilentlyContinue
}
