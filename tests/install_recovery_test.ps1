param([Parameter(Mandatory = $true)][string]$BuildDir)

# Tests crash recovery, PATH handling and uninstall preflight of
# scripts/install.ps1 with the real Windows package built from $BuildDir.
# Crashes are simulated deterministically: ARN_INSTALL_TEST_FAULT=kill:<step>
# makes a child installer process terminate itself at exactly that step, with
# no cleanup. Everything happens in temporary directories. The user PATH tests
# use a throwaway HKCU\Software\ARN-Install-Test-<guid> key (through
# ARN_INSTALL_TEST_USER_PATH_KEY) that is deleted at the end; the real user
# PATH is never read for writing or changed.
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
$root = Join-Path $tempBase ("arn-install-recovery-test-" + [guid]::NewGuid().ToString("N"))
# Installer download directories (%TEMP%\arn-install-<guid>) that existed
# before this run: the run must not add any. Every installer this test starts
# gets a temporary directory inside $root (see Invoke-Child and the TMP/TEMP
# redirection below), so even one killed mid-way leaves nothing outside $root,
# and nothing outside $root is ever deleted.
function Get-InstallerTemps { @(Get-ChildItem -LiteralPath $tempBase -Directory -Force -ErrorAction SilentlyContinue |
                               Where-Object { $_.Name -cmatch '^arn-install-[0-9a-f]{32}$' } | ForEach-Object Name) }
$installerTempsBefore = Get-InstallerTemps
$savedTemp = @{ TMP = $env:TMP; TEMP = $env:TEMP }
$childTemps = New-Object System.Collections.Generic.List[string]
$testKey = "Software\ARN-Install-Test-" + [guid]::NewGuid().ToString("N")
$powershell = (Get-Process -Id $PID).Path
$failures = New-Object System.Collections.Generic.List[string]
$cases = 0

function Check([string]$Name, [bool]$Condition, [string]$Detail) {
    $script:cases++
    if ($Condition) { Write-Host "ok: $Name" } else { $failures.Add("${Name}: $Detail") }
}

function New-Release([string]$Name, [string]$SourceDir) {
    $dir = Join-Path $root "releases\$Name"
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $zip = Join-Path $dir "arn-windows-x64.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory($SourceDir, $zip)
    $sums = Join-Path $dir "SHA256SUMS"
    [IO.File]::WriteAllText($sums, "$((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant())  arn-windows-x64.zip`n")
    return [pscustomobject]@{ Zip = $zip; Sums = $sums }
}

# Runs install.ps1 in this process; returns @{ Ok; Output } with everything it
# printed, followed by the error message when it failed.
function Invoke-Installer([hashtable]$Arguments) {
    if (-not $Arguments.ContainsKey("PathScope")) { $Arguments["PathScope"] = "None" }
    $printed = New-Object System.Collections.Generic.List[string]
    try {
        & $installer @Arguments *>&1 | ForEach-Object { $printed.Add("$_") }
        return [pscustomobject]@{ Ok = $true; Output = $printed -join " " }
    } catch {
        $printed.Add($_.Exception.Message)
        return [pscustomobject]@{ Ok = $false; Output = $printed -join " " }
    }
}

# Runs install.ps1 in a child PowerShell with ARN_INSTALL_TEST_FAULT set;
# returns @{ ExitCode; Output }.
function Invoke-Child([string]$Fault, [string[]]$Arguments) {
    # Its own temporary directory, recorded, so what a killed child leaves is
    # found and removed with $root.
    $childTemp = Join-Path $root ("child-temp\" + $childTemps.Count)
    New-Item -ItemType Directory -Path $childTemp -Force | Out-Null
    $childTemps.Add($childTemp)
    $outerTemp = @{ TMP = $env:TMP; TEMP = $env:TEMP }
    $env:ARN_INSTALL_TEST_FAULT = $Fault
    $env:TMP = $childTemp
    $env:TEMP = $childTemp
    # A child that fails (rather than being killed) writes to stderr; Windows
    # PowerShell would turn that into a terminating error here.
    $ErrorActionPreference = "Continue"
    try {
        $output = (& $powershell -NoProfile -ExecutionPolicy Bypass -File $installer @Arguments 2>&1 | ForEach-Object { "$_" }) -join " "
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
    } finally {
        Remove-Item Env:ARN_INSTALL_TEST_FAULT -ErrorAction SilentlyContinue
        $env:TMP = $outerTemp.TMP
        $env:TEMP = $outerTemp.TEMP
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

function Get-WorkDirs([string]$Dir) {
    return @(Get-ChildItem -LiteralPath (Split-Path -Parent $Dir) -Directory -Force |
             Where-Object { $_.Name -match '^\.arn-(staging|backup)-' })
}

function Get-TestPath {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($testKey)
    try {
        return [pscustomobject]@{
            Value = [string]$key.GetValue("Path", "", [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            Kind = [string]$key.GetValueKind("Path")
        }
    } finally { $key.Close() }
}

function Set-TestPath([string]$Value, [Microsoft.Win32.RegistryValueKind]$Kind) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($testKey)
    try { $key.SetValue("Path", $Value, $Kind) } finally { $key.Close() }
}

$savedPath = $env:Path
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    # Installers run in this process put their temporary files under $root too.
    $ownTemp = Join-Path $root "temp"
    New-Item -ItemType Directory -Path $ownTemp | Out-Null
    $env:TMP = $ownTemp
    $env:TEMP = $ownTemp
    $package = Join-Path $root "package"
    & (Join-Path $repo "scripts/package-windows.ps1") -BuildDir $BuildDir -OutputDir $package `
        -ArchivePath (Join-Path $root "package.zip") | Out-Null
    $v1Dir = Join-Path $root "v1"
    $v2Dir = Join-Path $root "v2"
    Copy-Item -LiteralPath $package -Destination $v1Dir -Recurse
    Copy-Item -LiteralPath $package -Destination $v2Dir -Recurse
    Set-Content -LiteralPath (Join-Path $v1Dir "old-notes.txt") -Value "only in v1"
    Add-Content -LiteralPath (Join-Path $v2Dir "README.md") -Value "Release two."
    Set-Content -LiteralPath (Join-Path $v2Dir "new-notes.txt") -Value "only in v2"
    # v2 gets its own arn.exe: the build commit inside it (printed by
    # --version) is replaced by another value of the same length, so the two
    # executables differ in content and in what they report.
    $v1Exe = Join-Path $v1Dir "arn.exe"
    $v2Exe = Join-Path $v2Dir "arn.exe"
    $v1Version = (& $v1Exe --version) -join "`n"
    if ($v1Version -notmatch '\(([^)]+)\)') { throw "Cannot find the build commit in '$v1Version'." }
    $token = $Matches[1]
    $otherToken = -join ($token.ToCharArray() | ForEach-Object {
        if ($_ -match '[0-8]') { [char]([int]$_ + 1) } elseif ($_ -eq '9') { '0' }
        elseif ($_ -match '[a-y]') { [char]([int]$_ + 1) } elseif ($_ -eq 'z') { 'a' } else { $_ } })
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $exeText = $latin1.GetString([IO.File]::ReadAllBytes($v2Exe))
    if (([regex]::Matches($exeText, [regex]::Escape($token))).Count -ne 1) { throw "The build commit '$token' is not stored exactly once in arn.exe." }
    [IO.File]::WriteAllBytes($v2Exe, $latin1.GetBytes($exeText.Replace($token, $otherToken)))
    $v2Version = (& $v2Exe --version) -join "`n"
    $v1Hash = (Get-FileHash -LiteralPath $v1Exe -Algorithm SHA256).Hash
    $v2Hash = (Get-FileHash -LiteralPath $v2Exe -Algorithm SHA256).Hash
    Check "v1 and v2 have different executables that report different versions" `
        ($v1Hash -ne $v2Hash -and $LASTEXITCODE -eq 0 -and $v2Version -cmatch '^arn [0-9]+\.[0-9]+\.[0-9]+ \(' -and $v2Version -cne $v1Version) "$v1Version / $v2Version"
    $v1 = New-Release "v1" $v1Dir
    $v2 = New-Release "v2" $v2Dir
    function Get-ExeHash([string]$Dir) {
        $exe = Join-Path $Dir "arn.exe"
        if (Test-Path -LiteralPath $exe) { (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash } else { "<missing>" }
    }
    function Get-ExeVersion([string]$Dir) { try { (& (Join-Path $Dir "arn.exe") --version) -join "`n" } catch { "<none>" } }
    $badSums = Join-Path $root "bad-SHA256SUMS"
    [IO.File]::WriteAllText($badSums, "$("0" * 64)  arn-windows-x64.zip`n")
    $v2Args = { param($Dir) @("-InstallDir", $Dir, "-PathScope", "None", "-ArchivePath", $v2.Zip, "-ChecksumsPath", $v2.Sums) }
    # A run whose download fails its checksum: it still recovers first, and
    # then changes nothing, so it shows exactly what recovery left.
    $probeRun = { param($Dir) Invoke-Installer @{ InstallDir = $Dir; ArchivePath = $v2.Zip; ChecksumsPath = $badSums } }

    # 1. Termination at each step of an update, then recovery by the next run
    #    and a second run that finds nothing left to do.
    $points = "after-journal", "after-first-backup", "after-backup", "after-first-activate", "after-activate", "after-manifest"
    foreach ($point in $points) {
        $dir = Join-Path $root "kill-$point\Arn\bin"
        $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums }
        if (-not $r.Ok) { Check "kill at ${point}: install v1" $false $r.Output; continue }
        Set-Content -LiteralPath (Join-Path $dir "my-notes.txt") -Value "mine"
        $before = Get-Snapshot $dir
        $manifestBefore = [IO.File]::ReadAllText((Join-Path $dir "install-manifest.json"))
        $child = Invoke-Child "kill:$point" (& $v2Args $dir)
        $journal = Join-Path $dir ".arn-transaction.json"
        Check "kill at ${point}: the installer was terminated mid-transaction" ($child.ExitCode -ne 0 -and (Test-Path -LiteralPath $journal)) "exit $($child.ExitCode): $($child.Output)"
        if ($point -eq "after-first-backup") {
            Check "kill at ${point}: arn.exe is only in the backup" (-not (Test-Path (Join-Path $dir "arn.exe"))) "arn.exe still present"
        }
        $r = & $probeRun $dir
        Check "kill at ${point}: the next run restores the previous installation exactly" `
            ((-not $r.Ok) -and $r.Output -match 'interrupted update of ARN Code was found and undone' -and $r.Output -match 'Checksum verification failed' -and
             (Get-Snapshot $dir) -ceq $before -and -not (Get-WorkDirs $dir).Count) "$($r.Output)"
        $manifestNow = if (Test-Path (Join-Path $dir "install-manifest.json")) { [IO.File]::ReadAllText((Join-Path $dir "install-manifest.json")) } else { "<missing>" }
        Check "kill at ${point}: the exact v1 arn.exe, manifest and files are back, v2's are gone" `
            ((Get-ExeHash $dir) -eq $v1Hash -and (Get-ExeVersion $dir) -ceq $v1Version -and $manifestNow -ceq $manifestBefore -and
             (Test-Path (Join-Path $dir "old-notes.txt")) -and -not (Test-Path (Join-Path $dir "new-notes.txt")) -and
             (Get-Content -Raw (Join-Path $dir "README.md")) -notmatch 'Release two\.' -and
             (Get-Content -Raw (Join-Path $dir "my-notes.txt")).Trim() -ceq "mine") "arn.exe $(Get-ExeVersion $dir); manifest equal: $($manifestNow -ceq $manifestBefore)"
        $r = & $probeRun $dir
        Check "kill at ${point}: running again changes nothing" `
            ($r.Output -notmatch 'interrupted' -and (Get-Snapshot $dir) -ceq $before -and -not (Get-WorkDirs $dir).Count) "$($r.Output)"
    }

    # 2. Termination after the commit: the next run finishes the cleanup.
    $dir = Join-Path $root "kill-after-commit\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    $child = Invoke-Child "kill:after-commit" (& $v2Args $dir)
    $leftBackup = @(Get-WorkDirs $dir | Where-Object Name -match 'backup').Count
    $r = & $probeRun $dir
    Check "kill after the commit: the next run keeps the new version and removes the backup" `
        ($child.ExitCode -ne 0 -and $leftBackup -eq 1 -and $r.Output -match 'Finished cleaning up' -and
         (Get-ExeHash $dir) -eq $v2Hash -and (Get-ExeVersion $dir) -ceq $v2Version -and (Test-Path (Join-Path $dir "new-notes.txt")) -and
         (Get-Content -Raw (Join-Path $dir "README.md")) -match 'Release two\.' -and -not (Test-Path (Join-Path $dir "old-notes.txt")) -and
         -not (Test-Path (Join-Path $dir ".arn-transaction.json")) -and -not (Get-WorkDirs $dir).Count) "$($child.Output) / $($r.Output)"

    # 3. Recovery itself interrupted, twice, then finished.
    foreach ($recoveryPoint in "recovery-move", "recovery-restore") {
        $dir = Join-Path $root "kill-$recoveryPoint\Arn\bin"
        [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
        $before = Get-Snapshot $dir
        $child = Invoke-Child "kill:after-activate" (& $v2Args $dir)
        $second = Invoke-Child "kill:$recoveryPoint" @("-InstallDir", $dir, "-PathScope", "None", "-ArchivePath", $v2.Zip, "-ChecksumsPath", $badSums)
        $third = Invoke-Child "kill:$recoveryPoint" @("-InstallDir", $dir, "-PathScope", "None", "-ArchivePath", $v2.Zip, "-ChecksumsPath", $badSums)
        $r = & $probeRun $dir
        $again = & $probeRun $dir
        Check "recovery interrupted at $recoveryPoint resumes and completes" `
            ($second.ExitCode -ne 0 -and $third.ExitCode -ne 0 -and $r.Output -match 'was found and undone' -and (Get-Snapshot $dir) -ceq $before -and
             $again.Output -notmatch 'interrupted' -and -not (Get-WorkDirs $dir).Count) "$($second.Output) / $($third.Output) / $($r.Output)"
    }

    # 4. An interrupted fresh installation is undone without claiming that a
    #    previous version was restored.
    $dir = Join-Path $root "kill-fresh\Arn\bin"
    $child = Invoke-Child "kill:after-first-activate" (& $v2Args $dir)
    $r = & $probeRun $dir
    Check "interrupted fresh installation is undone and its directory removed" `
        ($child.ExitCode -ne 0 -and $r.Output -match 'interrupted installation of ARN Code was found and undone' -and
         $r.Output -notmatch 'restored|installed again' -and -not (Test-Path -LiteralPath $dir) -and -not (Get-WorkDirs $dir).Count) "$($r.Output)"
    $dir = Join-Path $root "fail-fresh\Arn\bin"
    $env:ARN_INSTALL_TEST_FAULT = "verify"
    try { $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums } } finally { Remove-Item Env:ARN_INSTALL_TEST_FAULT }
    Check "a failed fresh installation says it was not installed" `
        ((-not $r.Ok) -and $r.Output -match 'ARN Code was not installed\.' -and $r.Output -notmatch 'previous installation' -and
         -not (Test-Path -LiteralPath $dir)) "$($r.Output)"

    # 4b. A fresh installation interrupted while staging has changed nothing in
    #     the directory. Files that appear there afterwards, even with the
    #     package's names or the manifest's, are not the transaction's: they
    #     stay byte for byte, and only the transaction's staging directory goes.
    function Get-NamedHashes([string]$Dir, [string[]]$Names) {
        return (@($Names | Sort-Object | ForEach-Object {
            $path = Join-Path $Dir $_
            if (Test-Path -LiteralPath $path) { "$_=$((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash)" } else { "$_=<missing>" }
        }) -join "`n")
    }
    $userNames = @(@(Get-ChildItem -LiteralPath $v2Dir -File | ForEach-Object Name) + @("install-manifest.json", "my-notes.txt") | Sort-Object -Unique)
    foreach ($point in "after-journal", "after-staging") {
        $dir = Join-Path $root "staging-$point\Arn\bin"
        $child = Invoke-Child "kill:$point" (& $v2Args $dir)
        $journalFile = Join-Path $dir ".arn-transaction.json"
        $state = try { ([IO.File]::ReadAllText($journalFile) | ConvertFrom-Json).state } catch { "<none>" }
        $staged = @(Get-WorkDirs $dir | Where-Object Name -match 'staging' | ForEach-Object { @(Get-ChildItem -LiteralPath $_.FullName).Count })
        Check "fresh installation killed $point stops in 'staging'$(if ($point -eq 'after-staging') { ' with its files staged' })" `
            ($child.ExitCode -ne 0 -and $state -ceq "staging" -and ($point -eq "after-journal" -or ($staged.Count -eq 1 -and $staged[0] -gt 0))) "exit $($child.ExitCode), state $state, staged $($staged -join ',')"
        foreach ($name in $userNames) {
            if ($name -eq "install-manifest.json") {
                [IO.File]::WriteAllText((Join-Path $dir $name), "{ `"note`": `"the user's own manifest-like file`" }`n")
            } else {
                Set-Content -LiteralPath (Join-Path $dir $name) -Value "the user's own $name"
            }
        }
        $userBefore = Get-NamedHashes $dir $userNames
        $r = & $probeRun $dir
        $listing = (@(Get-ChildItem -LiteralPath $dir -Force | ForEach-Object Name | Sort-Object) -join ",")
        Check "staging interruption ($point): recovery keeps every file with a package name byte for byte" `
            ($r.Output -match 'interrupted installation of ARN Code was found and undone' -and (Get-NamedHashes $dir $userNames) -ceq $userBefore -and
             $listing -eq ($userNames -join ",") -and -not (Test-Path -LiteralPath $journalFile) -and -not (Get-WorkDirs $dir).Count) "$($r.Output) / $listing"
        $r = & $probeRun $dir
        Check "staging interruption ($point): a second run changes nothing" `
            ($r.Output -notmatch 'interrupted' -and (Get-NamedHashes $dir $userNames) -ceq $userBefore -and -not (Get-WorkDirs $dir).Count) "$($r.Output)"
    }

    # 4c. A committed transaction is never rolled back.
    $dir = Join-Path $root "committed-direct\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    $parent = Split-Path -Parent $dir
    $heldBackup = Join-Path $parent (".arn-backup-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $heldBackup | Out-Null
    Set-Content -LiteralPath (Join-Path $heldBackup "arn.exe") -Value "an earlier arn.exe"
    $parentBefore = Get-Snapshot $parent
    # Undo-Transaction itself, called with a committed record: the function
    # definitions come straight from install.ps1.
    $installerAst = [System.Management.Automation.Language.Parser]::ParseFile($installer, [ref]$null, [ref]$null)
    $definitions = @($installerAst.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false) |
                     ForEach-Object { $_.Extent.Text })
    $refusal = & {
        $InstallDir = $dir
        $parentDir = $parent
        $manifestName = "install-manifest.json"
        $manifestPath = Join-Path $dir $manifestName
        $journalPath = Join-Path $dir ".arn-transaction.json"
        foreach ($definition in $definitions) { . ([scriptblock]::Create($definition)) }
        $committedRecord = [pscustomobject]@{
            State = "committed"; StageDir = ".arn-staging-" + [guid]::NewGuid().ToString("N"); BackupDir = (Split-Path -Leaf $heldBackup)
            Fresh = $false; HadManifest = $true; CreatedDir = $false; PathScope = "None"; PathAdded = $false
            Plan = @("arn.exe", "install-manifest.json"); Payload = @("arn.exe"); PreviousVersion = $null
        }
        try { [void](Undo-Transaction $committedRecord); "it rolled back" } catch { $_.Exception.Message }
    }
    Check "Undo-Transaction refuses a committed record and touches nothing" `
        ("$refusal" -match 'is committed; it is finished, not rolled back' -and (Get-Snapshot $parent) -ceq $parentBefore) "$refusal"
    Remove-Item -LiteralPath $heldBackup -Recurse -Force -ErrorAction SilentlyContinue

    $dir = Join-Path $root "committed-fault\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    $env:ARN_INSTALL_TEST_FAULT = "after-commit"
    try { $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v2.Zip; ChecksumsPath = $v2.Sums } } finally { Remove-Item Env:ARN_INSTALL_TEST_FAULT }
    Check "an error after the commit keeps the committed installation" `
        ((-not $r.Ok) -and $r.Output -match 'so the installation was kept' -and $r.Output -notmatch 'restored' -and
         (Get-ExeHash $dir) -eq $v2Hash -and (Test-Path (Join-Path $dir "new-notes.txt")) -and -not (Test-Path (Join-Path $dir "old-notes.txt")) -and
         -not (Test-Path (Join-Path $dir ".arn-transaction.json")) -and -not (Get-WorkDirs $dir).Count) "$($r.Output)"

    # 5. A transaction record that cannot be trusted stops everything and keeps the backup.
    $dir = Join-Path $root "bad-journal\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    $before = Get-Snapshot $dir
    $child = Invoke-Child "kill:after-backup" (& $v2Args $dir)
    $journal = Join-Path $dir ".arn-transaction.json"
    $goodJournal = [IO.File]::ReadAllText($journal)
    $backup = @(Get-WorkDirs $dir | Where-Object Name -match 'backup')[0].FullName
    $backupBefore = Get-Snapshot $backup
    $corrupt = [ordered]@{
        "truncated JSON" = $goodJournal.Substring(0, 40)
        "unknown state" = ($goodJournal -replace '"state":\s*"[a-z-]+"', '"state": "bogus"')
        "backup directory outside the parent" = ($goodJournal -replace '"backupDir":\s*"[^"]+"', '"backupDir": "..\\\\elsewhere"')
        "another installation directory" = ($goodJournal -replace '"installDir":\s*"[^"]+"', '"installDir": "C:\\\\Elsewhere"')
        "plan that does not match the backup" = ($goodJournal -replace '"hadManifest":\s*true', '"hadManifest": false')
        "a JSON string" = '"arn.exe"'
    }
    foreach ($case in $corrupt.GetEnumerator()) {
        [IO.File]::WriteAllText($journal, $case.Value)
        $installRun = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v2.Zip; ChecksumsPath = $v2.Sums }
        $uninstallRun = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true }
        Check "record with $($case.Key): install and uninstall refuse and keep the backup" `
            ((-not $installRun.Ok) -and (-not $uninstallRun.Ok) -and $installRun.Output -match 'record cannot be used' -and
             (Get-Snapshot $backup) -ceq $backupBefore -and [IO.File]::ReadAllText($journal) -ceq $case.Value) "$($installRun.Output) / $($uninstallRun.Output)"
    }
    [IO.File]::WriteAllText($journal, $goodJournal)
    $r = & $probeRun $dir
    Check "after the record is repaired, recovery restores the kept backup" `
        ($r.Output -match 'was found and undone' -and (Get-Snapshot $dir) -ceq $before -and -not (Get-WorkDirs $dir).Count) "$($r.Output)"

    # 6. A backup no record claims is never deleted; a committed one is.
    $dir = Join-Path $root "orphan\Arn\bin"
    $orphan = Join-Path (Split-Path -Parent $dir) (".arn-backup-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $orphan | Out-Null
    Set-Content -LiteralPath (Join-Path $orphan "arn.exe") -Value "the only copy"
    $committed = Join-Path (Split-Path -Parent $dir) (".arn-backup-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $committed | Out-Null
    Set-Content -LiteralPath (Join-Path $committed "arn.exe") -Value "old"
    Set-Content -LiteralPath (Join-Path $committed ".arn-committed") -Value "arn-install-transaction/1 committed"
    $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums }
    Check "an unclaimed backup is kept, a committed one is removed" `
        ($r.Ok -and (Test-Path (Join-Path $orphan "arn.exe")) -and $r.Output -match 'was kept' -and -not (Test-Path $committed)) "$($r.Output)"

    # 7. Several arn executables on PATH: the first one decides.
    $dir = Join-Path $root "two-arns\Arn\bin"
    $other = Join-Path $root "two-arns\other"
    New-Item -ItemType Directory -Path $other | Out-Null
    Set-Content -LiteralPath (Join-Path $other "arn.cmd") -Value "@echo another arn"
    $env:Path = "$other;$env:SystemRoot\System32"
    $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums; PathScope = "Process" }
    Check "another arn later on PATH does not break the installation" ($r.Ok -and $env:Path.StartsWith("$dir;")) "$($r.Output)"
    $env:Path = $savedPath

    # 8. The user PATH: REG_EXPAND_SZ and %VARIABLE% references are kept,
    #    a failure restores it exactly, uninstall removes only ARN's entry.
    $original = "%SystemRoot%\system32;C:\Keep\%USERNAME%;;C:\Last"
    Set-TestPath $original ([Microsoft.Win32.RegistryValueKind]::ExpandString)
    $env:ARN_INSTALL_TEST_USER_PATH_KEY = $testKey
    try {
        $dir = Join-Path $root "user-path\Arn\bin"
        [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
        $before = Get-Snapshot $dir
        $processBefore = $env:Path
        $env:ARN_INSTALL_TEST_FAULT = "after-path"
        try { $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v2.Zip; ChecksumsPath = $v2.Sums; PathScope = "User" } }
        finally { Remove-Item Env:ARN_INSTALL_TEST_FAULT }
        $state = Get-TestPath
        Check "a failure after the PATH change restores the user PATH, its type and the process PATH" `
            ((-not $r.Ok) -and $r.Output -match 'previous installation was restored' -and $state.Value -ceq $original -and
             $state.Kind -eq "ExpandString" -and $env:Path -ceq $processBefore -and (Get-Snapshot $dir) -ceq $before) "$($r.Output) / $($state.Kind) $($state.Value)"

        $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v2.Zip; ChecksumsPath = $v2.Sums; PathScope = "User" }
        $state = Get-TestPath
        Check "installation adds the entry in front and keeps REG_EXPAND_SZ and %VARIABLE% text" `
            ($r.Ok -and $state.Value -ceq "$dir;$original" -and $state.Kind -eq "ExpandString") "$($r.Output) / $($state.Kind) $($state.Value)"

        $lock = [IO.File]::Open((Join-Path $dir "arn.exe"), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
        try { $r = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true } } finally { $lock.Dispose() }
        $state = Get-TestPath
        $m = [IO.File]::ReadAllText((Join-Path $dir "install-manifest.json")) | ConvertFrom-Json
        Check "a partial uninstall keeps the PATH entry and a manifest of what is left" `
            ((-not $r.Ok) -and $r.Output -match 'Uninstall incomplete' -and $state.Value -ceq "$dir;$original" -and
             (@($m.files) -join ",") -eq "arn.exe" -and $m.addedPathEntry) "$($r.Output) / $($state.Value)"

        $r = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true }
        $state = Get-TestPath
        Check "uninstall removes only ARN's entry and keeps the value's type" `
            ($r.Ok -and $state.Value -ceq $original -and $state.Kind -eq "ExpandString" -and -not (Test-Path -LiteralPath $dir)) "$($r.Output) / $($state.Kind) $($state.Value)"

        # A crash after the PATH change: the next run takes the entry out again.
        $dir = Join-Path $root "user-path-kill\Arn\bin"
        [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
        $before = Get-Snapshot $dir
        $child = Invoke-Child "kill:after-path" @("-InstallDir", $dir, "-PathScope", "User", "-ArchivePath", $v2.Zip, "-ChecksumsPath", $v2.Sums)
        $added = (Get-TestPath).Value
        $r = & $probeRun $dir
        $state = Get-TestPath
        Check "a crash after the PATH change is undone, PATH included" `
            ($child.ExitCode -ne 0 -and $added -ceq "$dir;$original" -and $state.Value -ceq $original -and $state.Kind -eq "ExpandString" -and
             (Get-Snapshot $dir) -ceq $before) "$($child.Output) / $($r.Output) / $($state.Value)"

        # REG_SZ stays REG_SZ.
        Set-TestPath "C:\Plain" ([Microsoft.Win32.RegistryValueKind]::String)
        $dir = Join-Path $root "user-path-sz\Arn\bin"
        $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums; PathScope = "User" }
        $state = Get-TestPath
        $kindAfterInstall = $state.Kind
        [void](Invoke-Installer @{ InstallDir = $dir; Uninstall = $true })
        $state = Get-TestPath
        Check "a REG_SZ user PATH stays REG_SZ" ($r.Ok -and $kindAfterInstall -eq "String" -and $state.Kind -eq "String" -and $state.Value -ceq "C:\Plain") "$kindAfterInstall $($state.Kind) $($state.Value)"
    } finally {
        Remove-Item Env:ARN_INSTALL_TEST_USER_PATH_KEY -ErrorAction SilentlyContinue
        $env:Path = $savedPath
    }

    # 9. Uninstall checks every file before it removes any.
    $dir = Join-Path $root "preflight\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    Remove-Item -LiteralPath (Join-Path $dir "README.md")
    New-Item -ItemType Directory -Path (Join-Path $dir "README.md") | Out-Null
    $before = Get-Snapshot $dir
    $r = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true }
    Check "uninstall refuses before removing anything when a listed file became a directory" `
        ((-not $r.Ok) -and $r.Output -match 'Uninstall refused' -and (Get-Snapshot $dir) -ceq $before) "$($r.Output)"

    # 10. Uninstall interrupted after the files: the retry finishes it.
    $dir = Join-Path $root "uninstall-kill\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    Set-Content -LiteralPath (Join-Path $dir "my-notes.txt") -Value "mine"
    $child = Invoke-Child "kill:uninstall-after-files" @("-InstallDir", $dir, "-PathScope", "None", "-Uninstall")
    $manifestKept = Test-Path (Join-Path $dir "install-manifest.json")
    $r = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true }
    $left = @(Get-ChildItem -LiteralPath $dir -Force | ForEach-Object Name) -join ","
    Check "an interrupted uninstall keeps its manifest and the retry finishes" ($child.ExitCode -ne 0 -and $manifestKept -and $r.Ok -and $left -eq "my-notes.txt") "$($r.Output) / $left"

    # 11. Strict validation.
    $dir = Join-Path $root "strict\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    $manifestFile = Join-Path $dir "install-manifest.json"
    $good = [IO.File]::ReadAllText($manifestFile)
    foreach ($bad in @('"arn.exe"', ($good -replace '"files":\s*\[', '"files": [ "arn.exe\n",'), '["arn.exe", "ARN.EXE"]')) {
        [IO.File]::WriteAllText($manifestFile, $bad)
        $before = Get-Snapshot $dir
        $r = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true }
        Check "uninstall refuses the manifest $($bad.Substring(0, [Math]::Min(30, $bad.Length)))" ((-not $r.Ok) -and $r.Output -match 'Uninstall refused' -and (Get-Snapshot $dir) -ceq $before) "$($r.Output)"
    }
    [IO.File]::WriteAllText($manifestFile, $good)
    $r = Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums; Version = "1.2.3`n" }
    Check "a version with a trailing newline is refused" ((-not $r.Ok) -and $r.Output -match 'Version must be') "$($r.Output)"
    $r = Invoke-Installer @{ InstallDir = "$dir\..\bin\."; Uninstall = $true }
    Check "another spelling of the directory finds the same installation" ($r.Ok -and $r.Output -match 'was uninstalled' -and -not (Test-Path -LiteralPath $dir)) "$($r.Output)"

    # 13. Manifest files a user creates after an interruption are never
    #     deleted or overwritten. In "activating" the installer has not written
    #     a manifest yet; in "verifying" only a manifest whose content shows it
    #     is the installer's is removed. A blocked recovery changes nothing and
    #     stays blocked; once the user moves their files away it completes, and
    #     an update gets back exactly the previous manifest.
    $held = Join-Path $root "held"
    New-Item -ItemType Directory -Path $held | Out-Null
    function Move-UserFiles([string]$Dir, [string[]]$Names, [string]$Tag) {
        $to = Join-Path $held $Tag
        New-Item -ItemType Directory -Path $to -Force | Out-Null
        foreach ($name in $Names) {
            if (Test-Path -LiteralPath (Join-Path $Dir $name)) { Move-Item -LiteralPath (Join-Path $Dir $name) -Destination (Join-Path $to $name) }
        }
        return $to
    }
    function Get-JournalState([string]$Dir) {
        try { ([IO.File]::ReadAllText((Join-Path $Dir ".arn-transaction.json")) | ConvertFrom-Json).state } catch { "<none>" }
    }
    $blockedText = 'Recovery of the interrupted installation is blocked, and nothing was changed'
    $manifestCases = @(
        @("fresh", "after-first-activate", "activating"), @("fresh", "after-activate", "activating"),
        @("fresh", "after-path", "verifying"), @("fresh", "after-manifest", "verifying"),
        @("upgrade", "after-first-activate", "activating"), @("upgrade", "after-activate", "activating"),
        @("upgrade", "after-path", "verifying"), @("upgrade", "after-manifest", "verifying"))
    foreach ($case in $manifestCases) {
        $mode, $point, $expectedState = $case
        $tag = "$mode-$point"
        $dir = Join-Path $root "manifest-$tag\Arn\bin"
        $parent = Split-Path -Parent $dir
        $v1Before = $null
        if ($mode -eq "upgrade") {
            [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
            $v1Before = Get-Snapshot $dir
        }
        $child = Invoke-Child "kill:$point" (& $v2Args $dir)
        $state = Get-JournalState $dir
        # User files go only where nothing is: a manifest the installer already
        # wrote (after-manifest) stays the installer's.
        $placed = @("install-manifest.json", "install-manifest.json.new" | Where-Object { -not (Test-Path -LiteralPath (Join-Path $dir $_)) })
        foreach ($name in $placed) { [IO.File]::WriteAllText((Join-Path $dir $name), "{ `"owner`": `"the user`", `"file`": `"$name`", `"case`": `"$tag`" }`n") }
        $userHashes = Get-NamedHashes $dir $placed
        $before = Get-Snapshot $parent
        $r = & $probeRun $dir
        if ($mode -eq "fresh" -and $expectedState -eq "activating") {
            Check "user manifest files after $tag ($state): recovery completes and keeps them byte for byte" `
                ($child.ExitCode -ne 0 -and $state -ceq $expectedState -and $r.Output -match 'interrupted installation of ARN Code was found and undone' -and
                 (Get-NamedHashes $dir $placed) -ceq $userHashes -and (@(Get-ChildItem -LiteralPath $dir -Force | ForEach-Object Name | Sort-Object) -join ",") -eq (($placed | Sort-Object) -join ",") -and
                 -not (Get-WorkDirs $dir).Count) "state ${state}: $($r.Output)"
            $r = & $probeRun $dir
            Check "user manifest files after ${tag}: a second run changes nothing" ($r.Output -notmatch 'interrupted' -and (Get-NamedHashes $dir $placed) -ceq $userHashes) "$($r.Output)"
            continue
        }
        Check "user manifest files after $tag ($state): recovery is blocked, nothing changes, the files stay byte for byte" `
            ($child.ExitCode -ne 0 -and $state -ceq $expectedState -and $placed.Count -and $r.Output -match $blockedText -and
             (Get-Snapshot $parent) -ceq $before -and (Get-NamedHashes $dir $placed) -ceq $userHashes) "state $state, placed $($placed -join ','): $($r.Output)"
        $r = & $probeRun $dir
        Check "user manifest files after ${tag}: a second run is still blocked and changes nothing" `
            ($r.Output -match $blockedText -and (Get-Snapshot $parent) -ceq $before) "$($r.Output)"
        $movedTo = Move-UserFiles $dir $placed $tag
        $r = & $probeRun $dir
        $restored = if ($mode -eq "upgrade") { (Get-Snapshot $dir) -ceq $v1Before } else { -not (Test-Path -LiteralPath $dir) }
        Check "user manifest files after ${tag}: once they are moved away recovery completes$(if ($mode -eq 'upgrade') { ' with the exact v1 manifest' })" `
            ($r.Output -match 'was found and undone' -and $restored -and (Get-NamedHashes $movedTo $placed) -ceq $userHashes -and
             -not (Get-WorkDirs $dir).Count) "$($r.Output)"
    }

    # 14. Recovery interrupted right after the previous manifest was put back,
    #     between two releases with the same file names (so the previous
    #     manifest's file list equals the new one's): the next run keeps the
    #     previous manifest instead of taking it for the new one.
    $sameDir = Join-Path $root "v1same"
    Copy-Item -LiteralPath $v1Dir -Destination $sameDir -Recurse
    $thirdToken = -join ($token.ToCharArray() | ForEach-Object {
        if ($_ -match '[1-9]') { [char]([int]$_ - 1) } elseif ($_ -eq '0') { '9' }
        elseif ($_ -match '[b-z]') { [char]([int]$_ - 1) } elseif ($_ -eq 'a') { 'z' } else { $_ } })
    $sameExe = Join-Path $sameDir "arn.exe"
    [IO.File]::WriteAllBytes($sameExe, $latin1.GetBytes($latin1.GetString([IO.File]::ReadAllBytes($sameExe)).Replace($token, $thirdToken)))
    Add-Content -LiteralPath (Join-Path $sameDir "README.md") -Value "Same names, new release."
    $same = New-Release "v1same" $sameDir
    $dir = Join-Path $root "same-names\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    $before = Get-Snapshot $dir
    $v1ManifestText = [IO.File]::ReadAllText((Join-Path $dir "install-manifest.json"))
    $child = Invoke-Child "kill:after-manifest" @("-InstallDir", $dir, "-PathScope", "None", "-ArchivePath", $same.Zip, "-ChecksumsPath", $same.Sums)
    $second = Invoke-Child "kill:recovery-restored-install-manifest.json" @("-InstallDir", $dir, "-PathScope", "None", "-ArchivePath", $v2.Zip, "-ChecksumsPath", $badSums)
    $manifestAfterKill = [IO.File]::ReadAllText((Join-Path $dir "install-manifest.json"))
    $r = & $probeRun $dir
    $again = & $probeRun $dir
    Check "the previous manifest put back by an interrupted recovery is kept, and the v1 installation returns exactly" `
        ($child.ExitCode -ne 0 -and $second.ExitCode -ne 0 -and $manifestAfterKill -ceq $v1ManifestText -and $r.Output -match 'was found and undone' -and
         (Get-Snapshot $dir) -ceq $before -and (Get-ExeHash $dir) -eq $v1Hash -and $again.Output -notmatch 'interrupted') "$($r.Output)"

    # 15. A staging directory that has disappeared: in "activating" which files
    #     the installation put in place cannot be told, so nothing is done; in
    #     "verifying" every new file is in place and recovery goes ahead.
    $dir = Join-Path $root "no-stage-activating\Arn\bin"
    $parent = Split-Path -Parent $dir
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    $child = Invoke-Child "kill:after-first-activate" (& $v2Args $dir)
    @(Get-WorkDirs $dir | Where-Object Name -match 'staging') | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
    $before = Get-Snapshot $parent
    $r = & $probeRun $dir
    $again = & $probeRun $dir
    Check "a missing staging directory in 'activating' blocks recovery and changes nothing" `
        ($r.Output -match $blockedText -and $r.Output -match 'staging directory .* is missing' -and (Get-Snapshot $parent) -ceq $before -and
         $again.Output -match $blockedText -and (Get-Snapshot $parent) -ceq $before) "$($r.Output)"
    $dir = Join-Path $root "no-stage-verifying\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    $before = Get-Snapshot $dir
    $child = Invoke-Child "kill:after-path" (& $v2Args $dir)
    @(Get-WorkDirs $dir | Where-Object Name -match 'staging') | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
    $r = & $probeRun $dir
    Check "a missing staging directory in 'verifying' still restores the exact v1 installation" `
        ($r.Output -match 'was found and undone' -and (Get-Snapshot $dir) -ceq $before -and -not (Get-WorkDirs $dir).Count) "$($r.Output)"

    # 16. Two installations with the same parent: binA's update is interrupted
    #     and the user adds files there; an installation into binB then cleans
    #     up only its own leftovers and keeps binA's staging directory, and
    #     binA's recovery never touches the user's files.
    $parent = Join-Path $root "siblings\Arn"
    $binA = Join-Path $parent "binA"
    $binB = Join-Path $parent "binB"
    [void](Invoke-Installer @{ InstallDir = $binA; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    $aBefore = Get-Snapshot $binA
    $child = Invoke-Child "kill:after-first-activate" (& $v2Args $binA)
    $stageA = @(Get-ChildItem -LiteralPath $parent -Directory -Force | Where-Object Name -like '.arn-staging-*')[0].FullName
    $siblingNames = "README.md", "LICENSE", "libcrypto-4-x64.dll", "new-notes.txt", "install-manifest.json", "install-manifest.json.new"
    foreach ($name in $siblingNames) { [IO.File]::WriteAllText((Join-Path $binA $name), "binA user file $name $([guid]::NewGuid())") }
    $siblingHashes = Get-NamedHashes $binA $siblingNames
    $stageABefore = Get-Snapshot $stageA
    $orphanB = Join-Path $parent (".arn-staging-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $orphanB | Out-Null
    [IO.File]::WriteAllText((Join-Path $orphanB ".arn-owner"), "$binB`n")
    Set-Content -LiteralPath (Join-Path $orphanB "arn.exe") -Value "left by an earlier binB run"
    $emptyStage = Join-Path $parent (".arn-staging-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $emptyStage | Out-Null
    $r = Invoke-Installer @{ InstallDir = $binB; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums }
    Check "an installation into binB keeps binA's staging directory and removes only its own and empty ones" `
        ($child.ExitCode -ne 0 -and $r.Ok -and (Get-Snapshot $stageA) -ceq $stageABefore -and -not (Test-Path -LiteralPath $orphanB) -and
         -not (Test-Path -LiteralPath $emptyStage) -and $r.Output -match 'belongs to the installation in') "$($r.Output)"
    $before = Get-Snapshot $parent
    $r = & $probeRun $binA
    $again = & $probeRun $binA
    Check "binA's recovery is blocked by the user's files, changes nothing and keeps them byte for byte" `
        ($r.Output -match $blockedText -and $again.Output -match $blockedText -and (Get-Snapshot $parent) -ceq $before -and
         (Get-NamedHashes $binA $siblingNames) -ceq $siblingHashes) "$($r.Output)"
    $movedTo = Move-UserFiles $binA $siblingNames "siblings"
    $r = & $probeRun $binA
    Check "after the user moves the files, binA returns to exactly v1 and the user's files are intact" `
        ($r.Output -match 'was found and undone' -and (Get-Snapshot $binA) -ceq $aBefore -and (Get-NamedHashes $movedTo $siblingNames) -ceq $siblingHashes -and
         (Test-Path -LiteralPath (Join-Path $binB "arn.exe")) -and -not @(Get-ChildItem -LiteralPath $parent -Directory -Force | Where-Object Name -like '.arn-*').Count) "$($r.Output)"

    # 17. The Windows error mode while install.ps1 starts arn.exe
    #     (Invoke-WithoutErrorDialogs, taken straight from install.ps1). Under
    #     PowerShell 7 the dialog-suppressing bits are added only while the
    #     action runs, a child started then inherits them, every bit that was
    #     set before stays set, and the mode is put back exactly afterwards,
    #     also when the action fails. Windows PowerShell 5.1 already suppresses
    #     these dialogs, so there the function changes nothing.
    if (-not ("ArnTestErrorMode.Kernel" -as [type])) {
        Add-Type -Namespace ArnTestErrorMode -Name Kernel -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint GetErrorMode();
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint uMode);
'@
    }
    $wrapperText = @($installerAst.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                                                     $node.Name -eq "Invoke-WithoutErrorDialogs" }, $false))[0].Extent.Text
    $modeProbe = Join-Path $root "report-error-mode.ps1"
    Set-Content -LiteralPath $modeProbe -Value @'
if (-not ("ArnTestErrorModeChild.Kernel" -as [type])) {
    Add-Type -Namespace ArnTestErrorModeChild -Name Kernel -MemberDefinition '[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint GetErrorMode();'
}
[ArnTestErrorModeChild.Kernel]::GetErrorMode()
'@
    $isCore = $PSVersionTable.PSEdition -eq "Core"
    $outerMode = [ArnTestErrorMode.Kernel]::GetErrorMode()
    # A recognisable starting mode: SEM_NOGPFAULTERRORBOX (0x0002) only.
    $startMode = [uint32]0x0002
    $seen = @{}
    try {
        [void][ArnTestErrorMode.Kernel]::SetErrorMode($startMode)
        & {
            . ([scriptblock]::Create($wrapperText))
            $seen.Output = Invoke-WithoutErrorDialogs {
                $seen.During = [ArnTestErrorMode.Kernel]::GetErrorMode()
                & $powershell -NoProfile -ExecutionPolicy Bypass -File $modeProbe
                "second line"
            }
            $seen.AfterSuccess = [ArnTestErrorMode.Kernel]::GetErrorMode()
            try {
                Invoke-WithoutErrorDialogs { $seen.DuringFailure = [ArnTestErrorMode.Kernel]::GetErrorMode(); throw "action failed on purpose" }
                $seen.Caught = "<nothing was thrown>"
            } catch { $seen.Caught = $_.Exception.Message }
            $seen.AfterFailure = [ArnTestErrorMode.Kernel]::GetErrorMode()
        }
    } finally {
        [void][ArnTestErrorMode.Kernel]::SetErrorMode($outerMode)
    }
    $childMode = [uint32]@($seen.Output)[0]
    $expectedDuring = if ($isCore) { $startMode -bor 0x8001 } else { $startMode }
    $detail = "edition $($PSVersionTable.PSEdition): during 0x{0:X4}, child 0x{1:X4}, after 0x{2:X4}, during failure 0x{3:X4}, after failure 0x{4:X4}, caught '{5}'" -f `
        $seen.During, $childMode, $seen.AfterSuccess, $seen.DuringFailure, $seen.AfterFailure, $seen.Caught
    Check "the error mode is restored exactly after a successful arn.exe start ($($PSVersionTable.PSEdition))" `
        ($seen.During -eq $expectedDuring -and $seen.AfterSuccess -eq $startMode -and @($seen.Output)[1] -eq "second line") $detail
    Check "the error mode is restored exactly when the action fails, and the failure still reaches the caller ($($PSVersionTable.PSEdition))" `
        ($seen.DuringFailure -eq $expectedDuring -and $seen.AfterFailure -eq $startMode -and $seen.Caught -eq "action failed on purpose") $detail
    if ($isCore) {
        Check "a child started inside gets SEM_FAILCRITICALERRORS and SEM_NOOPENFILEERRORBOX and keeps the earlier bits" `
            ($childMode -eq ($startMode -bor 0x8001)) $detail
    } else {
        # A Windows PowerShell 5.1 child sets its own error mode as it starts,
        # so what it reports says nothing about inheritance; this case runs
        # under PowerShell 7 (pwsh), as in CI.
        Write-Host "skip: inherited error mode of a child (Windows PowerShell 5.1 children set their own)"
    }
    Check "the error mode of the test process is back as it was" ([ArnTestErrorMode.Kernel]::GetErrorMode() -eq $outerMode) ("0x{0:X4}" -f [ArnTestErrorMode.Kernel]::GetErrorMode())

    # 12. A leftover install-manifest.json.new does not survive uninstall when
    #     it is the installer's; a user's file of that name does.
    $dir = Join-Path $root "stray\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    Copy-Item -LiteralPath (Join-Path $dir "install-manifest.json") -Destination (Join-Path $dir "install-manifest.json.new")
    $r = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true }
    Check "uninstall removes a leftover install-manifest.json.new the installer wrote" ($r.Ok -and -not (Test-Path -LiteralPath $dir)) "$($r.Output)"
    $dir = Join-Path $root "stray-user\Arn\bin"
    [void](Invoke-Installer @{ InstallDir = $dir; ArchivePath = $v1.Zip; ChecksumsPath = $v1.Sums })
    [IO.File]::WriteAllText((Join-Path $dir "install-manifest.json.new"), "user's own notes`n")
    $userNew = (Get-FileHash -LiteralPath (Join-Path $dir "install-manifest.json.new") -Algorithm SHA256).Hash
    $r = Invoke-Installer @{ InstallDir = $dir; Uninstall = $true }
    Check "uninstall keeps a user's file named install-manifest.json.new" `
        ($r.Ok -and (@(Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue | ForEach-Object Name) -join ",") -eq "install-manifest.json.new" -and
         (Get-ExeHash $dir) -eq "<missing>" -and (Get-NamedHashes $dir @("install-manifest.json.new")) -ceq "install-manifest.json.new=$userNew") "$($r.Output)"

    # Killed children left their download directories, and only inside the
    # temporary directories this test gave them.
    $killedLeftovers = @($childTemps | ForEach-Object { Get-ChildItem -LiteralPath $_ -Directory -Force -ErrorAction SilentlyContinue } |
                         Where-Object { $_.Name -cmatch '^arn-install-[0-9a-f]{32}$' })
    Check "killed installers left their download directories only inside the test's own temporary directories" `
        ($killedLeftovers.Count -gt 0 -and -not @(Get-InstallerTemps | Where-Object { $installerTempsBefore -notcontains $_ }).Count) `
        "$($killedLeftovers.Count) inside; new in TEMP: $(@(Get-InstallerTemps | Where-Object { $installerTempsBefore -notcontains $_ }) -join ',')"
} finally {
    $env:Path = $savedPath
    $env:TMP = $savedTemp.TMP
    $env:TEMP = $savedTemp.TEMP
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($testKey, $false)
    if ($root.StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# After the cleanup above (which also runs when a check failed or the test
# threw): the run leaves nothing behind.
$newInstallerTemps = @(Get-InstallerTemps | Where-Object { $installerTempsBefore -notcontains $_ })
Write-Host "installer download directories in TEMP: $($installerTempsBefore.Count) before, $(@(Get-InstallerTemps).Count) after"
Check "the run leaves no installer download directory and no test directory behind" `
    (-not $newInstallerTemps.Count -and -not (Test-Path -LiteralPath $root)) "new: $($newInstallerTemps -join ','); root left: $(Test-Path -LiteralPath $root)"

Write-Host "$cases cases, $($failures.Count) failure(s)"
if ($failures.Count) {
    $failures | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
