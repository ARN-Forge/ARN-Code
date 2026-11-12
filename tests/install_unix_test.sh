#!/usr/bin/env bash
# Tests the checksum verification and version selection of scripts/install.sh.
#   install_unix_test.sh <install.sh>
# Nothing is downloaded: a fake curl earlier on PATH serves files from a
# temporary directory laid out like GitHub release downloads and logs every
# URL it is asked for. The archive holds a shell script standing in for arn.
# Exits 77 (skipped) on a platform install.sh has no release for.

set -u
export LC_ALL=C

installer="$1"
case "$(uname -s):$(uname -m)" in
    Linux:x86_64|Linux:amd64) package="arn-linux-x64" ;;
    Darwin:arm64|Darwin:aarch64) package="arn-macos-arm64" ;;
    Darwin:x86_64|Darwin:amd64) package="arn-macos-x64" ;;
    *) echo "skip: install.sh has no release for $(uname -s) $(uname -m)"; exit 77 ;;
esac
archive_name="$package.tar.gz"

root="$(mktemp -d "${TMPDIR:-/tmp}/arn-install-test.XXXXXX")"
trap 'rm -rf "$root"' EXIT
mkdir -p "$root/tmp" "$root/home" "$root/shim" "$root/releases"
export TMPDIR="$root/tmp" HOME="$root/home" ARN_SKIP_PATH_UPDATE=1
unset ARN_VERSION ARN_INSTALL_ARCHIVE ARN_INSTALL_CHECKSUMS ARN_REPOSITORY || true
failures=0
cases=0

hash_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi
}

# The release archive: a script that prints an arn version line.
mkdir -p "$root/payload/$package"
printf '#!/bin/sh\necho "arn 1.2.3 (0123456789ab)"\n' > "$root/payload/$package/arn"
chmod 755 "$root/payload/$package/arn"
printf 'MIT\n' > "$root/payload/$package/LICENSE"
printf '# ARN\n' > "$root/payload/$package/README.md"
(cd "$root/payload" && tar -czf "$root/$archive_name" "$package")
good_hash="$(hash_of "$root/$archive_name")"
other_hash="$(printf 'other' > "$root/other" && hash_of "$root/other")"

# Fake curl: FAKE_LATEST_TAG answers the releases/latest redirect; downloads
# map .../releases/download/<tag>/<file> to $root/releases/<tag>/<file>.
cat > "$root/shim/curl" <<EOF
#!/bin/sh
url=""; output=""; head=0; write_out=""
while [ \$# -gt 0 ]; do
    case "\$1" in
        --output|-o) output="\$2"; shift ;;
        --write-out|-w) write_out="\$2"; shift ;;
        --head|-I) head=1 ;;
        http*) url="\$1" ;;
    esac
    shift
done
printf '%s\n' "\$url" >> "$root/curl.log"
if [ "\$head" = 1 ]; then
    case "\$url" in
        */releases/latest) ;;
        *) exit 22 ;;
    esac
    [ -n "\${FAKE_LATEST_TAG:-}" ] || exit 22
    [ -n "\$write_out" ] && printf '%s' "https://github.com/owner/repo/releases/tag/\${FAKE_LATEST_TAG}"
    exit 0
fi
path="\${url#*/releases/download/}"
[ "\$path" != "\$url" ] && [ -f "$root/releases/\$path" ] || exit 22
cp "$root/releases/\$path" "\$output"
EOF
chmod 755 "$root/shim/curl"
export PATH="$root/shim:$PATH"

publish() { # publish <tag> <manifest text>; the archive is always the good one
    mkdir -p "$root/releases/$1"
    cp "$root/$archive_name" "$root/releases/$1/$archive_name"
    printf '%s' "$2" > "$root/releases/$1/SHA256SUMS"
}

# run <name> <pass|fail> <fragment> [VAR=value ...]: runs install.sh into a
# fresh directory; checks the outcome, the message, and that a failure left
# no installed arn.
run() {
    local name="$1" outcome="$2" fragment="$3" output status dir
    shift 3
    cases=$((cases + 1))
    dir="$root/bin-$cases"
    : > "$root/curl.log"
    output="$(env ARN_INSTALL_DIR="$dir" "$@" sh "$installer" 2>&1)"
    status=$?
    if [ "$outcome" = pass ] && { [ $status -ne 0 ] || [ ! -x "$dir/arn" ]; }; then
        echo "FAIL: $name: expected installation, got exit $status: $output"; failures=$((failures + 1)); return
    fi
    if [ "$outcome" = fail ] && [ $status -eq 0 ]; then
        echo "FAIL: $name: expected failure, but it installed: $output"; failures=$((failures + 1)); return
    fi
    if [ "$outcome" = fail ] && [ -e "$dir/arn" ]; then
        echo "FAIL: $name: arn was installed despite the failure"; failures=$((failures + 1)); return
    fi
    if [ -n "$fragment" ] && ! printf '%s' "$output" | grep -qF -- "$fragment"; then
        echo "FAIL: $name: output lacks '$fragment': $output"; failures=$((failures + 1)); return
    fi
    echo "ok: $name"
}

manifest="$root/SHA256SUMS"
local_install() { # local_install <name> <pass|fail> <fragment> <manifest text>
    printf '%s' "$4" > "$manifest"
    run "$1" "$2" "$3" ARN_INSTALL_ARCHIVE="$root/$archive_name" ARN_INSTALL_CHECKSUMS="$manifest"
}

# Local archive with a manifest.
local_install "valid manifest" pass "Verified $archive_name" "$good_hash  $archive_name
"
local_install "binary-mode marker and upper-case hash" pass "" "$(printf '%s' "$good_hash" | tr 'a-f' 'A-F') *$archive_name
"
local_install "manifest listing other assets too" pass "" "$other_hash  arn-windows-x64.zip
$good_hash  $archive_name
$other_hash  arn-macos-zzz.tar.gz
"
local_install "checksum mismatch" fail "has SHA-256 $good_hash, but SHA256SUMS lists $other_hash" "$other_hash  $archive_name
"
local_install "missing entry" fail "SHA256SUMS has no entry for $archive_name" "$good_hash  arn-windows-x64.zip
"
local_install "wrong file name (pattern lookalike)" fail "no entry for $archive_name" "$good_hash  ${package}-tar.gz
"
local_install "duplicate entries" fail "lists $archive_name more than once" "$good_hash  $archive_name
$good_hash  $archive_name
"
local_install "hash too short" fail "malformed line" "${good_hash%?}  $archive_name
"
local_install "hash not hexadecimal" fail "malformed line" "${good_hash%?}g  $archive_name
"
local_install "line without the two-character separator" fail "malformed line" "$good_hash $archive_name
"
local_install "blank line" fail "malformed line" "$good_hash  $archive_name

"
local_install "CRLF line ending" fail "malformed line" "$good_hash  $archive_name$(printf '\r')
"
local_install "empty manifest" fail "SHA256SUMS is missing or empty" ""
local_install "shell code in the manifest is never run" fail "malformed line" "\$(touch $root/pwned)  $archive_name
\`touch $root/pwned\`  $archive_name
"
if [ -e "$root/pwned" ]; then echo "FAIL: manifest content was executed"; failures=$((failures + 1)); fi
run "local archive without a manifest" fail "ARN_INSTALL_ARCHIVE needs ARN_INSTALL_CHECKSUMS" \
    ARN_INSTALL_ARCHIVE="$root/$archive_name"

# Downloads through the fake curl.
publish v1.2.3 "$good_hash  $archive_name
"
run "explicit version X.Y.Z" pass "Downloading ARN Code v1.2.3" ARN_VERSION=1.2.3
expected_urls="https://github.com/ARN-Forge/ARN-Code/releases/download/v1.2.3/$archive_name
https://github.com/ARN-Forge/ARN-Code/releases/download/v1.2.3/SHA256SUMS"
if [ "$(cat "$root/curl.log")" != "$expected_urls" ]; then
    echo "FAIL: explicit version requested: $(cat "$root/curl.log")"; failures=$((failures + 1))
fi
run "explicit version vX.Y.Z" pass "Downloading ARN Code v1.2.3" ARN_VERSION=v1.2.3
run "latest resolves to one tag" pass "Downloading ARN Code v1.2.3" FAKE_LATEST_TAG=v1.2.3
if [ "$(sed -n '2,3p' "$root/curl.log")" != "$expected_urls" ]; then
    echo "FAIL: latest requested: $(cat "$root/curl.log")"; failures=$((failures + 1))
fi
run "latest with an unexpected tag" fail "unexpected tag" "FAKE_LATEST_TAG=../../evil"
run "latest unavailable" fail "Could not find the latest ARN Code release" FAKE_LATEST_TAG=
mkdir -p "$root/releases/v2.0.0"
cp "$root/$archive_name" "$root/releases/v2.0.0/$archive_name"
run "release without SHA256SUMS" fail "Could not download SHA256SUMS for v2.0.0" ARN_VERSION=2.0.0
run "release without the archive" fail "Could not download $archive_name for v9.9.9" ARN_VERSION=9.9.9
publish v3.0.0 "$other_hash  $archive_name
"
run "downloaded archive with a wrong checksum" fail "but SHA256SUMS lists $other_hash" ARN_VERSION=3.0.0

# Invalid versions are rejected before anything is requested.
for bad in 1.2 v1.2.3-rc.1 01.2.3 1.2.3.4 ../1.2.3 "1.2.3;touch $root/pwned" "\$(touch $root/pwned)" "v1.2.3/../x" Latest " 1.2.3"; do
    run "invalid version '$bad'" fail "ARN_VERSION must be latest, X.Y.Z or vX.Y.Z" "ARN_VERSION=$bad"
    if [ -s "$root/curl.log" ]; then echo "FAIL: invalid version '$bad' reached curl"; failures=$((failures + 1)); fi
done
run "invalid repository" fail "ARN_REPOSITORY must have the form owner/name" "ARN_REPOSITORY=owner/name/../x"
if [ -e "$root/pwned" ]; then echo "FAIL: version input was executed"; failures=$((failures + 1)); fi

# Every run, failed ones included, removed its temporary files.
leftovers="$(ls -A "$TMPDIR")"
if [ -n "$leftovers" ]; then echo "FAIL: temporary files left behind: $leftovers"; failures=$((failures + 1)); fi

echo "$cases cases, $failures failure(s)"
[ "$failures" -eq 0 ]
