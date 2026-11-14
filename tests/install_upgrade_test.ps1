param([Parameter(Mandatory = $true)][string]$BuildDir)

# Tests upgrade, rollback, the installation manifest and uninstall of
# scripts/install.ps1 with the real Windows package built from $BuildDir.
# Everything happens in temporary directories; PATH changes use the process
# scope only, so the user's PATH and installation are never touched.
# Exits 77 (skipped) for a Debug build, which cannot be packaged.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $repo "scripts/windows-runtime.ps1")
$installer = Join-Path $repo "scripts/install.ps1"
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
if (@(Get-PeImports (Join-Path $BuildDir "arn.exe") | Where-Object { $_ -match '^(vcruntime140(_1)?d|msvcp140d)' }).Count) {
    Write-Host "skip: arn.exe is a Debug build; its debug Visual C++ runtime cannot be redistributed"
    exit 77
}

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$root = Join-Path $tempBase ("arn-install-upgrade-test-" + [guid]::NewGuid().ToString("N"))
$failures = New-Object System.Collections.Generic.List[string]
$cases = 0

function Fail([string]$Message) { $failures.Add($Message) }
function Pass([string]$Name) { Write-Host "ok: $Name" }

# Builds <name>.zip from a directory and a SHA256SUMS for it; returns both paths.
function New-Release([string]$Name, [string]$SourceDir) {
    $dir = Join-Path $root "releases\$Name"
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $zip = Join-Path $dir "arn-windows-x64.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory($SourceDir, $zip)
    $sums = Join-Path $dir "SHA256SUMS"
    [IO.File]::WriteAllText($sums, "$((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant())  arn-windows-x64.zip`n")
    return [pscustomobject]@{ Zip = $zip; Sums = $sums }
}

# Runs install.ps1 in this process; returns @{ Ok; Output }.
function Invoke-Installer([hashtable]$Arguments) {
    try {
        $output = (& $installer -PathScope None @Arguments *>&1 | ForEach-Object { "$_" }) -join " "
        return [pscustomobject]@{ Ok = $true; Output = $output }
    } catch {
        return [pscustomobject]@{ Ok = $false; Output = $_.Exception.Message }
    }
}

# Every file under $Dir (recursively) with its hash, as one comparable string.
function Get-Snapshot([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir)) { return "<missing>" }
    return (@(Get-ChildItem -LiteralPath $Dir -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($Dir.Length)
        if ($_.PSIsContainer) { "$relative/" } else { "$relative=$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)" }
    }) -join "`n")
}

function Get-Manifest([string]$Dir) {
    return [IO.File]::ReadAllText((Join-Path $Dir "install-manifest.json")) | ConvertFrom-Json
}

function Get-WorkDirs([string]$Dir) {
    return @(Get-ChildItem -LiteralPath (Split-Path -Parent $Dir) -Directory -Force |
             Where-Object { $_.Name -match '^\.arn-(staging|backup)-' })
}

# Expects a refused or failed run that left $Dir exactly as $Before.
function Test-Unchanged([string]$Name, $Result, [string]$Dir, [string]$Before, [string]$Fragment) {
    $script:cases++
    if ($Result.Ok) { Fail "${Name}: expected failure, but it succeeded: $($Result.Output)"; return }
    if ($Fragment -and $Result.Output.IndexOf($Fragment, [StringComparison]::Ordinal) -lt 0) {
        Fail "${Name}: message lacks '$Fragment': $($Result.Output)"; return
    }
    if ((Get-Snapshot $Dir) -cne $Before) { Fail "${Name}: the installation changed"; return }
    if ((Get-WorkDirs $Dir).Count) { Fail "${Name}: staging or backup directories were left behind"; return }
    Pass $Name
}

$savedPath = $env:Path
try {
    New-Item -ItemType Directory -Path $root | Out-Null

    # Two releases built from the real package: v1 has an extra file that v2
    # no longer ships; v2 has a different README.md.
    $package = Join-Path $root "package"
    & (Join-Path $repo "scripts/package-windows.ps1") -BuildDir $BuildDir -OutputDir $package `
        -ArchivePath (Join-Path $root "package.zip") | Out-Null
    $v1Dir = Join-Path $root "v1"
    $v2Dir = Join-Path $root "v2"
    Copy-Item -LiteralPath $package -Destination $v1Dir -Recurse
    Copy-Item -LiteralPath $package -Destination $v2Dir -Recurse
    Set-Content -LiteralPath (Join-Path $v1Dir "old-notes.txt") -Value "only in v1"
    Add-Content -LiteralPath (Join-Path $v2Dir "README.md") -Value "Release two."
    $v1 = New-Release "v1" $v1Dir
    $v2 = New-Release "v2" $v2Dir
    $corruptDir = Join-Path $root "releases\corrupt"
    New-Item -ItemType Directory -Path $corruptDir | Out-Null
    $corruptZip = Join-Path $corruptDir "arn-windows-x64.zip"
    [IO.File]::WriteAllBytes($corruptZip, [byte[]](1..200))
    $corruptSums = Join-Path $corruptDir "SHA256SUMS"
    [IO.File]::WriteAllText($corruptSums, "$((Get-FileHash -LiteralPath $corruptZip -Algorithm SHA256).Hash.ToLowerInvariant())  arn-windows-x64.zip`n")
    $badSums = Join-Path $root "bad-SHA256SUMS"
    [IO.File]::WriteAllText($badSums, "$("0" * 64)  arn-windows-x64.zip`n")
    $v1Names = @(Get-ChildItem -LiteralPath $v1Dir -File | ForEach-Object Name | Sort-Object)
    $v2Names = @(Get-ChildItem -LiteralPath $v2Dir -File | ForEach-Object Name | Sort-Object)

    $dir = Join-Path $root "apps\Arn\bin"

    # Fresh install.
    $script:cases++
    $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums }
    $m = if (Test-Path -LiteralPath (Join-Path $dir "install-manifest.json")) { Get-Manifest $dir } else { $null }
    if (-not $r.Ok) { Fail "fresh install failed: $($r.Output)" }
    elseif (-not $m -or $m.format -ne "arn-install-manifest/1" -or (@($m.files) -join ",") -ne ($v1Names -join ",") -or
            $m.installDir -ne $dir -or $m.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -or $m.pathScope -ne "None" -or $m.addedPathEntry) {
        Fail "fresh install: unexpected manifest $($m | ConvertTo-Json -Compress)"
    } elseif ((Get-WorkDirs $dir).Count) { Fail "fresh install left work directories" }
    else { Pass "fresh install writes a format-1 manifest of exactly the package files" }

    # User files survive an upgrade; files only v1 shipped are removed.
    $script:cases++
    Set-Content -LiteralPath (Join-Path $dir "my-notes.txt") -Value "mine"
    New-Item -ItemType Directory -Path (Join-Path $dir "my-folder") | Out-Null
    Set-Content -LiteralPath (Join-Path $dir "my-folder\data.txt") -Value "mine too"
    $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v2.Zip; ChecksumsPath = $v2.Sums }
    $m = Get-Manifest $dir
    if (-not $r.Ok) { Fail "upgrade failed: $($r.Output)" }
    elseif (-not (Test-Path (Join-Path $dir "my-notes.txt")) -or -not (Test-Path (Join-Path $dir "my-folder\data.txt"))) { Fail "upgrade removed user files" }
    elseif (Test-Path (Join-Path $dir "old-notes.txt")) { Fail "upgrade kept a file only the old release owned" }
    elseif ((Get-Content -Raw (Join-Path $dir "README.md")) -notmatch 'Release two\.') { Fail "upgrade did not install the new README.md" }
    elseif ((@($m.files) -join ",") -ne ($v2Names -join ",")) { Fail "upgrade manifest lists $(@($m.files) -join ',')" }
    elseif ((Get-WorkDirs $dir).Count) { Fail "upgrade left work directories" }
    else { Pass "upgrade replaces ARN's files, drops files the new release no longer has, keeps user files" }

    # Failures before the switch-over leave everything as it was.
    $before = Get-Snapshot $dir
    Test-Unchanged "checksum mismatch leaves the installation untouched" `
        (Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $badSums }) $dir $before "Checksum verification failed"
    Test-Unchanged "corrupt archive leaves the installation untouched" `
        (Invoke-Installer @{ InstallDir = $dir; ArchivePath = $corruptZip; ChecksumsPath = $corruptSums }) $dir $before ""

    # Failures during the switch-over are rolled back.
    foreach ($fault in @("activate", "verify")) {
        $env:ARN_INSTALL_TEST_FAULT = $fault
        try { $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums } }
        finally { Remove-Item Env:ARN_INSTALL_TEST_FAULT }
        Test-Unchanged "failure during $fault restores the previous version" $r $dir $before "The previous installation was restored"
    }

    # A locked arn.exe: no false success, nothing changed.
    $lock = [IO.File]::Open((Join-Path $dir "arn.exe"), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try { $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums } } finally { $lock.Dispose() }
    Test-Unchanged "locked arn.exe makes the update fail and restores everything" $r $dir $before "in use by another program"

    # A running arn.exe does not block an update; its old files are cleaned later.
    $script:cases++
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $dir "arn.exe"
    $start.Arguments = "--server"
    $start.WorkingDirectory = $root
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.CreateNoWindow = $true
    $running = [Diagnostics.Process]::Start($start)
    [void]$running.StandardOutput.ReadLine()
    try { $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums } }
    finally { $running.StandardInput.Close(); [void]$running.WaitForExit(10000) }
    $leftBehind = (Get-WorkDirs $dir).Count
    $again = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums }
    if (-not $r.Ok) { Fail "update while ARN runs failed: $($r.Output)" }
    elseif (-not (Test-Path (Join-Path $dir "old-notes.txt"))) { Fail "update while ARN runs did not install v1" }
    elseif ($leftBehind -ne 1 -or $r.Output -notmatch 'still running') { Fail "update while ARN runs: expected one kept backup and a warning ($leftBehind): $($r.Output)" }
    elseif (-not $again.Ok -or (Get-WorkDirs $dir).Count) { Fail "the next installation did not remove the old backup: $($again.Output)" }
    else { Pass "update while ARN runs succeeds and the next run removes the old files" }

    # Manifest validation: malformed, traversal, foreign and unsupported manifests are refused.
    $decoy = Join-Path $root "apps\Arn\outside.txt"
    Set-Content -LiteralPath $decoy -Value "not ARN's"
    $manifestFile = Join-Path $dir "install-manifest.json"
    $goodManifest = [IO.File]::ReadAllText($manifestFile)
    $bad = [ordered]@{
        "malformed JSON" = '{ "format": '
        "parent traversal" = ($goodManifest -replace '"files":\s*\[', '"files": [ "..\\outside.txt",')
        "drive-qualified path" = ($goodManifest -replace '"files":\s*\[', '"files": [ "C:\\Windows\\notepad.exe",')
        "sub-directory path" = ($goodManifest -replace '"files":\s*\[', '"files": [ "my-folder/data.txt",')
        "dot-dot entry" = ($goodManifest -replace '"files":\s*\[', '"files": [ "..",')
        "unsupported format" = ($goodManifest -replace 'arn-install-manifest/1', 'arn-install-manifest/9')
        "manifest of another directory" = ($goodManifest -replace [regex]::Escape(($dir -replace '\\', '\\')), 'C:\\\\Elsewhere')
    }
    foreach ($case in $bad.GetEnumerator()) {
        [IO.File]::WriteAllText($manifestFile, $case.Value)
        $before = Get-Snapshot $dir
        Test-Unchanged "uninstall refuses a manifest with $($case.Key)" (Invoke-Installer @{ InstallDir = $dir; Uninstall = $true }) $dir $before "Uninstall refused"
        Test-Unchanged "update refuses a manifest with $($case.Key)" `
            (Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums }) $dir $before "Update refused"
    }
    $script:cases++
    if (-not (Test-Path -LiteralPath $decoy)) { Fail "a file outside the installation was deleted" } else { Pass "files outside the installation are never touched" }
    [IO.File]::WriteAllText($manifestFile, $goodManifest)

    # Uninstall with a locked arn.exe: incomplete, said so, retry works.
    $script:cases++
    $lock = [IO.File]::Open((Join-Path $dir "arn.exe"), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try { $r = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true } } finally { $lock.Dispose() }
    $m = Get-Manifest $dir
    if ($r.Ok -or $r.Output -notmatch 'Uninstall incomplete') { Fail "uninstall with a locked arn.exe did not fail: $($r.Output)" }
    elseif ((@($m.files) -join ",") -ne "arn.exe" -or -not (Test-Path (Join-Path $dir "arn.exe"))) { Fail "after a partial uninstall the manifest lists $(@($m.files) -join ',')" }
    else { Pass "uninstall with a locked arn.exe reports failure and records what is left" }

    # Uninstall removes ARN's files only, then is safe to repeat.
    $script:cases++
    $r = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true }
    $left = @(Get-ChildItem -LiteralPath $dir -Force | ForEach-Object Name | Sort-Object)
    if (-not $r.Ok) { Fail "uninstall failed: $($r.Output)" }
    elseif (($left -join ",") -ne "my-folder,my-notes.txt") { Fail "uninstall left $($left -join ',')" }
    else { Pass "uninstall removes only ARN's files and keeps user files and folders" }
    $script:cases++
    $before = Get-Snapshot $dir
    $r = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true }
    if (-not $r.Ok -or $r.Output -notmatch 'not installed' -or (Get-Snapshot $dir) -cne $before) { Fail "repeated uninstall: $($r.Output)" }
    else { Pass "repeating the uninstall changes nothing" }

    # Without a manifest nothing is guessed: install and uninstall refuse,
    # -AdoptExisting replaces the files of the same names.
    $legacyDir = Join-Path $root "legacy-none\bin"
    New-Item -ItemType Directory -Path $legacyDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $v1Dir "arn.exe"), (Join-Path $v1Dir "LICENSE") -Destination $legacyDir
    Set-Content -LiteralPath (Join-Path $legacyDir "keep.txt") -Value "mine"
    $before = Get-Snapshot $legacyDir
    Test-Unchanged "update refuses files no manifest records" `
        (Invoke-Installer @{ InstallDir = $legacyDir; ArchivePath = $v2.Zip; ChecksumsPath = $v2.Sums }) $legacyDir $before "-AdoptExisting"
    Test-Unchanged "uninstall refuses a directory without a manifest" `
        (Invoke-Installer @{ InstallDir = $legacyDir; Uninstall = $true }) $legacyDir $before "no install-manifest.json"
    $script:cases++
    $r = Invoke-Installer @{ InstallDir = $legacyDir; ArchivePath = $v2.Zip; ChecksumsPath = $v2.Sums; AdoptExisting = $true }
    if (-not $r.Ok -or -not (Test-Path (Join-Path $legacyDir "keep.txt")) -or (@((Get-Manifest $legacyDir).files) -join ",") -ne ($v2Names -join ",")) {
        Fail "-AdoptExisting: $($r.Output)"
    } else { Pass "-AdoptExisting replaces same-named files and keeps the rest" }

    # A legacy manifest (JSON array) from earlier installers is honoured and upgraded.
    $script:cases++
    $legacyArray = Join-Path $root "legacy-array\bin"
    New-Item -ItemType Directory -Path $legacyArray | Out-Null
    foreach ($name in $v1Names) { Copy-Item -LiteralPath (Join-Path $v1Dir $name) -Destination $legacyArray }
    Set-Content -LiteralPath (Join-Path $legacyArray "install-manifest.json") -Encoding utf8 `
        -Value ((@($v1Names) + @("install-manifest.json")) | ConvertTo-Json)
    Set-Content -LiteralPath (Join-Path $legacyArray "keep.txt") -Value "mine"
    $r = Invoke-Installer @{ InstallDir = $legacyArray; ArchivePath = $v2.Zip; ChecksumsPath = $v2.Sums }
    $m = if ($r.Ok) { Get-Manifest $legacyArray } else { $null }
    if (-not $r.Ok) { Fail "upgrade from a legacy manifest failed: $($r.Output)" }
    elseif ($m.format -ne "arn-install-manifest/1" -or (Test-Path (Join-Path $legacyArray "old-notes.txt")) -or -not (Test-Path (Join-Path $legacyArray "keep.txt"))) {
        Fail "upgrade from a legacy manifest: manifest $($m.format), old-notes $(Test-Path (Join-Path $legacyArray 'old-notes.txt'))"
    } else { Pass "an earlier installer's file list is honoured and replaced by a format-1 manifest" }

    # PATH: only the installer's own entry is removed, others keep text and order.
    $script:cases++
    $pathDir = Join-Path $root "path-test\bin"
    $env:Path = "C:\First;C:\Second\;C:\Third"
    $r1 = try { (& $installer -InstallDir $pathDir -ArchivePath $v1.Zip -ChecksumsPath $v1.Sums -PathScope Process *>&1) -join " "; $true } catch { $_.Exception.Message }
    $afterInstall = $env:Path
    $r2 = try { (& $installer -InstallDir $pathDir -Uninstall *>&1) -join " "; $true } catch { $_.Exception.Message }
    if ($afterInstall -ne "$pathDir;C:\First;C:\Second\;C:\Third") { Fail "PATH after install: $afterInstall ($r1)" }
    elseif ($env:Path -cne "C:\First;C:\Second\;C:\Third") { Fail "PATH after uninstall: $env:Path ($r2)" }
    elseif (Test-Path -LiteralPath $pathDir) { Fail "uninstall left the empty installation directory" }
    else { Pass "uninstall removes the installer's PATH entry, keeps the others, and removes the empty directory" }

    $script:cases++
    $userDir = Join-Path $root "path-user\bin"
    $env:Path = "C:\First;$userDir;C:\Third"
    try { & $installer -InstallDir $userDir -ArchivePath $v1.Zip -ChecksumsPath $v1.Sums -PathScope Process *>&1 | Out-Null
          & $installer -InstallDir $userDir -Uninstall *>&1 | Out-Null } catch { Fail "pre-existing PATH entry: $($_.Exception.Message)" }
    if ($env:Path -notmatch [regex]::Escape($userDir)) { Fail "uninstall removed a PATH entry the user had added: $env:Path" }
    else { Pass "a PATH entry that existed before installation stays" }
} finally {
    $env:Path = $savedPath
    if ($root.StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "$cases cases, $($failures.Count) failure(s)"
if ($failures.Count) {
    $failures | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
