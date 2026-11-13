#!/usr/bin/env bash
# Tests release resolution and downgrade protection in scripts/install.sh
# (cases T1-T13 of the installer migration design).
#   install_resolver_test.sh <install.sh>
# Nothing is downloaded and no live release is used: a fake curl earlier on
# PATH answers the GitHub API from JSON files and serves release downloads
# from a temporary directory, logging every URL; a fake uname picks the
# platform, so every platform's package is tested on any POSIX host. The
# archives hold a shell script standing in for arn.

set -u
export LC_ALL=C

installer="$1"
root="$(mktemp -d "${TMPDIR:-/tmp}/arn-resolver-test.XXXXXX")"
trap 'rm -rf "$root"' EXIT
mkdir -p "$root/tmp" "$root/home" "$root/shim" "$root/payloads"
export TMPDIR="$root/tmp" HOME="$root/home" ARN_SKIP_PATH_UPDATE=1
unset ARN_VERSION ARN_INSTALL_ARCHIVE ARN_INSTALL_CHECKSUMS ARN_REPOSITORY ARN_ALLOW_DOWNGRADE \
    ARN_ADOPT_EXISTING ARN_UNINSTALL ARN_INSTALL_DIR ARN_PROFILE ARN_INSTALL_TEST_FAULT || true
failures=0
cases=0
fail() { echo "FAIL: $1"; failures=$((failures + 1)); }

hash_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi
}

# Fake uname: FAKE_UNAME_S / FAKE_UNAME_M (Linux x86_64 by default).
cat > "$root/shim/uname" <<'EOF'
#!/bin/sh
case "${1:-}" in
    -s) echo "${FAKE_UNAME_S:-Linux}" ;;
    -m) echo "${FAKE_UNAME_M:-x86_64}" ;;
    *) echo "${FAKE_UNAME_S:-Linux}" ;;
esac
EOF

# Fake curl. API requests map to files in $root/api:
#   .../releases/latest          -> latest
#   .../releases/tags/<tag>      -> tags/<tag>
#   .../releases?per_page=100    -> list
# <key>.json is the body (absent: HTTP 404), <key>.status the status
# (default 200), <key>.headers extra response headers, and <key>.timeout
# makes curl fail as on a timeout. Downloads map
# .../releases/download/<tag>/<file> to $root/dl/<tag>/<file>.
cat > "$root/shim/curl" <<EOF
#!/bin/sh
url=""; output=""; write_out=""; dump=""
while [ \$# -gt 0 ]; do
    case "\$1" in
        --output|-o) output="\$2"; shift ;;
        --write-out|-w) write_out="\$2"; shift ;;
        --dump-header|-D) dump="\$2"; shift ;;
        --header|-H|--user-agent|-A|--connect-timeout|--max-time|--proto) shift ;;
        http*) url="\$1" ;;
    esac
    shift
done
printf '%s\n' "\$url" >> "$root/curl.log"
case "\$url" in
    https://api.github.com/repos/*/releases/latest) key=latest ;;
    https://api.github.com/repos/*/releases/tags/*) key="tags/\${url##*/}" ;;
    'https://api.github.com/repos/'*'/releases?per_page=100') key=list ;;
    https://api.github.com/*) key=unknown ;;
    https://github.com/*/releases/download/*)
        path="\${url#*/releases/download/}"
        [ -f "$root/dl/\$path" ] || { echo "curl: (22) The requested URL returned error: 404" >&2; exit 22; }
        cp "$root/dl/\$path" "\$output"
        exit 0
        ;;
    *) echo "curl: (6) Could not resolve host" >&2; exit 6 ;;
esac
base="$root/api/\$key"
if [ -f "\$base.timeout" ]; then echo "curl: (28) Operation timed out after 60000 milliseconds" >&2; printf '000'; exit 28; fi
status=200
[ -f "\$base.status" ] && status="\$(cat "\$base.status")"
[ -f "\$base.json" ] || status=404
if [ -n "\$dump" ]; then
    { printf 'HTTP/2 %s\r\n' "\$status"; [ -f "\$base.headers" ] && cat "\$base.headers"; printf '\r\n'; } > "\$dump"
fi
if [ -f "\$base.json" ]; then cp "\$base.json" "\$output"; else printf '{"message":"Not Found"}' > "\$output"; fi
[ -n "\$write_out" ] && printf '%s' "\$status"
exit 0
EOF
chmod 755 "$root/shim/uname" "$root/shim/curl"
export PATH="$root/shim:$PATH"

package_for() { # package_for <uname -s> <uname -m>
    case "$1:$2" in
        Linux:x86_64) echo arn-linux-x64 ;;
        Darwin:arm64) echo arn-macos-arm64 ;;
        Darwin:x86_64) echo arn-macos-x64 ;;
    esac
}

# archive <package> <version>: path of a tar.gz whose arn reports that
# version (and, if ever run, records that it ran).
archive() {
    local file="$root/payloads/$2/$1.tar.gz"
    if [ ! -f "$file" ]; then
        mkdir -p "$root/payloads/$2/src/$1"
        printf '#!/bin/sh\necho run >> "%s/executed-%s"\necho "arn %s (0123456789ab)"\n' "$root" "$2" "$2" > "$root/payloads/$2/src/$1/arn"
        chmod 755 "$root/payloads/$2/src/$1/arn"
        (cd "$root/payloads/$2/src" && tar -czf "$file" "$1")
    fi
    printf '%s\n' "$file"
}

reset_world() {
    rm -rf "$root/api" "$root/dl"
    rm -f "$root"/executed-*
    mkdir -p "$root/api/tags" "$root/dl"
    : > "$root/list.items"
}

# release <tag> <true|false prerelease> <asset>...: publishes a release.
# Assets named arn-* get archives whose arn reports the tag's version;
# SHA256SUMS (when listed) has correct entries for them. SUMS=<file> uses
# that SHA256SUMS text instead. The API metadata lists exactly the assets
# named here (a name prefixed with "-" is listed but not downloadable).
release() {
    local tag="$1" pre="$2" version="${1#v}" assets="" name file listed
    shift 2
    mkdir -p "$root/dl/$tag"
    : > "$root/dl/$tag/.sums"
    for name in "$@"; do
        listed="${name#-}"
        assets="$assets${assets:+,}{\"url\":\"https://api.github.com/x\",\"name\":\"$listed\",\"size\":10,\"uploader\":{\"login\":\"github-actions[bot]\",\"name\":null}}"
        case "$name" in
            -*) continue ;;
            arn-*.tar.gz|arn-*.zip)
                file="$(archive "${name%.tar.gz}" "$version")"
                cp "$file" "$root/dl/$tag/$name"
                printf '%s  %s\n' "$(hash_of "$file")" "$name" >> "$root/dl/$tag/.sums"
                ;;
        esac
    done
    for name in "$@"; do
        if [ "$name" = SHA256SUMS ]; then
            if [ -n "${SUMS:-}" ]; then cp "$SUMS" "$root/dl/$tag/SHA256SUMS"; else cp "$root/dl/$tag/.sums" "$root/dl/$tag/SHA256SUMS"; fi
        fi
    done
    printf '{"url":"https://api.github.com/repos/o/r/releases/1","tag_name":"%s","name":"ARN Code %s","draft":false,"prerelease":%s,"body":"Notes: \\"quoted\\", a\\\\b, caf\\u00e9, [brackets] {braces} \\n","author":{"login":"github-actions[bot]","name":null},"assets":[%s],"reactions":{"+1":0}}' \
        "$tag" "$tag" "$pre" "$assets" > "$root/api/tags/$tag.json"
    printf '%s\n' "$tag" >> "$root/list.items"
    write_list
}
write_list() {
    local first=1 tag
    { printf '['
      while read -r tag; do
          [ $first = 1 ] || printf ','
          first=0
          cat "$root/api/tags/$tag.json"
      done < "$root/list.items"
      printf ']'; } > "$root/api/list.json"
}
set_latest() { cp "$root/api/tags/$1.json" "$root/api/latest.json"; }

all_platforms="arn-windows-x64.zip arn-linux-x64.tar.gz arn-macos-arm64.tar.gz arn-macos-x64.tar.gz arn-ide-windows-x64.zip"

# run <name> <pass|fail> <fragment> [VAR=value ...]: runs install.sh into
# $dir (INSTALL_DIR=<dir> picks another); checks the outcome, the message,
# and that a failure left the installation as it was.
dir=""
last_output=""
run() {
    local name="$1" outcome="$2" fragment="$3" output status before after
    shift 3
    cases=$((cases + 1))
    : > "$root/curl.log"
    before="$( [ -f "$dir/arn" ] && "$dir/arn" 2>/dev/null; cat "$dir/.arn-install-manifest" 2>/dev/null)"
    output="$(env ARN_INSTALL_DIR="$dir" "$@" sh "$installer" 2>&1)"
    status=$?
    last_output="$output"
    after="$( [ -f "$dir/arn" ] && "$dir/arn" 2>/dev/null; cat "$dir/.arn-install-manifest" 2>/dev/null)"
    if [ "$outcome" = pass ] && { [ $status -ne 0 ] || [ ! -x "$dir/arn" ]; }; then
        fail "$name: expected installation, got exit $status: $output"; return 1
    fi
    if [ "$outcome" = fail ] && [ $status -eq 0 ]; then
        fail "$name: expected failure, but it installed: $output"; return 1
    fi
    if [ "$outcome" = fail ] && [ "$before" != "$after" ]; then
        fail "$name: the failure changed the installation"; return 1
    fi
    if [ -n "$fragment" ] && ! printf '%s' "$output" | grep -qF -- "$fragment"; then
        fail "$name: output lacks '$fragment': $output"; return 1
    fi
    echo "ok: $name"
}
fresh_dir() { dir="$root/bin-$((cases + 1))"; }
installed() { "$dir/arn" 2>/dev/null | sed -n '$p'; }
requested() { grep -qF -- "$1" "$root/curl.log"; }
no_downloads() { ! grep -q '/releases/download/' "$root/curl.log"; }
expect_no_downloads() { no_downloads || fail "$1: something was downloaded: $(cat "$root/curl.log")"; }

# --- T1: v0.8.0 is the latest release and has no SHA256SUMS ---------------
reset_world
release v0.8.0 false $all_platforms
set_latest v0.8.0
fresh_dir
run "T1 latest v0.8.0 without SHA256SUMS is refused" fail \
    "The latest ARN Code release, v0.8.0, has no SHA256SUMS, so its arn-linux-x64.tar.gz cannot be verified."
printf '%s' "$last_output" | grep -qF "No compatible release: no stable ARN Code release has both arn-linux-x64.tar.gz and SHA256SUMS." ||
    fail "T1: no 'No compatible release' explanation: $last_output"
expect_no_downloads "T1"
[ ! -e "$dir" ] || fail "T1: the installation directory was created"

# --- T2: v0.8.0 with a SHA256SUMS added afterwards -------------------------
reset_world
release v0.8.0 false $all_platforms SHA256SUMS
set_latest v0.8.0
fresh_dir
run "T2 latest v0.8.0 with SHA256SUMS installs" pass "Verified arn-linux-x64.tar.gz" &&
    [ "$(installed)" = "arn 0.8.0 (0123456789ab)" ] || fail "T2: wrong version installed: $(installed)"

# --- T3: a full release; every platform gets its own archive ---------------
reset_world
release v0.8.0 false $all_platforms
release v0.9.0 false $all_platforms SHA256SUMS
set_latest v0.9.0
for platform in Linux:x86_64 Darwin:arm64 Darwin:x86_64; do
    s="${platform%:*}"; m="${platform#*:}"; package="$(package_for "$s" "$m")"
    fresh_dir
    run "T3 full release on $s $m" pass "Verified $package.tar.gz" FAKE_UNAME_S="$s" FAKE_UNAME_M="$m" || continue
    expected="https://api.github.com/repos/ARN-Forge/ARN-Code/releases/latest
https://github.com/ARN-Forge/ARN-Code/releases/download/v0.9.0/SHA256SUMS
https://github.com/ARN-Forge/ARN-Code/releases/download/v0.9.0/$package.tar.gz"
    [ "$(cat "$root/curl.log")" = "$expected" ] || fail "T3 $s $m: requests: $(cat "$root/curl.log")"
done
fresh_dir
run "T3 platform without a build (Linux aarch64)" fail "ARN Code does not have a prebuilt release for Linux aarch64" \
    FAKE_UNAME_M=aarch64
[ ! -s "$root/curl.log" ] || fail "T3 unsupported platform made requests"

# --- T4: a partial prerelease while v0.8.0 is the latest -------------------
reset_world
release v0.8.0 false $all_platforms
release v0.9.0 true arn-linux-x64.tar.gz arn-macos-arm64.tar.gz arn-macos-x64.tar.gz SHA256SUMS
set_latest v0.8.0
fresh_dir
run "T4 default stays on the latest v0.8.0 (refused without SHA256SUMS)" fail "v0.8.0, has no SHA256SUMS"
printf '%s' "$last_output" | grep -qF "The prerelease v0.9.0 has them; prereleases are installed only when named:" &&
    printf '%s' "$last_output" | grep -qF "curl -fsSL https://raw.githubusercontent.com/ARN-Forge/ARN-Code/main/scripts/install.sh | ARN_VERSION=0.9.0 sh" ||
    fail "T4: the prerelease command is not offered: $last_output"
expect_no_downloads "T4 default"
for platform in Linux:x86_64 Darwin:arm64 Darwin:x86_64; do
    s="${platform%:*}"; m="${platform#*:}"
    fresh_dir
    run "T4 ARN_VERSION=0.9.0 installs the prerelease on $s $m" pass "Note: v0.9.0 is a prerelease" \
        ARN_VERSION=0.9.0 FAKE_UNAME_S="$s" FAKE_UNAME_M="$m" &&
        { [ "$(installed)" = "arn 0.9.0 (0123456789ab)" ] || fail "T4 $s $m: wrong version: $(installed)"; }
done
reset_world
release v0.9.0 true arn-linux-x64.tar.gz SHA256SUMS
fresh_dir
run "T4 explicit release without this platform's archive" fail \
    "ARN Code v0.9.0 has no arn-macos-arm64.tar.gz (no build for Darwin arm64)" ARN_VERSION=0.9.0 FAKE_UNAME_S=Darwin FAKE_UNAME_M=arm64
expect_no_downloads "T4 missing platform"

# --- T5: the latest release lacks this platform's archive ------------------
reset_world
release v0.7.1 false $all_platforms SHA256SUMS
release v0.8.0 false $all_platforms SHA256SUMS
release v0.8.1 false arn-windows-x64.zip arn-linux-x64.tar.gz SHA256SUMS
set_latest v0.8.1
fresh_dir
run "T5 latest without a macOS archive is refused, not replaced" fail \
    "The latest ARN Code release, v0.8.1, has no arn-macos-arm64.tar.gz (no build for Darwin arm64)." FAKE_UNAME_S=Darwin FAKE_UNAME_M=arm64
printf '%s' "$last_output" | grep -qF "The newest stable release that has arn-macos-arm64.tar.gz and SHA256SUMS is v0.8.0." &&
    printf '%s' "$last_output" | grep -qF "  curl -fsSL https://raw.githubusercontent.com/ARN-Forge/ARN-Code/main/scripts/install.sh | ARN_VERSION=0.8.0 sh" ||
    fail "T5: no ready-to-copy command for v0.8.0: $last_output"
expect_no_downloads "T5"
run "T5 the suggested command installs v0.8.0" pass "Verified arn-macos-arm64.tar.gz" \
    ARN_VERSION=0.8.0 FAKE_UNAME_S=Darwin FAKE_UNAME_M=arm64
fresh_dir
run "T5 the same latest release installs where it has the archive" pass "Downloading ARN Code v0.8.1"
echo 500 > "$root/api/list.status"
fresh_dir
run "T5 suggestion lookup fails: still refused, with the reason" fail \
    "Could not look for another release that has arn-macos-arm64.tar.gz: the GitHub API answered HTTP 500." FAKE_UNAME_S=Darwin FAKE_UNAME_M=arm64
expect_no_downloads "T5 lookup failure"
rm "$root/api/list.status"
# Listed in the metadata, but the download fails.
reset_world
printf '%s  arn-macos-x64.tar.gz\n' "$(hash_of "$(archive arn-macos-x64 0.8.1)")" > "$root/sums-dl"
SUMS="$root/sums-dl" release v0.8.1 false arn-linux-x64.tar.gz -arn-macos-x64.tar.gz SHA256SUMS
release v0.8.2 false arn-linux-x64.tar.gz -SHA256SUMS
fresh_dir
run "T5 listed archive that cannot be downloaded" fail "Could not download arn-macos-x64.tar.gz for v0.8.1" \
    ARN_VERSION=0.8.1 FAKE_UNAME_S=Darwin FAKE_UNAME_M=x86_64
run "T5 listed SHA256SUMS that cannot be downloaded" fail "Could not download SHA256SUMS for v0.8.2" ARN_VERSION=0.8.2
requested "arn-linux-x64.tar.gz" && fail "T5: the archive was downloaded without SHA256SUMS"

# A full page of 100 releases of realistic size (about 450 KB, raw UTF-8 in
# the notes) is parsed in linear time by every awk: BWK awk, the base of the
# macOS awk, once took minutes for it.
reset_world
digest="sha256:$(printf 'd%.0s' $(seq 64))"
big_body="$(printf -- '- Item, with text, \\"quotes\\" and caf\303\251 \342\200\224 details.\\r\\n%.0s' $(seq 30))"
big_release() { # big_release <tag> <asset>...: release metadata as the API sends it
    local tag="$1" assets="" name id=0
    shift
    for name in "$@"; do
        id=$((id + 1))
        assets="$assets${assets:+,}{\"url\":\"https://api.github.com/repos/ARN-Forge/ARN-Code/releases/assets/$id\",\"id\":$id,\"name\":\"$name\",\"label\":\"\",\"uploader\":{\"login\":\"github-actions[bot]\",\"id\":41898282,\"avatar_url\":\"https://avatars.githubusercontent.com/in/15368?v=4\",\"type\":\"Bot\",\"site_admin\":false},\"content_type\":\"application/gzip\",\"state\":\"uploaded\",\"size\":856938,\"digest\":\"$digest\",\"download_count\":3,\"created_at\":\"2026-10-04T18:50:16Z\",\"browser_download_url\":\"https://github.com/ARN-Forge/ARN-Code/releases/download/$tag/$name\"}"
    done
    printf '{"tag_name":"%s","name":"ARN Code %s","draft":false,"prerelease":false,"body":"%s","assets":[%s]}' "$tag" "$tag" "$big_body" "$assets"
}
big_release v1.0.99 arn-linux-x64.tar.gz SHA256SUMS > "$root/api/latest.json"
{
    printf '['
    big_release v1.0.99 arn-linux-x64.tar.gz SHA256SUMS
    for k in $(seq 98 -1 0); do printf ','; big_release "v1.0.$k" $all_platforms SHA256SUMS; done
    printf ']'
} > "$root/api/list.json"
fresh_dir
started="$(date +%s)"
run "T5 a page of 100 large releases is searched for a compatible one" fail \
    "The newest stable release that has arn-macos-arm64.tar.gz and SHA256SUMS is v1.0.98." FAKE_UNAME_S=Darwin FAKE_UNAME_M=arm64
elapsed=$(( $(date +%s) - started ))
[ "$elapsed" -le 60 ] || fail "T5: searching 100 releases took ${elapsed}s (limit 60s)"

# --- T6: explicit versions that do not exist or are invalid ----------------
reset_world
release v0.8.0 false $all_platforms SHA256SUMS
set_latest v0.8.0
fresh_dir
run "T6 explicit version that does not exist" fail "There is no ARN Code release v9.9.9" ARN_VERSION=9.9.9
[ "$(cat "$root/curl.log")" = "https://api.github.com/repos/ARN-Forge/ARN-Code/releases/tags/v9.9.9" ] ||
    fail "T6: other releases were requested: $(cat "$root/curl.log")"
for bad in 0.9 v0.9.0-rc.1 00.9.0 "0.9.0 " latest2; do
    run "T6 invalid version '$bad'" fail "ARN_VERSION must be latest, X.Y.Z or vX.Y.Z" "ARN_VERSION=$bad"
    [ ! -s "$root/curl.log" ] || fail "T6 invalid version '$bad' made a request"
done
cp "$root/api/tags/v0.8.0.json" "$root/api/tags/v0.8.5.json"
run "T6 explicit version answered with another tag" fail "GitHub returned malformed release metadata for ARN Code v0.8.5" ARN_VERSION=0.8.5
expect_no_downloads "T6 tag mismatch"
sed 's/"draft":false/"draft":true/' "$root/api/tags/v0.8.0.json" > "$root/api/tags/v0.8.6.json"
sed -i.bak 's/v0\.8\.0/v0.8.6/g' "$root/api/tags/v0.8.6.json"
run "T6 explicit draft release" fail "ARN Code v0.8.6 is an unpublished draft release" ARN_VERSION=0.8.6
expect_no_downloads "T6 draft"

# --- T7: a wrong hash in SHA256SUMS ---------------------------------------
reset_world
printf '%s  arn-linux-x64.tar.gz\n' "$(printf '0%.0s' $(seq 64))" > "$root/bad-sums"
SUMS="$root/bad-sums" release v0.9.0 false $all_platforms SHA256SUMS
set_latest v0.9.0
fresh_dir
run "T7 wrong hash is refused before extraction" fail "but SHA256SUMS lists $(printf '0%.0s' $(seq 64))"
[ ! -e "$root/executed-0.9.0" ] || fail "T7: the unverified archive was run"
requested "/v0.9.0/SHA256SUMS" && [ "$(grep -n 'SHA256SUMS' "$root/curl.log" | cut -d: -f1)" -lt "$(grep -n 'tar.gz' "$root/curl.log" | cut -d: -f1)" ] ||
    fail "T7: SHA256SUMS was not fetched before the archive: $(cat "$root/curl.log")"

# --- T8: SHA256SUMS missing, empty, malformed or with duplicate entries -----
reset_world
release v0.9.0 false $all_platforms
set_latest v0.9.0
fresh_dir
run "T8 latest without SHA256SUMS" fail "has no SHA256SUMS"
expect_no_downloads "T8 missing"
run "T8 explicit version without SHA256SUMS" fail "ARN Code v0.9.0 has no SHA256SUMS, so its arn-linux-x64.tar.gz cannot be verified" ARN_VERSION=0.9.0
expect_no_downloads "T8 explicit missing"
good="$(hash_of "$(archive arn-linux-x64 0.9.1)")"
check_sums() { # check_sums <name> <fragment> <SHA256SUMS text>
    reset_world
    printf '%s' "$3" > "$root/sums-case"
    SUMS="$root/sums-case" release v0.9.1 false $all_platforms SHA256SUMS
    set_latest v0.9.1
    fresh_dir
    run "T8 $1" fail "$2"
    requested "arn-linux-x64.tar.gz" && fail "T8 $1: the archive was downloaded although SHA256SUMS was unusable"
    [ ! -e "$root/executed-0.9.1" ] || fail "T8 $1: the archive was run"
}
check_sums "empty SHA256SUMS" "SHA256SUMS is missing or empty" ""
check_sums "SHA256SUMS without an entry for the archive" "SHA256SUMS has no entry for arn-linux-x64.tar.gz" "$good  arn-windows-x64.zip
"
check_sums "malformed SHA256SUMS line" "SHA256SUMS contains a malformed line" "$good arn-linux-x64.tar.gz
"
check_sums "duplicate SHA256SUMS entries" "lists arn-linux-x64.tar.gz more than once" "$good  arn-linux-x64.tar.gz
$good  arn-linux-x64.tar.gz
"
reset_world
release v0.9.0 false $all_platforms SHA256SUMS
sed 's/"assets":\[/"assets":[{"name":"SHA256SUMS"},/' "$root/api/tags/v0.9.0.json" > "$root/api/latest.json"
fresh_dir
run "T8 an asset listed twice" fail "an asset is listed twice"
expect_no_downloads "T8 duplicate asset"

# --- T9: GitHub API failures and malformed metadata ------------------------
reset_world
release v0.9.0 false $all_platforms SHA256SUMS
set_latest v0.9.0
fresh_dir
echo 403 > "$root/api/latest.status"
printf 'x-ratelimit-limit: 60\r\nx-ratelimit-remaining: 0\r\n' > "$root/api/latest.headers"
run "T9 rate limit (403, remaining 0)" fail "the GitHub API rate limit was reached"
expect_no_downloads "T9 rate limit"
rm "$root/api/latest.headers"
printf '{"message":"API rate limit exceeded for 192.0.2.1."}' > "$root/api/latest.json"
run "T9 rate limit (403, message)" fail "the GitHub API rate limit was reached"
echo 429 > "$root/api/latest.status"
run "T9 rate limit (429)" fail "the GitHub API rate limit was reached"
set_latest v0.9.0
echo 403 > "$root/api/latest.status"
run "T9 forbidden without a rate limit" fail "Could not look up the latest ARN Code release: the GitHub API answered HTTP 403."
echo 500 > "$root/api/latest.status"
run "T9 server error" fail "the GitHub API answered HTTP 500"
echo 503 > "$root/api/tags/v0.9.0.status"
run "T9 server error for an explicit version" fail "Could not look up ARN Code v0.9.0: the GitHub API answered HTTP 503." ARN_VERSION=0.9.0
rm "$root/api/latest.status" "$root/api/tags/v0.9.0.status"
touch "$root/api/latest.timeout"
run "T9 timeout" fail "GitHub could not be reached ((28) Operation timed out"
rm "$root/api/latest.timeout"
rm "$root/api/latest.json"
run "T9 no stable release (404 for latest)" fail "ARN-Forge/ARN-Code has no published stable ARN Code release."
malformed() { # malformed <name> <latest.json text>
    printf '%s' "$2" > "$root/api/latest.json"
    run "T9 malformed metadata: $1" fail "GitHub returned malformed release metadata for the latest ARN Code release"
    expect_no_downloads "T9 $1"
}
tag_json="$(cat "$root/api/tags/v0.9.0.json")"
malformed "truncated JSON" "${tag_json%?????}"
malformed "trailing data" "$tag_json}"
malformed "HTML instead of JSON" "<html><body>Unicorn!</body></html>"
malformed "empty body" ""
malformed "an array instead of a release" "[$tag_json]"
malformed "tag_name is a number" '{"tag_name":9,"draft":false,"prerelease":false,"assets":[]}'
malformed "tag_name is not vX.Y.Z" '{"tag_name":"v0.9.0-rc1","draft":false,"prerelease":false,"assets":[]}'
malformed "tag_name with a path" '{"tag_name":"../../v0.9.0","draft":false,"prerelease":false,"assets":[]}'
malformed "two tag_name keys" '{"tag_name":"v0.9.0","tag_name":"v0.9.1","draft":false,"prerelease":false,"assets":[]}'
malformed "draft is a string" '{"tag_name":"v0.9.0","draft":"false","prerelease":false,"assets":[]}'
malformed "no prerelease flag" '{"tag_name":"v0.9.0","draft":false,"assets":[]}'
malformed "no assets" '{"tag_name":"v0.9.0","draft":false,"prerelease":false}'
malformed "assets is an object" '{"tag_name":"v0.9.0","draft":false,"prerelease":false,"assets":{"name":"SHA256SUMS"}}'
malformed "asset name is a number" '{"tag_name":"v0.9.0","draft":false,"prerelease":false,"assets":[{"name":1}]}'
malformed "invalid escape" '{"tag_name":"v0.9.0","draft":false,"prerelease":false,"body":"\q","assets":[]}'
malformed "raw control character" "{\"tag_name\":\"v0.9.0\",\"draft\":false,\"prerelease\":false,\"body\":\"a$(printf '\001')b\",\"assets\":[]}"
malformed "bare word" '{"tag_name":"v0.9.0","draft":false,"prerelease":nope,"assets":[]}'
malformed "missing comma" '{"tag_name":"v0.9.0" "draft":false,"prerelease":false,"assets":[]}'
printf '%s' '{"tag_name":"$(touch '"$root"'/pwned)","draft":false,"prerelease":false,"assets":[]}' > "$root/api/latest.json"
run "T9 metadata is never evaluated" fail "malformed release metadata"
[ ! -e "$root/pwned" ] || fail "T9: metadata content was executed"
printf '%s' "$(printf '%s' "$tag_json" | sed 's/"prerelease":false/"prerelease":true/')" > "$root/api/latest.json"
run "T9 latest marked as a prerelease" fail "a draft or prerelease, as the latest release"
expect_no_downloads "T9 prerelease as latest"
cat > "$root/api/latest.json" <<'EOF'
{
  "tag_name": "v0.9.0",
  "draft": false,
  "prerelease": false,
  "body": "Line one\nLine two, with \"quotes\"",
  "assets": [
    { "name": "arn-linux-x64.tar.gz", "size": 10 },
    { "name": "SHA256SUMS", "size": 1 }
  ]
}
EOF
fresh_dir
run "T9 pretty-printed metadata is accepted" pass "Verified arn-linux-x64.tar.gz"

# --- T10: no compatible release at all ------------------------------------
reset_world
release v0.7.1 false $all_platforms
release v0.8.0 false arn-windows-x64.zip SHA256SUMS
set_latest v0.8.0
fresh_dir
run "T10 no release has a verifiable archive for this platform" fail \
    "No compatible release: no stable ARN Code release has both arn-linux-x64.tar.gz and SHA256SUMS."
printf '%s' "$last_output" | grep -qF "ARN_VERSION=" && fail "T10: a command was offered although nothing is compatible"
expect_no_downloads "T10"
printf '[]' > "$root/api/list.json"
run "T10 empty release list" fail "No compatible release"
printf '{"message":"x"}' > "$root/api/list.json"
run "T10 malformed release list" fail \
    "Could not look for another release that has arn-linux-x64.tar.gz: GitHub returned malformed release metadata."

# --- T11: downgrade protection --------------------------------------------
reset_world
release v0.8.0 false $all_platforms SHA256SUMS
release v0.9.0 true $all_platforms SHA256SUMS
release v0.10.0 false $all_platforms SHA256SUMS
set_latest v0.8.0
fresh_dir
run "T11 install the prerelease 0.9.0" pass "Note: v0.9.0 is a prerelease" ARN_VERSION=0.9.0
run "T11 the latest stable 0.8.0 would downgrade: refused" fail \
    "Downgrade refused: ARN Code 0.9.0 is installed in $dir, and 0.8.0 is older."
printf '%s' "$last_output" | grep -qF "curl -fsSL https://raw.githubusercontent.com/ARN-Forge/ARN-Code/main/scripts/install.sh | ARN_ALLOW_DOWNGRADE=1 ARN_VERSION=0.8.0 sh" ||
    fail "T11: no override command: $last_output"
expect_no_downloads "T11 refused (checked before downloading)"
run "T11 an explicit older version is refused too" fail "Downgrade refused" ARN_VERSION=0.8.0
expect_no_downloads "T11 explicit"
for value in yes true 2 " 1" 01; do
    run "T11 ARN_ALLOW_DOWNGRADE='$value' is rejected" fail "ARN_ALLOW_DOWNGRADE must be 1 (or unset)" "ARN_ALLOW_DOWNGRADE=$value"
done
run "T11 ARN_ALLOW_DOWNGRADE=0 still refuses" fail "Downgrade refused" ARN_ALLOW_DOWNGRADE=0
run "T11 ARN_ALLOW_DOWNGRADE=1 downgrades, with a warning" pass \
    "Warning: replacing ARN Code 0.9.0 with the older 0.8.0, because ARN_ALLOW_DOWNGRADE=1." ARN_ALLOW_DOWNGRADE=1 &&
    { [ "$(installed)" = "arn 0.8.0 (0123456789ab)" ] || fail "T11: not downgraded: $(installed)"; }
[ "$(printf '%s' "$last_output" | grep -c 'Warning: replacing')" = 1 ] || fail "T11: the warning was not printed exactly once"
run "T11 the same version again is not a downgrade" pass "arn 0.8.0 (0123456789ab) installed"
run "T11 0.10.0 is an upgrade from 0.9.0, not a downgrade" pass "arn 0.10.0" ARN_VERSION=0.10.0 &&
    { [ "$(installed)" = "arn 0.10.0 (0123456789ab)" ] || fail "T11: 0.10.0 not installed"; }
run "T11 0.9.0 after 0.10.0 is a downgrade (numeric, not text, order)" fail \
    "Downgrade refused: ARN Code 0.10.0 is installed in $dir, and 0.9.0 is older." ARN_VERSION=0.9.0
# A local package: its version is known only after verification.
local_sums="$root/local-sums"
printf '%s  arn-linux-x64.tar.gz\n' "$(hash_of "$(archive arn-linux-x64 0.9.0)")" > "$local_sums"
run "T11 a local older package is refused after verification" fail "Downgrade refused: ARN Code 0.10.0 is installed in $dir, and 0.9.0 is older." \
    ARN_INSTALL_ARCHIVE="$(archive arn-linux-x64 0.9.0)" ARN_INSTALL_CHECKSUMS="$local_sums"
# The installed version comes from the manifest, else from arn itself; one
# that is not X.Y.Z is unknown and never compared.
sed -i.bak 's/^version .*/version 0.10/' "$dir/.arn-install-manifest"
run "T11 malformed manifest version: the installed arn's own version is used" fail \
    "Downgrade refused: ARN Code 0.10.0 is installed" ARN_VERSION=0.9.0
printf '#!/bin/sh\necho "arn 1.0"\n' > "$dir/arn"
run "T11 no valid installed version: nothing to compare, the update goes ahead" pass "arn 0.9.0 (0123456789ab) installed" ARN_VERSION=0.9.0
fresh_dir
mkdir -p "$dir"
printf '#!/bin/sh\necho "arn 2.0.0 (cccccccccccc)"\n' > "$dir/arn"
chmod 755 "$dir/arn"
run "T11 an adopted installation without a manifest is checked too" fail \
    "Downgrade refused: ARN Code 2.0.0 is installed" ARN_ADOPT_EXISTING=1
# An arn that no manifest records (and that is not being adopted) is not
# ARN's: it is never run, and the update stops because of it, not because of
# its version.
fresh_dir
mkdir -p "$dir"
printf '#!/bin/sh\n[ "$1" = --version ] && touch "%s/unowned-ran"\necho "arn 9.9.9 (dddddddddddd)"\n' "$root" > "$dir/arn"
chmod 755 "$dir/arn"
run "T11 an arn that is not ARN's is never run for its version" fail \
    "already has arn, which no .arn-install-manifest records as ARN's"
printf '%s' "$last_output" | grep -qF "Downgrade refused" && fail "T11: an unowned arn's version caused a downgrade refusal"
[ ! -e "$root/unowned-ran" ] || fail "T11: an arn that is not ARN's was run"

# --- T12: a prerelease newer than the latest release ----------------------
reset_world
release v0.8.0 false $all_platforms SHA256SUMS
release v0.9.0 true $all_platforms SHA256SUMS
set_latest v0.8.0
fresh_dir
run "T12 a newer prerelease is not selected" pass "Downloading ARN Code v0.8.0" &&
    { [ "$(installed)" = "arn 0.8.0 (0123456789ab)" ] || fail "T12: not 0.8.0: $(installed)"; }
requested "v0.9.0" && fail "T12: the prerelease was requested: $(cat "$root/curl.log")"

# --- T13: the README command through the migration steps -----------------
# Step 0: today (latest v0.8.0 without SHA256SUMS).
reset_world
release v0.8.0 false $all_platforms
set_latest v0.8.0
fresh_dir
run "T13 step 0: latest v0.8.0 without SHA256SUMS is refused" fail "has no SHA256SUMS"
# Step 1: partial v0.9.0 published as a prerelease; latest stays v0.8.0.
release v0.9.0 true arn-linux-x64.tar.gz arn-macos-arm64.tar.gz arn-macos-x64.tar.gz SHA256SUMS
run "T13 step 1: the default still refuses and names the prerelease" fail "ARN_VERSION=0.9.0 sh"
run "T13 step 1: the tag-pinned command installs the prerelease" pass "Note: v0.9.0 is a prerelease" ARN_VERSION=0.9.0
# Step 2: the full v0.9.1 becomes the latest release.
release v0.9.1 false $all_platforms SHA256SUMS
set_latest v0.9.1
run "T13 step 2: the default upgrades to the full release" pass "arn 0.9.1 (0123456789ab) installed"
fresh_dir
run "T13 step 2: a new installation gets the full release" pass "arn 0.9.1 (0123456789ab) installed"
# Rollback: v0.9.1 withdrawn, v0.8.0 (with SHA256SUMS added) is the latest again.
reset_world
release v0.8.0 false $all_platforms SHA256SUMS
release v0.9.1 true $all_platforms SHA256SUMS
set_latest v0.8.0
run "T13 rollback: an installed 0.9.1 is not downgraded silently" fail "Downgrade refused: ARN Code 0.9.1 is installed"
fresh_dir
run "T13 rollback: a new installation gets v0.8.0" pass "arn 0.8.0 (0123456789ab) installed"

# Every run, failed ones included, removed its temporary files.
leftovers="$(ls -A "$TMPDIR")"
if [ -n "$leftovers" ]; then fail "temporary files left behind: $leftovers"; fi

echo "$cases cases, $failures failure(s)"
[ "$failures" -eq 0 ]
