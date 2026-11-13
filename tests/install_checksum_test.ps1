param([Parameter(Mandatory = $true)][string]$BuildDir)

# Tests the checksum verification and version selection of scripts/install.ps1
# with the real Windows package built from $BuildDir. Nothing is downloaded:
# Invoke-WebRequest and Invoke-RestMethod are replaced by functions (which
# PowerShell finds before cmdlets) that serve files from a temporary
# directory laid out like GitHub release downloads and log every URL.
# Exits 77 (skipped) for a Debug build, which cannot be packaged.

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
$root = Join-Path $tempBase ("arn-install-checksum-test-" + [guid]::NewGuid().ToString("N"))
$archiveName = "arn-windows-x64.zip"
$failures = New-Object System.Collections.Generic.List[string]
$cases = 0
# State of the replaced cmdlets. Global, because $script: inside them would
# name the scope of install.ps1, which calls them.
$global:ArnTestRequests = New-Object System.Collections.Generic.List[string]
$global:ArnTestLatestTag = $null
$global:ArnTestReleases = Join-Path $root "releases"

function Invoke-WebRequest {
    param([hashtable]$Headers, [string]$Uri, [string]$OutFile)
    $global:ArnTestRequests.Add($Uri)
    $path = $Uri -replace '^https://github\.com/[^/]+/[^/]+/releases/download/', ''
    $source = Join-Path $global:ArnTestReleases $path
    if ($path -eq $Uri -or -not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "404 Not Found: $Uri" }
    Copy-Item -LiteralPath $source -Destination $OutFile
}
# An HTTP error as the web cmdlets report it: an exception with a Response
# that has a StatusCode.
if (-not ("ArnTestHttpError" -as [type])) {
    Add-Type -TypeDefinition @'
public class ArnTestHttpError : System.Exception {
    public ArnTestHttpError(string message, object response) : base(message) { Response = response; }
    public object Response;
}
'@
}
# The GitHub API describes the release in releases\<tag> (its files are the
# assets; HTTP 404 without that directory), and releases/latest the one
# $global:ArnTestLatestTag names. (install_resolver_test.ps1 covers release
# resolution in depth.)
function Invoke-RestMethod {
    param([hashtable]$Headers, [string]$Uri, [int]$TimeoutSec)
    $global:ArnTestRequests.Add($Uri)
    $tag = $null
    if ($Uri -match '/releases/latest$') { $tag = $global:ArnTestLatestTag }
    elseif ($Uri -match '/releases/tags/([^/]+)$') { $tag = $Matches[1] }
    $dir = if ($tag) { Join-Path $global:ArnTestReleases $tag } else { $null }
    if (-not $tag -or (-not (Test-Path -LiteralPath $dir -PathType Container) -and $tag -cne $global:ArnTestLatestTag)) {
        throw [ArnTestHttpError]::new("404 Not Found: $Uri", [pscustomobject]@{ StatusCode = 404; Headers = @{} })
    }
    $assets = @()
    if (Test-Path -LiteralPath $dir -PathType Container) {
        $assets = @(Get-ChildItem -LiteralPath $dir -File | ForEach-Object { [pscustomobject]@{ name = $_.Name } })
    }
    return [pscustomobject]@{ tag_name = $tag; draft = $false; prerelease = $false; assets = $assets }
}

# Runs install.ps1 into a fresh directory; checks the outcome, the message,
# and that a failure installed nothing.
function Test-Install([string]$Name, [bool]$ShouldPass, [string]$Fragment, [hashtable]$Arguments) {
    $script:cases++
    $global:ArnTestRequests.Clear()
    $dir = Join-Path $root ("bin-" + $script:cases)
    $passed = $true
    try {
        $output = (& $installer -InstallDir $dir -PathScope None @Arguments *>&1 | ForEach-Object { "$_" }) -join " "
    } catch {
        $passed = $false
        $output = $_.Exception.Message
    }
    $installed = Test-Path -LiteralPath (Join-Path $dir "arn.exe")
    if ($ShouldPass -and (-not $passed -or -not $installed)) {
        $failures.Add("${Name}: expected installation, got: $output")
    } elseif (-not $ShouldPass -and $passed) {
        $failures.Add("${Name}: expected failure, but it installed: $output")
    } elseif (-not $ShouldPass -and $installed) {
        $failures.Add("${Name}: arn.exe was installed despite the failure")
    } elseif ($Fragment -and $output.IndexOf($Fragment, [StringComparison]::Ordinal) -lt 0) {
        $failures.Add("${Name}: output lacks '$Fragment': $output")
    } else {
        Write-Host "ok: $Name"
    }
}

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
    New-Item -ItemType Directory -Path $root, (Join-Path $root "tmp"), (Join-Path $root "releases") | Out-Null
    $archive = Join-Path $root $archiveName
    & (Join-Path $repo "scripts/package-windows.ps1") -BuildDir $BuildDir -OutputDir (Join-Path $root "package") `
        -ArchivePath $archive | Out-Null
    $good = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    $other = "0" * 64
    # install.ps1's own temporary files go here, so leftovers can be seen.
    $env:TMP = Join-Path $root "tmp"
    $env:TEMP = $env:TMP
    $manifest = Join-Path $root "SHA256SUMS"

    function Test-Local([string]$Name, [bool]$ShouldPass, [string]$Fragment, [string]$Text) {
        [IO.File]::WriteAllText($manifest, $Text)
        Test-Install $Name $ShouldPass $Fragment @{ ArchivePath = $archive; ChecksumsPath = $manifest }
    }

    # Local archive with a manifest.
    Test-Local "valid manifest" $true "Verified $archiveName" "$good  $archiveName`n"
    Test-Local "binary-mode marker and upper-case hash" $true "" "$($good.ToUpperInvariant()) *$archiveName`n"
    Test-Local "manifest listing other assets too" $true "" "$other  arn-linux-x64.tar.gz`n$good  $archiveName`n$other  zz.zip`n"
    Test-Local "checksum mismatch" $false "has SHA-256 $good, but SHA256SUMS lists $other" "$other  $archiveName`n"
    Test-Local "missing entry" $false "SHA256SUMS has no entry for $archiveName" "$good  arn-linux-x64.tar.gz`n"
    Test-Local "wrong file name (lookalike)" $false "no entry for $archiveName" "$good  arn-windows-x64-zip`n"
    Test-Local "duplicate entries" $false "lists $archiveName more than once" "$good  $archiveName`n$good  $archiveName`n"
    Test-Local "hash too short" $false "malformed line" "$($good.Substring(1))  $archiveName`n"
    Test-Local "hash not hexadecimal" $false "malformed line" "$($good.Substring(1))g  $archiveName`n"
    Test-Local "line without the two-character separator" $false "malformed line" "$good $archiveName`n"
    Test-Local "blank line" $false "malformed line" "$good  $archiveName`n`n"
    Test-Local "CRLF line ending" $false "malformed line" "$good  $archiveName`r`n"
    Test-Local "empty manifest" $false "SHA256SUMS is missing or empty" ""
    $pwned = Join-Path $root "pwned"
    Test-Local "code in the manifest is never run" $false "malformed line" "`$(New-Item '$pwned')  $archiveName`n"
    if (Test-Path -LiteralPath $pwned) { $failures.Add("manifest content was executed") }
    Test-Install "local archive without a manifest" $false "-ArchivePath needs -ChecksumsPath" @{ ArchivePath = $archive }

    # Downloads through the replaced web cmdlets.
    # A [string] parameter turns $null into "", so a missing SHA256SUMS needs
    # its own switch.
    function Publish([string]$Tag, [string]$Text, [switch]$NoManifest) {
        $dir = Join-Path (Join-Path $root "releases") $Tag
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
        Copy-Item -LiteralPath $archive -Destination (Join-Path $dir $archiveName)
        if (-not $NoManifest) { [IO.File]::WriteAllText((Join-Path $dir "SHA256SUMS"), $Text) }
    }
    Publish "v1.2.3" "$good  $archiveName`n"
    # The release is looked up first; SHA256SUMS is downloaded before the archive.
    $expectedUrls = "https://github.com/ARN-Forge/ARN-Code/releases/download/v1.2.3/SHA256SUMS " +
                    "https://github.com/ARN-Forge/ARN-Code/releases/download/v1.2.3/$archiveName"
    Test-Install "explicit version X.Y.Z" $true "Downloading ARN Code v1.2.3" @{ Version = "1.2.3" }
    if (($global:ArnTestRequests -join " ") -ne "https://api.github.com/repos/ARN-Forge/ARN-Code/releases/tags/v1.2.3 $expectedUrls") {
        $failures.Add("explicit version requested: $($global:ArnTestRequests -join ' ')")
    }
    Test-Install "explicit version vX.Y.Z" $true "Downloading ARN Code v1.2.3" @{ Version = "v1.2.3" }
    $env:ARN_VERSION = "1.2.3"
    try { Test-Install "version from ARN_VERSION" $true "Downloading ARN Code v1.2.3" @{} } finally { Remove-Item Env:ARN_VERSION }
    $global:ArnTestLatestTag = "v1.2.3"
    Test-Install "latest resolves to one tag" $true "Downloading ARN Code v1.2.3" @{}
    if (($global:ArnTestRequests | Select-Object -Skip 1) -join " " -ne $expectedUrls) {
        $failures.Add("latest requested: $($global:ArnTestRequests -join ' ')")
    }
    $global:ArnTestLatestTag = "../../evil"
    Test-Install "latest with an unexpected tag" $false "malformed release metadata" @{}
    $global:ArnTestLatestTag = $null
    Test-Install "latest unavailable" $false "has no published stable ARN Code release" @{}
    Publish "v2.0.0" -NoManifest
    Test-Install "release without SHA256SUMS" $false "ARN Code v2.0.0 has no SHA256SUMS" @{ Version = "2.0.0" }
    Publish "v2.1.0" ""
    Test-Install "release with an empty SHA256SUMS" $false "SHA256SUMS is missing or empty" @{ Version = "2.1.0" }
    Test-Install "release that does not exist" $false "There is no ARN Code release v9.9.9" @{ Version = "9.9.9" }
    Publish "v3.0.0" "$other  $archiveName`n"
    Test-Install "downloaded archive with a wrong checksum" $false "but SHA256SUMS lists $other" @{ Version = "3.0.0" }

    # Invalid versions are rejected before anything is requested.
    foreach ($bad in @("1.2", "v1.2.3-rc.1", "01.2.3", "1.2.3.4", "../1.2.3", "1.2.3;New-Item $pwned",
                       "`$(New-Item $pwned)", "v1.2.3/../x", "Latest", " 1.2.3", "https://example.com/1.2.3")) {
        Test-Install "invalid version '$bad'" $false "Version must be latest, X.Y.Z or vX.Y.Z" @{ Version = $bad }
        if ($global:ArnTestRequests.Count) { $failures.Add("invalid version '$bad' made a request: $($global:ArnTestRequests -join ' ')") }
    }
    Test-Install "invalid repository" $false "Repository must have the form owner/name" @{ Repository = "owner/name/../x" }
    if (Test-Path -LiteralPath $pwned) { $failures.Add("version input was executed") }

    # Every run, failed ones included, removed its temporary files.
    $script:cases++
    $leftovers = @(Get-ChildItem -LiteralPath (Join-Path $root "tmp") -Force)
    if ($leftovers.Count) { $failures.Add("temporary files left behind: $($leftovers.Name -join ', ')") }
    else { Write-Host "ok: no temporary files left behind" }
} finally {
    Remove-Variable -Scope Global -Name ArnTestRequests, ArnTestLatestTag, ArnTestReleases -ErrorAction SilentlyContinue
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
