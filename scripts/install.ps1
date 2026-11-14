[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Arn\bin"),
    [string]$Repository = "ARN-Forge/ARN-Code",
    [string]$ArchivePath = $env:ARN_INSTALL_ARCHIVE,
    # SHA256SUMS to verify -ArchivePath with; required together with it.
    [string]$ChecksumsPath = $env:ARN_INSTALL_CHECKSUMS,
    [ValidateSet("User", "Process", "None")]
    [string]$PathScope = "User",
    # "latest", or X.Y.Z / vX.Y.Z for that release (tag vX.Y.Z).
    [string]$Version = $(if ($env:ARN_VERSION) { $env:ARN_VERSION } else { "latest" }),
    # Removes the files that install-manifest.json records as ARN's, and the
    # PATH entry the installer added; user files stay.
    [switch]$Uninstall = ($env:ARN_UNINSTALL -eq "1"),
    # Replaces existing files of the same names that no manifest records,
    # for example from an installation made before manifests existed.
    [switch]$AdoptExisting = ($env:ARN_ADOPT_EXISTING -eq "1")
)

$ErrorActionPreference = "Stop"

# \z, not $: "$" would also accept a trailing newline.
if ($Repository -notmatch "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\z") {
    throw "Repository must have the form owner/name."
}
# Validated before any URL is built; the release tag is always vX.Y.Z.
$semver = '(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'
$tag = $null
if ($Version -cne "latest") {
    if ($Version -cnotmatch "^v?$semver\z") {
        throw "Version must be latest, X.Y.Z or vX.Y.Z (for example 0.9.0), got '$Version'."
    }
    $tag = "v" + $Version.TrimStart('v')
}
if (-not $Uninstall -and $ArchivePath -and -not $ChecksumsPath) {
    throw "-ArchivePath needs -ChecksumsPath, the SHA256SUMS file to verify it with."
}
# An older version replaces an installed newer one only with
# ARN_ALLOW_DOWNGRADE=1 (there is deliberately no parameter for it).
$allowDowngrade = $false
if (-not $Uninstall) {
    switch -CaseSensitive ([string]$env:ARN_ALLOW_DOWNGRADE) {
        "" { }
        "0" { }
        "1" { $allowDowngrade = $true }
        default { throw "ARN_ALLOW_DOWNGRADE must be 1 (or unset), got '$env:ARN_ALLOW_DOWNGRADE'." }
    }
}

# Relative paths are taken from the PowerShell location; ".", ".." and
# repeated separators are resolved, so one directory has one spelling.
$InstallDir = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($InstallDir)).TrimEnd('\')
$parentDir = Split-Path -Parent $InstallDir
if (-not $parentDir) { throw "InstallDir must not be a drive root." }
$manifestName = "install-manifest.json"
$manifestFormat = "arn-install-manifest/1"
$manifestPath = Join-Path $InstallDir $manifestName
# The transaction record of an installation in progress (see Undo-Transaction).
$journalName = ".arn-transaction.json"
$journalFormat = "arn-install-transaction/1"
$journalPath = Join-Path $InstallDir $journalName
$journalStates = @("staging", "backing-up", "activating", "verifying", "committed")
# Written into a backup directory once its installation is committed: only
# then may the backup be deleted.
$committedMarker = ".arn-committed"
# Written into a staging directory: the installation directory it belongs to.
$ownerMarker = ".arn-owner"
$temporaryDir = Join-Path ([IO.Path]::GetTempPath()) ("arn-install-" + [guid]::NewGuid().ToString("N"))
$archiveName = "arn-windows-x64.zip"
$temporaryZip = Join-Path $temporaryDir $archiveName
$temporaryManifest = Join-Path $temporaryDir "SHA256SUMS"
$extractDir = Join-Path $temporaryDir "payload"
$headers = @{ "User-Agent" = "ARN-Code-Installer" }

# The user PATH lives in HKCU\Environment. Tests point this at a throwaway
# HKCU\Software\ARN-Install-Test-<32 hex> key instead.
$userPathKey = "Environment"
if ($env:ARN_INSTALL_TEST_USER_PATH_KEY) {
    if ($env:ARN_INSTALL_TEST_USER_PATH_KEY -cnotmatch '^Software\\ARN-Install-Test-[0-9a-f]{32}\z') {
        throw "ARN_INSTALL_TEST_USER_PATH_KEY must name a Software\ARN-Install-Test-<32 hex digits> key."
    }
    $userPathKey = $env:ARN_INSTALL_TEST_USER_PATH_KEY
}

# Runs $Action, which starts arn.exe, so that a missing DLL makes arn.exe fail
# instead of opening a "System Error" dialog that waits for someone to click
# OK (and would stop the installer). A process passes its Windows error mode
# on to every process it creates, fixed at creation. Windows PowerShell 5.1
# already runs with SEM_FAILCRITICALERRORS; PowerShell 7 does not, so there
# SEM_FAILCRITICALERRORS (0x0001) and SEM_NOOPENFILEERRORBOX (0x8000) are added
# to the current mode only while $Action runs, and the mode is put back
# exactly, also when $Action fails.
function Invoke-WithoutErrorDialogs([scriptblock]$Action) {
    if ($PSVersionTable.PSEdition -ne "Core") { return & $Action }
    if (-not ("ArnInstaller.ErrorMode" -as [type])) {
        Add-Type -Namespace ArnInstaller -Name ErrorMode -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint GetErrorMode();
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint uMode);
'@
    }
    $saved = [ArnInstaller.ErrorMode]::GetErrorMode()
    [void][ArnInstaller.ErrorMode]::SetErrorMode($saved -bor 0x8001)
    try { & $Action } finally { [void][ArnInstaller.ErrorMode]::SetErrorMode($saved) }
}

# The Microsoft Visual C++ runtime is a prerequisite of arn.exe and of the
# OpenSSL DLLs. ARN Code does not ship it, and this installer never downloads
# or installs it: users install the Microsoft Visual C++ Redistributable from
# Microsoft. The check only looks in System32, so it needs no administrator
# rights (installing the Redistributable does).
$vcRedistUrl = "https://aka.ms/vc14/vc_redist.x64.exe"
$vcRuntimeDlls = @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_atomic_wait.dll")
function Get-MissingVcRuntime {
    $system = Join-Path $env:SystemRoot "System32"
    # A 32-bit PowerShell sees the 64-bit System32 as Sysnative.
    $sysnative = Join-Path $env:SystemRoot "Sysnative"
    if (-not [Environment]::Is64BitProcess -and (Test-Path -LiteralPath $sysnative)) { $system = $sysnative }
    # Test hook, unset in normal use: the directory to look in instead.
    if ($env:ARN_INSTALL_TEST_SYSTEM_DIR) { $system = $env:ARN_INSTALL_TEST_SYSTEM_DIR }
    return @($vcRuntimeDlls | Where-Object { -not (Test-Path -LiteralPath (Join-Path $system $_) -PathType Leaf) })
}
function Get-VcRuntimeMessage([string]$Problem) {
    return ("ARN Code needs the Microsoft Visual C++ Redistributable for x64 (Visual Studio 2017-2026), and $Problem.`n" +
            "Install it from Microsoft: $vcRedistUrl (installing it needs administrator rights), then run this installer again.`n" +
            "ARN Code was not installed.")
}
# Windows status codes of a process that could not load its DLLs:
# STATUS_DLL_NOT_FOUND and STATUS_ENTRYPOINT_NOT_FOUND (a DLL that is too old).
function Test-MissingDllStatus([int]$ExitCode) {
    $status = [BitConverter]::ToUInt32([BitConverter]::GetBytes($ExitCode), 0)
    return $status -eq 0xC0000135 -or $status -eq 0xC0000139
}

# Test hooks, unset in normal use. ARN_INSTALL_TEST_FAULT="<point>" makes that
# step fail with an error; "kill:<point>" terminates the process there without
# any cleanup, as a crash or power loss would.
function Invoke-TestFault([string]$Point) {
    $fault = $env:ARN_INSTALL_TEST_FAULT
    if (-not $fault) { return }
    if ($fault -ceq "kill:$Point") { [Diagnostics.Process]::GetCurrentProcess().Kill() }
    if ($fault -ceq $Point) { throw "Injected test failure at $Point." }
}

function Test-SamePath([string]$A, [string]$B) {
    return [string]::Equals($A.TrimEnd('\'), $B.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)
}

function Test-ReparsePoint($Item) {
    return [bool]($Item.Attributes -band [IO.FileAttributes]::ReparsePoint)
}

# A file the installer may own: a plain file name in the installation
# directory. Rejects ".", "..", rooted and drive-qualified paths, and any
# directory separator, so a manifest cannot point outside the directory.
function Test-OwnedName($Name) {
    return ($Name -is [string]) -and ($Name -cmatch '^[A-Za-z0-9][A-Za-z0-9._-]*\z') -and
           -not [string]::Equals($Name, $manifestName, [StringComparison]::OrdinalIgnoreCase)
}

# A JSON array of names: each one owned (or, with -AllowManifest, the
# manifest's own name) and none twice, ignoring case as Windows does.
function Get-NameList($Value, [string]$What, [switch]$AllowManifest) {
    if (-not ($Value -is [array])) { throw "$What is not a list of file names." }
    $seen = @{}
    foreach ($name in $Value) {
        $isManifest = $AllowManifest -and ($name -is [string]) -and ($name -ceq $manifestName)
        if (-not ((Test-OwnedName $name) -or $isManifest)) { throw "$What lists '$name', which is not a plain file name." }
        if ($seen.ContainsKey($name.ToLowerInvariant())) { throw "$What lists '$name' twice." }
        $seen[$name.ToLowerInvariant()] = $true
    }
    return , @($Value)
}

# Reads install-manifest.json. Returns $null when there is none; throws when
# it exists but is not a valid manifest for this directory.
#   format 1:  { "format": "arn-install-manifest/1", "version", "commit",
#                "installDir", "pathScope", "addedPathEntry", "files": [...] }
#   legacy:    a JSON array of file names, written by installers before 1.
function Read-InstallManifest {
    if (-not (Test-Path -LiteralPath $manifestPath)) { return $null }
    $item = Get-Item -LiteralPath $manifestPath -Force
    if ($item.PSIsContainer -or (Test-ReparsePoint $item)) {
        throw "$manifestPath is not a regular file."
    }
    try { $data = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json } catch { throw "$manifestPath is not valid JSON." }
    if ($null -eq $data) { throw "$manifestPath is empty." }
    if ($data -is [array]) {
        $names = @($data | Where-Object { -not (($_ -is [string]) -and [string]::Equals($_, $manifestName, [StringComparison]::OrdinalIgnoreCase)) })
        $names = Get-NameList $names $manifestPath
        return [pscustomobject]@{ Format = "legacy"; Files = $names; Version = $null; Commit = $null
                                  PathScope = "User"; AddedPathEntry = $null }
    }
    if (-not ($data -is [Management.Automation.PSCustomObject])) { throw "$manifestPath is not an installation manifest." }
    if ($data.format -cne $manifestFormat) { throw "$manifestPath has an unsupported format '$($data.format)'." }
    if (-not ($data.installDir -is [string]) -or -not (Test-SamePath $data.installDir $InstallDir)) {
        throw "$manifestPath belongs to '$($data.installDir)', not to $InstallDir."
    }
    if (@("User", "Process", "None") -cnotcontains $data.pathScope -or -not ($data.addedPathEntry -is [bool])) {
        throw "$manifestPath has an invalid PATH record."
    }
    $files = Get-NameList $data.files $manifestPath
    return [pscustomobject]@{ Format = "1"; Files = $files; Version = $data.version; Commit = $data.commit
                              PathScope = $data.pathScope; AddedPathEntry = $data.addedPathEntry }
}

# True when the error, or an exception inside it, is Windows' "file in use":
# exactly HRESULT 0x80070020 (ERROR_SHARING_VIOLATION) or 0x80070021
# (ERROR_LOCK_VIOLATION). Only these are retried, briefly: a virus scan or
# indexer often holds a new file for a moment.
function Test-FileInUse($ErrorRecord) {
    for ($exception = $ErrorRecord.Exception; $exception; $exception = $exception.InnerException) {
        if ($exception -is [IO.IOException] -and ('{0:X8}' -f $exception.HResult) -in @('80070020', '80070021')) {
            return $true
        }
    }
    return $false
}

function Invoke-FileOperation([scriptblock]$Operation, [string]$What) {
    for ($attempt = 1; ; $attempt++) {
        try { & $Operation; return } catch {
            if ($attempt -lt 10 -and (Test-FileInUse $_)) { Start-Sleep -Milliseconds 200; continue }
            $reason = if (Test-FileInUse $_) { "it is in use by another program (close ARN and try again)" } else { $_.Exception.Message }
            throw "Could not $What`: $reason"
        }
    }
}

function Move-ArnFile([string]$From, [string]$To, [string]$What) {
    Invoke-FileOperation { [IO.File]::Move($From, $To) } $What
}

# Writes $Text to $Path through a temporary file renamed into place, so a
# reader sees the old or the new content, never a partial file.
function Write-FileAtomically([string]$Path, [string]$Text, [string]$What) {
    $next = "$Path.new"
    [IO.File]::WriteAllText($next, $Text)
    try {
        if (Test-Path -LiteralPath $Path) {
            # [NullString]: PowerShell would pass $null as "" (no backup file wanted).
            Invoke-FileOperation { [IO.File]::Replace($next, $Path, [NullString]::Value) } $What
        } else {
            Move-ArnFile $next $Path $What
        }
    } finally {
        if (Test-Path -LiteralPath $next) { Remove-Item -LiteralPath $next -Force -ErrorAction SilentlyContinue }
    }
}

# Written after a successful installation (or a partial uninstall).
function Write-InstallManifest([string[]]$Files, [string]$VersionLine, [string]$Scope, [bool]$AddedPathEntry) {
    $versionValue = $null
    $commitValue = $null
    if ($VersionLine -match '^arn ([0-9]+\.[0-9]+\.[0-9]+)(?: \(([^)]+)\))?$') {
        $versionValue = $Matches[1]
        if ($Matches[2] -and $Matches[2] -ne "unknown") { $commitValue = $Matches[2] }
    }
    $data = [ordered]@{
        format = $manifestFormat; version = $versionValue; commit = $commitValue; installDir = $InstallDir
        pathScope = $Scope; addedPathEntry = $AddedPathEntry; files = @($Files | Sort-Object)
    }
    Write-FileAtomically $manifestPath (($data | ConvertTo-Json) + "`n") "write $manifestName"
}

# The user PATH as stored: the raw text with %VARIABLE% references unexpanded,
# and its registry type (REG_EXPAND_SZ or REG_SZ).
function Get-UserPath {
    $absent = [pscustomobject]@{ Exists = $false; Value = ""; Kind = [Microsoft.Win32.RegistryValueKind]::ExpandString }
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($userPathKey, $false)
    if (-not $key) { return $absent }
    try {
        if ($key.GetValueNames() -notcontains "Path") { return $absent }
        $kind = $key.GetValueKind("Path")
        if ($kind -ne [Microsoft.Win32.RegistryValueKind]::ExpandString -and $kind -ne [Microsoft.Win32.RegistryValueKind]::String) {
            throw "The user PATH has the unexpected registry type $kind; it is not changed. Add $InstallDir to it yourself."
        }
        $value = [string]$key.GetValue("Path", "", [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        return [pscustomobject]@{ Exists = $true; Value = $value; Kind = $kind }
    } finally { $key.Close() }
}

function Set-UserPath($State) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($userPathKey)
    try {
        if ($State.Exists) { $key.SetValue("Path", $State.Value, $State.Kind) } else { $key.DeleteValue("Path", $false) }
    } finally { $key.Close() }
    if ($userPathKey -ceq "Environment") {
        # Tells running programs (Explorer, new terminals) that the environment
        # changed: .NET broadcasts WM_SETTINGCHANGE after this call. The
        # variable it removes is one nothing uses.
        [Environment]::SetEnvironmentVariable("ARN_INSTALLER_SETTING_CHANGE", $null, "User")
    }
}

# A PATH entry names $Directory as written or after %VARIABLE% expansion.
function Test-PathEntry([string]$Entry, [string]$Directory) {
    if (-not $Entry) { return $false }
    return (Test-SamePath $Entry $Directory) -or (Test-SamePath ([Environment]::ExpandEnvironmentVariables($Entry)) $Directory)
}

function Get-PathValue([string]$Scope) {
    if ($Scope -eq "User") { return (Get-UserPath).Value }
    return $env:Path
}

function Test-InPath([string]$Directory, [string]$Scope) {
    return [bool](@((Get-PathValue $Scope) -split ';' | Where-Object { Test-PathEntry $_ $Directory }).Count)
}

# Puts $Directory first in this process's PATH and, for the User scope, adds it
# at the front of the user PATH unless an entry already names it. Every other
# entry keeps its exact text and place, and the value keeps its registry type.
# Returns the user PATH text it wrote, or $null.
function Add-ArnToPath([string]$Directory, [string]$Scope) {
    if ($Scope -eq "None") { return $null }
    $written = $null
    if ($Scope -eq "User") {
        $state = Get-UserPath
        if (-not @($state.Value -split ';' | Where-Object { Test-PathEntry $_ $Directory }).Count) {
            $written = if ($state.Value) { "$Directory;$($state.Value)" } else { $Directory }
            Set-UserPath ([pscustomobject]@{ Exists = $true; Value = $written; Kind = $state.Kind })
            Write-Host "Added $Directory to your user PATH."
        }
    }
    $otherEntries = @($env:Path -split ';' | Where-Object { $_ -and -not (Test-PathEntry $_ $Directory) })
    $env:Path = (@($Directory) + $otherEntries) -join ';'
    return $written
}

# Removes only the entries naming $Directory; every other entry keeps its
# text and its place, and the user PATH keeps its registry type.
function Remove-ArnFromPath([string]$Directory, [string]$Scope) {
    if ($Scope -eq "None") { return }
    if ($Scope -eq "User") {
        $state = Get-UserPath
        if ($state.Exists) {
            $updated = (@($state.Value -split ';' | Where-Object { -not (Test-PathEntry $_ $Directory) })) -join ';'
            if ($updated -cne $state.Value) {
                Set-UserPath ([pscustomobject]@{ Exists = $true; Value = $updated; Kind = $state.Kind })
                Write-Host "Removed $Directory from the user PATH."
            }
        }
    }
    $updated = (@($env:Path -split ';' | Where-Object { -not (Test-PathEntry $_ $Directory) })) -join ';'
    if ($updated -cne $env:Path) {
        $env:Path = $updated
        if ($Scope -eq "Process") { Write-Host "Removed $Directory from the process PATH." }
    }
}

# A staging or backup directory named by a transaction: absent, or a plain
# directory (not a junction or symbolic link).
function Assert-WorkDirectory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (-not $item.PSIsContainer -or (Test-ReparsePoint $item)) { throw "$Path is not a plain directory." }
}

# A staging directory records the installation directory it belongs to, so
# another installation sharing the parent directory never takes it for its
# own leftover (see Remove-StaleWorkDirectories).
function New-StageDirectory([string]$Path) {
    New-Item -ItemType Directory -Path $Path | Out-Null
    [IO.File]::WriteAllText((Join-Path $Path $ownerMarker), "$InstallDir`n")
}

# The installation directory a staging directory belongs to, or $null when it
# does not say.
function Get-WorkOwner([string]$Path) {
    $file = Join-Path $Path $ownerMarker
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return $null }
    try { $owner = [IO.File]::ReadAllText($file).TrimEnd("`r", "`n") } catch { return $null }
    if (-not $owner -or $owner -match '[\r\n]') { return $null }
    return $owner
}

# True only when $Path holds a manifest this transaction would have written:
# the supported format, this installation directory, and exactly the
# transaction's payload as its files. Anything else (a user's file, an older
# manifest, a partly written file) is not the transaction's. Never throws.
function Test-TransactionManifest([string]$Path, $T) {
    try {
        $item = Get-Item -LiteralPath $Path -Force
        if ($item.PSIsContainer -or (Test-ReparsePoint $item)) { return $false }
        $data = [IO.File]::ReadAllText($Path) | ConvertFrom-Json
        if (-not ($data -is [Management.Automation.PSCustomObject])) { return $false }
        if ($data.format -cne $manifestFormat -or -not ($data.installDir -is [string]) -or -not (Test-SamePath $data.installDir $InstallDir)) {
            return $false
        }
        if (-not ($data.files -is [array])) { return $false }
        $expected = @($T.Payload | ForEach-Object { [string]$_ } | Sort-Object) -join "`n"
        $actual = @($data.files | ForEach-Object { [string]$_ } | Sort-Object) -join "`n"
        return $expected -ceq $actual
    } catch { return $false }
}

# True when $Path is a JSON record of the given format for this installation
# directory (a manifest or transaction record this installer writes). Never throws.
function Test-InstallerRecord([string]$Path, [string]$Format) {
    try {
        $item = Get-Item -LiteralPath $Path -Force
        if ($item.PSIsContainer -or (Test-ReparsePoint $item)) { return $false }
        $data = [IO.File]::ReadAllText($Path) | ConvertFrom-Json
        return ($data -is [Management.Automation.PSCustomObject]) -and ($data.format -ceq $Format) -and
               ($data.installDir -is [string]) -and (Test-SamePath $data.installDir $InstallDir)
    } catch { return $false }
}

# Deletes a work directory. Files go one by one and the commit marker last,
# so a backup that cannot be emptied (an ARN still running from it) keeps its
# marker and the next run can still delete it. Returns $true when it is gone.
function Remove-WorkDirectory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $true }
    foreach ($item in @(Get-ChildItem -LiteralPath $Path -Force | Where-Object { $_.Name -cne $committedMarker })) {
        Remove-Item -LiteralPath $item.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (@(Get-ChildItem -LiteralPath $Path -Force | Where-Object { $_.Name -cne $committedMarker }).Count) { return $false }
    Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue
    return -not (Test-Path -LiteralPath $Path)
}

function Write-Transaction($Transaction) {
    Write-FileAtomically $journalPath (($Transaction | ConvertTo-Json) + "`n") "record the installation in $journalName"
}

# Reads and validates .arn-transaction.json:
#   { "format": "arn-install-transaction/1", "installDir", "state",
#     "stageDir": ".arn-staging-<32 hex>", "backupDir": ".arn-backup-<32 hex>",
#     "fresh", "hadManifest", "createdDir", "pathScope", "pathAdded",
#     "plan": [names moved aside, the manifest last], "payload": [new names],
#     "previousVersion": "arn X.Y.Z (...)" or null }
# Throws on anything unexpected; nothing is touched then.
function Read-Transaction {
    if (-not (Test-Path -LiteralPath $journalPath)) { return $null }
    $item = Get-Item -LiteralPath $journalPath -Force
    if ($item.PSIsContainer -or (Test-ReparsePoint $item)) { throw "$journalPath is not a regular file." }
    try { $data = [IO.File]::ReadAllText($journalPath) | ConvertFrom-Json } catch { throw "$journalPath is not valid JSON." }
    if (-not ($data -is [Management.Automation.PSCustomObject])) { throw "$journalPath is not a transaction record." }
    $invalid = "$journalPath is not a valid transaction record"
    if ($data.format -cne $journalFormat) { throw "$invalid (format '$($data.format)')." }
    if (-not ($data.installDir -is [string]) -or -not (Test-SamePath $data.installDir $InstallDir)) {
        throw "$invalid (it belongs to '$($data.installDir)')."
    }
    if (-not ($data.state -is [string]) -or $journalStates -cnotcontains $data.state) { throw "$invalid (state '$($data.state)')." }
    if (-not ($data.stageDir -is [string]) -or $data.stageDir -cnotmatch '^\.arn-staging-[0-9a-f]{32}\z') { throw "$invalid (staging directory)." }
    if (-not ($data.backupDir -is [string]) -or $data.backupDir -cnotmatch '^\.arn-backup-[0-9a-f]{32}\z') { throw "$invalid (backup directory)." }
    foreach ($flag in "fresh", "hadManifest", "createdDir", "pathAdded") {
        if (-not ($data.$flag -is [bool])) { throw "$invalid ($flag)." }
    }
    if (@("User", "Process", "None") -cnotcontains $data.pathScope) { throw "$invalid (PATH scope)." }
    $plan = Get-NameList $data.plan "$journalPath (plan)" -AllowManifest
    $payload = Get-NameList $data.payload "$journalPath (payload)"
    $previous = $data.previousVersion
    if (-not (($null -eq $previous) -or (($previous -is [string]) -and $previous -cmatch '^arn [0-9]+\.[0-9]+\.[0-9]+[^\r\n]*\z'))) {
        throw "$invalid (previous version)."
    }
    $planHasManifest = [bool]@($plan | Where-Object { $_ -ceq $manifestName }).Count
    $planHasFiles = [bool]@($plan | Where-Object { $_ -cne $manifestName }).Count
    if ($data.hadManifest -ne $planHasManifest -or $data.fresh -eq $planHasFiles) { throw "$invalid (inconsistent plan)." }
    return [pscustomobject]@{
        State = $data.state; StageDir = $data.stageDir; BackupDir = $data.backupDir; Fresh = $data.fresh
        HadManifest = $data.hadManifest; CreatedDir = $data.createdDir; PathScope = $data.pathScope
        PathAdded = $data.pathAdded; Plan = $plan; Payload = $payload; PreviousVersion = $previous
    }
}

# Rolls back an installation that did not commit, from what is on disk, so
# it works the same after an error, a crash or a power loss, and can be
# repeated if it is itself interrupted. What may have changed depends on the
# recorded state:
#   staging     nothing in the installation directory: only the staging
#               directory and an empty backup directory are removed;
#   backing-up  previous files may be in the backup: they move back;
#   activating  additionally new files may be in place: a new file is in the
#               installation when its old file is in the backup (or it had
#               none and is no longer staged); it goes back to staging. This
#               needs the staging directory: without it, which files were
#               moved in cannot be told, and nothing is done;
#   verifying   every new file is in place (they go back to staging), and the
#               new manifest may be: a manifest (or install-manifest.json.new)
#               is deleted only when its content shows this transaction wrote
#               it (see Test-TransactionManifest);
#   all states  everything in the backup moves back (in "staging" the backup
#               must be empty), and a PATH entry the transaction added is
#               removed.
# Everything is checked before anything changes: a file in the way that is
# not provably the transaction's (a user's file, even with a package or
# manifest name) stops the recovery with nothing changed, and is never
# deleted or overwritten. If an operation itself then fails, the error says
# what was already done; the record and backup stay, and running again
# continues from there. A committed transaction is never rolled back (see
# Complete-Transaction). Afterwards the previous version is checked; returns
# $true when it was verified (or there was no previous installation).
function Undo-Transaction($T) {
    if ($T.State -ceq "committed") {
        throw "The installation recorded in $journalPath is committed; it is finished, not rolled back."
    }
    $stage = Join-Path $parentDir $T.StageDir
    $backup = Join-Path $parentDir $T.BackupDir
    Assert-WorkDirectory $stage
    Assert-WorkDirectory $backup
    $planned = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $T.Plan) { [void]$planned.Add($name) }
    $vacated = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $conflicts = New-Object System.Collections.Generic.List[string]
    $actions = New-Object System.Collections.Generic.List[object]

    # 1. New files back to staging.
    if (@("activating", "verifying") -ccontains $T.State) {
        if ($T.State -ceq "activating" -and -not (Test-Path -LiteralPath $stage)) {
            $conflicts.Add("its staging directory $stage is missing, so it cannot be told which files in $InstallDir the installation put there")
        } else {
            foreach ($name in $T.Payload) {
                $current = Join-Path $InstallDir $name
                if (-not (Test-Path -LiteralPath $current -PathType Leaf)) { continue }
                $staged = Join-Path $stage $name
                $stillStaged = Test-Path -LiteralPath $staged
                $isNew = if ($planned.Contains($name)) { Test-Path -LiteralPath (Join-Path $backup $name) } else { -not $stillStaged }
                if (-not $isNew) { continue }
                if ($stillStaged) {
                    $conflicts.Add("$current is not the installer's (the new $name is still staged in $stage)")
                    continue
                }
                $actions.Add([pscustomobject]@{ Kind = "stage"; Name = $name; From = $current; To = $staged; What = "took back the new $name" })
                [void]$vacated.Add($name)
            }
        }
    }

    # 2. The new manifest, written only in "verifying".
    if ($T.State -ceq "verifying") {
        $nextManifest = "$manifestPath.new"
        if (Test-Path -LiteralPath $nextManifest) {
            if (Test-TransactionManifest $nextManifest $T) {
                $actions.Add([pscustomobject]@{ Kind = "delete"; Name = "$manifestName.new"; From = $nextManifest; To = $null; What = "removed the new $manifestName.new" })
            } else {
                $conflicts.Add("$nextManifest cannot be identified as written by the installer")
            }
        }
        if ((Test-Path -LiteralPath $manifestPath) -and ((-not $T.HadManifest) -or (Test-Path -LiteralPath (Join-Path $backup $manifestName)))) {
            if (Test-TransactionManifest $manifestPath $T) {
                $actions.Add([pscustomobject]@{ Kind = "delete"; Name = $manifestName; From = $manifestPath; To = $null; What = "removed the new $manifestName" })
                [void]$vacated.Add($manifestName)
            } else {
                $conflicts.Add("$manifestPath is not the manifest the installer wrote")
            }
        }
    }

    # 3. The previous files back from the backup, never over a file that stays.
    $backupItems = if (Test-Path -LiteralPath $backup) { @(Get-ChildItem -LiteralPath $backup -Force) } else { @() }
    if ($T.State -ceq "staging" -and $backupItems.Count) {
        # Nothing is moved aside before "backing-up"; a backup with files in
        # this state is not one this record explains.
        $conflicts.Add("$backup is not empty, although nothing was moved aside yet")
        $backupItems = @()
    }
    foreach ($item in $backupItems) {
        if ($item.PSIsContainer -or (Test-ReparsePoint $item) -or -not $planned.Contains($item.Name)) {
            $conflicts.Add("$($item.FullName) is not a file this installation moved aside")
            continue
        }
        $target = Join-Path $InstallDir $item.Name
        if ((Test-Path -LiteralPath $target) -and -not $vacated.Contains($item.Name)) {
            $conflicts.Add("$target is in the way of the previous $($item.Name), which is kept in $backup")
            continue
        }
        $actions.Add([pscustomobject]@{ Kind = "restore"; Name = $item.Name; From = $item.FullName; To = $target; What = "restored the previous $($item.Name)" })
    }

    if ($conflicts.Count) {
        throw ("Recovery of the interrupted installation is blocked, and nothing was changed: $($conflicts -join '; '). " +
               "$journalPath and the backup $backup were kept. Files named here that are yours are not touched by the installer: " +
               "move them out of $InstallDir (or put back what is missing), then run the installer again.")
    }

    $done = New-Object System.Collections.Generic.List[string]
    try {
        foreach ($action in $actions) {
            switch ($action.Kind) {
                "stage" {
                    if (-not (Test-Path -LiteralPath $stage)) { New-StageDirectory $stage }
                    Move-ArnFile $action.From $action.To "take back the new $($action.Name)"
                    $done.Add($action.What)
                    Invoke-TestFault "recovery-move"
                }
                "delete" {
                    Invoke-FileOperation { [IO.File]::Delete($action.From) } "remove $($action.From)"
                    $done.Add($action.What)
                }
                "restore" {
                    Move-ArnFile $action.From $action.To "restore the previous $($action.Name)"
                    $done.Add($action.What)
                    Invoke-TestFault "recovery-restore"
                    Invoke-TestFault "recovery-restored-$($action.Name)"
                }
            }
        }
        if ($T.PathAdded -and $T.PathScope -eq "User") {
            Remove-ArnFromPath $InstallDir "User"
            $done.Add("removed $InstallDir from the user PATH")
        }
    } catch {
        throw ("Recovery of the interrupted installation stopped part-way: $($_.Exception.Message). " +
               "Already done: $(if ($done.Count) { $done -join '; ' } else { 'nothing' }). $journalPath and what is left in " +
               "$backup were kept; run the installer again to finish (it continues from the current state).")
    }

    $verified = $true
    if (-not $T.Fresh) {
        if ($planned.Contains("arn.exe")) {
            $restored = Join-Path $InstallDir "arn.exe"
            if (-not (Test-Path -LiteralPath $restored -PathType Leaf)) { $verified = $false }
            elseif ($T.PreviousVersion) {
                $restoredVersion = try { (Invoke-WithoutErrorDialogs { & $restored --version 2>$null }) -join "`n" } catch { $null }
                if ($LASTEXITCODE -ne 0 -or $restoredVersion -cne $T.PreviousVersion) { $verified = $false }
            }
        }
        if ($T.HadManifest) { try { if (-not (Read-InstallManifest)) { $verified = $false } } catch { $verified = $false } }
    }

    # The staging directory holds only new files, the backup is now empty.
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    if ((Test-Path -LiteralPath $backup) -and -not @(Get-ChildItem -LiteralPath $backup -Force).Count) {
        Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue
    }
    Invoke-FileOperation { [IO.File]::Delete($journalPath) } "remove $journalName"
    if ($T.CreatedDir -and (Test-Path -LiteralPath $InstallDir) -and -not @(Get-ChildItem -LiteralPath $InstallDir -Force).Count) {
        Remove-Item -LiteralPath $InstallDir -Force -ErrorAction SilentlyContinue
    }
    return $verified
}

# Finishes a committed installation: marks the backup as no longer needed,
# drops the record, then deletes the work directories. Returns $false when the
# backup could not be deleted yet (an older ARN still running from it); its
# marker lets a later run delete it.
function Complete-Transaction($T) {
    $stage = Join-Path $parentDir $T.StageDir
    $backup = Join-Path $parentDir $T.BackupDir
    Assert-WorkDirectory $stage
    Assert-WorkDirectory $backup
    if ((Test-Path -LiteralPath $backup) -and @(Get-ChildItem -LiteralPath $backup -Force).Count) {
        [IO.File]::WriteAllText((Join-Path $backup $committedMarker), "$journalFormat committed`n")
    }
    Invoke-FileOperation { [IO.File]::Delete($journalPath) } "remove $journalName"
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    return Remove-WorkDirectory $backup
}

# Recovers an installation that was interrupted (by an error, a crash or a
# power loss) before anything else looks at the directory.
function Resolve-PendingTransaction([string]$Action) {
    if (-not (Test-Path -LiteralPath $journalPath)) { return }
    try { $transaction = Read-Transaction } catch {
        throw ("$Action refused: an interrupted installation was found, but its record cannot be used: $($_.Exception.Message)`n" +
               "Nothing was changed, and every .arn-backup-* directory in $parentDir was kept: one may hold the only copy " +
               "of the previous ARN version. To recover by hand, close ARN, move the files of the matching .arn-backup-* " +
               "directory back into $InstallDir (replacing the files of the same names), then delete $journalPath.")
    }
    if ($transaction.State -ceq "committed") {
        if (-not (Complete-Transaction $transaction)) {
            Write-Warning "The previous ARN version is still running, so its files remain in $(Join-Path $parentDir $transaction.BackupDir); a later installation removes them."
        }
        Write-Host "Finished cleaning up after an earlier ARN Code installation."
        return
    }
    try { $verified = Undo-Transaction $transaction } catch { throw "$Action refused: $($_.Exception.Message)" }
    if ($transaction.Fresh) {
        Write-Host "An interrupted installation of ARN Code was found and undone."
    } elseif ($verified) {
        $what = if ($transaction.PreviousVersion) { $transaction.PreviousVersion } else { "the previous version" }
        Write-Host "An interrupted update of ARN Code was found and undone; $what is installed again."
    } else {
        Write-Warning ("An interrupted update of ARN Code was found and its previous files were moved back, but the " +
                       "restored installation could not be verified. Install ARN Code again to repair it.")
    }
}

# Staging and backup directories this installer names, left by an
# interrupted run or by an ARN that was still running during an update. Only
# called once no transaction of this directory is pending. Other installation
# directories with the same parent may have interrupted transactions of their
# own, so:
#   a staging directory is deleted when it belongs to this installation
#   directory (its .arn-owner says so) or is empty; one of another (or an
#   unknown) installation is kept: its recovery may still need it;
#   a backup is deleted only when it is empty or marked as committed:
#   otherwise it may hold the only copy of an earlier version.
function Remove-StaleWorkDirectories {
    if (Test-Path -LiteralPath $journalPath) { return }
    # Temporary files of an interrupted write, removed only when their content
    # shows they are this installer's records for this directory.
    foreach ($leftover in @(@("$manifestPath.new", $manifestFormat), @("$journalPath.new", $journalFormat))) {
        if ((Test-Path -LiteralPath $leftover[0] -PathType Leaf) -and (Test-InstallerRecord $leftover[0] $leftover[1])) {
            Remove-Item -LiteralPath $leftover[0] -Force -ErrorAction SilentlyContinue
        }
    }
    if (-not (Test-Path -LiteralPath $parentDir)) { return }
    foreach ($dir in @(Get-ChildItem -LiteralPath $parentDir -Directory -Force -ErrorAction SilentlyContinue |
                       Where-Object { $_.Name -cmatch '^\.arn-(staging|backup)-[0-9a-f]{32}\z' })) {
        if (Test-ReparsePoint $dir) { Write-Warning "$($dir.FullName) is a link, not a directory this installer made; it was left alone."; continue }
        if ($dir.Name.StartsWith(".arn-staging-")) {
            $owner = Get-WorkOwner $dir.FullName
            if (($owner -and (Test-SamePath $owner $InstallDir)) -or -not @(Get-ChildItem -LiteralPath $dir.FullName -Force).Count) {
                Remove-Item -LiteralPath $dir.FullName -Recurse -Force -ErrorAction SilentlyContinue
            } else {
                Write-Warning ("$($dir.FullName) belongs to $(if ($owner) { "the installation in $owner" } else { 'another installation' }), " +
                               "whose interrupted update may still need it, so it was kept. Run the installer for that directory to finish it.")
            }
            continue
        }
        $content = @(Get-ChildItem -LiteralPath $dir.FullName -Force)
        if ($content.Count -and -not (Test-Path -LiteralPath (Join-Path $dir.FullName $committedMarker) -PathType Leaf)) {
            Write-Warning ("$($dir.FullName) holds files of an interrupted installation, possibly the only copy of an earlier " +
                           "ARN version, so it was kept. Run the installer for the directory it belongs to, or delete it once you no longer need it.")
            continue
        }
        if (-not (Remove-WorkDirectory $dir.FullName)) {
            Write-Warning "$($dir.FullName) is still in use (an older ARN may be running); delete it after closing ARN."
        }
    }
}

function Stop-WithManualCleanup([string]$Reason) {
    throw ("$Reason`nNothing was changed. To clean up by hand: close ARN, then delete the files ARN Code installed in " +
           "$InstallDir (arn.exe, its DLLs, LICENSE, README.md and $manifestName; keep your own files) and remove " +
           "$InstallDir from your user PATH if the installer added it.")
}

# Exactly X.Y.Z, decimal numbers without leading zeros.
function Test-Semver($Value) {
    return ($Value -is [string]) -and ($Value -cmatch "^$semver\z")
}

# -1, 0 or 1 for two validated X.Y.Z versions. Each part is compared as a
# number of any size (the shorter digit string is smaller, equal lengths
# compare digit by digit), never as a whole string, so 0.10.0 is newer than 0.9.0.
function Compare-ArnVersion([string]$A, [string]$B) {
    $x = $A.Split('.')
    $y = $B.Split('.')
    for ($k = 0; $k -lt 3; $k++) {
        if ($x[$k].Length -ne $y[$k].Length) { if ($x[$k].Length -lt $y[$k].Length) { return -1 } else { return 1 } }
        $order = [string]::CompareOrdinal($x[$k], $y[$k])
        if ($order -lt 0) { return -1 }
        if ($order -gt 0) { return 1 }
    }
    return 0
}

# The installed version, for the downgrade check: the version the manifest
# records, or else what the installed arn.exe reports. arn.exe is run only
# when it is ARN's: the manifest lists it, or -AdoptExisting claims it (any
# other arn.exe makes the update stop later anyway). $null when no valid
# X.Y.Z is found (the version is then unknown, never guessed).
function Get-InstalledArnVersion {
    $owned = [bool]$AdoptExisting
    try {
        $recorded = Read-InstallManifest
        if ($recorded -and (Test-Semver $recorded.Version)) { return $recorded.Version }
        if ($recorded -and @($recorded.Files | Where-Object { [string]::Equals($_, "arn.exe", [StringComparison]::OrdinalIgnoreCase) }).Count) { $owned = $true }
    } catch { }
    if (-not $owned) { return $null }
    $executable = Join-Path $InstallDir "arn.exe"
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { return $null }
    if (Test-ReparsePoint (Get-Item -LiteralPath $executable -Force)) { return $null }
    try {
        $reported = @(Invoke-WithoutErrorDialogs { & $executable --version 2>$null }) -join "`n"
        if ($LASTEXITCODE -eq 0 -and $reported -cmatch "^arn ($semver)(?: [^\r\n]*)?\z") { return $Matches[1] }
    } catch { }
    return $null
}

# The ready-to-copy command that runs this installer from the repository's
# main branch with the given environment variables.
function Get-InstallCommand([System.Collections.Specialized.OrderedDictionary]$Variables) {
    $url = "https://raw.githubusercontent.com/$Repository/main/scripts/install.ps1"
    $settings = @($Variables.Keys | ForEach-Object { "`$env:$_ = `"$($Variables[$_])`"" }) -join "; "
    if ($Repository -ceq "ARN-Forge/ARN-Code") { return "$settings; irm $url | iex" }
    return "$settings; & ([scriptblock]::Create((irm $url))) -Repository $Repository"
}

# Refuses to replace a newer installed version unless ARN_ALLOW_DOWNGRADE=1.
$script:downgradeWarned = $false
function Assert-NotDowngrade([string]$Installed, [string]$New, [string]$Retry) {
    if (-not $Installed -or (Compare-ArnVersion $New $Installed) -ge 0) { return }
    if ($allowDowngrade) {
        if (-not $script:downgradeWarned) {
            Write-Warning "Replacing ARN Code $Installed with the older $New, because ARN_ALLOW_DOWNGRADE=1."
            $script:downgradeWarned = $true
        }
        return
    }
    throw ("Downgrade refused: ARN Code $Installed is installed in $InstallDir, and $New is older.`n" +
           "Nothing was changed. To install $New anyway, run the installer again with ARN_ALLOW_DOWNGRADE=1$Retry")
}

# A header of an error response, from Windows PowerShell (WebHeaderCollection)
# or PowerShell 7 (HttpResponseHeaders); $null when absent.
function Get-ResponseHeader($Response, [string]$Name) {
    try {
        $all = $Response.Headers
        if ($all -is [Collections.IDictionary]) {
            foreach ($key in @($all.Keys)) { if ([string]$key -ieq $Name) { return [string](@($all[$key])[0]) } }
            return $null
        }
        if ($all -is [Net.WebHeaderCollection]) { return $all[$Name] }
        $values = $null
        if ($all.TryGetValues($Name, [ref]$values)) { return [string](@($values)[0]) }
    } catch { }
    return $null
}

# GET https://api.github.com/repos/<Repository>/<Path>. Returns Ok, Status
# (the HTTP status, 0 without an answer), Data and Problem. Never retries.
function Invoke-GitHubApi([string]$Path) {
    $apiHeaders = @{ "User-Agent" = "ARN-Code-Installer"; "Accept" = "application/vnd.github+json"; "X-GitHub-Api-Version" = "2022-11-28" }
    try {
        $data = Invoke-RestMethod -Headers $apiHeaders -Uri "https://api.github.com/repos/$Repository/$Path" -TimeoutSec 60
        return [pscustomobject]@{ Ok = $true; Status = 200; Data = $data; Problem = $null }
    } catch {
        $status = 0
        $response = $_.Exception.Response
        if ($response) { try { $status = [int]$response.StatusCode } catch { $status = 0 } }
        $body = if ($_.ErrorDetails) { [string]$_.ErrorDetails.Message } else { "" }
        if ($status -eq 429 -or ($status -eq 403 -and ((Get-ResponseHeader $response "X-RateLimit-Remaining") -ceq "0" -or $body -match 'rate limit'))) {
            $problem = "the GitHub API rate limit was reached (GitHub allows 60 requests an hour from one address without signing in); try again later"
        } elseif ($status) {
            $problem = "the GitHub API answered HTTP $status"
        } else {
            $problem = "GitHub could not be reached ($($_.Exception.Message))"
        }
        return [pscustomobject]@{ Ok = $false; Status = $status; Data = $null; Problem = $problem }
    }
}

# Checks one release object from the GitHub API: a string tag_name vX.Y.Z,
# boolean draft and prerelease, and an assets array of objects with string
# names. Returns its tag, flags and how often the archive and SHA256SUMS are
# listed, or $null when the metadata is malformed.
function Get-ReleaseInfo($Release) {
    if (-not ($Release -is [Management.Automation.PSCustomObject])) { return $null }
    $tagName = $Release.tag_name
    if (-not ($tagName -is [string]) -or $tagName -cnotmatch "^v$semver\z") { return $null }
    if (-not ($Release.draft -is [bool]) -or -not ($Release.prerelease -is [bool])) { return $null }
    $assets = $Release.assets
    if (-not ($assets -is [array])) { return $null }
    $archiveCount = 0
    $sumsCount = 0
    foreach ($asset in $assets) {
        if (-not ($asset -is [Management.Automation.PSCustomObject]) -or -not ($asset.name -is [string])) { return $null }
        if ($asset.name -ceq $archiveName) { $archiveCount++ }
        elseif ($asset.name -ceq "SHA256SUMS") { $sumsCount++ }
    }
    return [pscustomobject]@{ Tag = $tagName; Version = $tagName.Substring(1); Draft = $Release.draft
                              Prerelease = $Release.prerelease; ArchiveCount = $archiveCount; SumsCount = $sumsCount }
}

# The latest stable release cannot be installed here. Nothing older is
# installed in its place: the message names the newest release that could
# be, as a command to run.
function Stop-WithoutRelease([string]$Reason, [string]$LatestTag) {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add($Reason)
    $answer = Invoke-GitHubApi "releases?per_page=100"
    # PowerShell 7 hands a JSON array out item by item (none for "[]").
    $releases = if ($answer.Ok) { @($answer.Data | Where-Object { $null -ne $_ }) } else { $null }
    if (-not $answer.Ok) {
        $lines.Add("Could not look for another release that has ${archiveName}: $($answer.Problem).")
    } elseif (@($releases | Where-Object { -not ($_ -is [Management.Automation.PSCustomObject]) }).Count) {
        $lines.Add("Could not look for another release that has ${archiveName}: GitHub returned malformed release metadata.")
    } else {
        $stable = $null
        $prerelease = $null
        foreach ($release in $releases) {
            $info = Get-ReleaseInfo $release
            if (-not $info -or $info.Draft -or $info.Tag -ceq $LatestTag -or $info.ArchiveCount -ne 1 -or $info.SumsCount -ne 1) { continue }
            if (-not $info.Prerelease) {
                if (-not $stable -or (Compare-ArnVersion $info.Version $stable) -gt 0) { $stable = $info.Version }
            } elseif (-not $prerelease -or (Compare-ArnVersion $info.Version $prerelease) -gt 0) {
                $prerelease = $info.Version
            }
        }
        if ($stable) {
            $lines.Add("The newest stable release that has $archiveName and SHA256SUMS is v$stable.")
            $lines.Add("It is not installed automatically. To install it, run:")
            $lines.Add("  " + (Get-InstallCommand ([ordered]@{ ARN_VERSION = $stable })))
        } else {
            $lines.Add("No compatible release: no stable ARN Code release has both $archiveName and SHA256SUMS.")
        }
        if ($prerelease -and (-not $stable -or (Compare-ArnVersion $prerelease $stable) -gt 0)) {
            $lines.Add("The prerelease v$prerelease has them; prereleases are installed only when named:")
            $lines.Add("  " + (Get-InstallCommand ([ordered]@{ ARN_VERSION = $prerelease })))
        }
    }
    $lines.Add("ARN Code was not installed.")
    throw ($lines -join "`n")
}

Resolve-PendingTransaction $(if ($Uninstall) { "Uninstall" } else { "Update" })

if ($Uninstall) {
    if (-not (Test-Path -LiteralPath $InstallDir -PathType Container)) {
        Write-Host "ARN Code is not installed in $InstallDir; nothing to remove."
        return
    }
    try { $manifest = Read-InstallManifest } catch { Stop-WithManualCleanup "Uninstall refused: $($_.Exception.Message)" }
    if (-not $manifest) {
        if (Test-Path -LiteralPath (Join-Path $InstallDir "arn.exe")) {
            Stop-WithManualCleanup "Uninstall refused: $InstallDir has arn.exe but no $manifestName, so it is unknown which files are ARN's."
        }
        Remove-StaleWorkDirectories
        Write-Host "ARN Code is not installed in $InstallDir; nothing to remove."
        return
    }
    # Check every listed file first: nothing is removed unless all of them
    # are still plain files.
    foreach ($name in $manifest.Files) {
        $path = Join-Path $InstallDir $name
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $item = Get-Item -LiteralPath $path -Force
        if ($item.PSIsContainer -or (Test-ReparsePoint $item)) {
            Stop-WithManualCleanup "Uninstall refused: $path is not a regular file any more, so it is not ARN's to remove."
        }
    }
    $remaining = New-Object System.Collections.Generic.List[string]
    foreach ($name in $manifest.Files) {
        $path = Join-Path $InstallDir $name
        if (-not (Test-Path -LiteralPath $path)) { continue }
        try {
            Invoke-FileOperation { [IO.File]::Delete($path) } "remove $path"
        } catch {
            Write-Warning $_.Exception.Message
            $remaining.Add($name)
        }
    }
    # A legacy manifest does not say whether the installer added the PATH entry;
    # those installers always added it to the user PATH.
    $addedPathEntry = if ($null -eq $manifest.AddedPathEntry) { Test-InPath $InstallDir "User" } else { $manifest.AddedPathEntry }
    if ($remaining.Count) {
        Write-InstallManifest $remaining.ToArray() "arn $($manifest.Version)" $manifest.PathScope $addedPathEntry
        throw "Uninstall incomplete: $($remaining -join ', ') could not be removed (close ARN, then run the uninstall again). The PATH was not changed."
    }
    Invoke-TestFault "uninstall-after-files"
    # The PATH goes before the manifest: if it cannot be changed, the manifest
    # still records the entry for the next attempt.
    if ($addedPathEntry) { Remove-ArnFromPath $InstallDir $manifest.PathScope }
    Invoke-FileOperation { [IO.File]::Delete($manifestPath) } "remove $manifestPath"
    Remove-StaleWorkDirectories
    $left = @(Get-ChildItem -LiteralPath $InstallDir -Force)
    if ($left.Count) {
        Write-Host "ARN Code was uninstalled. These files are not ARN's and were kept in ${InstallDir}: $($left.Name -join ', ')"
    } else {
        Remove-Item -LiteralPath $InstallDir -Force
        Write-Host "ARN Code was uninstalled and $InstallDir removed."
    }
    return
}

# Before anything is looked up or downloaded.
$missingRuntime = @(Get-MissingVcRuntime)
if ($missingRuntime.Count) {
    throw (Get-VcRuntimeMessage "it is not installed on this computer (System32 has no $($missingRuntime -join ', '))")
}

$currentVersion = Get-InstalledArnVersion
try {
    New-Item -ItemType Directory -Path $temporaryDir, $extractDir -Force | Out-Null
    # Release resolution. The release is looked up first; nothing is
    # downloaded unless it is the requested (or latest stable) release, is
    # published, and lists both the archive for this platform and SHA256SUMS.
    # SHA256SUMS comes first and must have an entry for the archive; the
    # archive is downloaded only then, and it is checked before it is
    # extracted or run.
    if ($ArchivePath) {
        $resolvedArchive = (Resolve-Path -LiteralPath $ArchivePath).Path
        $resolvedManifest = (Resolve-Path -LiteralPath $ChecksumsPath).Path
        Copy-Item -LiteralPath $resolvedManifest -Destination $temporaryManifest
        Write-Host "Installing ARN Code from local package..."
    } else {
        $what = if ($tag) { "ARN Code $tag" } else { "the latest ARN Code release" }
        $answer = Invoke-GitHubApi $(if ($tag) { "releases/tags/$tag" } else { "releases/latest" })
        if (-not $answer.Ok) {
            if ($answer.Status -eq 404 -and $tag) { throw "There is no ARN Code release $tag (no published release has that tag in $Repository). ARN Code was not installed." }
            if ($answer.Status -eq 404) { throw "$Repository has no published stable ARN Code release. ARN Code was not installed." }
            throw "Could not look up ${what}: $($answer.Problem). ARN Code was not installed."
        }
        $info = Get-ReleaseInfo $answer.Data
        if (-not $info -or ($tag -and $info.Tag -cne $tag)) {
            throw "GitHub returned malformed release metadata for $what; ARN Code was not installed."
        }
        if (-not $tag -and ($info.Draft -or $info.Prerelease)) {
            throw "GitHub named $($info.Tag), a draft or prerelease, as the latest release; it is not installed without ARN_VERSION."
        }
        if ($info.Draft) { throw "ARN Code $tag is an unpublished draft release; it is not installed." }
        if ($info.ArchiveCount -gt 1 -or $info.SumsCount -gt 1) {
            throw "GitHub returned malformed release metadata for $what (an asset is listed twice); ARN Code was not installed."
        }
        if (-not $tag) {
            $tag = $info.Tag
            if ($info.ArchiveCount -eq 0) { Stop-WithoutRelease "The latest ARN Code release, $tag, has no $archiveName (no build for Windows x64)." $tag }
            if ($info.SumsCount -eq 0) { Stop-WithoutRelease "The latest ARN Code release, $tag, has no SHA256SUMS, so its $archiveName cannot be verified." $tag }
        } else {
            if ($info.ArchiveCount -eq 0) { throw "ARN Code $tag has no $archiveName (no build for Windows x64); ARN Code was not installed." }
            if ($info.SumsCount -eq 0) { throw "ARN Code $tag has no SHA256SUMS, so its $archiveName cannot be verified; ARN Code was not installed." }
            if ($info.Prerelease) { Write-Host "Note: $tag is a prerelease; it is installed because ARN_VERSION names it." }
        }
        Assert-NotDowngrade $currentVersion $info.Version (", for example:`n  " +
            (Get-InstallCommand ([ordered]@{ ARN_ALLOW_DOWNGRADE = "1"; ARN_VERSION = $info.Version })))
        # Both files come from the same tag.
        $download = "https://github.com/$Repository/releases/download/$tag"
        Write-Host "Downloading ARN Code $tag..."
        try {
            Invoke-WebRequest -Headers $headers -Uri "$download/SHA256SUMS" -OutFile $temporaryManifest
        } catch {
            throw "Could not download SHA256SUMS for $tag; the archive cannot be verified, so it is not installed: $($_.Exception.Message)"
        }
    }

    # Verify the archive before anything is extracted or run. Every manifest
    # line must be "<64 hex digits><space><space or *><file name>"; the
    # manifest is only matched against a pattern, never evaluated.
    # Read as-is (Get-Content would drop a "\r"), so CRLF lines are rejected
    # exactly as install.sh rejects them.
    $text = if (Test-Path -LiteralPath $temporaryManifest -PathType Leaf) { [IO.File]::ReadAllText($temporaryManifest) } else { "" }
    if (-not $text) { throw "Checksum verification failed: SHA256SUMS is missing or empty. ARN Code was not installed." }
    $lines = @($text -split "`n")
    if ($text.EndsWith("`n")) { $lines = @($lines | Select-Object -First ($lines.Count - 1)) }
    $entries = @()
    foreach ($line in $lines) {
        if ($line -cnotmatch '^([0-9A-Fa-f]{64}) [ *]([A-Za-z0-9._-]+)$') {
            throw "Checksum verification failed: SHA256SUMS contains a malformed line. ARN Code was not installed."
        }
        if ($Matches[2] -ceq $archiveName) { $entries += $Matches[1] }
    }
    if ($entries.Count -eq 0) { throw "Checksum verification failed: SHA256SUMS has no entry for $archiveName. ARN Code was not installed." }
    if ($entries.Count -gt 1) { throw "Checksum verification failed: SHA256SUMS lists $archiveName more than once. ARN Code was not installed." }
    if ($ArchivePath) {
        Copy-Item -LiteralPath $resolvedArchive -Destination $temporaryZip
    } else {
        try {
            Invoke-WebRequest -Headers $headers -Uri "$download/$archiveName" -OutFile $temporaryZip
        } catch {
            throw "Could not download $archiveName for ${tag}: $($_.Exception.Message)"
        }
    }
    $actualHash = (Get-FileHash -LiteralPath $temporaryZip -Algorithm SHA256).Hash
    if (-not [string]::Equals($actualHash, $entries[0], [StringComparison]::OrdinalIgnoreCase)) {
        throw "Checksum verification failed: $archiveName has SHA-256 $($actualHash.ToLowerInvariant()), but SHA256SUMS lists $($entries[0].ToLowerInvariant()). ARN Code was not installed."
    }
    Write-Host "Verified $archiveName (SHA-256 $($actualHash.ToLowerInvariant()))."

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

    $version = Invoke-WithoutErrorDialogs { & $stagedExecutable --version }
    $versionExit = $LASTEXITCODE
    if ($versionExit -ne 0 -and (Test-MissingDllStatus $versionExit)) {
        throw (Get-VcRuntimeMessage ("the downloaded arn.exe could not start (Windows status 0x{0:X8}: a DLL it needs is missing or too old); the installed Redistributable may be older than this ARN Code needs" -f [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$versionExit), 0)))
    }
    if ($versionExit -ne 0 -or $version -notmatch '^arn [0-9]+\.[0-9]+\.[0-9]+') {
        throw "The downloaded ARN executable failed its version check."
    }
    $versionText = @($version) -join "`n"
    if ($versionText -cnotmatch "^arn ($semver)(?: [^\r\n]*)?\z") {
        throw "The downloaded ARN executable reports an invalid version ('$versionText')."
    }
    # The version the package itself reports decides (for a local package it
    # is the only one known).
    Assert-NotDowngrade $currentVersion $Matches[1] "."
    $payload = @(Get-ChildItem -LiteralPath $payloadRoot -File)
    foreach ($file in $payload) {
        if (-not (Test-OwnedName $file.Name)) { throw "The package contains an unexpected file name '$($file.Name)'." }
    }

    # Nothing in the installation has changed so far (an interrupted earlier
    # run was recovered at the start). Work out which files are ARN's before
    # touching any.
    New-Item -ItemType Directory -Path $parentDir -Force | Out-Null
    Remove-StaleWorkDirectories
    try { $previous = Read-InstallManifest } catch { Stop-WithManualCleanup "Update refused: $($_.Exception.Message)" }
    $owned = @{}
    if ($previous) { foreach ($name in $previous.Files) { $owned[$name.ToLowerInvariant()] = $name } }
    $unknown = @()
    foreach ($file in $payload) {
        $target = Join-Path $InstallDir $file.Name
        if (Test-Path -LiteralPath $target -PathType Container) { Stop-WithManualCleanup "Update refused: $target is a directory." }
        if ((Test-Path -LiteralPath $target) -and -not $owned.ContainsKey($file.Name.ToLowerInvariant())) { $unknown += $file.Name }
    }
    if ($unknown.Count) {
        if (-not $AdoptExisting) {
            Stop-WithManualCleanup ("Update refused: $InstallDir already has $($unknown -join ', '), which no $manifestName records as ARN's " +
                "(perhaps from an installation made before manifests existed). If they are ARN Code's files, run the installer " +
                "again with -AdoptExisting (or ARN_ADOPT_EXISTING=1) to replace them.")
        }
        foreach ($name in $unknown) { $owned[$name.ToLowerInvariant()] = $name }
        Write-Host "Adopting the existing $($unknown -join ', ') as ARN Code's files."
    }

    # The transaction: which files move aside (arn.exe first, the manifest
    # last), which new files come in (arn.exe first), and what to check when
    # undoing it.
    $firstArn = { $_ -ne "arn.exe" }
    $plan = @(@($owned.Values) | Where-Object { Test-Path -LiteralPath (Join-Path $InstallDir $_) -PathType Leaf } |
              Sort-Object $firstArn, { $_ })
    $hadManifest = Test-Path -LiteralPath $manifestPath -PathType Leaf
    if ($hadManifest) { $plan += $manifestName }
    $fresh = -not @($plan | Where-Object { $_ -cne $manifestName }).Count
    $payloadNames = @($payload | ForEach-Object Name | Sort-Object $firstArn, { $_ })
    $previousVersion = $null
    if ($plan -contains "arn.exe") {
        try {
            $previousOutput = (Invoke-WithoutErrorDialogs { & (Join-Path $InstallDir "arn.exe") --version 2>$null }) -join "`n"
            if ($LASTEXITCODE -eq 0 -and $previousOutput -cmatch '^arn [0-9]+\.[0-9]+\.[0-9]+[^\r\n]*\z') { $previousVersion = $previousOutput }
        } catch { }
    }
    $wasInPath = ($PathScope -ne "None") -and (Test-InPath $InstallDir $PathScope)
    $userPathBefore = if ($PathScope -eq "User") { Get-UserPath } else { $null }
    $processPathBefore = $env:Path
    $createdDir = -not (Test-Path -LiteralPath $InstallDir)
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    $stageName = ".arn-staging-" + [guid]::NewGuid().ToString("N")
    $backupName = ".arn-backup-" + [guid]::NewGuid().ToString("N")
    $stageDir = Join-Path $parentDir $stageName
    $backupDir = Join-Path $parentDir $backupName
    $transaction = [ordered]@{
        format = $journalFormat; installDir = $InstallDir; state = "staging"; stageDir = $stageName; backupDir = $backupName
        fresh = $fresh; hadManifest = $hadManifest; createdDir = $createdDir; pathScope = $PathScope; pathAdded = $false
        plan = @($plan); payload = @($payloadNames); previousVersion = $previousVersion
    }
    $journalWritten = $false
    $userPathWritten = $null
    try {
        Write-Transaction $transaction
        $journalWritten = $true
        Invoke-TestFault "after-journal"

        # Stage the new files next to the installation (same volume, so the
        # moves below are renames) and check that they run from there.
        New-StageDirectory $stageDir
        New-Item -ItemType Directory -Path $backupDir | Out-Null
        foreach ($name in $payloadNames) { Copy-Item -LiteralPath (Join-Path $payloadRoot $name) -Destination (Join-Path $stageDir $name) }
        $stagedVersion = Invoke-WithoutErrorDialogs { & (Join-Path $stageDir "arn.exe") --version }
        if ($LASTEXITCODE -ne 0 -or $stagedVersion -ne $version) { throw "The staged ARN executable failed its version check." }
        Invoke-TestFault "after-staging"

        # Switch over: move ARN's current files and manifest aside, move the
        # new files in, and check the result. Windows lets a running arn.exe be
        # renamed, so an open ARN does not block this; a file another program
        # has locked does. The record is updated before each phase, so an
        # interruption anywhere is undone by the next run.
        $transaction.state = "backing-up"
        Write-Transaction $transaction
        foreach ($name in $plan) {
            Move-ArnFile (Join-Path $InstallDir $name) (Join-Path $backupDir $name) "move the installed $name aside"
            if ($name -ceq $plan[0]) { Invoke-TestFault "after-first-backup" }
        }
        Invoke-TestFault "after-backup"
        $transaction.state = "activating"
        Write-Transaction $transaction
        foreach ($name in $payloadNames) {
            Move-ArnFile (Join-Path $stageDir $name) (Join-Path $InstallDir $name) "install $name"
            if ($name -ceq $payloadNames[0]) {
                Invoke-TestFault "after-first-activate"
                Invoke-TestFault "activate"
            }
        }
        Invoke-TestFault "after-activate"
        $installedExecutable = Join-Path $InstallDir "arn.exe"
        $installedVersion = Invoke-WithoutErrorDialogs { & $installedExecutable --version }
        if ($LASTEXITCODE -ne 0 -or $installedVersion -ne $version -or $env:ARN_INSTALL_TEST_FAULT -ceq "verify") {
            throw "The installed ARN executable failed its version check."
        }

        $transaction.state = "verifying"
        if ($PathScope -eq "User" -and -not $wasInPath) { $transaction.pathAdded = $true }
        Write-Transaction $transaction
        $userPathWritten = Add-ArnToPath $InstallDir $PathScope
        Invoke-TestFault "after-path"
        if ($PathScope -ne "None") {
            # Get-Command lists every arn on PATH, in PATH order; the first is
            # the one a terminal runs.
            $discovered = @(Get-Command arn -CommandType Application -ErrorAction SilentlyContinue)
            $resolved = if ($discovered.Count) { [IO.Path]::GetFullPath([string]$discovered[0].Source) } else { $null }
            if (-not $resolved -or -not (Test-SamePath $resolved $installedExecutable)) {
                throw "ARN was installed, but the arn command resolves to $(if ($resolved) { $resolved } else { 'nothing' }), not to the new installation."
            }
        }
        # The PATH entry counts as the installer's if it added it now, an
        # earlier run recorded adding it, or a legacy installer (which always
        # added it) made the previous installation.
        $addedPathEntry = ($PathScope -ne "None") -and ((-not $wasInPath) -or
            ($previous -and $previous.Format -eq "1" -and $previous.AddedPathEntry -and $previous.PathScope -eq $PathScope) -or
            ($previous -and $previous.Format -eq "legacy"))
        Write-InstallManifest $payloadNames $installedVersion $PathScope $addedPathEntry
        Invoke-TestFault "after-manifest"
        $transaction.state = "committed"
        Write-Transaction $transaction
        Invoke-TestFault "after-commit"
    } catch {
        $failure = $_.Exception.Message
        if (-not $journalWritten) {
            if ($createdDir -and (Test-Path -LiteralPath $InstallDir) -and -not @(Get-ChildItem -LiteralPath $InstallDir -Force).Count) {
                Remove-Item -LiteralPath $InstallDir -Force -ErrorAction SilentlyContinue
            }
            throw
        }
        try { $recorded = Read-Transaction } catch { throw "$failure`nRestoring the previous state also failed: $($_.Exception.Message)" }
        if ($recorded.State -ceq "committed") {
            # The failure came after the commit: the new installation stays
            # (PATH included) and is only finished.
            try { [void](Complete-Transaction $recorded) } catch { Write-Warning $_.Exception.Message }
            throw "$failure The error came after $installedVersion was installed and committed, so the installation was kept."
        }
        # The PATH goes back to exactly what it was, unless someone changed
        # it meanwhile; then only the entry added here is taken out (below).
        $env:Path = $processPathBefore
        if ($null -ne $userPathWritten) {
            try {
                if ((Get-UserPath).Value -ceq $userPathWritten) { Set-UserPath $userPathBefore }
            } catch { }
        }
        try {
            $verified = Undo-Transaction $recorded
        } catch {
            throw "$failure`nRestoring the previous state also failed: $($_.Exception.Message)"
        }
        if ($fresh) { throw "$failure ARN Code was not installed." }
        if ($verified) { throw "$failure The previous installation was restored; ARN Code was not updated." }
        throw "$failure The previous files were moved back, but the restored installation could not be verified; install ARN Code again to repair it."
    }

    # Committed: the previous files are no longer needed.
    if (-not (Complete-Transaction (Read-Transaction))) {
        Write-Warning "The previous ARN version is still running, so its files remain in $backupDir; the next installation removes them."
    }
    Write-Host "$installedVersion installed to $InstallDir"
    if ($PathScope -eq "User") { Write-Host "Open a new terminal, then run: arn" }
    else { Write-Host "Run: arn" }
} finally {
    Remove-Item -LiteralPath $temporaryDir -Recurse -Force -ErrorAction SilentlyContinue
}
