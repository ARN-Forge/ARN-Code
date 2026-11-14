param([Parameter(Mandatory = $true)][string]$BuildDir)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$root = Join-Path $tempBase ("arn-install-test-" + [guid]::NewGuid().ToString("N"))
$packageDir = Join-Path $root "package"
$archive = Join-Path $root "arn-windows-x64.zip"
$installDir = Join-Path $root "installed"
$projectDir = Join-Path $root "test-project"

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

# A Debug arn.exe needs the debug Visual C++ runtime, which cannot be
# redistributed, so it cannot be packaged: skip (CTest SKIP_RETURN_CODE 77).
. (Join-Path $repo "scripts/windows-runtime.ps1")
if (@(Get-PeImports (Join-Path $BuildDir "arn.exe") | Where-Object { $_ -match '^(vcruntime140(_1)?d|msvcp140d)' }).Count) {
    Write-Host "skip: arn.exe is a Debug build; its debug Visual C++ runtime cannot be redistributed"
    exit 77
}

try {
    New-Item -ItemType Directory -Path $root, $projectDir -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $projectDir "README.md") -Value "# Installer workspace"
    Set-Content -LiteralPath (Join-Path $projectDir "main.cpp") -Value "int main() { return 0; }"

    & (Join-Path $repo "scripts/package-windows.ps1") `
        -BuildDir $BuildDir -OutputDir $packageDir -ArchivePath $archive
    Assert-True ($LASTEXITCODE -eq 0) "Windows package creation failed."
    # The installer verifies a local archive against a SHA256SUMS as well.
    $checksums = Join-Path $root "SHA256SUMS"
    [IO.File]::WriteAllText($checksums,
        "$((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant())  arn-windows-x64.zip`n")

    # The package does not contain the Microsoft Visual C++ runtime; without it
    # the installer stops before anything is looked up or changed, and names
    # Microsoft's download. ARN_INSTALL_TEST_SYSTEM_DIR stands in for System32.
    $runtimeDlls = @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_atomic_wait.dll")
    foreach ($case in @(@{ Name = "no Visual C++ runtime"; Present = @() },
                        @{ Name = "an older Visual C++ runtime without msvcp140_atomic_wait.dll"; Present = $runtimeDlls[0..2] })) {
        $fakeSystem = Join-Path $root ("system-" + [guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Path $fakeSystem | Out-Null
        foreach ($dll in $case.Present) { [IO.File]::WriteAllText((Join-Path $fakeSystem $dll), "stand-in") }
        $missing = @($runtimeDlls | Where-Object { $case.Present -notcontains $_ })
        $refusal = $null
        $env:ARN_INSTALL_TEST_SYSTEM_DIR = $fakeSystem
        try {
            & (Join-Path $repo "scripts/install.ps1") -InstallDir $installDir -ArchivePath $archive -ChecksumsPath $checksums -PathScope None
        } catch {
            $refusal = $_.Exception.Message
        } finally {
            Remove-Item Env:\ARN_INSTALL_TEST_SYSTEM_DIR
        }
        Assert-True ($null -ne $refusal) "With $($case.Name) the installer did not stop."
        foreach ($fragment in @("Microsoft Visual C++ Redistributable", "https://aka.ms/vc14/vc_redist.x64.exe",
                                "System32 has no $($missing -join ', ')", "ARN Code was not installed.")) {
            Assert-True $refusal.Contains($fragment) "With $($case.Name) the message lacks '$fragment': $refusal"
        }
        Assert-True (-not (Test-Path -LiteralPath $installDir)) "With $($case.Name) the installer created $installDir."
        Write-Host "ok: with $($case.Name) the installer stops with Microsoft's link and changes nothing"
    }

    $oldPath = $env:Path
    try {
        $env:Path = Join-Path $env:SystemRoot "System32"
        & (Join-Path $repo "scripts/install.ps1") `
            -InstallDir $installDir -ArchivePath $archive -ChecksumsPath $checksums -PathScope Process
        Assert-True ($LASTEXITCODE -eq 0) "First local installation failed."
        Assert-True ((Get-Command arn -ErrorAction SilentlyContinue).Source -eq
                     (Join-Path $installDir "arn.exe")) "Installed arn was not found through PATH."

        Set-Content -LiteralPath (Join-Path $installDir "unrelated.txt") -Value "preserve me"
        & (Join-Path $repo "scripts/install.ps1") `
            -InstallDir $installDir -ArchivePath $archive -ChecksumsPath $checksums -PathScope Process
        Assert-True (Test-Path -LiteralPath (Join-Path $installDir "unrelated.txt")) `
            "A repeated install removed an unrelated file."

        Push-Location $projectDir
        try {
            $version = & arn --version
            Assert-True ($LASTEXITCODE -eq 0 -and
                         $version -match '^arn [0-9]+\.[0-9]+\.[0-9]+ \(([0-9a-f]{12}(-dirty)?|unknown)\)$') `
                "Installed command failed from an unrelated project directory."
            $server = New-Object Diagnostics.Process
            $server.StartInfo = New-Object Diagnostics.ProcessStartInfo
            $server.StartInfo.FileName = (Join-Path $installDir "arn.exe")
            $server.StartInfo.Arguments = "--server"
            $server.StartInfo.WorkingDirectory = $projectDir
            $server.StartInfo.UseShellExecute = $false
            $server.StartInfo.RedirectStandardInput = $true
            $server.StartInfo.RedirectStandardOutput = $true
            $server.StartInfo.RedirectStandardError = $true
            $server.StartInfo.CreateNoWindow = $true
            Assert-True $server.Start() "Installed server failed to launch."
            $server.StandardInput.Close()
            $serverOutput = $server.StandardOutput.ReadToEnd()
            Assert-True $server.WaitForExit(10000) "Installed server did not handle EOF."
            Assert-True ($server.ExitCode -eq 0) "Installed server smoke test failed."
            $ready = (($serverOutput.Trim() -split '\r?\n')[0]) | ConvertFrom-Json
            Assert-True ($ready.type -eq "ready" -and $ready.protocol -eq 2) `
                "Installed server did not start."
            Assert-True ([IO.Path]::GetFullPath($ready.workspace) -eq [IO.Path]::GetFullPath($projectDir)) `
                "Installed ARN used its installation directory instead of the current project."

            $process = New-Object Diagnostics.Process
            $process.StartInfo = New-Object Diagnostics.ProcessStartInfo
            $process.StartInfo.FileName = (Join-Path $installDir "arn.exe")
            $process.StartInfo.WorkingDirectory = $projectDir
            $process.StartInfo.UseShellExecute = $false
            $process.StartInfo.RedirectStandardInput = $true
            $process.StartInfo.RedirectStandardOutput = $true
            $process.StartInfo.RedirectStandardError = $true
            $process.StartInfo.CreateNoWindow = $true
            Assert-True $process.Start() "Installed ARN failed to launch."
            $process.StandardInput.Close()
            Assert-True $process.WaitForExit(10000) "Installed ARN did not handle redirected EOF."
            Assert-True ($process.ExitCode -eq 0) "Installed ARN launch smoke test failed."
        } finally {
            Pop-Location
        }
    } finally {
        $env:Path = $oldPath
    }

    $forbidden = @(Get-ChildItem -LiteralPath $installDir -Recurse -File | Where-Object {
        $_.Extension -in @('.cpp', '.hpp', '.obj', '.pdb') -or $_.Name -match 'test|diagnostic'
    })
    Assert-True ($forbidden.Count -eq 0) "Installer copied tests, sources, or diagnostics."
} finally {
    if ($root.StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}
