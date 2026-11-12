#!/usr/bin/env bash
# Tests crash recovery, signal handling and profile safety of
# scripts/install.sh.
#   install_unix_recovery_test.sh <install.sh>
# Crashes and signals are injected deterministically: with
# ARN_INSTALL_TEST_FAULT=signal:<NAME>:<step> the installer sends itself that
# signal at exactly that step (KILL stands in for a crash or power loss: no
# trap runs). Everything happens in a temporary directory with its own HOME
# and shell profile; the packages hold shell scripts standing in for arn.
# Set ARN_TEST_SMALL_FS to an empty directory on a small, otherwise unused
# file system (for example a 1 MB tmpfs) to also run the disk-full cases.
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

root="$(mktemp -d "${TMPDIR:-/tmp}/arn-recovery-test.XXXXXX")"
trap 'chmod -R u+w "$root" 2>/dev/null; rm -rf "$root"' EXIT
mkdir -p "$root/tmp" "$root/home" "$root/pkg"
export TMPDIR="$root/tmp" HOME="$root/home" SHELL=/bin/sh
unset ARN_VERSION ARN_INSTALL_ARCHIVE ARN_INSTALL_CHECKSUMS ARN_REPOSITORY ARN_SKIP_PATH_UPDATE \
    ARN_PROFILE ARN_UNINSTALL ARN_ADOPT_EXISTING ARN_INSTALL_TEST_FAULT ARN_INSTALL_DIR || true
profile="$root/home/.profile"
failures=0
cases=0

hash_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi
}

# make_package <name> <arn version line> [padding bytes]
make_package() {
    local dir="$root/pkg/$1"
    mkdir -p "$dir/src/$package"
    printf '#!/bin/sh\necho "%s"\n' "$2" > "$dir/src/$package/arn"
    if [ -n "${3:-}" ]; then
        { printf '# '; head -c "$3" /dev/zero | tr '\0' 'x'; printf '\n'; } >> "$dir/src/$package/arn"
    fi
    chmod 755 "$dir/src/$package/arn"
    printf 'MIT\n' > "$dir/src/$package/LICENSE"
    (cd "$dir/src" && tar -czf "$dir/$archive_name" "$package")
    printf '%s  %s\n' "$(hash_of "$dir/$archive_name")" "$archive_name" > "$dir/SHA256SUMS"
}
make_package v1 "arn 1.0.0 (aaaaaaaaaaaa)"
make_package v2 "arn 2.0.0 (bbbbbbbbbbbb)"
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
# probe <dir>: a run whose download fails its checksum. It still recovers an
# interrupted installation first, then changes nothing, so it shows exactly
# what recovery left.
probe() { install "$1" badsum; }

# snapshot <dir>: every entry under the directory with its type and content
# hash, plus the profile's content.
snapshot() {
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

check() { # check <name> <condition result 0|1> <detail>
    cases=$((cases + 1))
    if [ "$2" = 0 ]; then echo "ok: $1"; else failures=$((failures + 1)); echo "FAIL: $1: $3"; fi
}
has() { printf '%s' "$output" | grep -qF -- "$1"; }
no_leftovers() { ! ls -A "$1" 2>/dev/null | grep -Eq '^\.arn-install(\.|-transaction)'; }
version_of() { "$1/arn" 2>/dev/null; }

# 1. A crash at each step of an update; the next run restores the previous
#    installation exactly, and a further run finds nothing to do.
for point in after-journal after-backup after-activate after-profile after-manifest; do
    dir="$root/kill-$point/bin"
    printf '# my profile\n' > "$profile"
    if [ "$point" = after-profile ]; then
        install "$dir" v1 ARN_SKIP_PATH_UPDATE=1   # so the update is the run that adds the PATH lines
    else
        install "$dir" v1
    fi
    printf 'notes\n' > "$dir/my-notes.txt"
    before="$(snapshot "$dir")"
    install "$dir" v2 "ARN_INSTALL_TEST_FAULT=signal:KILL:$point"
    killed=$status
    journal_left=1
    [ -f "$dir/.arn-install-transaction" ] && journal_left=0
    probe "$dir"
    result=1
    if has "interrupted update of ARN Code was found and undone" && has "but SHA256SUMS lists" &&
        [ "$(snapshot "$dir")" = "$before" ] && no_leftovers "$dir"; then result=0; fi
    check "crash at $point: the next run restores the previous installation" \
        "$([ "$killed" = 137 ] && [ "$journal_left" = 0 ] && [ "$result" = 0 ] && echo 0 || echo 1)" "killed=$killed journal=$journal_left: $output"
    probe "$dir"
    check "crash at $point: running again changes nothing" \
        "$(! has interrupted && [ "$(snapshot "$dir")" = "$before" ] && no_leftovers "$dir" && echo 0 || echo 1)" "$output"
done

# 2. A crash after the commit: the next run keeps the new version and cleans up.
dir="$root/kill-after-commit/bin"
install "$dir" v1
install "$dir" v2 ARN_INSTALL_TEST_FAULT=signal:KILL:after-commit
killed=$status
probe "$dir"
check "crash after the commit: the next run finishes it" \
    "$([ "$killed" = 137 ] && has "Finished cleaning up" && [ "$(version_of "$dir")" = "arn 2.0.0 (bbbbbbbbbbbb)" ] &&
        grep -qx 'version 2.0.0' "$dir/.arn-install-manifest" && no_leftovers "$dir" && echo 0 || echo 1)" "$output"

# 3. A crash during a fresh installation: undone, directory and profile lines included.
for point in after-activate after-profile; do
    dir="$root/kill-fresh-$point/bin"
    printf '# my profile\n' > "$profile"
    before_profile="$(hash_of "$profile")"
    install "$dir" v1 "ARN_INSTALL_TEST_FAULT=signal:KILL:$point"
    killed=$status
    probe "$dir"
    check "crash at $point of a fresh installation is undone" \
        "$([ "$killed" = 137 ] && has "interrupted installation of ARN Code was found and undone" && ! has "previous" &&
            [ ! -e "$dir" ] && [ "$(hash_of "$profile")" = "$before_profile" ] && echo 0 || echo 1)" "killed=$killed: $output"
done

# 4. Recovery itself crashes, twice; the third run finishes it.
dir="$root/kill-recovery/bin"
printf '# my profile\n' > "$profile"
install "$dir" v1
before="$(snapshot "$dir")"
install "$dir" v2 ARN_INSTALL_TEST_FAULT=signal:KILL:after-activate
probe_with_kill() { output="$(env ARN_INSTALL_DIR="$dir" ARN_PROFILE="$profile" ARN_INSTALL_ARCHIVE="$root/pkg/badsum/$archive_name" \
    ARN_INSTALL_CHECKSUMS="$root/pkg/badsum/SHA256SUMS" ARN_INSTALL_TEST_FAULT=signal:KILL:recovery-restore sh "$installer" 2>&1)"; status=$?; }
probe_with_kill
first=$status
probe_with_kill
second=$status
probe "$dir"
third_ok=1
has "was found and undone" && [ "$(snapshot "$dir")" = "$before" ] && no_leftovers "$dir" && third_ok=0
probe "$dir"
check "recovery interrupted twice resumes and completes" \
    "$([ "$first" = 137 ] && [ "$second" = 137 ] && [ "$third_ok" = 0 ] && ! has interrupted && [ "$(snapshot "$dir")" = "$before" ] && echo 0 || echo 1)" \
    "$first $second $third_ok: $output"

# 5. Handled signals roll back in the process, with the signal's exit status;
#    a second signal during the rollback is ignored.
for spec in PIPE:after-activate:141 QUIT:after-backup:131 INT:after-activate:130 HUP:after-manifest:129 \
            "TERM:after-activate,signal:TERM:rollback:143"; do
    signal="${spec%%:*}"
    rest="${spec#*:}"
    expected="${rest##*:}"
    point="${rest%:*}"
    dir="$root/signal-$signal/bin"
    printf '# my profile\n' > "$profile"
    install "$dir" v1
    before="$(snapshot "$dir")"
    install "$dir" v2 "ARN_INSTALL_TEST_FAULT=signal:$signal:$point"
    check "SIG$signal at ${point%%,*} rolls back with status $expected" \
        "$([ "$status" = "$expected" ] && has "The previous installation was kept" && [ "$(snapshot "$dir")" = "$before" ] && no_leftovers "$dir" && echo 0 || echo 1)" \
        "status $status: $output"
done

# 6. A script cut off mid-way (as by a dropped download into "| sh") rolls
#    back and does not exit with status 0.
dir="$root/truncated/bin"
install "$dir" v1
before="$(snapshot "$dir")"
output="$(sed '/^test_fault after-activate$/q' "$installer" | env ARN_INSTALL_DIR="$dir" ARN_PROFILE="$profile" \
    ARN_INSTALL_ARCHIVE="$root/pkg/v2/$archive_name" ARN_INSTALL_CHECKSUMS="$root/pkg/v2/SHA256SUMS" sh 2>&1)"
status=$?
check "a truncated script rolls back and fails" \
    "$([ "$status" = 1 ] && has "The previous installation was kept" && [ "$(snapshot "$dir")" = "$before" ] && echo 0 || echo 1)" "status $status: $output"

# 7. A profile that cannot be updated is a warning, not a failed installation.
dir="$root/profile-missing/bin"
output="$(env ARN_INSTALL_DIR="$dir" ARN_PROFILE="$root/no-such-dir/.profile" ARN_INSTALL_ARCHIVE="$root/pkg/v1/$archive_name" \
    ARN_INSTALL_CHECKSUMS="$root/pkg/v1/SHA256SUMS" sh "$installer" 2>&1)"
status=$?
check "an unwritable profile only warns" \
    "$([ "$status" = 0 ] && has "Warning: could not add $dir to $root/no-such-dir/.profile" && ! has "Restoring" && ! has "Added $dir" &&
        [ "$(version_of "$dir")" = "arn 1.0.0 (aaaaaaaaaaaa)" ] && grep -qx 'profile -' "$dir/.arn-install-manifest" && echo 0 || echo 1)" "$output"

# 8. The profile is replaced atomically through its symbolic link, keeping
#    the link and the file's permissions.
real="$root/home/real-profile"
rm -f "$profile"
printf '# my profile\nexport EDITOR=vi\n' > "$real"
chmod 640 "$real"
ln -s "$real" "$profile"
original="$(hash_of "$real")"
mode_of() { ls -l "$1" | cut -c1-10; }
dir="$root/profile-mode/bin"
install "$dir" v1
added=1
[ "$status" = 0 ] && [ -L "$profile" ] && [ "$(mode_of "$real")" = "-rw-r-----" ] &&
    grep -qxF "# Added by ARN Code installer: $dir" "$real" && no_leftovers "$dir" && added=0
uninstall "$dir"
check "the profile keeps its link and permissions through install and uninstall" \
    "$([ "$added" = 0 ] && [ "$status" = 0 ] && [ -L "$profile" ] && [ "$(mode_of "$real")" = "-rw-r-----" ] &&
        [ "$(hash_of "$real")" = "$original" ] && ! ls -A "$root/home" | grep -q '^\.arn-profile' && echo 0 || echo 1)" "$output"
rm -f "$profile" "$real"

# 9. A profile changed by someone else while it was being updated is left alone.
#    The installer stops itself (SIGSTOP) at that step; the test edits the
#    profile and lets it continue.
wait_stopped() {
    local n=0 state
    while [ $n -lt 400 ]; do
        if [ -r "/proc/$1/stat" ]; then state="$(awk '{ print $3 }' "/proc/$1/stat")"; else state="$(ps -o stat= -p "$1" 2>/dev/null)"; fi
        case "$state" in T*) return 0 ;; esac
        sleep 0.05
        n=$((n + 1))
    done
    return 1
}
printf '# my profile\n' > "$profile"
dir="$root/profile-race/bin"
env ARN_INSTALL_DIR="$dir" ARN_PROFILE="$profile" ARN_INSTALL_ARCHIVE="$root/pkg/v1/$archive_name" \
    ARN_INSTALL_CHECKSUMS="$root/pkg/v1/SHA256SUMS" ARN_INSTALL_TEST_FAULT=signal:STOP:profile-before-rename \
    sh "$installer" > "$root/race.log" 2>&1 &
pid=$!
if wait_stopped "$pid"; then
    printf 'export EDITOR=vi\n' >> "$profile"
    edited="$(hash_of "$profile")"
    kill -s CONT "$pid"
    wait "$pid"
    status=$?
    output="$(cat "$root/race.log")"
    check "a profile changed during the update is not overwritten" \
        "$([ "$status" = 0 ] && has "changed while it was being updated" && [ "$(hash_of "$profile")" = "$edited" ] &&
            grep -qx 'profile -' "$dir/.arn-install-manifest" && echo 0 || echo 1)" "status $status: $output"
else
    kill -s KILL "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    check "a profile changed during the update is not overwritten" 1 "the installer did not stop at the injected point"
fi

# 10. Uninstall interrupted: nothing is lost, the retry finishes.
for point in uninstall-after-files uninstall-after-profile; do
    printf '# my profile\n' > "$profile"
    before_profile="$(hash_of "$profile")"
    dir="$root/uninstall-$point/bin"
    install "$dir" v1
    printf 'notes\n' > "$dir/my-notes.txt"
    uninstall "$dir" "ARN_INSTALL_TEST_FAULT=signal:KILL:$point"
    killed=$status
    kept=1
    [ -f "$dir/.arn-install-manifest" ] && kept=0
    uninstall "$dir"
    check "uninstall killed at $point keeps its manifest; the retry finishes" \
        "$([ "$killed" = 137 ] && [ "$kept" = 0 ] && [ "$status" = 0 ] && [ ! -e "$dir/arn" ] && [ ! -e "$dir/.arn-install-manifest" ] &&
            [ -f "$dir/my-notes.txt" ] && [ "$(hash_of "$profile")" = "$before_profile" ] && echo 0 || echo 1)" "killed=$killed kept=$kept: $output"
done
printf '# my profile\n' > "$profile"
dir="$root/uninstall-term/bin"
install "$dir" v1
uninstall "$dir" ARN_INSTALL_TEST_FAULT=signal:TERM:uninstall-after-files
term_status=$status
leftover=1
[ -e "$dir/.arn-install-manifest.new" ] || leftover=0
uninstall "$dir"
check "SIGTERM during uninstall exits 143 without temporary files; the retry finishes" \
    "$([ "$term_status" = 143 ] && [ "$leftover" = 0 ] && [ "$status" = 0 ] && ! grep -q 'Added by ARN' "$profile" && echo 0 || echo 1)" "$term_status: $output"

# 11. A transaction record that cannot be trusted stops everything and keeps
#     the copies of the previous version.
printf '# my profile\n' > "$profile"
dir="$root/bad-journal/bin"
install "$dir" v1
before="$(snapshot "$dir")"
install "$dir" v2 ARN_INSTALL_TEST_FAULT=signal:KILL:after-activate
journal="$dir/.arn-install-transaction"
cp "$journal" "$root/good-journal"
work="$(ls -d "$dir"/.arn-install.* | head -n 1)"
corrupt() { # corrupt <name> <command producing the new record from the good one>
    rm -f "$journal"
    eval "$2" < "$root/good-journal" > "$journal"
    local state_before
    state_before="$(snapshot "$dir")"
    install "$dir" v2
    local install_status=$status install_output="$output"
    uninstall "$dir"
    check "record with $1: install and uninstall refuse and keep the previous arn" \
        "$([ "$install_status" != 0 ] && [ "$status" != 0 ] && printf '%s' "$install_output" | grep -qF "record cannot be used" &&
            [ -f "$work/arn.previous" ] && [ "$(snapshot "$dir")" = "$state_before" ] && echo 0 || echo 1)" "$install_output / $output"
}
corrupt "an unknown state" "sed 's/^state .*/state bogus/'"
corrupt "a work directory outside" "sed 's|^work_dir .*|work_dir ../escape|'"
corrupt "another directory" "sed 's|^install_dir .*|install_dir /elsewhere|'"
corrupt "an unknown key" "cat; echo 'run rm -rf /'"
corrupt "a duplicate key" "cat; echo 'fresh 1'"
corrupt "CRLF line endings" "awk '{ printf \"%s\\r\\n\", \$0 }'"
corrupt "a truncated record" "head -n 3"
rm -f "$journal"
ln -s "$root/good-journal" "$journal"
install "$dir" v2
check "a record that is a symbolic link is refused" "$([ "$status" != 0 ] && has "record cannot be used" && echo 0 || echo 1)" "$output"
rm -f "$journal"
mv "$work" "$work.real"
ln -s "$work.real" "$work"
cp "$root/good-journal" "$journal"
install "$dir" v2
check "a work directory that is a symbolic link is refused" "$([ "$status" != 0 ] && has "is not a directory" && [ -f "$work.real/arn.previous" ] && echo 0 || echo 1)" "$output"
rm -f "$work"
mv "$work.real" "$work"
probe "$dir"
check "after the record is repaired, recovery restores the previous installation" \
    "$(has "was found and undone" && [ "$(snapshot "$dir")" = "$before" ] && no_leftovers "$dir" && echo 0 || echo 1)" "$output"

# 11b. A record in "activating" whose work directory was removed by something
#      else: nothing in it proves whose arn, manifest or profile lines are in
#      place, so recovery refuses, changes nothing, keeps the record, and
#      keeps refusing.
work_missing() { # work_missing <name> <dir>: removes the transaction's work directory
    rm -rf "$2"/.arn-install.*
    [ -f "$2/.arn-install-transaction" ] && ! ls -d "$2"/.arn-install.* >/dev/null 2>&1
}
check_untouched() { # check_untouched <name> <dir> <snapshot before>
    local name="$1" dir="$2" before_state="$3" first second
    probe "$dir"
    first="$status: $output"
    local first_ok=1
    [ "$status" != 0 ] && has "work directory" && has "is missing" && [ -f "$dir/.arn-install-transaction" ] &&
        [ "$(snapshot "$dir")" = "$before_state" ] && first_ok=0
    probe "$dir"
    check "$name" "$([ "$first_ok" = 0 ] && [ "$status" != 0 ] && has "is missing" && [ "$(snapshot "$dir")" = "$before_state" ] && echo 0 || echo 1)" "$first / $output"
}
printf '# my profile\n' > "$profile"
dir="$root/work-missing-fresh/bin"
install "$dir" v1 "ARN_INSTALL_TEST_FAULT=signal:KILL:after-activate"
killed=$status
work_missing fresh "$dir" || echo "note: setup of the fresh case is incomplete"
printf '#!/bin/sh\necho "the user'"'"'s own arn"\n' > "$dir/arn"
chmod 755 "$dir/arn"
printf 'the user'"'"'s own notes, not a manifest\n' > "$dir/.arn-install-manifest"
user_arn="$(hash_of "$dir/arn")"
user_manifest="$(hash_of "$dir/.arn-install-manifest")"
before="$(snapshot "$dir")"
check_untouched "fresh install, work directory gone: a foreign arn and manifest stay byte for byte" "$dir" "$before"
check "fresh install, work directory gone: the foreign files are exactly the user's" \
    "$([ "$killed" = 137 ] && [ "$(hash_of "$dir/arn")" = "$user_arn" ] && [ "$(hash_of "$dir/.arn-install-manifest")" = "$user_manifest" ] &&
        [ "$("$dir/arn")" = "the user's own arn" ] && echo 0 || echo 1)" "killed=$killed arn=$("$dir/arn" 2>&1)"

dir="$root/work-missing-installer-arn/bin"
install "$dir" v1 "ARN_INSTALL_TEST_FAULT=signal:KILL:after-activate"
work_missing installer-arn "$dir" || echo "note: setup of the installer-arn case is incomplete"
before="$(snapshot "$dir")"
check_untouched "fresh install, work directory gone: even the installer's own arn is not removed without proof" "$dir" "$before"

printf '# my profile\n' > "$profile"
dir="$root/work-missing-profile/bin"
install "$dir" v1 "ARN_INSTALL_TEST_FAULT=signal:KILL:after-profile"
work_missing profile "$dir" || echo "note: setup of the profile case is incomplete"
before="$(snapshot "$dir")"
check_untouched "fresh install killed after the profile change, work directory gone: arn and profile lines stay" "$dir" "$before"
check "fresh install killed after the profile change: the profile still has its lines" \
    "$(grep -qxF "# Added by ARN Code installer: $dir" "$profile" && echo 0 || echo 1)" "$(cat "$profile")"

printf '# my profile\n' > "$profile"
dir="$root/work-missing-upgrade/bin"
install "$dir" v1
install "$dir" v2 "ARN_INSTALL_TEST_FAULT=signal:KILL:after-activate"
work_missing upgrade "$dir" || echo "note: setup of the upgrade case is incomplete"
before="$(snapshot "$dir")"
check_untouched "update, work directory gone: arn and the manifest stay as they are" "$dir" "$before"
check "update, work directory gone: the active arn and the v1 manifest are unchanged" \
    "$([ "$(version_of "$dir")" = "arn 2.0.0 (bbbbbbbbbbbb)" ] && grep -qx 'version 1.0.0' "$dir/.arn-install-manifest" && echo 0 || echo 1)" "$(version_of "$dir")"

# 11c. Where there was no manifest before, a manifest that appears during an
#      interrupted installation is removed only when it is byte for byte the
#      one the installer wrote. A user's file (new, or written over the
#      installer's) stays, the record stays, and nothing else changes; once
#      the user moves it away, recovery completes as usual.
manifest_refused() { # manifest_refused <name> <dir> <snapshot before> <user manifest hash>
    local name="$1" dir="$2" before_state="$3" user_hash="$4" first_ok=1 first
    probe "$dir"
    first="$status: $output"
    [ "$status" != 0 ] && has "is not the manifest the installer wrote" && [ -f "$dir/.arn-install-transaction" ] &&
        [ "$(snapshot "$dir")" = "$before_state" ] && [ "$(hash_of "$dir/.arn-install-manifest")" = "$user_hash" ] && first_ok=0
    probe "$dir"
    check "$name" "$([ "$first_ok" = 0 ] && [ "$status" != 0 ] && has "is not the manifest the installer wrote" &&
        [ "$(snapshot "$dir")" = "$before_state" ] && [ "$(hash_of "$dir/.arn-install-manifest")" = "$user_hash" ] && echo 0 || echo 1)" "$first / $output"
}
user_manifest_text='arn-install-manifest 1
version 9.9.9
commit -
install_dir /somewhere/else
profile -
created_dir 0
file arn
'
for point in after-activate after-profile; do
    printf '# my profile\n' > "$profile"
    before_profile="$(hash_of "$profile")"
    dir="$root/user-manifest-$point/bin"
    install "$dir" v1 "ARN_INSTALL_TEST_FAULT=signal:KILL:$point"
    killed=$status
    printf '%s' "$user_manifest_text" > "$dir/.arn-install-manifest"
    user_hash="$(hash_of "$dir/.arn-install-manifest")"
    before="$(snapshot "$dir")"
    manifest_refused "fresh install killed $point, then a user's .arn-install-manifest: kept byte for byte, recovery refused twice" "$dir" "$before" "$user_hash"
    if [ "$point" = after-profile ]; then
        check "fresh install killed after-profile with a user's manifest: the profile lines stay while refused" \
            "$(grep -qxF "# Added by ARN Code installer: $dir" "$profile" && echo 0 || echo 1)" "$(cat "$profile")"
    fi
    mkdir -p "$root/held"
    mv "$dir/.arn-install-manifest" "$root/held/manifest-$point"
    probe "$dir"
    check "fresh install killed $point: once the user's manifest is moved away, recovery completes" \
        "$([ "$killed" = 137 ] && has "interrupted installation of ARN Code was found and undone" && [ ! -e "$dir" ] &&
            [ "$(hash_of "$root/held/manifest-$point")" = "$user_hash" ] && [ "$(hash_of "$profile")" = "$before_profile" ] && echo 0 || echo 1)" "killed=$killed: $output"
done

# The installer's own manifest in place: the ordinary rollback removes it.
printf '# my profile\n' > "$profile"
before_profile="$(hash_of "$profile")"
dir="$root/own-manifest/bin"
install "$dir" v1 "ARN_INSTALL_TEST_FAULT=signal:KILL:after-manifest"
killed=$status
written=1
work="$(ls -d "$dir"/.arn-install.* 2>/dev/null | head -n 1)"
[ -f "$work/manifest.written" ] && cmp -s "$work/manifest.written" "$dir/.arn-install-manifest" && written=0
probe "$dir"
check "fresh install killed after-manifest: the installer's own manifest is recognised and the rollback completes" \
    "$([ "$killed" = 137 ] && [ "$written" = 0 ] && has "interrupted installation of ARN Code was found and undone" && [ ! -e "$dir" ] &&
        [ "$(hash_of "$profile")" = "$before_profile" ] && echo 0 || echo 1)" "killed=$killed written=$written: $output"

# The installer's manifest written over by the user after the interruption.
dir="$root/overwritten-manifest/bin"
install "$dir" v1 "ARN_INSTALL_TEST_FAULT=signal:KILL:after-manifest"
printf '%s' "$user_manifest_text" > "$dir/.arn-install-manifest"
user_hash="$(hash_of "$dir/.arn-install-manifest")"
before="$(snapshot "$dir")"
manifest_refused "fresh install killed after-manifest, manifest then rewritten by the user: kept byte for byte, recovery refused twice" "$dir" "$before" "$user_hash"

# An adopted earlier arn (no manifest before, but not a fresh installation).
dir="$root/adopted-manifest/bin"
mkdir -p "$dir"
printf '#!/bin/sh\necho "arn 0.8.0"\n' > "$dir/arn"
chmod 755 "$dir/arn"
legacy_hash="$(hash_of "$dir/arn")"
install "$dir" v2 ARN_ADOPT_EXISTING=1 ARN_SKIP_PATH_UPDATE=1 "ARN_INSTALL_TEST_FAULT=signal:KILL:after-activate"
printf '%s' "$user_manifest_text" > "$dir/.arn-install-manifest"
user_hash="$(hash_of "$dir/.arn-install-manifest")"
before="$(snapshot "$dir")"
manifest_refused "adopted arn, interrupted, then a user's .arn-install-manifest: kept byte for byte, recovery refused twice" "$dir" "$before" "$user_hash"
mv "$dir/.arn-install-manifest" "$root/held/manifest-adopted"
probe "$dir"
check "adopted arn: once the user's manifest is moved away, the earlier arn is back" \
    "$(has "was found and undone" && [ "$(hash_of "$dir/arn")" = "$legacy_hash" ] && [ ! -e "$dir/.arn-install-manifest" ] &&
        [ "$(hash_of "$root/held/manifest-adopted")" = "$user_hash" ] && no_leftovers "$dir" && echo 0 || echo 1)" "$output"

# 12. One directory, one spelling; values with line breaks are refused.
printf '# my profile\n' > "$profile"
mkdir -p "$root/norm"
install "$root/norm/bin/" v1
manifest_dir="$(sed -n 's/^install_dir //p' "$root/norm/bin/.arn-install-manifest" 2>/dev/null)"
uninstall "$root/norm/x/../bin/."
check "another spelling of the directory finds the same installation" \
    "$([ "$manifest_dir" = "$root/norm/bin" ] && [ "$status" = 0 ] && [ ! -e "$root/norm/bin" ] && echo 0 || echo 1)" "$manifest_dir: $output"
install "$root/norm2//bin" v1
uninstall "$root/norm2/bin"
check "repeated slashes are the same directory" "$([ "$status" = 0 ] && has "was uninstalled" && echo 0 || echo 1)" "$output"
install "$root/nl/bin" v1 "ARN_VERSION=1.2.3
x"
check "a version with a line break is refused" "$([ "$status" != 0 ] && has "ARN_VERSION must be" && [ ! -e "$root/nl" ] && echo 0 || echo 1)" "$output"
install "$root/nl/bin" v1 "ARN_REPOSITORY=owner/name
x"
check "a repository with a line break is refused" "$([ "$status" != 0 ] && has "ARN_REPOSITORY must have" && echo 0 || echo 1)" "$output"

# 13. Disk full (only with ARN_TEST_SMALL_FS).
if [ -n "${ARN_TEST_SMALL_FS:-}" ] && [ -d "$ARN_TEST_SMALL_FS" ] && [ -w "$ARN_TEST_SMALL_FS" ]; then
    make_package v1big "arn 1.0.0 (aaaaaaaaaaaa)" 600000
    make_package v2big "arn 2.0.0 (bbbbbbbbbbbb)" 600000
    printf '# my profile\n' > "$profile"
    dir="$ARN_TEST_SMALL_FS/bin"
    install "$dir" v1big ARN_SKIP_PATH_UPDATE=1
    first=$status
    before="$(snapshot "$dir")"
    install "$dir" v2big ARN_SKIP_PATH_UPDATE=1
    check "a full disk while staging leaves the installation intact" \
        "$([ "$first" = 0 ] && [ "$status" != 0 ] && has "The previous installation was kept" && [ "$(snapshot "$dir")" = "$before" ] &&
            no_leftovers "$dir" && echo 0 || echo 1)" "$first: $output"
    uninstall "$dir"
    full_profile="$ARN_TEST_SMALL_FS/profile"
    printf '# my profile\n' > "$full_profile"
    dd if=/dev/zero of="$ARN_TEST_SMALL_FS/filler" bs=4096 2>/dev/null || true
    full_hash="$(hash_of "$full_profile")"
    dir="$root/full-profile/bin"
    output="$(env ARN_INSTALL_DIR="$dir" ARN_PROFILE="$full_profile" ARN_INSTALL_ARCHIVE="$root/pkg/v1/$archive_name" \
        ARN_INSTALL_CHECKSUMS="$root/pkg/v1/SHA256SUMS" sh "$installer" 2>&1)"
    status=$?
    check "a profile on a full disk only warns and stays unchanged" \
        "$([ "$status" = 0 ] && has "Warning: could not add" && [ "$(hash_of "$full_profile")" = "$full_hash" ] &&
            ! ls -A "$ARN_TEST_SMALL_FS" | grep -q '^\.arn-profile' && echo 0 || echo 1)" "$output"
    rm -f "$ARN_TEST_SMALL_FS/filler" "$full_profile"
else
    echo "skip: disk-full cases (set ARN_TEST_SMALL_FS to a directory on a small file system)"
fi

# A killed run cannot remove its download directory (mktemp -d in TMPDIR);
# every other temporary file must be gone.
leftovers="$(ls -A "$TMPDIR" | grep -v '^tmp\.[A-Za-z0-9]*$')"
check "no temporary files are left except the download directories of killed runs" "$([ -z "$leftovers" ] && echo 0 || echo 1)" "$leftovers"

echo "$cases cases, $failures failure(s)"
[ "$failures" -eq 0 ]
