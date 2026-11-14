#!/usr/bin/env bash
# Tests upgrades, rollback, the installation manifest and uninstall of
# scripts/install.sh.
#   install_unix_upgrade_test.sh <install.sh>
# Everything happens in a temporary directory with its own HOME and shell
# profile; the packages are local archives holding shell scripts that stand in
# for arn. Exits 77 (skipped) on a platform install.sh has no release for.

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

# macOS sets TMPDIR with a trailing slash; install.sh reports and records the
# install directory without "//", so the test root is named without it too.
tmp_base="${TMPDIR:-/tmp}"
root="$(mktemp -d "${tmp_base%/}/arn-upgrade-test.XXXXXX")"
trap 'chmod -R u+w "$root" 2>/dev/null; rm -rf "$root"' EXIT
mkdir -p "$root/tmp" "$root/home" "$root/pkg"
export TMPDIR="$root/tmp" HOME="$root/home" SHELL=/bin/sh
unset ARN_VERSION ARN_INSTALL_ARCHIVE ARN_INSTALL_CHECKSUMS ARN_REPOSITORY ARN_SKIP_PATH_UPDATE \
    ARN_PROFILE ARN_UNINSTALL ARN_ADOPT_EXISTING ARN_INSTALL_TEST_FAULT ARN_INSTALL_DIR ARN_ALLOW_DOWNGRADE || true
profile="$root/home/.profile"
failures=0
cases=0

hash_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi
}

# make_package <name> <arn version line>: an archive and its SHA256SUMS.
make_package() {
    local dir="$root/pkg/$1"
    mkdir -p "$dir/src/$package"
    printf '#!/bin/sh\necho "%s"\n' "$2" > "$dir/src/$package/arn"
    chmod 755 "$dir/src/$package/arn"
    printf 'MIT\n' > "$dir/src/$package/LICENSE"
    (cd "$dir/src" && tar -czf "$dir/$archive_name" "$package")
    printf '%s  %s\n' "$(hash_of "$dir/$archive_name")" "$archive_name" > "$dir/SHA256SUMS"
}
make_package v1 "arn 1.0.0 (aaaaaaaaaaaa)"
make_package v2 "arn 2.0.0 (bbbbbbbbbbbb)"
# A corrupt archive whose checksum matches, so only extraction fails.
mkdir -p "$root/pkg/corrupt"
printf 'not a tar archive\n' > "$root/pkg/corrupt/$archive_name"
printf '%s  %s\n' "$(hash_of "$root/pkg/corrupt/$archive_name")" "$archive_name" > "$root/pkg/corrupt/SHA256SUMS"
# The v2 archive with a SHA256SUMS that lists another hash.
mkdir -p "$root/pkg/badsum"
cp "$root/pkg/v2/$archive_name" "$root/pkg/badsum/"
printf '%064d  %s\n' 0 "$archive_name" > "$root/pkg/badsum/SHA256SUMS"

# install <dir> <package> [VAR=value ...]; uninstall <dir> [VAR=value ...]
# Both leave the output in $output and the exit status in $status.
install() {
    local dir="$1" pkg="$2"
    shift 2
    output="$(env ARN_INSTALL_DIR="$dir" ARN_PROFILE="$profile" ARN_INSTALL_ARCHIVE="$root/pkg/$pkg/$archive_name" \
        ARN_INSTALL_CHECKSUMS="$root/pkg/$pkg/SHA256SUMS" "$@" sh "$installer" 2>&1)"
    status=$?
}
uninstall() {
    local dir="$1"
    shift
    output="$(env ARN_INSTALL_DIR="$dir" ARN_PROFILE="$profile" ARN_UNINSTALL=1 "$@" sh "$installer" 2>&1)"
    status=$?
}

# snapshot <dir>: every entry under the directory with its type and content
# hash, plus the profile, so "nothing changed" can be compared exactly.
snapshot() {
    {
        if [ -d "$1" ]; then
            (cd "$1" && find . | sort | while IFS= read -r entry; do
                if [ -L "$entry" ]; then printf 'link %s -> %s\n' "$entry" "$(readlink "$entry")"
                elif [ -f "$entry" ]; then printf 'file %s %s\n' "$entry" "$(hash_of "$entry")"
                else printf 'dir %s\n' "$entry"; fi
            done)
        else
            echo "no directory"
        fi
        if [ -f "$profile" ]; then echo "profile $(hash_of "$profile")"; else echo "no profile"; fi
    }
}

ok() { cases=$((cases + 1)); echo "ok: $1"; }
fail() { cases=$((cases + 1)); failures=$((failures + 1)); echo "FAIL: $1"; }
# expect <name> <0|nonzero> <fragment>: checks $status and $output.
expect() {
    if [ "$2" = 0 ] && [ "$status" -ne 0 ]; then fail "$1: expected success, got exit $status: $output"; return 1; fi
    if [ "$2" != 0 ] && [ "$status" -eq 0 ]; then fail "$1: expected failure, got success: $output"; return 1; fi
    if [ -n "$3" ] && ! printf '%s' "$output" | grep -qF -- "$3"; then fail "$1: output lacks '$3': $output"; return 1; fi
    return 0
}
version_of() { "$1/arn" 2>/dev/null; }
no_work_dirs() { ! ls -A "$1" 2>/dev/null | grep -q '^\.arn-install\.'; }

# Fresh install into a new directory: arn, a manifest and the profile lines.
printf '# my profile\nexport EDITOR=vi\n' > "$profile"
dir="$root/fresh/bin"
install "$dir" v1
if expect "fresh install" 0 "arn 1.0.0 (aaaaaaaaaaaa) installed to $dir/arn" &&
    [ "$(version_of "$dir")" = "arn 1.0.0 (aaaaaaaaaaaa)" ] &&
    [ "$(cat "$dir/.arn-install-manifest")" = "arn-install-manifest 1
version 1.0.0
commit aaaaaaaaaaaa
install_dir $dir
profile $profile
created_dir 1
file arn" ] &&
    [ "$(tail -n 2 "$profile")" = "# Added by ARN Code installer: $dir
export PATH=\"$dir:\$PATH\"" ] && no_work_dirs "$dir"; then
    ok "fresh install"
else
    [ "$status" -eq 0 ] && fail "fresh install: unexpected result: $(cat "$dir/.arn-install-manifest" 2>&1) / $(cat "$profile")"
fi

# Upgrade: arn is replaced, files that are not ARN's stay.
printf 'notes\n' > "$dir/my-notes.txt"
mkdir "$dir/my-folder"
install "$dir" v2
if expect "upgrade" 0 "arn 2.0.0 (bbbbbbbbbbbb) installed" &&
    [ "$(version_of "$dir")" = "arn 2.0.0 (bbbbbbbbbbbb)" ] && [ -f "$dir/my-notes.txt" ] && [ -d "$dir/my-folder" ] &&
    grep -qx 'version 2.0.0' "$dir/.arn-install-manifest" && grep -qx "profile $profile" "$dir/.arn-install-manifest" &&
    grep -qx 'created_dir 1' "$dir/.arn-install-manifest" && no_work_dirs "$dir" &&
    [ "$(grep -c 'Added by ARN Code installer' "$profile")" = 1 ]; then
    ok "upgrade keeps user files and the profile lines once"
else
    [ "$status" -eq 0 ] && fail "upgrade: unexpected state: $(ls -A "$dir"; cat "$dir/.arn-install-manifest" "$profile")"
fi

# Failures before activation leave the installation exactly as it was.
check_unchanged() { # check_unchanged <name> <fragment> <package> [VAR=value ...]
    local name="$1" fragment="$2" pkg="$3" before
    shift 3
    before="$(snapshot "$dir")"
    install "$dir" "$pkg" "$@"
    expect "$name" nonzero "$fragment" || return
    if [ "$(snapshot "$dir")" != "$before" ]; then
        fail "$name: the installation changed: $(snapshot "$dir")"
    elif [ "$(version_of "$dir")" != "arn 2.0.0 (bbbbbbbbbbbb)" ]; then
        fail "$name: arn is not the previous version"
    else
        ok "$name"
    fi
}
check_unchanged "checksum mismatch leaves the installation intact" "but SHA256SUMS lists" badsum
check_unchanged "corrupt archive leaves the installation intact" "" corrupt
# These replace 2.0.0 with 1.0.0, a downgrade, so they allow one: what they
# test is the rollback.
check_unchanged "activation failure keeps the previous arn" "The previous installation was kept" v1 ARN_ALLOW_DOWNGRADE=1 ARN_INSTALL_TEST_FAULT=activate
check_unchanged "verification failure restores the previous arn" "The installed ARN executable failed its version check" v1 ARN_ALLOW_DOWNGRADE=1 ARN_INSTALL_TEST_FAULT=verify
check_unchanged "manifest failure restores the previous arn" "Injected test failure at manifest" v1 ARN_ALLOW_DOWNGRADE=1 ARN_INSTALL_TEST_FAULT=manifest
check_unchanged "a downgrade is refused before anything changes" "Downgrade refused: ARN Code 2.0.0 is installed in $dir, and 1.0.0 is older." v1

# A failed fresh install takes out the profile lines it added and the
# directory it created.
printf '# my profile\n' > "$profile"
before_profile="$(hash_of "$profile")"
install "$root/failed/bin" v1 ARN_INSTALL_TEST_FAULT=manifest
if expect "failed fresh install" nonzero "ARN Code was not installed." &&
    [ ! -e "$root/failed/bin" ] && [ "$(hash_of "$profile")" = "$before_profile" ]; then
    ok "failed fresh install removes its profile lines and directory"
else
    [ "$status" -ne 0 ] && fail "failed fresh install left: $(ls -A "$root/failed" 2>&1) / $(cat "$profile")"
fi

# Uninstall removes arn, the manifest and the profile lines; user files, the
# directory and every other profile line stay. The profile is a symbolic link
# that must survive as one.
mv "$profile" "$root/home/real-profile"
printf '\n# Added by ARN Code installer: /elsewhere\nexport PATH="/elsewhere:$PATH"\n' >> "$root/home/real-profile"
ln -s "$root/home/real-profile" "$profile"
before_profile="$(hash_of "$root/home/real-profile")"
dir="$root/shared/bin"
mkdir -p "$dir"
printf 'mine\n' > "$dir/other-tool"
install "$dir" v1
expect "install before uninstall" 0 "installed" && grep -qx 'created_dir 0' "$dir/.arn-install-manifest" &&
    grep -qxF "# Added by ARN Code installer: $dir" "$root/home/real-profile" ||
    fail "install into an existing directory: no created_dir 0 or no profile lines"
printf 'notes\n' > "$dir/my-notes.txt"
uninstall "$dir"
if expect "uninstall" 0 "was uninstalled from $dir" &&
    [ ! -e "$dir/arn" ] && [ ! -e "$dir/.arn-install-manifest" ] && [ -f "$dir/other-tool" ] && [ -f "$dir/my-notes.txt" ] &&
    [ -L "$profile" ] && [ "$(hash_of "$root/home/real-profile")" = "$before_profile" ]; then
    ok "uninstall removes only ARN's files and profile lines"
else
    [ "$status" -eq 0 ] && fail "uninstall left: $(ls -A "$dir") / $(cat "$root/home/real-profile")"
fi
uninstall "$dir"
if expect "repeated uninstall" 0 "ARN Code is not installed in $dir; nothing to remove." && [ -f "$dir/other-tool" ]; then
    ok "repeated uninstall is safe"
fi
rm -f "$profile"
mv "$root/home/real-profile" "$profile"

# Uninstall of a directory the installer created removes it once it is empty.
uninstall "$root/fresh/bin"
if expect "uninstall keeps a directory with user files" 0 "files there that are not ARN's were kept" &&
    [ -f "$root/fresh/bin/my-notes.txt" ] && grep -q '^# my profile$' "$profile" && ! grep -qF "$root/fresh/bin" "$profile"; then
    ok "uninstall keeps a created directory that still has user files"
fi
dir="$root/empty/bin"
install "$dir" v1 ARN_SKIP_PATH_UPDATE=1
uninstall "$dir"
if expect "uninstall of a created directory" 0 "and $dir removed" && [ ! -e "$dir" ]; then
    ok "uninstall removes the directory it created once empty"
fi
uninstall "$root/never/bin"
if expect "uninstall of a missing directory" 0 "nothing to remove" && [ ! -e "$root/never" ]; then
    ok "uninstall of a directory that was never installed to"
fi

# A profile line the user changed is not touched.
dir="$root/edited/bin"
install "$dir" v1
sed "s|^export PATH=\"$dir:|export PATH=\"$dir:/extra:|" "$profile" > "$root/profile.edit" && cat "$root/profile.edit" > "$profile"
before_profile="$(hash_of "$profile")"
uninstall "$dir"
if expect "uninstall with an edited profile" 0 "was uninstalled" && [ "$(hash_of "$profile")" = "$before_profile" ]; then
    ok "uninstall leaves profile lines the user changed"
fi

# Installations made before manifests existed are not taken over silently.
dir="$root/legacy/bin"
mkdir -p "$dir"
printf '#!/bin/sh\necho "arn 0.8.0"\n' > "$dir/arn"
chmod 755 "$dir/arn"
before="$(snapshot "$dir")"
install "$dir" v2
if expect "legacy update" nonzero "ARN_ADOPT_EXISTING=1" && [ "$(snapshot "$dir")" = "$before" ]; then
    ok "legacy installation without a manifest is not replaced"
fi
uninstall "$dir"
if expect "legacy uninstall" nonzero "has arn but no .arn-install-manifest" && [ "$(snapshot "$dir")" = "$before" ]; then
    ok "legacy installation without a manifest is not uninstalled"
fi
install "$dir" v2 ARN_ADOPT_EXISTING=1
if expect "legacy adopt" 0 "Adopting the existing $dir/arn" && [ "$(version_of "$dir")" = "arn 2.0.0 (bbbbbbbbbbbb)" ] &&
    grep -qx 'created_dir 0' "$dir/.arn-install-manifest"; then
    ok "ARN_ADOPT_EXISTING=1 takes over a legacy installation"
fi
uninstall "$dir"
if expect "uninstall after adopting" 0 "was uninstalled" && [ ! -e "$dir/arn" ] && [ -d "$dir" ]; then
    ok "an adopted installation uninstalls"
fi
# A manifest that does not list arn does not make an existing arn ARN's.
dir="$root/collision/bin"
install "$dir" v1 ARN_SKIP_PATH_UPDATE=1
grep -v '^file arn$' "$dir/.arn-install-manifest" > "$root/m" && cat "$root/m" > "$dir/.arn-install-manifest"
before="$(snapshot "$dir")"
install "$dir" v2 ARN_SKIP_PATH_UPDATE=1
if expect "unrecorded arn" nonzero "which no .arn-install-manifest records" && [ "$(snapshot "$dir")" = "$before" ]; then
    ok "an arn the manifest does not list is not replaced"
fi

# Malformed manifests: both update and uninstall refuse and change nothing.
# The outside file is what a traversal would reach.
printf 'outside\n' > "$root/outside"
outside_hash="$(hash_of "$root/outside")"
dir="$root/bad/bin"
install "$dir" v1 ARN_SKIP_PATH_UPDATE=1
good_manifest="$(cat "$dir/.arn-install-manifest")"
check_refused() { # check_refused <name> <fragment>
    local before
    before="$(snapshot "$dir")"
    uninstall "$dir"
    if ! expect "$1 (uninstall)" nonzero "$2"; then :
    elif [ "$(snapshot "$dir")" != "$before" ]; then fail "$1: uninstall changed the installation"
    else ok "$1: uninstall refused"; fi
    install "$dir" v2 ARN_SKIP_PATH_UPDATE=1
    if ! expect "$1 (update)" nonzero "$2"; then :
    elif [ "$(snapshot "$dir")" != "$before" ]; then fail "$1: update changed the installation"
    else ok "$1: update refused"; fi
    if [ "$(hash_of "$root/outside")" != "$outside_hash" ]; then fail "$1: a file outside the directory changed"; fi
}
set_manifest() { rm -f "$dir/.arn-install-manifest"; printf '%s\n' "$1" > "$dir/.arn-install-manifest"; }
set_manifest "$(printf '%s\n' "$good_manifest" | sed 's/^arn-install-manifest 1$/arn-install-manifest 2/')"
check_refused "unsupported manifest format" "unsupported format"
set_manifest "$good_manifest
file ../outside"
check_refused "manifest with a parent-directory traversal" "malformed line"
set_manifest "$good_manifest
file $root/outside"
check_refused "manifest with an absolute path" "malformed line"
set_manifest "$good_manifest
file sub/arn"
check_refused "manifest with a nested path" "malformed line"
set_manifest "$good_manifest
run rm -rf /"
check_refused "manifest with an unknown key" "malformed line"
set_manifest "$good_manifest
file arn"
check_refused "manifest listing a file twice" "lists a file more than once"
set_manifest "$(printf '%s\n' "$good_manifest" | sed "s|^install_dir .*|install_dir $root|")"
check_refused "manifest of another directory" "belongs to another directory"
set_manifest "$(printf '%s\n' "$good_manifest" | grep -v '^version ')"
check_refused "manifest without a version" "exactly one 'version' line"
set_manifest "$(printf '%s\n' "$good_manifest" | awk '{ printf "%s\r\n", $0 }')"
check_refused "manifest with CRLF line endings" "malformed line"
set_manifest ""
check_refused "empty manifest" "unsupported format"
rm -f "$dir/.arn-install-manifest"
printf '%s\n' "$good_manifest" > "$root/linked-manifest"
ln -s "$root/linked-manifest" "$dir/.arn-install-manifest"
check_refused "manifest that is a symbolic link" "is not a regular file"
rm -f "$dir/.arn-install-manifest"
set_manifest "$good_manifest"

# Symbolic links in place of owned files are never followed or replaced.
rm -f "$dir/arn"
ln -s "$root/outside" "$dir/arn"
check_refused "arn that is a symbolic link" "regular file"
rm -f "$dir/arn"
set_manifest "$good_manifest
file notes"
ln -s "$root/outside" "$dir/notes"
before="$(snapshot "$dir")"
uninstall "$dir"
if expect "owned name that is a symbolic link" nonzero "no longer a regular file" && [ "$(snapshot "$dir")" = "$before" ] &&
    [ "$(hash_of "$root/outside")" = "$outside_hash" ]; then
    ok "uninstall refuses an owned name that became a symbolic link"
fi
rm -f "$dir/notes"
set_manifest "$good_manifest"

# Uninstall that cannot remove a file reports it and keeps it recorded.
if [ "$(id -u)" != 0 ]; then
    install "$dir" v1 ARN_SKIP_PATH_UPDATE=1
    chmod 555 "$dir"
    uninstall "$dir"
    chmod 755 "$dir"
    if expect "uninstall of a read-only directory" nonzero "Uninstall incomplete" && [ -f "$dir/arn" ] &&
        grep -qx 'file arn' "$dir/.arn-install-manifest"; then
        ok "uninstall that cannot remove arn does not claim success"
    fi
else
    echo "skip: read-only uninstall (running as root)"
fi

# A stale work directory of a killed installer is cleaned up.
mkdir -p "$dir/.arn-install.stale1"
printf 'x\n' > "$dir/.arn-install.stale1/arn"
install "$dir" v2 ARN_SKIP_PATH_UPDATE=1
if expect "stale work directory" 0 "installed" && no_work_dirs "$dir"; then
    ok "stale work directories are removed"
fi

leftovers="$(ls -A "$TMPDIR")"
if [ -n "$leftovers" ]; then fail "temporary files left behind: $leftovers"; fi

echo "$cases cases, $failures failure(s)"
[ "$failures" -eq 0 ]
