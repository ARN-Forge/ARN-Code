#!/usr/bin/env bash
# Tests scripts/package-unix.sh with temporary fixtures.
#   package_unix_test.sh <package-unix.sh> <python3>
# A shell-script "arn" stands in for the executable, so the test needs no build.
# Malformed archives (duplicates, "..", absolute paths, links, wrong modes) are
# written with Python's tarfile, which works the same on GNU and BSD systems.

set -u
export LC_ALL=C

script="$1"
python="$2"
root="$(mktemp -d "${TMPDIR:-/tmp}/arn-package-test.XXXXXX")"
trap 'rm -rf "$root"' EXIT
export TMPDIR="$root/tmp"
mkdir "$TMPDIR"

name="arn-test-x64"
version="1.2.3"
commit="0123456789abcdef0123456789abcdef01234567"
good_arn=$'#!/bin/sh\necho "arn 1.2.3 (0123456789ab)"\n'
failures=0
cases=0

# craft <out.tar.gz> <spec>...   spec: path|file|<octal mode>|<content>
#                                       path|symlink|<target>
craft() {
    "$python" - "$@" <<'PY'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w:gz", format=tarfile.PAX_FORMAT) as archive:
    for spec in sys.argv[2:]:
        path, kind, rest = spec.split("|", 2)
        info = tarfile.TarInfo(path)
        if kind == "symlink":
            info.type = tarfile.SYMTYPE
            info.linkname = rest
            archive.addfile(info)
        else:
            mode, content = rest.split("|", 1)
            data = content.encode()
            info.mode = int(mode, 8)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
PY
}

# expect <description> <pass|fail> <message fragment or ""> -- <command...>
expect() {
    local description="$1" outcome="$2" fragment="$3" output status
    shift 4
    cases=$((cases + 1))
    output="$("$@" 2>&1)"
    status=$?
    if [ "$outcome" = pass ] && [ $status -ne 0 ]; then
        echo "FAIL: $description: expected success, got exit $status: $output"
        failures=$((failures + 1))
    elif [ "$outcome" = fail ] && [ $status -eq 0 ]; then
        echo "FAIL: $description: expected failure, but it passed: $output"
        failures=$((failures + 1))
    elif [ -n "$fragment" ] && ! printf '%s' "$output" | grep -qF -- "$fragment"; then
        echo "FAIL: $description: output lacks '$fragment': $output"
        failures=$((failures + 1))
    else
        echo "ok: $description"
    fi
}

verify() {
    bash "$script" verify --archive "$1" --name "$name" --version "${2:-$version}" --commit "${3:-$commit}"
}

license="LICENSE|file|644|MIT"
readme="README.md|file|644|# ARN"

# Valid package from a binary, then the same archive verified on its own.
printf '%s' "$good_arn" > "$root/arn"
chmod 755 "$root/arn"
expect "package a valid archive" pass "Verified" -- \
    bash "$script" package --binary "$root/arn" --name "$name" --archive "$root/good.tar.gz" \
    --version "$version" --commit "$commit"
expect "verify the packaged archive" pass "Extracted arn: arn 1.2.3 (0123456789ab)" -- verify "$root/good.tar.gz"
listing="$(tar -tzf "$root/good.tar.gz" | sort | tr '\n' ' ')"
if [ "$listing" != "$name/LICENSE $name/README.md $name/arn " ]; then
    echo "FAIL: packaged archive lists '$listing'"
    failures=$((failures + 1))
fi
expect "upper-case commit is accepted" pass "" -- verify "$root/good.tar.gz" "$version" "0123456789ABCDEF0123"

# Version and commit checks, and a failing executable.
expect "wrong version" fail "expected 'arn 1.2.4 (0123456789ab)'" -- verify "$root/good.tar.gz" 1.2.4
expect "wrong commit" fail "expected 'arn 1.2.3 (ffffffffffff)'" -- \
    verify "$root/good.tar.gz" "$version" ffffffffffffffffffffffffffffffffffffffff
expect "invalid version argument" fail "--version must be X.Y.Z" -- verify "$root/good.tar.gz" 1.2
expect "invalid commit argument" fail "--commit must be 12-40 hexadecimal digits" -- \
    verify "$root/good.tar.gz" "$version" abc123

printf '#!/bin/sh\necho "broken" >&2\nexit 3\n' > "$root/failing-arn"
chmod 755 "$root/failing-arn"
expect "executable that fails" fail "failed with exit code 3" -- \
    bash "$script" package --binary "$root/failing-arn" --name "$name" --archive "$root/failing.tar.gz" \
    --version "$version" --commit "$commit"
if [ -e "$root/failing.tar.gz" ]; then
    echo "FAIL: the archive of a failing executable was left behind"
    failures=$((failures + 1))
fi

# Malformed archives.
craft "$root/missing.tar.gz" "$name/arn|file|755|$good_arn" "$name/$readme"
expect "missing LICENSE" fail "missing: [$name/LICENSE]" -- verify "$root/missing.tar.gz"

craft "$root/extra.tar.gz" "$name/arn|file|755|$good_arn" "$name/$license" "$name/$readme" \
    "$name/notes.txt|file|644|extra"
expect "unexpected file" fail "unexpected: [$name/notes.txt]" -- verify "$root/extra.tar.gz"

craft "$root/apple.tar.gz" "$name/arn|file|755|$good_arn" "$name/$license" "$name/$readme" \
    "$name/._arn|file|644|metadata"
expect "macOS AppleDouble entry" fail "unexpected: [$name/._arn]" -- verify "$root/apple.tar.gz"

craft "$root/duplicate.tar.gz" "$name/arn|file|755|$good_arn" "$name/$license" "$name/$readme" \
    "$name/$license"
expect "duplicate entry" fail "duplicate archive entries: $name/LICENSE" -- verify "$root/duplicate.tar.gz"

craft "$root/traversal.tar.gz" "$name/arn|file|755|$good_arn" "$name/$license" "$name/$readme" \
    "$name/../evil|file|644|evil"
expect "path traversal entry" fail "path traversal in archive: '$name/../evil'" -- verify "$root/traversal.tar.gz"

craft "$root/absolute.tar.gz" "$name/arn|file|755|$good_arn" "$name/$license" "$name/$readme" \
    "/tmp/arn-evil|file|644|evil"
expect "absolute path entry" fail "absolute path in archive" -- verify "$root/absolute.tar.gz"

craft "$root/symlink.tar.gz" "$name/arn|symlink|/bin/sh" "$name/$license" "$name/$readme"
expect "arn is a symbolic link" fail "is not a regular file" -- verify "$root/symlink.tar.gz"

craft "$root/noexec.tar.gz" "$name/arn|file|644|$good_arn" "$name/$license" "$name/$readme"
expect "arn is not executable" fail "is not executable" -- verify "$root/noexec.tar.gz"

craft "$root/wrongdir.tar.gz" "other/arn|file|755|$good_arn" "other/$license" "other/$readme"
expect "wrong top-level directory" fail "missing:" -- verify "$root/wrongdir.tar.gz"

printf 'not a gzip archive' > "$root/corrupt.tar.gz"
expect "corrupt archive" fail "cannot list" -- verify "$root/corrupt.tar.gz"
expect "archive that does not exist" fail "not found" -- verify "$root/absent.tar.gz"

# Nothing may be left in the temporary directory, failures included.
leftovers="$(ls -A "$TMPDIR")"
if [ -n "$leftovers" ]; then
    echo "FAIL: temporary files were left behind: $leftovers"
    failures=$((failures + 1))
fi

echo "$cases cases, $failures failure(s)"
[ "$failures" -eq 0 ]
