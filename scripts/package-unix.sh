#!/usr/bin/env bash
# Packages and verifies the Linux/macOS CLI release archive.
#
#   package-unix.sh package --binary <arn> --name <dir> --archive <file.tar.gz>
#                           --version <X.Y.Z> --commit <sha>
#   package-unix.sh verify  --archive <file.tar.gz> --name <dir>
#                           --version <X.Y.Z> --commit <sha>
#
# The archive holds exactly <dir>/arn, <dir>/LICENSE and <dir>/README.md.
# verify rejects missing, unexpected and duplicate entries, absolute and ".."
# paths, links and other non-regular entries, and a non-executable arn. It
# then extracts the archive into a fresh temporary directory, runs the
# extracted arn --version with a minimal environment, requires exactly
# "arn <version> (<first 12 digits of commit>)", and lists the executable's
# shared libraries (ldd on Linux, otool -L on macOS).
# package builds the archive from the binary, LICENSE and README.md only, and
# deletes it again if verification fails. Temporary files are always removed.
# Written for bash 3.2 (macOS) as well as newer bash, and for GNU and BSD tar.

set -eu
export LC_ALL=C

staging_dir=""
verify_dir=""
archive_to_remove=""

cleanup() {
    if [ -n "$staging_dir" ]; then rm -rf "$staging_dir"; fi
    if [ -n "$verify_dir" ]; then rm -rf "$verify_dir"; fi
    if [ -n "$archive_to_remove" ]; then rm -f "$archive_to_remove"; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

fail() {
    echo "package-unix.sh: error: $*" >&2
    exit 1
}

# Sets made_dir to a new private temporary directory. Not used through $(...),
# so the caller can register the directory for cleanup in this shell.
make_temp_dir() {
    made_dir="$(mktemp -d "${TMPDIR:-/tmp}/arn-package.XXXXXX")" || fail "could not create a temporary directory"
}

usage() {
    sed -n '2,8p' "$0" >&2
    exit 2
}

[ $# -ge 1 ] || usage
mode="$1"
shift
binary=""
name=""
archive=""
version=""
commit=""
while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || usage
    case "$1" in
        --binary) binary="$2" ;;
        --name) name="$2" ;;
        --archive) archive="$2" ;;
        --version) version="$2" ;;
        --commit) commit="$2" ;;
        *) usage ;;
    esac
    shift 2
done

case "$mode" in package|verify) ;; *) usage ;; esac
[ -n "$archive" ] || fail "--archive is required"
case "$name" in
    ""|.|..|*/*) fail "--name must be a plain directory name, got '$name'" ;;
esac
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "--version must be X.Y.Z, got '$version'"
commit="$(printf '%s' "$commit" | tr 'A-F' 'a-f')"
[[ "$commit" =~ ^[0-9a-f]{12,40}$ ]] || fail "--commit must be 12-40 hexadecimal digits, got '$commit'"
expected_version_line="arn $version (${commit:0:12})"

report_dependencies() {
    local exe="$1" output status line
    echo "Shared libraries of the packaged arn:"
    case "$(uname -s)" in
        Darwin)
            if ! command -v otool >/dev/null 2>&1; then
                echo "  otool is not available; dependency inspection skipped."
                return 0
            fi
            set +e
            output="$(otool -L "$exe" 2>&1)"
            status=$?
            set -e
            if [ $status -ne 0 ] || ! printf '%s\n' "$output" | sed -n '2,$p' | grep -q .; then
                echo "  not a Mach-O executable; dependency inspection skipped."
                return 0
            fi
            printf '%s\n' "$output" | sed -n '2,$p' | while IFS= read -r line; do
                line="$(printf '%s' "$line" | sed 's/^[[:space:]]*//')"
                case "$line" in
                    /usr/lib/*|/System/Library/*) echo "  system:     $line" ;;
                    *) echo "  non-system: $line" ;;
                esac
            done
            ;;
        *)
            if ! command -v ldd >/dev/null 2>&1; then
                echo "  ldd is not available; dependency inspection skipped."
                return 0
            fi
            set +e
            output="$(ldd "$exe" 2>&1)"
            status=$?
            set -e
            if [ $status -ne 0 ]; then
                echo "  not a dynamic executable; dependency inspection skipped."
                return 0
            fi
            printf '%s\n' "$output" | sed 's/^[[:space:]]*/  /'
            if printf '%s\n' "$output" | grep -q 'not found'; then
                fail "the packaged arn has unresolved shared libraries"
            fi
            ;;
    esac
}

verify_archive() {
    local work names long extracted entry mode output status
    [ -f "$archive" ] || fail "archive '$archive' not found"
    make_temp_dir
    verify_dir="$made_dir"
    work="$verify_dir"
    names="$work/names"
    long="$work/long"

    # -P keeps leading "/" and ".." in the listing on GNU and BSD tar alike.
    tar -tPzf "$archive" > "$names" 2>"$work/tar-errors" || fail "cannot list '$archive': $(cat "$work/tar-errors")"
    tar -tvPzf "$archive" > "$long" 2>"$work/tar-errors" || fail "cannot list '$archive': $(cat "$work/tar-errors")"

    while IFS= read -r entry; do
        case "$entry" in
            /*) fail "absolute path in archive: '$entry'" ;;
        esac
        case "/$entry/" in
            */../*) fail "path traversal in archive: '$entry'" ;;
        esac
    done < "$names"

    if [ -n "$(sort "$names" | uniq -d)" ]; then
        fail "duplicate archive entries: $(sort "$names" | uniq -d | tr '\n' ' ')"
    fi

    printf '%s\n' "$name/LICENSE" "$name/README.md" "$name/arn" | sort > "$work/expected"
    sort "$names" > "$work/actual"
    if ! cmp -s "$work/expected" "$work/actual"; then
        local missing unexpected
        missing="$(comm -23 "$work/expected" "$work/actual" | tr '\n' ' ')"
        unexpected="$(comm -13 "$work/expected" "$work/actual" | tr '\n' ' ')"
        fail "archive contents differ from the release files; missing: [${missing% }] unexpected: [${unexpected% }]"
    fi

    # The listing and its long form have the same order: pair them by line.
    awk 'NR == FNR { n[FNR] = $0; next } { print substr($0, 1, 10) "\t" n[FNR] }' "$names" "$long" > "$work/modes"
    while IFS="$(printf '\t')" read -r mode entry; do
        case "$mode" in
            -*) ;;
            *) fail "'$entry' is not a regular file (mode $mode)" ;;
        esac
        if [ "$entry" = "$name/arn" ]; then
            case "$mode" in
                ???[xs]*) ;;
                *) fail "'$entry' is not executable (mode $mode)" ;;
            esac
        fi
    done < "$work/modes"

    extracted="$work/extracted"
    mkdir "$extracted"
    tar -xzf "$archive" -C "$extracted" 2>"$work/tar-errors" || fail "cannot extract '$archive': $(cat "$work/tar-errors")"
    [ -f "$extracted/$name/arn" ] || fail "extracted archive has no $name/arn"

    # Run from the extraction directory with a minimal environment, so nothing
    # from the build tree or the caller's environment can be picked up.
    set +e
    output="$(cd "$extracted" && env -i PATH=/usr/bin:/bin HOME="$extracted" TMPDIR="$work" \
        "$extracted/$name/arn" --version 2>"$work/stderr")"
    status=$?
    set -e
    output="$(printf '%s' "$output" | tr -d '\r')"
    if [ $status -ne 0 ]; then
        fail "extracted arn --version failed with exit code $status: $(cat "$work/stderr")"
    fi
    if [ "$output" != "$expected_version_line" ]; then
        fail "extracted arn --version printed '$output', expected '$expected_version_line'"
    fi
    echo "Extracted arn: $output"

    report_dependencies "$extracted/$name/arn"
}

if [ "$mode" = "package" ]; then
    [ -n "$binary" ] || fail "--binary is required"
    [ -f "$binary" ] || fail "binary '$binary' not found"
    repo_root="$(cd "$(dirname "$0")/.." && pwd)"
    make_temp_dir
    staging_dir="$made_dir"
    staging="$staging_dir"
    mkdir "$staging/$name"
    cp "$binary" "$staging/$name/arn"
    cp "$repo_root/LICENSE" "$staging/$name/LICENSE"
    cp "$repo_root/README.md" "$staging/$name/README.md"
    chmod 755 "$staging/$name/arn"
    chmod 644 "$staging/$name/LICENSE" "$staging/$name/README.md"

    archive_dir="$(cd "$(dirname "$archive")" && pwd)"
    archive="$archive_dir/$(basename "$archive")"
    rm -f "$archive"
    archive_to_remove="$archive"
    # COPYFILE_DISABLE keeps macOS tar from adding AppleDouble "._" entries.
    (cd "$staging" && COPYFILE_DISABLE=1 tar -czf "$archive" "$name/arn" "$name/LICENSE" "$name/README.md") ||
        fail "could not create '$archive'"
fi

verify_archive
archive_to_remove=""
echo "Verified $archive: $name/arn, $name/LICENSE, $name/README.md"
