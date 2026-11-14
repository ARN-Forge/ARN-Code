#!/usr/bin/env bash
# Checks the minimum OS a built arn needs, so a runner image update cannot
# silently raise it.
#
#   check-min-os.sh macos <binary> <minos>
#       The Mach-O LC_BUILD_VERSION minos must equal <minos> (the
#       CMAKE_OSX_DEPLOYMENT_TARGET the build was configured with).
#   check-min-os.sh linux <binary> <max GLIBC> <max GLIBCXX> <max CXXABI>
#       No version the ELF binary needs (its .gnu.version_r section, which
#       the dynamic loader checks; read with readelf --version-info) may be
#       newer than the given GLIBC_, GLIBCXX_ and CXXABI_ versions (the
#       baseline distribution's). The full list of needed versions is printed.
#
# Prints what it found. Written for bash 3.2 (macOS) as well as newer bash.
set -eu

fail() { echo "check-min-os.sh: error: $*" >&2; exit 1; }

# Prints the larger of two dotted versions (numeric, field by field).
newer() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        n = split(a, x, "."); m = split(b, y, "."); k = (n > m ? n : m)
        for (i = 1; i <= k; i++) {
            if ((x[i] + 0) > (y[i] + 0)) { print a; exit }
            if ((x[i] + 0) < (y[i] + 0)) { print b; exit }
        }
        print a
    }'
}

mode="${1:-}"; binary="${2:-}"
[ -f "$binary" ] || fail "binary '$binary' not found"
case "$mode" in
    macos)
        [ $# -eq 3 ] || fail "usage: check-min-os.sh macos <binary> <minos>"
        expected="$3"
        minos="$(otool -l "$binary" | awk '$1 == "cmd" && $2 == "LC_BUILD_VERSION" { found = 1 } found && $1 == "minos" { print $2; exit }')"
        [ -n "$minos" ] || fail "$binary has no LC_BUILD_VERSION minos"
        echo "$binary: minos $minos"
        [ "$minos" = "$expected" ] || fail "$binary needs macOS $minos, expected $expected"
        ;;
    linux)
        [ $# -eq 5 ] || fail "usage: check-min-os.sh linux <binary> <max GLIBC> <max GLIBCXX> <max CXXABI>"
        info="$(readelf --version-info --wide "$binary")" || fail "readelf --version-info failed for $binary"
        symbols="$(printf '%s\n' "$info" | sed -n '/Version needs section/,$p')"
        [ -n "$symbols" ] || fail "$binary has no version needs section; is it a dynamic glibc executable?"
        echo "$binary: version needs (readelf --version-info):"
        printf '%s\n' "$symbols" | grep -E 'File:|Name:' | sed -E 's/^ *0x[0-9a-f]+: +//; s/^ *[0-9a-f]+: +//; s/ +Flags:.*//; s/ +Cnt:.*//; s/^/  /'
        status=0
        for pair in "GLIBC:$3" "GLIBCXX:$4" "CXXABI:$5"; do
            prefix="${pair%%:*}"; limit="${pair#*:}"
            highest=""
            for version in $(printf '%s\n' "$symbols" | grep -oE "[( ]${prefix}_[0-9][0-9.]*" | sed "s/^.${prefix}_//" | sort -u); do
                if [ -z "$highest" ]; then highest="$version"; else highest="$(newer "$highest" "$version")"; fi
            done
            [ -n "$highest" ] || fail "$binary needs no ${prefix}_ symbol versions; is it a dynamic glibc executable?"
            echo "$binary: highest ${prefix}_ version needed is $highest (limit $limit)"
            if [ "$highest" != "$limit" ] && [ "$(newer "$highest" "$limit")" = "$highest" ]; then
                echo "check-min-os.sh: error: $binary needs ${prefix}_$highest, newer than ${prefix}_$limit" >&2
                status=1
            fi
        done
        exit $status
        ;;
    *)
        fail "first argument must be macos or linux"
        ;;
esac
