[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Arn\bin"),
    [string]$Repository = "ARN-Forge/ARN-Code"
)

$ErrorActionPreference = "Stop"

if ($Repository -notmatch "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$") {
    throw "Repository must have the form owner/name."
}

$headers = @{ "User-Agent" = "Arn-Agent-Code-Installer" }
$release = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$Repository/releases/latest"
$asset = $release.assets | Where-Object { $_.name -eq "arn-windows-x64.zip" } | Select-Object -First 1
if (-not $asset) {
    throw "The latest release does not contain arn-windows-x64.zip."
}

$temporaryZip = Join-Path $env:TEMP "arn-windows-x64.zip"
$temporaryDir = Join-Path $env:TEMP "arn-install"
Remove-Item -LiteralPath $temporaryZip -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $temporaryDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "Downloading ARN $($release.tag_name)..."
Invoke-WebRequest -Headers $headers -Uri $asset.browser_download_url -OutFile $temporaryZip
Expand-Archive -LiteralPath $temporaryZip -DestinationPath $temporaryDir -Force

New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
Copy-Item -Path (Join-Path $temporaryDir "*") -Destination $InstallDir -Recurse -Force

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
$pathEntries = @($userPath -split ';' | Where-Object { $_ })
if ($pathEntries -notcontains $InstallDir) {
    [Environment]::SetEnvironmentVariable("Path", ($pathEntries + $InstallDir) -join ';', "User")
    $env:Path += ";$InstallDir"
    Write-Host "Added $InstallDir to your user PATH. Open a new terminal after installation."
}

Write-Host "ARN installed. Run: arn"
