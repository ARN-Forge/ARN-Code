param([Parameter(Mandatory = $true)][string]$BuildDir)

# Tests release resolution and downgrade protection in scripts/install.ps1
# (cases T1-T13 of the installer migration design) with the real Windows
# package built from $BuildDir. Nothing is downloaded and no live release is
# used: Invoke-RestMethod and Invoke-WebRequest are replaced by functions
# (which PowerShell finds before cmdlets) that answer the GitHub API from
# in-memory fixtures and serve downloads from a temporary directory, logging
# every URL. Exits 77 (skipped) for a Debug build, which cannot be packaged.

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $repo "scripts/windows-runtime.ps1")
$installer = Join-Path $repo "scripts/install.ps1"
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
if (@(Get-PeImports (Join-Path $BuildDir "arn.exe") | Where-Object { $_ -match '^(vcruntime140(_1)?d|msvcp140d)' }).Count) {
    Write-Host "skip: arn.exe is a Debug build; its debug Visual C++ runtime cannot be redistributed"
    exit 77
}

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$root = Join-Path $tempBase ("arn-install-resolver-test-" + [guid]::NewGuid().ToString("N"))
$archiveName = "arn-windows-x64.zip"
$allPlatforms = @("arn-windows-x64.zip", "arn-linux-x64.tar.gz", "arn-macos-arm64.tar.gz", "arn-macos-x64.tar.gz", "arn-ide-windows-x64.zip")
$failures = New-Object System.Collections.Generic.List[string]
$cases = 0
# State of the replaced cmdlets. Global, because $script: inside them would
# name the scope of install.ps1, which calls them.
$global:ArnTestRequests = New-Object System.Collections.Generic.List[string]
$global:ArnTestApi = @{}
$global:ArnTestDownloads = Join-Path $root "dl"

if (-not ("ArnTestHttpError" -as [type])) {
    Add-Type -TypeDefinition @'
public class ArnTestHttpError : System.Exception {
    public ArnTestHttpError(string message, object response) : base(message) { Response = response; }
    public object Response;
}
'@
}

# API answers by key ("latest", "tags/<tag>", "list"): @{ Body; Status;
# Headers; ErrorBody; Timeout }. No entry: HTTP 404. A body that is not JSON
# comes back as a string, as Invoke-RestMethod returns it.
function Invoke-RestMethod {
    param([hashtable]$Headers, [string]$Uri, [int]$TimeoutSec)
    $global:ArnTestRequests.Add($Uri)
    $key = switch -Regex ($Uri) {
        '^https://api\.github\.com/repos/[^/]+/[^/]+/releases/latest$' { "latest"; break }
        '^https://api\.github\.com/repos/[^/]+/[^/]+/releases/tags/([^/]+)$' { "tags/" + $Matches[1]; break }
        '^https://api\.github\.com/repos/[^/]+/[^/]+/releases\?per_page=100$' { "list"; break }
        default { "unknown" }
    }
    $entry = $global:ArnTestApi[$key]
    if ($entry -and $entry.Timeout) { throw "The operation has timed out." }
    $status = if (-not $entry) { 404 } elseif ($entry.Status) { $entry.Status } else { 200 }
    if ($status -ne 200) {
        $headers = if ($entry -and $entry.Headers) { $entry.Headers } else { @{} }
        $exception = [ArnTestHttpError]::new("The remote server returned an error: ($status).", [pscustomobject]@{ StatusCode = $status; Headers = $headers })
        $record = New-Object Management.Automation.ErrorRecord($exception, "WebCmdletWebResponseException", "InvalidOperation", $null)
        $body = if ($entry -and $entry.ErrorBody) { $entry.ErrorBody } else { '{"message":"Not Found"}' }
        $record.ErrorDetails = New-Object Management.Automation.ErrorDetails($body)
        throw $record
    }
    if ($entry.Body.Trim() -ceq "[]") { return , @() }
    try { $parsed = $entry.Body | ConvertFrom-Json } catch { return $entry.Body }
    if ($null -eq $parsed) { return $entry.Body }
    # Invoke-RestMethod returns a JSON array as one array object, in Windows
    # PowerShell and PowerShell 7 alike; ConvertFrom-Json in PowerShell 7
    # hands out a one-element array as its element, hence @().
    if ($entry.Body.TrimStart().StartsWith("[")) { return , @($parsed) }
    return $parsed
}
function Invoke-WebRequest {
    param([hashtable]$Headers, [string]$Uri, [string]$OutFile)
    $global:ArnTestRequests.Add($Uri)
    $path = $Uri -replace '^https://github\.com/[^/]+/[^/]+/releases/download/', ''
    $source = Join-Path $global:ArnTestDownloads $path
    if ($path -eq $Uri -or -not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw [ArnTestHttpError]::new("404 Not Found: $Uri", [pscustomobject]@{ StatusCode = 404; Headers = @{} })
    }
    Copy-Item -LiteralPath $source -Destination $OutFile
}

function Fail([string]$Message) { $script:failures.Add($Message) }

$script:listTags = New-Object System.Collections.Generic.List[string]
function Reset-World {
    $global:ArnTestApi.Clear()
    $script:listTags.Clear()
    if (Test-Path -LiteralPath $global:ArnTestDownloads) { Remove-Item -LiteralPath $global:ArnTestDownloads -Recurse -Force }
    New-Item -ItemType Directory -Path $global:ArnTestDownloads | Out-Null
}

# Publishes a release. The metadata lists exactly $Assets (a name prefixed
# with "-" is listed but not downloadable); arn-windows-x64.zip is the real
# package, other archives are stand-ins. SHA256SUMS (when listed) has correct
# entries for them, or $SumsText instead.
function Add-Release([string]$Tag, [bool]$Prerelease, [string[]]$Assets, [string]$SumsText = $null) {
    $dir = Join-Path $global:ArnTestDownloads $Tag
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $sums = ""
    $listed = @()
    foreach ($name in $Assets) {
        $plain = $name.TrimStart('-')
        $listed += [ordered]@{ url = "https://api.github.com/x"; name = $plain; size = 10; uploader = [ordered]@{ login = "github-actions[bot]" } }
        if ($name.StartsWith("-") -or $plain -ceq "SHA256SUMS") { continue }
        $target = Join-Path $dir $plain
        if ($plain -ceq $archiveName) { Copy-Item -LiteralPath $script:archive -Destination $target }
        else { [IO.File]::WriteAllText($target, "stand-in $plain") }
        $sums += "$((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant())  $plain`n"
    }
    if ($Assets -ccontains "SHA256SUMS") {
        # A [string] parameter turns $null into "", so only a passed text counts.
        [IO.File]::WriteAllText((Join-Path $dir "SHA256SUMS"), $(if ($PSBoundParameters.ContainsKey("SumsText")) { $SumsText } else { $sums }))
    }
    $body = [ordered]@{ url = "https://api.github.com/repos/o/r/releases/1"; tag_name = $Tag; name = "ARN Code $Tag"; draft = $false
                        prerelease = $Prerelease; body = "Notes: `"quoted`", a\b, caf$([char]0xe9) [x] {y}`n"; assets = @($listed) }
    $global:ArnTestApi["tags/$Tag"] = @{ Body = ($body | ConvertTo-Json -Depth 6 -Compress) }
    $script:listTags.Add($Tag)
    $global:ArnTestApi["list"] = @{ Body = "[" + (@($script:listTags | ForEach-Object { $global:ArnTestApi["tags/$_"].Body }) -join ",") + "]" }
}
function Set-Latest([string]$Tag) { $global:ArnTestApi["latest"] = @{ Body = $global:ArnTestApi["tags/$Tag"].Body } }

function Get-State([string]$Dir) {
    $exe = Join-Path $Dir "arn.exe"
    $manifest = Join-Path $Dir "install-manifest.json"
    $hash = if (Test-Path -LiteralPath $exe) { (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash } else { "-" }
    $text = if (Test-Path -LiteralPath $manifest) { [IO.File]::ReadAllText($manifest) } else { "-" }
    return "$hash|$text"
}

# Runs install.ps1 into $script:dir with the given parameters and environment
# variables; checks the outcome, the message, and that a failure left the
# installation as it was.
$script:dir = $null
$script:lastOutput = ""
function Test-Install([string]$Name, [bool]$ShouldPass, [string]$Fragment, [hashtable]$Arguments = @{}, [hashtable]$Environment = @{}) {
    $script:cases++
    $global:ArnTestRequests.Clear()
    $before = Get-State $script:dir
    $saved = @{}
    foreach ($key in $Environment.Keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key, "Process"); [Environment]::SetEnvironmentVariable($key, $Environment[$key], "Process") }
    $passed = $true
    try {
        $output = (& $installer -InstallDir $script:dir -PathScope None @Arguments *>&1 | ForEach-Object { "$_" }) -join "`n"
    } catch {
        $passed = $false
        $output = (@($output) + $_.Exception.Message) -join "`n"
    } finally {
        foreach ($key in $Environment.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], "Process") }
    }
    $script:lastOutput = $output
    $installed = Test-Path -LiteralPath (Join-Path $script:dir "arn.exe")
    if ($ShouldPass -and (-not $passed -or -not $installed)) { Fail "${Name}: expected installation, got: $output"; return $false }
    if (-not $ShouldPass -and $passed) { Fail "${Name}: expected failure, but it installed: $output"; return $false }
    if (-not $ShouldPass -and (Get-State $script:dir) -cne $before) { Fail "${Name}: the failure changed the installation"; return $false }
    if ($Fragment -and $output.IndexOf($Fragment, [StringComparison]::Ordinal) -lt 0) { Fail "${Name}: output lacks '$Fragment': $output"; return $false }
    Write-Host "ok: $Name"
    return $true
}
function New-Dir { $script:dir = Join-Path $root ("bin-" + ($script:cases + 1)) }
function Test-Output([string]$Name, [string]$Fragment) {
    if ($script:lastOutput.IndexOf($Fragment, [StringComparison]::Ordinal) -lt 0) { Fail "${Name}: output lacks '$Fragment': $($script:lastOutput)" }
}
function Test-NoDownloads([string]$Name) {
    if (@($global:ArnTestRequests | Where-Object { $_ -match '/releases/download/' }).Count) { Fail "${Name}: something was downloaded: $($global:ArnTestRequests -join ' ')" }
}
function Set-ManifestVersion([string]$Version) {
    $path = Join-Path $script:dir "install-manifest.json"
    $data = [IO.File]::ReadAllText($path) | ConvertFrom-Json
    $data.version = $Version
    [IO.File]::WriteAllText($path, ($data | ConvertTo-Json) + "`n")
}
$command = 'irm https://raw.githubusercontent.com/ARN-Forge/ARN-Code/main/scripts/install.ps1 | iex'

$oldTmp = $env:TMP
$oldTemp = $env:TEMP
# install.ps1 takes defaults from these; a value inherited from the caller's
# environment must not change a case. They are put back at the end.
$installerVariables = @("ARN_VERSION", "ARN_ALLOW_DOWNGRADE", "ARN_INSTALL_ARCHIVE", "ARN_INSTALL_CHECKSUMS",
                        "ARN_UNINSTALL", "ARN_ADOPT_EXISTING", "ARN_INSTALL_TEST_FAULT")
$savedVariables = @{}
foreach ($name in $installerVariables) {
    $savedVariables[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
    [Environment]::SetEnvironmentVariable($name, $null, "Process")
}
try {
    New-Item -ItemType Directory -Path $root, (Join-Path $root "tmp") | Out-Null
    $script:archive = Join-Path $root $archiveName
    & (Join-Path $repo "scripts/package-windows.ps1") -BuildDir $BuildDir -OutputDir (Join-Path $root "package") `
        -ArchivePath $script:archive | Out-Null
    $packaged = (& (Join-Path $BuildDir "arn.exe") --version) -join "`n"
    if ($packaged -cnotmatch '^arn (\d+)\.(\d+)\.(\d+)') { throw "Cannot read the version of the package: '$packaged'." }
    $v = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
    # install.ps1's own temporary files go here, so leftovers can be seen.
    $env:TMP = Join-Path $root "tmp"
    $env:TEMP = $env:TMP
    $zero = "0" * 64

    # --- T1: v0.8.0 is the latest release and has no SHA256SUMS -------------
    Reset-World
    Add-Release "v0.8.0" $false $allPlatforms
    Set-Latest "v0.8.0"
    New-Dir
    [void](Test-Install "T1 latest v0.8.0 without SHA256SUMS is refused" $false `
        "The latest ARN Code release, v0.8.0, has no SHA256SUMS, so its arn-windows-x64.zip cannot be verified.")
    Test-Output "T1" "No compatible release: no stable ARN Code release has both arn-windows-x64.zip and SHA256SUMS."
    Test-NoDownloads "T1"
    if (Test-Path -LiteralPath $script:dir) { Fail "T1: the installation directory was created" }

    # --- T2: v0.8.0 with a SHA256SUMS added afterwards ----------------------
    Reset-World
    Add-Release "v0.8.0" $false ($allPlatforms + "SHA256SUMS")
    Set-Latest "v0.8.0"
    New-Dir
    [void](Test-Install "T2 latest v0.8.0 with SHA256SUMS installs" $true "Verified arn-windows-x64.zip")

    # --- T3: a full release --------------------------------------------------
    Reset-World
    Add-Release "v0.8.0" $false $allPlatforms
    Add-Release "v0.9.0" $false ($allPlatforms + "SHA256SUMS")
    Set-Latest "v0.9.0"
    New-Dir
    if (Test-Install "T3 full release installs the Windows archive" $true "Verified arn-windows-x64.zip") {
        $expected = "https://api.github.com/repos/ARN-Forge/ARN-Code/releases/latest " +
                    "https://github.com/ARN-Forge/ARN-Code/releases/download/v0.9.0/SHA256SUMS " +
                    "https://github.com/ARN-Forge/ARN-Code/releases/download/v0.9.0/arn-windows-x64.zip"
        if (($global:ArnTestRequests -join " ") -cne $expected) { Fail "T3: requests: $($global:ArnTestRequests -join ' ')" }
    }

    # --- T4: a partial prerelease while v0.8.0 is the latest -----------------
    Reset-World
    Add-Release "v0.8.0" $false $allPlatforms
    Add-Release "v0.9.0" $true @("arn-linux-x64.tar.gz", "arn-macos-arm64.tar.gz", "arn-macos-x64.tar.gz", "SHA256SUMS")
    Set-Latest "v0.8.0"
    New-Dir
    [void](Test-Install "T4 default stays on the latest v0.8.0 (refused without SHA256SUMS)" $false "v0.8.0, has no SHA256SUMS")
    Test-Output "T4" "No compatible release"
    if ($script:lastOutput -match 'ARN_VERSION') { Fail "T4: a command was offered for a release without a Windows build" }
    Test-NoDownloads "T4 default"
    [void](Test-Install "T4 ARN_VERSION=0.9.0 has no Windows build" $false `
        "ARN Code v0.9.0 has no arn-windows-x64.zip (no build for Windows x64)" @{} @{ ARN_VERSION = "0.9.0" })
    Test-NoDownloads "T4 explicit"
    Add-Release "v0.9.1" $true @("arn-windows-x64.zip", "SHA256SUMS")
    [void](Test-Install "T4 an explicitly named prerelease with a Windows build installs" $true `
        "Note: v0.9.1 is a prerelease; it is installed because ARN_VERSION names it." @{ Version = "0.9.1" })

    # --- T5: the latest release lacks the Windows archive -------------------
    Reset-World
    Add-Release "v0.7.1" $false ($allPlatforms + "SHA256SUMS")
    Add-Release "v0.8.0" $false ($allPlatforms + "SHA256SUMS")
    Add-Release "v0.8.1" $false @("arn-linux-x64.tar.gz", "arn-macos-arm64.tar.gz", "SHA256SUMS")
    Set-Latest "v0.8.1"
    New-Dir
    [void](Test-Install "T5 latest without a Windows archive is refused, not replaced" $false `
        "The latest ARN Code release, v0.8.1, has no arn-windows-x64.zip (no build for Windows x64).")
    Test-Output "T5" "The newest stable release that has arn-windows-x64.zip and SHA256SUMS is v0.8.0."
    Test-Output "T5" "  `$env:ARN_VERSION = `"0.8.0`"; $command"
    Test-NoDownloads "T5"
    [void](Test-Install "T5 the suggested command installs v0.8.0" $true "Downloading ARN Code v0.8.0" @{} @{ ARN_VERSION = "0.8.0" })
    $global:ArnTestApi["list"].Status = 500
    New-Dir
    [void](Test-Install "T5 suggestion lookup fails: still refused, with the reason" $false `
        "Could not look for another release that has arn-windows-x64.zip: the GitHub API answered HTTP 500.")
    Test-NoDownloads "T5 lookup failure"
    Reset-World
    Add-Release "v0.8.1" $false @("-arn-windows-x64.zip", "SHA256SUMS") "$((Get-FileHash -LiteralPath $script:archive -Algorithm SHA256).Hash)  arn-windows-x64.zip`n"
    Add-Release "v0.8.2" $false @("arn-windows-x64.zip", "-SHA256SUMS")
    New-Dir
    [void](Test-Install "T5 listed archive that cannot be downloaded" $false "Could not download arn-windows-x64.zip for v0.8.1" @{ Version = "0.8.1" })
    [void](Test-Install "T5 listed SHA256SUMS that cannot be downloaded" $false "Could not download SHA256SUMS for v0.8.2" @{ Version = "0.8.2" })
    if ($global:ArnTestRequests -match 'arn-windows-x64\.zip$') { Fail "T5: the archive was downloaded without SHA256SUMS" }

    # --- T6: explicit versions that do not exist or are invalid -------------
    Reset-World
    Add-Release "v0.8.0" $false ($allPlatforms + "SHA256SUMS")
    Set-Latest "v0.8.0"
    New-Dir
    [void](Test-Install "T6 explicit version that does not exist" $false "There is no ARN Code release v9.9.9" @{ Version = "9.9.9" })
    if (($global:ArnTestRequests -join " ") -cne "https://api.github.com/repos/ARN-Forge/ARN-Code/releases/tags/v9.9.9") {
        Fail "T6: other releases were requested: $($global:ArnTestRequests -join ' ')"
    }
    foreach ($bad in @("0.9", "v0.9.0-rc.1", "00.9.0", "0.9.0 ", "latest2")) {
        [void](Test-Install "T6 invalid version '$bad'" $false "Version must be latest, X.Y.Z or vX.Y.Z" @{ Version = $bad })
        if ($global:ArnTestRequests.Count) { Fail "T6 invalid version '$bad' made a request" }
    }
    $global:ArnTestApi["tags/v0.8.5"] = @{ Body = $global:ArnTestApi["tags/v0.8.0"].Body }
    [void](Test-Install "T6 explicit version answered with another tag" $false "GitHub returned malformed release metadata for ARN Code v0.8.5" @{ Version = "0.8.5" })
    Test-NoDownloads "T6 tag mismatch"
    $global:ArnTestApi["tags/v0.8.6"] = @{ Body = $global:ArnTestApi["tags/v0.8.0"].Body.Replace('"draft":false', '"draft":true').Replace("v0.8.0", "v0.8.6") }
    [void](Test-Install "T6 explicit draft release" $false "ARN Code v0.8.6 is an unpublished draft release" @{ Version = "0.8.6" })
    Test-NoDownloads "T6 draft"

    # --- T7: a wrong hash in SHA256SUMS -------------------------------------
    Reset-World
    Add-Release "v0.9.0" $false ($allPlatforms + "SHA256SUMS") "$zero  arn-windows-x64.zip`n"
    Set-Latest "v0.9.0"
    New-Dir
    [void](Test-Install "T7 wrong hash is refused before extraction" $false "but SHA256SUMS lists $zero")
    $order = @($global:ArnTestRequests | Where-Object { $_ -match '/releases/download/' } | ForEach-Object { $_ -replace '^.*/', '' }) -join " "
    if ($order -cne "SHA256SUMS arn-windows-x64.zip") { Fail "T7: SHA256SUMS was not fetched before the archive: $order" }
    if (@(Get-ChildItem -LiteralPath (Join-Path $root "tmp") -Force).Count) { Fail "T7: temporary files (an extracted package) were left" }

    # --- T8: SHA256SUMS missing, empty, malformed or with duplicate entries --
    Reset-World
    Add-Release "v0.9.0" $false $allPlatforms
    Set-Latest "v0.9.0"
    New-Dir
    [void](Test-Install "T8 latest without SHA256SUMS" $false "has no SHA256SUMS")
    Test-NoDownloads "T8 missing"
    [void](Test-Install "T8 explicit version without SHA256SUMS" $false "ARN Code v0.9.0 has no SHA256SUMS, so its arn-windows-x64.zip cannot be verified" @{ Version = "0.9.0" })
    Test-NoDownloads "T8 explicit missing"
    $good = (Get-FileHash -LiteralPath $script:archive -Algorithm SHA256).Hash.ToLowerInvariant()
    foreach ($case in @(
            @("empty SHA256SUMS", "SHA256SUMS is missing or empty", ""),
            @("SHA256SUMS without an entry for the archive", "SHA256SUMS has no entry for arn-windows-x64.zip", "$good  arn-linux-x64.tar.gz`n"),
            @("malformed SHA256SUMS line", "SHA256SUMS contains a malformed line", "$good arn-windows-x64.zip`n"),
            @("duplicate SHA256SUMS entries", "lists arn-windows-x64.zip more than once", "$good  arn-windows-x64.zip`n$good  arn-windows-x64.zip`n"))) {
        Reset-World
        Add-Release "v0.9.1" $false ($allPlatforms + "SHA256SUMS") $case[2]
        Set-Latest "v0.9.1"
        New-Dir
        [void](Test-Install "T8 $($case[0])" $false $case[1])
        if ($global:ArnTestRequests -match 'arn-windows-x64\.zip$') { Fail "T8 $($case[0]): the archive was downloaded although SHA256SUMS was unusable" }
    }
    Reset-World
    Add-Release "v0.9.0" $false ($allPlatforms + "SHA256SUMS")
    $global:ArnTestApi["latest"] = @{ Body = $global:ArnTestApi["tags/v0.9.0"].Body.Replace('"assets":[', '"assets":[{"name":"SHA256SUMS"},') }
    New-Dir
    [void](Test-Install "T8 an asset listed twice" $false "an asset is listed twice")
    Test-NoDownloads "T8 duplicate asset"

    # --- T9: GitHub API failures and malformed metadata ----------------------
    Reset-World
    Add-Release "v0.9.0" $false ($allPlatforms + "SHA256SUMS")
    $tagJson = $global:ArnTestApi["tags/v0.9.0"].Body
    New-Dir
    $global:ArnTestApi["latest"] = @{ Body = $tagJson; Status = 403; Headers = @{ "X-RateLimit-Remaining" = "0" } }
    [void](Test-Install "T9 rate limit (403, remaining 0)" $false "the GitHub API rate limit was reached")
    Test-NoDownloads "T9 rate limit"
    $global:ArnTestApi["latest"] = @{ Body = $tagJson; Status = 403; ErrorBody = '{"message":"API rate limit exceeded for 192.0.2.1."}' }
    [void](Test-Install "T9 rate limit (403, message)" $false "the GitHub API rate limit was reached")
    $global:ArnTestApi["latest"] = @{ Body = $tagJson; Status = 429 }
    [void](Test-Install "T9 rate limit (429)" $false "the GitHub API rate limit was reached")
    $global:ArnTestApi["latest"] = @{ Body = $tagJson; Status = 403 }
    [void](Test-Install "T9 forbidden without a rate limit" $false "Could not look up the latest ARN Code release: the GitHub API answered HTTP 403.")
    $global:ArnTestApi["latest"] = @{ Body = $tagJson; Status = 500 }
    [void](Test-Install "T9 server error" $false "the GitHub API answered HTTP 500")
    $global:ArnTestApi["tags/v0.9.0"].Status = 503
    [void](Test-Install "T9 server error for an explicit version" $false "Could not look up ARN Code v0.9.0: the GitHub API answered HTTP 503." @{ Version = "0.9.0" })
    $global:ArnTestApi["tags/v0.9.0"].Status = $null
    $global:ArnTestApi["latest"] = @{ Timeout = $true }
    [void](Test-Install "T9 timeout" $false "GitHub could not be reached (The operation has timed out.)")
    $global:ArnTestApi.Remove("latest")
    [void](Test-Install "T9 no stable release (404 for latest)" $false "ARN-Forge/ARN-Code has no published stable ARN Code release.")
    foreach ($case in @(
            @("truncated JSON", $tagJson.Substring(0, $tagJson.Length - 5)),
            @("HTML instead of JSON", "<html><body>Unicorn!</body></html>"),
            @("an array instead of a release", "[$tagJson]"),
            @("tag_name is a number", '{"tag_name":9,"draft":false,"prerelease":false,"assets":[]}'),
            @("tag_name is not vX.Y.Z", '{"tag_name":"v0.9.0-rc1","draft":false,"prerelease":false,"assets":[]}'),
            @("tag_name with a path", '{"tag_name":"../../v0.9.0","draft":false,"prerelease":false,"assets":[]}'),
            @("draft is a string", '{"tag_name":"v0.9.0","draft":"false","prerelease":false,"assets":[]}'),
            @("no prerelease flag", '{"tag_name":"v0.9.0","draft":false,"assets":[]}'),
            @("no assets", '{"tag_name":"v0.9.0","draft":false,"prerelease":false}'),
            @("assets is an object", '{"tag_name":"v0.9.0","draft":false,"prerelease":false,"assets":{"name":"SHA256SUMS"}}'),
            @("asset name is a number", '{"tag_name":"v0.9.0","draft":false,"prerelease":false,"assets":[{"name":1}]}'),
            @("code in the tag", '{"tag_name":"$(New-Item ' + ($root -replace '\\', '/') + '/pwned)","draft":false,"prerelease":false,"assets":[]}'))) {
        $global:ArnTestApi["latest"] = @{ Body = $case[1] }
        [void](Test-Install "T9 malformed metadata: $($case[0])" $false "GitHub returned malformed release metadata for the latest ARN Code release")
        Test-NoDownloads "T9 $($case[0])"
    }
    if (Test-Path -LiteralPath (Join-Path $root "pwned")) { Fail "T9: metadata content was executed" }
    $global:ArnTestApi["latest"] = @{ Body = $tagJson.Replace('"prerelease":false', '"prerelease":true') }
    [void](Test-Install "T9 latest marked as a prerelease" $false "a draft or prerelease, as the latest release")
    Test-NoDownloads "T9 prerelease as latest"

    # --- T10: no compatible release at all -----------------------------------
    Reset-World
    Add-Release "v0.7.1" $false $allPlatforms
    Add-Release "v0.8.0" $false @("arn-linux-x64.tar.gz", "SHA256SUMS")
    Set-Latest "v0.8.0"
    New-Dir
    [void](Test-Install "T10 no release has a verifiable Windows archive" $false `
        "No compatible release: no stable ARN Code release has both arn-windows-x64.zip and SHA256SUMS.")
    if ($script:lastOutput -match 'ARN_VERSION') { Fail "T10: a command was offered although nothing is compatible" }
    Test-NoDownloads "T10"
    $global:ArnTestApi["list"] = @{ Body = "[]" }
    [void](Test-Install "T10 empty release list" $false "No compatible release")
    $global:ArnTestApi["list"] = @{ Body = "not json" }
    [void](Test-Install "T10 malformed release list" $false `
        "Could not look for another release that has arn-windows-x64.zip: GitHub returned malformed release metadata.")

    # --- T11: downgrade protection -------------------------------------------
    Reset-World
    foreach ($release in @("0.0.1", $v, "0.9.0", "0.10.0") | Select-Object -Unique) {
        Add-Release "v$release" $false ($allPlatforms + "SHA256SUMS")
    }
    Set-Latest "v$v"
    New-Dir
    [void](Test-Install "T11 install $v" $true "arn $v")
    Set-ManifestVersion "99.0.0"
    [void](Test-Install "T11 the latest stable $v would downgrade 99.0.0: refused" $false "Downgrade refused: ARN Code 99.0.0 is installed in $($script:dir), and $v is older.")
    Test-Output "T11" "  `$env:ARN_ALLOW_DOWNGRADE = `"1`"; `$env:ARN_VERSION = `"$v`"; $command"
    Test-NoDownloads "T11 refused (checked before downloading)"
    [void](Test-Install "T11 an explicit older version is refused too" $false "Downgrade refused" @{ Version = $v })
    foreach ($value in @("yes", "true", "2", " 1", "01")) {
        [void](Test-Install "T11 ARN_ALLOW_DOWNGRADE='$value' is rejected" $false "ARN_ALLOW_DOWNGRADE must be 1 (or unset)" @{} @{ ARN_ALLOW_DOWNGRADE = $value })
    }
    [void](Test-Install "T11 ARN_ALLOW_DOWNGRADE=0 still refuses" $false "Downgrade refused" @{} @{ ARN_ALLOW_DOWNGRADE = "0" })
    [void](Test-Install "T11 a local older package is refused after verification" $false "Downgrade refused: ARN Code 99.0.0 is installed in $($script:dir), and $v is older." `
        @{ ArchivePath = $script:archive; ChecksumsPath = (Join-Path (Join-Path $global:ArnTestDownloads "v$v") "SHA256SUMS") })
    if (Test-Install "T11 ARN_ALLOW_DOWNGRADE=1 downgrades, with a warning" $true "Replacing ARN Code 99.0.0 with the older $v, because ARN_ALLOW_DOWNGRADE=1." @{} @{ ARN_ALLOW_DOWNGRADE = "1" }) {
        if (([regex]::Matches($script:lastOutput, 'Replacing ARN Code')).Count -ne 1) { Fail "T11: the warning was not shown exactly once" }
    }
    [void](Test-Install "T11 the same version again is not a downgrade" $true "arn $v")
    Set-ManifestVersion "0.10.0"
    [void](Test-Install "T11 0.9.0 after 0.10.0 is a downgrade (numeric, not text, order)" $false `
        "Downgrade refused: ARN Code 0.10.0 is installed in $($script:dir), and 0.9.0 is older." @{ Version = "0.9.0" })
    Set-ManifestVersion "0.9.0"
    if ([version]$v -lt [version]"0.9.0") {
        Write-Host "note: the package is older than 0.9.0; the 0.10.0 upgrade case is skipped"
    } else {
        [void](Test-Install "T11 0.10.0 after 0.9.0 is an upgrade" $true "Downloading ARN Code v0.10.0" @{ Version = "0.10.0" })
    }
    Set-ManifestVersion "0.10"
    [void](Test-Install "T11 malformed manifest version: the installed arn.exe's own version is used" $false `
        "Downgrade refused: ARN Code $v is installed in $($script:dir), and 0.0.1 is older." @{ Version = "0.0.1" })
    # Without a manifest, arn.exe is not known to be ARN's: it is not run for
    # its version (the update stops because of it, not because of its
    # version), unless it is being adopted.
    Remove-Item -LiteralPath (Join-Path $script:dir "install-manifest.json")
    if (Test-Install "T11 an arn.exe that is not ARN's is not run for its version" $false "-AdoptExisting" @{ Version = "0.0.1" }) {
        if ($script:lastOutput -match 'Downgrade refused') { Fail "T11: an unowned arn.exe's version caused a downgrade refusal" }
    }
    [void](Test-Install "T11 an adopted arn.exe is checked for a downgrade" $false `
        "Downgrade refused: ARN Code $v is installed in $($script:dir), and 0.0.1 is older." @{ Version = "0.0.1"; AdoptExisting = $true })

    # --- T12: a prerelease newer than the latest release ---------------------
    Reset-World
    Add-Release "v0.8.0" $false ($allPlatforms + "SHA256SUMS")
    Add-Release "v99.0.0" $true ($allPlatforms + "SHA256SUMS")
    Set-Latest "v0.8.0"
    New-Dir
    [void](Test-Install "T12 a newer prerelease is not selected" $true "Downloading ARN Code v0.8.0")
    if ($global:ArnTestRequests -match 'v99\.0\.0') { Fail "T12: the prerelease was requested: $($global:ArnTestRequests -join ' ')" }

    # --- T13: the README command through the migration steps ----------------
    Reset-World
    Add-Release "v0.8.0" $false $allPlatforms
    Set-Latest "v0.8.0"
    New-Dir
    [void](Test-Install "T13 step 0: latest v0.8.0 without SHA256SUMS is refused" $false "has no SHA256SUMS")
    Add-Release "v0.9.0" $true @("arn-linux-x64.tar.gz", "arn-macos-arm64.tar.gz", "arn-macos-x64.tar.gz", "SHA256SUMS")
    [void](Test-Install "T13 step 1: a partial prerelease without Windows changes nothing" $false "No compatible release")
    [void](Test-Install "T13 step 1: naming the prerelease is refused on Windows" $false "has no arn-windows-x64.zip" @{ Version = "0.9.0" })
    Add-Release "v$v" $false ($allPlatforms + "SHA256SUMS")
    Set-Latest "v$v"
    [void](Test-Install "T13 step 2: the full release installs" $true "arn $v")
    Reset-World
    Add-Release "v0.8.0" $false ($allPlatforms + "SHA256SUMS")
    Add-Release "v$v" $true ($allPlatforms + "SHA256SUMS")
    Set-Latest "v0.8.0"
    if ([version]$v -gt [version]"0.8.0") {
        [void](Test-Install "T13 rollback: the installed $v is not downgraded silently" $false "Downgrade refused: ARN Code $v is installed")
    }
    New-Dir
    [void](Test-Install "T13 rollback: a new installation gets v0.8.0" $true "Downloading ARN Code v0.8.0")

    # Every run, failed ones included, removed its temporary files.
    $script:cases++
    $leftovers = @(Get-ChildItem -LiteralPath (Join-Path $root "tmp") -Force)
    if ($leftovers.Count) { Fail "temporary files left behind: $($leftovers.Name -join ', ')" }
    else { Write-Host "ok: no temporary files left behind" }
} catch {
    Fail "the test stopped at line $($_.InvocationInfo.ScriptLineNumber): $($_.Exception.Message)"
} finally {
    Remove-Variable -Scope Global -Name ArnTestRequests, ArnTestApi, ArnTestDownloads -ErrorAction SilentlyContinue
    foreach ($name in $installerVariables) { [Environment]::SetEnvironmentVariable($name, $savedVariables[$name], "Process") }
    $env:TMP = $oldTmp
    $env:TEMP = $oldTemp
    if ($root.StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "$cases cases, $($failures.Count) failure(s)"
if ($failures.Count) {
    $failures | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
