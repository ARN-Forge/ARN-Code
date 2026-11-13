#!/usr/bin/env sh

set -eu
# Character ranges in the patterns below mean ASCII letters and digits.
LC_ALL=C
export LC_ALL

repository="${ARN_REPOSITORY:-ARN-Forge/ARN-Code}"
install_dir="${ARN_INSTALL_DIR:-${HOME}/.local/bin}"
archive_override="${ARN_INSTALL_ARCHIVE:-}"
checksums_override="${ARN_INSTALL_CHECKSUMS:-}"
requested_version="${ARN_VERSION:-latest}"
system="$(uname -s)"
machine="$(uname -m)"
uninstall="${ARN_UNINSTALL:-0}"
adopt_existing="${ARN_ADOPT_EXISTING:-0}"
allow_downgrade="${ARN_ALLOW_DOWNGRADE:-0}"
newline='
'
carriage_return="$(printf '\r')"

# canonical_path <absolute path>: the path without ".", ".." and repeated or
# trailing slashes, resolved by text as "cd -L" does, so one directory has
# one spelling.
canonical_path() {
    printf '%s\n' "$1" | awk -F/ '{
        n = 0
        for (i = 1; i <= NF; i++) {
            if ($i == "" || $i == ".") continue
            if ($i == "..") { if (n > 0) n--; continue }
            part[++n] = $i
        }
        path = ""
        for (i = 1; i <= n; i++) path = path "/" part[i]
        print (path == "" ? "/" : path)
    }'
}

# The install directory is one absolute, single-line path: the installation
# manifest and the transaction record are line-based and record it.
case "${install_dir}" in
    *"${newline}"*|*"${carriage_return}"*)
        echo "ARN_INSTALL_DIR must be a single-line path." >&2
        exit 1
        ;;
    /*) ;;
    *) install_dir="$(pwd)/${install_dir}" ;;
esac
install_dir="$(canonical_path "${install_dir}")"
manifest_name=".arn-install-manifest"
manifest_path="${install_dir}/${manifest_name}"
journal_path="${install_dir}/.arn-install-transaction"
marker="# Added by ARN Code installer: ${install_dir}"
export_line="export PATH=\"${install_dir}:\$PATH\""
profile="${ARN_PROFILE:-${HOME}/.profile}"
case "${SHELL:-}" in
    */zsh) profile="${ARN_PROFILE:-${HOME}/.zprofile}" ;;
esac
case "${profile}" in
    /*) ;;
    *) profile="$(pwd)/${profile}" ;;
esac
profile_tmp=""
profile_snapshot=""
profile_problem=""

# Test hooks, unset in normal use. ARN_INSTALL_TEST_FAULT is a comma-separated
# list: "<point>" makes that step fail; "signal:<NAME>:<point>" sends the
# installer that signal there (KILL stands in for a crash or a power loss).
test_fault() {
    case ",${ARN_INSTALL_TEST_FAULT:-}," in
        *",$1,"*)
            echo "Injected test failure at $1." >&2
            return 1
            ;;
    esac
    for test_signal in HUP INT QUIT PIPE TERM KILL STOP; do
        case ",${ARN_INSTALL_TEST_FAULT:-}," in
            *",signal:${test_signal}:$1,"*) kill -s "${test_signal}" $$ ;;
        esac
    done
    return 0
}

# A handled signal ends the script with 128 + its number; the EXIT trap then
# undoes what is unfinished. While that runs, further signals are ignored.
set_signal_traps() {
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 131' QUIT
    trap 'exit 141' PIPE
    trap 'exit 143' TERM
}
ignore_signals() { trap '' HUP INT QUIT PIPE TERM; }

# single_line <value>: grep matches line by line, so a value with a line
# break could otherwise pass on its first line alone.
single_line() {
    case "$1" in
        *"${newline}"*) return 1 ;;
    esac
    return 0
}

# exists <path>: anything is there, a dangling symbolic link included.
exists() { [ -e "$1" ] || [ -L "$1" ]; }

# not_regular <path>: something other than a regular file is in its place.
not_regular() { [ -L "$1" ] || { [ -e "$1" ] && [ ! -f "$1" ]; }; }

# has_profile_block <file>: the file has the two lines this installer adds.
has_profile_block() {
    [ -f "$1" ] || return 1
    ARN_MARKER="${marker}" ARN_EXPORT="${export_line}" awk '
        BEGIN { m = ENVIRON["ARN_MARKER"]; e = ENVIRON["ARN_EXPORT"] }
        previous && $0 == e { found = 1 }
        { previous = ($0 == m) }
        END { exit found ? 0 : 1 }' "$1"
}

cleanup_profile_files() {
    if [ -n "${profile_tmp}" ]; then rm -f "${profile_tmp}"; fi
    if [ -n "${profile_snapshot}" ]; then rm -f "${profile_snapshot}"; fi
    profile_tmp=""
    profile_snapshot=""
}

# resolve_link <path>: the file a chain of symbolic links ends at.
resolve_link() {
    link_path="$1"
    link_count=0
    while [ -L "${link_path}" ]; do
        link_count=$((link_count + 1))
        [ "${link_count}" -le 40 ] || return 1
        link_target="$(readlink "${link_path}")" || return 1
        case "${link_target}" in
            /*) link_path="${link_target}" ;;
            *) link_path="$(dirname "${link_path}")/${link_target}" ;;
        esac
    done
    printf '%s\n' "${link_path}"
}

# rewrite_profile <profile> <add|remove>: adds or removes this installer's two
# lines (and the blank line written before them). The new text goes to a
# temporary file in the profile's own directory, started as a copy so that it
# has the profile's permissions, and is renamed over the profile: the profile
# is never left half-written, and a symbolic link stays a link to it. Nothing
# is replaced if the profile changed while the new text was prepared. Sets
# profile_problem and returns 1 on failure.
rewrite_profile() {
    profile_problem=""
    if ! profile_file="$(resolve_link "$1")"; then
        profile_problem="its symbolic links could not be followed"
        return 1
    fi
    profile_dir="$(dirname "${profile_file}")"
    if [ ! -d "${profile_dir}" ]; then
        profile_problem="${profile_dir} does not exist"
        return 1
    fi
    if not_regular "${profile_file}"; then
        profile_problem="it is not a regular file"
        return 1
    fi
    if ! profile_snapshot="$(mktemp "${TMPDIR:-/tmp}/arn-profile.XXXXXX")" ||
        ! profile_tmp="$(mktemp "${profile_dir}/.arn-profile.XXXXXX")"; then
        profile_problem="no temporary file could be created"
        cleanup_profile_files
        return 1
    fi
    if [ -f "${profile_file}" ]; then
        if ! cp "${profile_file}" "${profile_snapshot}" || ! cp -p "${profile_file}" "${profile_tmp}"; then
            profile_problem="it could not be copied"
            cleanup_profile_files
            return 1
        fi
    else
        # A new profile gets the permissions the umask gives new files.
        chmod "$(printf '%o' $((0666 & ~$(umask))))" "${profile_tmp}" || true
    fi
    case "$2" in
        add)
            { cat "${profile_snapshot}" && printf '\n%s\n%s\n' "${marker}" "${export_line}"; } > "${profile_tmp}"
            ;;
        remove)
            ARN_MARKER="${marker}" ARN_EXPORT="${export_line}" awk '
                BEGIN { m = ENVIRON["ARN_MARKER"]; e = ENVIRON["ARN_EXPORT"] }
                { line[NR] = $0 }
                END {
                    for (i = 1; i < NR; i++) {
                        if (line[i] == m && line[i + 1] == e) {
                            skip[i] = 1; skip[i + 1] = 1
                            if (i > 1 && line[i - 1] == "") skip[i - 1] = 1
                        }
                    }
                    for (i = 1; i <= NR; i++) if (!(i in skip)) print line[i]
                }' "${profile_snapshot}" > "${profile_tmp}"
            ;;
    esac
    if [ $? -ne 0 ]; then
        profile_problem="the new text could not be written"
        cleanup_profile_files
        return 1
    fi
    if ! test_fault profile-before-rename; then
        profile_problem="injected test failure"
        cleanup_profile_files
        return 1
    fi
    if [ -f "${profile_file}" ]; then
        changed=0
        cmp -s "${profile_file}" "${profile_snapshot}" || changed=1
    else
        changed=0
        if exists "${profile_file}"; then changed=1; fi
    fi
    if [ "${changed}" = 1 ]; then
        profile_problem="it changed while it was being updated, so it was left as it is"
        cleanup_profile_files
        return 1
    fi
    if ! mv -f "${profile_tmp}" "${profile_file}"; then
        profile_problem="it could not be replaced"
        cleanup_profile_files
        return 1
    fi
    profile_tmp=""
    cleanup_profile_files
    return 0
}

# read_manifest: validates the installation manifest and sets owned_files (one
# name per line), recorded_version, recorded_commit, recorded_profile and
# created_dir. The file is only matched against patterns, never evaluated; on
# any problem it sets manifest_problem and returns 1.
read_manifest() {
    if [ -L "${manifest_path}" ] || [ ! -f "${manifest_path}" ]; then
        manifest_problem="${manifest_path} is not a regular file."
        return 1
    fi
    if grep -q "${carriage_return}" "${manifest_path}"; then
        manifest_problem="${manifest_path} contains a malformed line."
        return 1
    fi
    if [ "$(sed -n 1p "${manifest_path}")" != "arn-install-manifest 1" ]; then
        manifest_problem="${manifest_path} has an unsupported format."
        return 1
    fi
    if grep -Evq '^(arn-install-manifest 1|version [^ ]+|commit [^ ]+|install_dir /.*|profile (-|/.*)|created_dir [01]|file [A-Za-z0-9][A-Za-z0-9._-]*)$' "${manifest_path}"; then
        manifest_problem="${manifest_path} contains a malformed line."
        return 1
    fi
    for key in arn-install-manifest version commit install_dir profile created_dir; do
        if [ "$(grep -c "^${key} " "${manifest_path}" || true)" != 1 ]; then
            manifest_problem="${manifest_path} must have exactly one '${key}' line."
            return 1
        fi
    done
    if [ "$(sed -n 's/^install_dir //p' "${manifest_path}")" != "${install_dir}" ]; then
        manifest_problem="${manifest_path} belongs to another directory, not to ${install_dir}."
        return 1
    fi
    owned_files="$(sed -n 's/^file //p' "${manifest_path}")"
    if [ -n "$(printf '%s\n' "${owned_files}" | sort | uniq -d)" ]; then
        manifest_problem="${manifest_path} lists a file more than once."
        return 1
    fi
    recorded_version="$(sed -n 's/^version //p' "${manifest_path}")"
    recorded_commit="$(sed -n 's/^commit //p' "${manifest_path}")"
    recorded_profile="$(sed -n 's/^profile //p' "${manifest_path}")"
    created_dir="$(sed -n 's/^created_dir //p' "${manifest_path}")"
}

# write_manifest <file> <version> <commit> <profile> <created_dir> <name>...
write_manifest() {
    manifest_file="$1"
    shift
    {
        printf 'arn-install-manifest 1\n'
        printf 'version %s\n' "$1"
        printf 'commit %s\n' "$2"
        printf 'install_dir %s\n' "${install_dir}"
        printf 'profile %s\n' "$3"
        printf 'created_dir %s\n' "$4"
        shift 4
        for name in "$@"; do printf 'file %s\n' "${name}"; done
    } > "${manifest_file}"
}

# The transaction record of an installation in progress, through a temporary
# file renamed into place:
#   arn-install-transaction 1
#   install_dir <dir>
#   work_dir .arn-install.<random>   (in install_dir: the new arn while it is
#                                     staged, arn.previous and manifest.previous)
#   state staging|activating|committed
#   fresh 0|1                        (1: there was no arn before)
#   had_manifest 0|1
#   created_dir 0|1                  (1: this installation created install_dir)
#   previous_version X.Y.Z|-
#   profile_change <profile>|-       (a profile this installation adds lines to)
# "staging": nothing in the installation has changed yet. "activating": the
# copies of the previous arn and manifest are complete, and the new arn,
# profile lines and manifest may be in place. "committed": done.
write_journal() {
    {
        printf 'arn-install-transaction 1\n'
        printf 'install_dir %s\n' "${install_dir}"
        printf 'work_dir %s\n' "${work_name}"
        printf 'state %s\n' "$1"
        printf 'fresh %s\n' "${fresh}"
        printf 'had_manifest %s\n' "${had_manifest}"
        printf 'created_dir %s\n' "${made_dir}"
        printf 'previous_version %s\n' "${previous_version}"
        printf 'profile_change %s\n' "${profile_change}"
    } > "${journal_path}.new" && mv -f "${journal_path}.new" "${journal_path}"
}

# read_journal: validates the transaction record (patterns only, never
# evaluated) and sets the j_* variables; on any problem it sets
# journal_problem and returns 1.
read_journal() {
    if [ -L "${journal_path}" ] || [ ! -f "${journal_path}" ]; then
        journal_problem="${journal_path} is not a regular file"
        return 1
    fi
    if grep -q "${carriage_return}" "${journal_path}" ||
        [ "$(sed -n 1p "${journal_path}")" != "arn-install-transaction 1" ] ||
        grep -Evq '^(arn-install-transaction 1|install_dir /.*|work_dir \.arn-install\.[A-Za-z0-9]+|state (staging|activating|committed)|fresh [01]|had_manifest [01]|created_dir [01]|previous_version (-|[0-9]+\.[0-9]+\.[0-9]+)|profile_change (-|/.*))$' "${journal_path}"; then
        journal_problem="${journal_path} is malformed"
        return 1
    fi
    for key in arn-install-transaction install_dir work_dir state fresh had_manifest created_dir previous_version profile_change; do
        if [ "$(grep -c "^${key} " "${journal_path}" || true)" != 1 ]; then
            journal_problem="${journal_path} must have exactly one '${key}' line"
            return 1
        fi
    done
    if [ "$(sed -n 's/^install_dir //p' "${journal_path}")" != "${install_dir}" ]; then
        journal_problem="${journal_path} belongs to another directory"
        return 1
    fi
    j_work_dir="${install_dir}/$(sed -n 's/^work_dir //p' "${journal_path}")"
    j_state="$(sed -n 's/^state //p' "${journal_path}")"
    j_fresh="$(sed -n 's/^fresh //p' "${journal_path}")"
    j_had_manifest="$(sed -n 's/^had_manifest //p' "${journal_path}")"
    j_created_dir="$(sed -n 's/^created_dir //p' "${journal_path}")"
    j_previous_version="$(sed -n 's/^previous_version //p' "${journal_path}")"
    j_profile_change="$(sed -n 's/^profile_change //p' "${journal_path}")"
}

# restore_copy <saved> <destination>: puts a copy of a saved file in place
# with a rename, keeping the saved file, so it can be repeated.
restore_copy() {
    rm -f "${j_work_dir}/restore.tmp"
    { ln "$1" "${j_work_dir}/restore.tmp" 2>/dev/null || cp -p "$1" "${j_work_dir}/restore.tmp"; } &&
        mv -f "${j_work_dir}/restore.tmp" "$2"
}

# recover_transaction: brings the installation back to its state before the
# recorded transaction (or finishes it, once committed), from what is on disk,
# so it works the same after an error, a signal, a crash or a power loss, and
# can be repeated if it is itself interrupted. The record is removed last; on
# any problem it stays, with the work directory, for a retry. Sets
# recovered_verified (1: the previous arn answered with its recorded version,
# or there was none to check) and recovery_problem; returns 1 on a problem.
recover_transaction() {
    recovery_problem=""
    recovered_verified=1
    target="${install_dir}/arn"
    if [ -L "${j_work_dir}" ] || { exists "${j_work_dir}" && [ ! -d "${j_work_dir}" ]; }; then
        recovery_problem="${j_work_dir} is not a directory"
        return 1
    fi
    if [ "${j_state}" = committed ]; then
        rm -f "${journal_path}" && rm -rf "${j_work_dir}" || {
            recovery_problem="${journal_path} or ${j_work_dir} could not be removed"
            return 1
        }
        return 0
    fi
    if [ "${j_state}" = activating ]; then
        # Everything below reads its evidence from the work directory: that the
        # new arn is no longer staged, and the saved previous arn and manifest.
        # The installer itself removes the work directory only after the
        # record, so a record in this state without it means something else
        # removed it, and an arn there may not be the installer's. Nothing is
        # touched then.
        if [ ! -d "${j_work_dir}" ]; then
            recovery_problem="its work directory ${j_work_dir} is missing, so it cannot be told whether ${target} and ${manifest_path} are the installer's; nothing was changed"
            return 1
        fi
        # Where there was no manifest before, one in place now is the
        # installer's only when it is byte for byte the copy the installer kept
        # (manifest.written) before putting its manifest there. Anything else
        # (created or changed by someone else after the interruption) is kept,
        # and nothing is touched.
        remove_new_manifest=0
        if [ "${j_had_manifest}" = 0 ] && exists "${manifest_path}"; then
            if [ -f "${manifest_path}" ] && [ ! -L "${manifest_path}" ] && [ -f "${j_work_dir}/manifest.written" ] &&
                cmp -s "${manifest_path}" "${j_work_dir}/manifest.written"; then
                remove_new_manifest=1
            else
                recovery_problem="${manifest_path} is not the manifest the installer wrote, so it is kept; nothing was changed"
                return 1
            fi
        fi
        if [ "${j_fresh}" = 1 ]; then
            # The new arn is in place once it is no longer staged.
            if [ ! -e "${j_work_dir}/arn" ] && [ -f "${target}" ] && [ ! -L "${target}" ]; then
                rm -f "${target}" || recovery_problem="${target} could not be removed"
            fi
        elif [ -f "${j_work_dir}/arn.previous" ]; then
            if [ ! "${target}" -ef "${j_work_dir}/arn.previous" ]; then
                restore_copy "${j_work_dir}/arn.previous" "${target}" || recovery_problem="the previous arn could not be put back"
            fi
        else
            recovery_problem="the previous arn is missing from ${j_work_dir}"
        fi
        test_fault recovery-restore
        if [ "${j_had_manifest}" = 1 ]; then
            if [ -f "${j_work_dir}/manifest.previous" ]; then
                restore_copy "${j_work_dir}/manifest.previous" "${manifest_path}" ||
                    recovery_problem="${recovery_problem:+${recovery_problem}; }the previous manifest could not be put back"
            else
                recovery_problem="${recovery_problem:+${recovery_problem}; }the previous manifest is missing from ${j_work_dir}"
            fi
        elif [ "${remove_new_manifest}" = 1 ]; then
            rm -f "${manifest_path}" || recovery_problem="${recovery_problem:+${recovery_problem}; }the new manifest could not be removed"
        fi
        if [ "${j_profile_change}" != "-" ] && has_profile_block "${j_profile_change}"; then
            rewrite_profile "${j_profile_change}" remove ||
                recovery_problem="${recovery_problem:+${recovery_problem}; }the PATH lines could not be taken out of ${j_profile_change} (${profile_problem})"
        fi
    fi
    if [ -n "${recovery_problem}" ]; then return 1; fi
    if [ "${j_fresh}" = 0 ] && [ "${j_previous_version}" != "-" ]; then
        case "$("${target}" --version 2>/dev/null || true)" in
            "arn ${j_previous_version}"|"arn ${j_previous_version} "*) ;;
            *) recovered_verified=0 ;;
        esac
    fi
    rm -f "${journal_path}" || { recovery_problem="${journal_path} could not be removed"; return 1; }
    rm -rf "${j_work_dir}"
    if [ "${j_created_dir}" = 1 ]; then rmdir "${install_dir}" 2>/dev/null || true; fi
    return 0
}

# Work directories of a run that ended before it wrote its record (they never
# hold the only copy of anything), and temporary files of interrupted writes.
# Only called when no record is pending.
remove_stale_files() {
    for stale in "${install_dir}"/.arn-install.*; do
        if [ -d "${stale}" ] && [ ! -L "${stale}" ]; then rm -rf "${stale}"; fi
    done
    for stale in "${manifest_path}.new" "${journal_path}.new"; do
        if [ -f "${stale}" ] && [ ! -L "${stale}" ]; then rm -f "${stale}"; fi
    done
}

# resolve_pending <Update|Uninstall>: an earlier run that was interrupted is
# recovered before anything else looks at the directory.
resolve_pending() {
    exists "${journal_path}" || return 0
    if ! read_journal; then
        echo "$1 refused: an interrupted installation was found, but its record cannot be used: ${journal_problem}." >&2
        echo "Nothing was changed, and the .arn-install.* directories in ${install_dir} were kept: one may hold the previous" >&2
        echo "arn (arn.previous) and its manifest (manifest.previous). To recover by hand, move those back to" >&2
        echo "${install_dir}/arn and ${manifest_path}, then delete ${journal_path}." >&2
        exit 1
    fi
    if ! recover_transaction; then
        echo "$1 refused: an interrupted installation was found and could not be undone: ${recovery_problem}." >&2
        if [ -d "${j_work_dir}" ]; then kept_work=" and ${j_work_dir}"; else kept_work=""; fi
        echo "Nothing else was changed; ${journal_path}${kept_work} were kept. Run the installer again to retry once the problem is resolved." >&2
        exit 1
    fi
    if [ "${j_state}" = committed ]; then
        echo "Finished cleaning up after an earlier ARN Code installation."
    elif [ "${j_fresh}" = 1 ]; then
        echo "An interrupted installation of ARN Code was found and undone."
    elif [ "${recovered_verified}" = 1 ]; then
        echo "An interrupted update of ARN Code was found and undone; the previous version is installed again."
    else
        echo "Warning: an interrupted update of ARN Code was found and the previous arn was put back, but it could not be verified. Install ARN Code again to repair it." >&2
    fi
}

refuse() { # refuse <Update|Uninstall> <reason>
    echo "$1 refused: $2" >&2
    echo "Nothing was changed. To clean up by hand, delete ${install_dir}/arn and ${manifest_path}," >&2
    echo "and remove the line '${marker}' and the PATH line after it from your shell profile." >&2
    exit 1
}

trap 'cleanup_profile_files' EXIT
set_signal_traps

if [ "${uninstall}" = "1" ]; then
    uninstall_exit() {
        status=$?
        ignore_signals
        cleanup_profile_files
        rm -f "${manifest_path}.new"
        exit "${status}"
    }
    trap uninstall_exit EXIT
    resolve_pending Uninstall
    if ! exists "${manifest_path}"; then
        if exists "${install_dir}/arn"; then
            refuse "Uninstall" "${install_dir} has arn but no ${manifest_name}, so it is unknown which files are ARN's (installers before this one did not record their files). Run the installer with ARN_ADOPT_EXISTING=1 to record the installation, then uninstall again."
        fi
        if [ -d "${install_dir}" ]; then remove_stale_files; fi
        echo "ARN Code is not installed in ${install_dir}; nothing to remove."
        exit 0
    fi
    read_manifest || refuse "Uninstall" "${manifest_problem}"
    # The names are validated plain file names: no separators, spaces or glob
    # characters, so they split safely. Every one is checked before any is
    # removed.
    for name in ${owned_files}; do
        if not_regular "${install_dir}/${name}"; then
            refuse "Uninstall" "${install_dir}/${name} is no longer a regular file, so it is not ARN's to remove."
        fi
    done
    remaining=""
    for name in ${owned_files}; do
        rm -f "${install_dir}/${name}" 2>/dev/null || true
        if exists "${install_dir}/${name}"; then remaining="${remaining}${remaining:+ }${name}"; fi
    done
    if [ -n "${remaining}" ]; then
        # shellcheck disable=SC2086 # plain file names, see above
        write_manifest "${manifest_path}.new" "${recorded_version}" "${recorded_commit}" "${recorded_profile}" "${created_dir}" ${remaining} &&
            mv -f "${manifest_path}.new" "${manifest_path}" || rm -f "${manifest_path}.new"
        echo "Uninstall incomplete: ${remaining} could not be removed from ${install_dir}. Fix the permissions and run the uninstall again; your shell profile was not changed." >&2
        exit 1
    fi
    test_fault uninstall-after-files
    # The profile goes before the manifest: if it cannot be changed, the
    # manifest still records it for the next attempt.
    if [ "${recorded_profile}" != "-" ] && has_profile_block "${recorded_profile}"; then
        if rewrite_profile "${recorded_profile}" remove; then
            echo "Removed ${install_dir} from ${recorded_profile}."
        else
            echo "Uninstall incomplete: could not edit ${recorded_profile} (${profile_problem}). Remove '${marker}' and the line after it by hand, then run the uninstall again." >&2
            exit 1
        fi
    fi
    test_fault uninstall-after-profile
    if ! rm -f "${manifest_path}"; then
        echo "Uninstall incomplete: could not remove ${manifest_path}." >&2
        exit 1
    fi
    remove_stale_files
    if [ "${created_dir}" = 1 ] && rmdir "${install_dir}" 2>/dev/null; then
        echo "ARN Code ${recorded_version} was uninstalled and ${install_dir} removed."
    else
        echo "ARN Code ${recorded_version} was uninstalled from ${install_dir}; files there that are not ARN's were kept."
    fi
    exit 0
fi

resolve_pending Update

case "${system}:${machine}" in
    Linux:x86_64|Linux:amd64) package="arn-linux-x64" ;;
    Darwin:arm64|Darwin:aarch64) package="arn-macos-arm64" ;;
    Darwin:x86_64|Darwin:amd64) package="arn-macos-x64" ;;
    *)
        echo "ARN Code does not have a prebuilt release for ${system} ${machine}." >&2
        exit 1
        ;;
esac
archive_name="${package}.tar.gz"

# Validated before any URL is built: owner/name, and latest or X.Y.Z / vX.Y.Z
# (the release tag is always vX.Y.Z).
if ! single_line "${repository}" || ! printf '%s\n' "${repository}" | grep -Eq '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$'; then
    echo "ARN_REPOSITORY must have the form owner/name." >&2
    exit 1
fi
semver='(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'
case "${requested_version}" in
    latest) tag="" ;;
    *)
        if ! single_line "${requested_version}" || ! printf '%s\n' "${requested_version}" | grep -Eq "^v?${semver}\$"; then
            echo "ARN_VERSION must be latest, X.Y.Z or vX.Y.Z (for example 0.9.0), got '${requested_version}'." >&2
            exit 1
        fi
        tag="v${requested_version#v}"
        ;;
esac

case "${allow_downgrade}" in
    0|"") allow_downgrade=0 ;;
    1) ;;
    *)
        echo "ARN_ALLOW_DOWNGRADE must be 1 (or unset), got '${allow_downgrade}'." >&2
        exit 1
        ;;
esac
if [ -n "${archive_override}" ] && [ -z "${checksums_override}" ]; then
    echo "ARN_INSTALL_ARCHIVE needs ARN_INSTALL_CHECKSUMS, the SHA256SUMS file to verify it with." >&2
    exit 1
fi
if ! command -v curl >/dev/null 2>&1 && [ -z "${archive_override}" ]; then
    echo "curl is required to install ARN Code." >&2
    exit 1
fi

# is_semver <value>: exactly X.Y.Z, decimal numbers without leading zeros.
is_semver() {
    single_line "$1" && printf '%s\n' "$1" | grep -Eq "^${semver}\$"
}

# vcmp(a, b) for awk: -1, 0 or 1 for two validated X.Y.Z versions. Each part
# is compared as a number of any size (the shorter digit string is smaller,
# equal lengths compare digit by digit), never as a whole string, so 0.10.0
# is newer than 0.9.0.
awk_vcmp='
function vcmp(a, b,   x, y, k) {
    split(a, x, "."); split(b, y, ".")
    for (k = 1; k <= 3; k++) {
        if (length(x[k]) != length(y[k])) return length(x[k]) < length(y[k]) ? -1 : 1
        if ((x[k] "") != (y[k] "")) return (x[k] "") < (y[k] "") ? -1 : 1
    }
    return 0
}'

# version_compare <a> <b>: prints -1, 0 or 1 (a older, the same, newer).
version_compare() {
    awk -v a="$1" -v b="$2" "${awk_vcmp}"'
BEGIN { print vcmp(a, b) }'
}

# The installed version, for the downgrade check: the version the manifest
# records, or else what the installed arn reports. The arn is run only when
# it is ARN's: the manifest lists it, or ARN_ADOPT_EXISTING=1 claims it (any
# other arn makes the update stop later anyway). Empty when no valid X.Y.Z is
# found (the version is then unknown, never guessed).
installed_version=""
detect_installed_version() {
    installed_version=""
    arn_owned="${adopt_existing}"
    if [ -f "${manifest_path}" ] && [ ! -L "${manifest_path}" ]; then
        recorded="$(sed -n 's/^version //p' "${manifest_path}" 2>/dev/null || true)"
        if is_semver "${recorded}"; then
            installed_version="${recorded}"
            return 0
        fi
        if grep -qx 'file arn' "${manifest_path}" 2>/dev/null; then arn_owned=1; fi
    fi
    if [ "${arn_owned}" = 1 ] && [ -f "${install_dir}/arn" ] && [ -x "${install_dir}/arn" ] && [ ! -L "${install_dir}/arn" ]; then
        reported="$("${install_dir}/arn" --version </dev/null 2>/dev/null | sed -n '1s/^arn \([^ ]*\)\( .*\)\{0,1\}$/\1/p' || true)"
        if is_semver "${reported}"; then installed_version="${reported}"; fi
    fi
    return 0
}

# check_downgrade <new X.Y.Z> <how to retry>: refuses to replace a newer
# installed version unless ARN_ALLOW_DOWNGRADE=1.
downgrade_warned=0
check_downgrade() {
    [ -n "${installed_version}" ] || return 0
    [ "$(version_compare "$1" "${installed_version}")" = -1 ] || return 0
    if [ "${allow_downgrade}" = 1 ]; then
        if [ "${downgrade_warned}" = 0 ]; then
            echo "Warning: replacing ARN Code ${installed_version} with the older $1, because ARN_ALLOW_DOWNGRADE=1." >&2
            downgrade_warned=1
        fi
        return 0
    fi
    echo "Downgrade refused: ARN Code ${installed_version} is installed in ${install_dir}, and $1 is older." >&2
    echo "Nothing was changed. To install $1 anyway, run the installer again with ARN_ALLOW_DOWNGRADE=1$2" >&2
    exit 1
}

# The ready-to-copy command that installs version $1 with this installer.
install_command() {
    command_vars="ARN_VERSION=$1"
    if [ "${repository}" != "ARN-Forge/ARN-Code" ]; then command_vars="ARN_REPOSITORY=${repository} ${command_vars}"; fi
    printf 'curl -fsSL https://raw.githubusercontent.com/%s/main/scripts/install.sh | %s sh\n' "${repository}" "${command_vars}"
}

# json_flatten: reads one JSON document and prints one line per value,
# "<path><TAB><kind>:<text>": kind "o" or "a" opens an object or array
# (empty text), "s" is a string (its JSON escapes left as written), "l" a
# number, true, false or null. Paths are like .assets[0].name or [2].tag_name.
# Exits 2 on anything that is not well-formed JSON: the release metadata is
# only parsed, never evaluated. A string longer than 256 characters is checked
# in full but printed as "(long string)": no value the installer reads is
# that long, and keeping only short ones keeps the work linear.
json_flatten_awk='
function after_value() { if (depth == 0) done = 1; else state = "," }
function vpath() {
    if (depth == 0) return ""
    if (ctype[depth] == "o") return cpath[depth] "." ckey[depth]
    return cpath[depth] "[" cidx[depth] "]"
}
function read_string(   j, c, h, str, len) {
    j = i + 1
    str = ""
    len = 0
    while (j <= n) {
        c = ch[j]
        if (c == "\"") { i = j + 1; return (len > 256 ? "(long string)" : str) }
        if (c == "\\") {
            c = ch[j + 1]
            if (c == "u") {
                h = ch[j + 2] ch[j + 3] ch[j + 4] ch[j + 5]
                if (h !~ /^[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]$/) exit 2
                c = "\\u" h
                j += 6
            } else {
                if (c == "" || index("\"\\/bfnrt", c) == 0) exit 2
                c = "\\" c
                j += 2
            }
        } else {
            if (c < " ") exit 2
            j++
        }
        len++
        if (len <= 256) str = str c
    }
    exit 2
}
{ s = s $0 "\n" }
END {
    # One character per element, split once: indexing is constant time in
    # every awk, while substr() measures the whole string on each call in
    # some (BWK awk, the base of the macOS awk, and BusyBox), which made
    # parsing a release list quadratic there.
    n = split(s, ch, "")
    s = ""
    i = 1; depth = 0; state = "v"; done = 0
    while (i <= n) {
        c = ch[i]
        if (c == " " || c == "\t" || c == "\n" || c == "\r") { i++; continue }
        if (done) exit 2
        if (state == "v" || state == "v]") {
            if (state == "v]" && c == "]") { depth--; i++; after_value(); continue }
            p = vpath()
            if (c == "{") { depth++; ctype[depth] = "o"; cpath[depth] = p; print p "\to:"; state = "k}"; i++; continue }
            if (c == "[") { depth++; ctype[depth] = "a"; cpath[depth] = p; cidx[depth] = 0; print p "\ta:"; state = "v]"; i++; continue }
            if (c == "\"") { v = read_string(); print p "\ts:" v; after_value(); continue }
            v = ""
            while (i <= n && index("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-.", ch[i]) > 0) {
                if (length(v) >= 64) exit 2
                v = v ch[i]
                i++
            }
            if (v !~ /^(true|false|null|-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)$/) exit 2
            print p "\tl:" v
            after_value()
            continue
        }
        if (state == "k}" || state == "k") {
            if (state == "k}" && c == "}") { depth--; i++; after_value(); continue }
            if (c != "\"") exit 2
            ckey[depth] = read_string()
            state = ":"
            continue
        }
        if (state == ":") { if (c != ":") exit 2; i++; state = "v"; continue }
        if (c == ",") {
            i++
            if (ctype[depth] == "o") state = "k"; else { cidx[depth]++; state = "v" }
            continue
        }
        if ((c == "}" && ctype[depth] == "o") || (c == "]" && ctype[depth] == "a")) { depth--; i++; after_value(); continue }
        exit 2
    }
    if (!done) exit 2
}'

# release_summary (awk -v mode=one|list -v archive=<name>) reads json_flatten
# output of one release (mode one) or a list of releases (mode list) and
# prints one line per release: "<index> <tag> <draft> <prerelease> <archive
# assets> <SHA256SUMS assets> <valid>" (tab-separated; tag "-" when invalid).
# A release is valid with exactly one string tag_name of the form vX.Y.Z,
# boolean draft and prerelease, and an assets array whose names are strings.
# Exits 2 when the document is not an object (one) or array (list).
release_summary_awk='
BEGIN { FS = "\t" }
{
    path = $1
    val = substr($0, length($1) + 2)
    if (path == "") { root = val; next }
    if (mode == "list") {
        if (!match(path, /^\[[0-9]+\]/)) { bad = 1; next }
        g = substr(path, 2, RLENGTH - 2)
        rest = substr(path, RLENGTH + 1)
        if (rest == "") { seen[g] = 1; if (val != "o:") invalid[g] = 1; next }
    } else {
        g = 0
        rest = path
    }
    seen[g] = 1
    if (rest == ".tag_name") { tagn[g]++; if (substr(val, 1, 2) == "s:") tag[g] = substr(val, 3); else invalid[g] = 1 }
    else if (rest == ".draft") { draftn[g]++; draft[g] = val }
    else if (rest == ".prerelease") { pren[g]++; pre[g] = val }
    else if (rest == ".assets") { assetsn[g]++; if (val != "a:") invalid[g] = 1 }
    else if (rest ~ /^\.assets\[[0-9]+\]\.name$/) {
        if (substr(val, 1, 2) != "s:") invalid[g] = 1
        else if (substr(val, 3) == archive) arch[g]++
        else if (substr(val, 3) == "SHA256SUMS") sums[g]++
    }
}
END {
    if (bad || root != (mode == "list" ? "a:" : "o:")) exit 2
    for (g in seen) {
        ok = !invalid[g] && tagn[g] == 1 && draftn[g] == 1 && pren[g] == 1 && assetsn[g] == 1 &&
             (draft[g] == "l:true" || draft[g] == "l:false") && (pre[g] == "l:true" || pre[g] == "l:false") &&
             tag[g] ~ /^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$/
        printf "%s\t%s\t%d\t%d\t%d\t%d\t%d\n", g, (ok ? tag[g] : "-"), (draft[g] == "l:true"), (pre[g] == "l:true"), arch[g], sums[g], ok
    }
}'

# github_api <path> <output>: GET https://api.github.com/repos/<repository>/<path>.
# Returns 0 for HTTP 200; otherwise sets api_status (the HTTP status, or 000
# without an answer) and api_problem, and returns 1. Never retries.
github_api() {
    api_problem=""
    : > "${temporary_dir}/api-headers"
    if ! api_status="$(curl --silent --show-error --location --proto '=https' --connect-timeout 20 --max-time 60 \
            --header 'Accept: application/vnd.github+json' --header 'X-GitHub-Api-Version: 2022-11-28' \
            --user-agent ARN-Code-Installer --dump-header "${temporary_dir}/api-headers" \
            --output "$2" --write-out '%{http_code}' \
            "https://api.github.com/repos/${repository}/$1" 2>"${temporary_dir}/api-error")"; then
        api_status=000
        api_problem="GitHub could not be reached ($(sed -n '1s/^curl: //p' "${temporary_dir}/api-error" | cut -c1-200))"
        return 1
    fi
    case "${api_status}" in
        200) return 0 ;;
        [1-5][0-9][0-9]) ;;
        *)
            api_problem="GitHub sent no valid answer"
            api_status=000
            return 1
            ;;
    esac
    if [ "${api_status}" = 429 ] || { [ "${api_status}" = 403 ] &&
            { grep -Eiq '^x-ratelimit-remaining:[[:space:]]*0[[:space:]]*$' "${temporary_dir}/api-headers" ||
              grep -iq 'rate limit' "$2" 2>/dev/null; }; }; then
        api_problem="the GitHub API rate limit was reached (GitHub allows 60 requests an hour from one address without signing in); try again later"
    else
        api_problem="the GitHub API answered HTTP ${api_status}"
    fi
    return 1
}

# summarize <json file> <one|list> <summary file>: 1 for malformed metadata.
summarize() {
    awk "${json_flatten_awk}" < "$1" > "$1.flat" 2>/dev/null &&
        awk -v mode="$2" -v archive="${archive_name}" "${release_summary_awk}" < "$1.flat" > "$3" 2>/dev/null
}

# stop_without_release <reason>: the latest stable release cannot be
# installed here. Nothing older is installed in its place: this names the
# newest release that could be, as a command to run.
stop_without_release() {
    echo "$1" >&2
    if ! github_api "releases?per_page=100" "${temporary_dir}/releases.json"; then
        echo "Could not look for another release that has ${archive_name}: ${api_problem}." >&2
    elif ! summarize "${temporary_dir}/releases.json" list "${temporary_dir}/releases.sum"; then
        echo "Could not look for another release that has ${archive_name}: GitHub returned malformed release metadata." >&2
    else
        # The newest valid, published release with the archive and exactly one
        # SHA256SUMS: stable ones first; a prerelease is only mentioned.
        best="$(awk -v exclude="${tag}" "${awk_vcmp}"'
BEGIN { FS = "\t" }
$7 == 1 && $3 == 0 && $5 == 1 && $6 == 1 && $2 != exclude {
    v = substr($2, 2)
    if ($4 == 0) { if (stable == "" || vcmp(v, stable) > 0) stable = v }
    else if (pre == "" || vcmp(v, pre) > 0) pre = v
}
END { print (stable == "" ? "-" : stable) " " (pre == "" ? "-" : pre) }' "${temporary_dir}/releases.sum")"
        best_stable="${best% *}"
        best_pre="${best#* }"
        if [ "${best_stable}" != "-" ]; then
            echo "The newest stable release that has ${archive_name} and SHA256SUMS is v${best_stable}." >&2
            echo "It is not installed automatically. To install it, run:" >&2
            echo "  $(install_command "${best_stable}")" >&2
        else
            echo "No compatible release: no stable ARN Code release has both ${archive_name} and SHA256SUMS." >&2
        fi
        if [ "${best_pre}" != "-" ] && { [ "${best_stable}" = "-" ] || [ "$(version_compare "${best_pre}" "${best_stable}")" = 1 ]; }; then
            echo "The prerelease v${best_pre} has them; prereleases are installed only when named:" >&2
            echo "  $(install_command "${best_pre}")" >&2
        fi
    fi
    echo "ARN Code was not installed." >&2
    exit 1
}

detect_installed_version

temporary_dir="$(mktemp -d)"
trap 'cleanup_profile_files; rm -rf "${temporary_dir}"' EXIT
archive="${temporary_dir}/${archive_name}"
manifest="${temporary_dir}/SHA256SUMS"

# Verify the archive before anything is extracted or run. Every manifest line
# must be "<64 hex digits><space><space or *><file name>"; the manifest is only
# matched against patterns, never evaluated.
fail_verification() {
    echo "Checksum verification failed: $1" >&2
    echo "ARN Code was not installed." >&2
    exit 1
}

# Release resolution. The release is looked up first; nothing is downloaded
# unless it is the requested (or latest stable) release, is published, and
# lists both the archive for this platform and SHA256SUMS. SHA256SUMS comes
# first and must have an entry for the archive; the archive is downloaded
# only then, and it is checked before it is extracted or run.
if [ -n "${archive_override}" ]; then
    [ -f "${archive_override}" ] || { echo "ARN_INSTALL_ARCHIVE '${archive_override}' does not exist." >&2; exit 1; }
    [ -f "${checksums_override}" ] || { echo "ARN_INSTALL_CHECKSUMS '${checksums_override}' does not exist." >&2; exit 1; }
    cp "${checksums_override}" "${manifest}"
    echo "Installing ARN Code from local package..."
else
    if [ -n "${tag}" ]; then
        what="ARN Code ${tag}"
        api_path="releases/tags/${tag}"
    else
        what="the latest ARN Code release"
        api_path="releases/latest"
    fi
    if ! github_api "${api_path}" "${temporary_dir}/release.json"; then
        if [ "${api_status}" = 404 ] && [ -n "${tag}" ]; then
            echo "There is no ARN Code release ${tag} (no published release has that tag in ${repository})." >&2
        elif [ "${api_status}" = 404 ]; then
            echo "${repository} has no published stable ARN Code release." >&2
        else
            echo "Could not look up ${what}: ${api_problem}." >&2
        fi
        echo "ARN Code was not installed." >&2
        exit 1
    fi
    if ! summarize "${temporary_dir}/release.json" one "${temporary_dir}/release.sum" ||
        [ "$(wc -l < "${temporary_dir}/release.sum" | tr -d ' ')" != 1 ]; then
        echo "GitHub returned malformed release metadata for ${what}; ARN Code was not installed." >&2
        exit 1
    fi
    IFS='	' read -r _ found_tag is_draft is_prerelease archive_assets sums_assets metadata_valid < "${temporary_dir}/release.sum"
    if [ "${metadata_valid}" != 1 ] || { [ -n "${tag}" ] && [ "${found_tag}" != "${tag}" ]; }; then
        echo "GitHub returned malformed release metadata for ${what}; ARN Code was not installed." >&2
        exit 1
    fi
    if [ -z "${tag}" ] && { [ "${is_draft}" = 1 ] || [ "${is_prerelease}" = 1 ]; }; then
        echo "GitHub named ${found_tag}, a draft or prerelease, as the latest release; it is not installed without ARN_VERSION." >&2
        exit 1
    fi
    if [ "${is_draft}" = 1 ]; then
        echo "ARN Code ${tag} is an unpublished draft release; it is not installed." >&2
        exit 1
    fi
    if [ "${archive_assets}" -gt 1 ] || [ "${sums_assets}" -gt 1 ]; then
        echo "GitHub returned malformed release metadata for ${what} (an asset is listed twice); ARN Code was not installed." >&2
        exit 1
    fi
    if [ -z "${tag}" ]; then
        tag="${found_tag}"
        if [ "${archive_assets}" = 0 ]; then
            stop_without_release "The latest ARN Code release, ${tag}, has no ${archive_name} (no build for ${system} ${machine})."
        fi
        if [ "${sums_assets}" = 0 ]; then
            stop_without_release "The latest ARN Code release, ${tag}, has no SHA256SUMS, so its ${archive_name} cannot be verified."
        fi
    else
        if [ "${archive_assets}" = 0 ]; then
            echo "ARN Code ${tag} has no ${archive_name} (no build for ${system} ${machine}); ARN Code was not installed." >&2
            exit 1
        fi
        if [ "${sums_assets}" = 0 ]; then
            echo "ARN Code ${tag} has no SHA256SUMS, so its ${archive_name} cannot be verified; ARN Code was not installed." >&2
            exit 1
        fi
        if [ "${is_prerelease}" = 1 ]; then
            echo "Note: ${tag} is a prerelease; it is installed because ARN_VERSION names it."
        fi
    fi
    retry_command="$(install_command "${tag#v}" | sed 's/| ARN_/| ARN_ALLOW_DOWNGRADE=1 ARN_/')"
    check_downgrade "${tag#v}" ", for example:
  ${retry_command}"

    releases="https://github.com/${repository}/releases"
    echo "Downloading ARN Code ${tag} (${package})..."
    if ! curl --fail --location --show-error --silent --proto '=https' "${releases}/download/${tag}/SHA256SUMS" --output "${manifest}"; then
        echo "Could not download SHA256SUMS for ${tag}; the archive cannot be verified, so it is not installed." >&2
        exit 1
    fi
fi

[ -s "${manifest}" ] || fail_verification "SHA256SUMS is missing or empty."
if grep -Evq '^[0-9A-Fa-f]{64} [ *][A-Za-z0-9._-]+$' "${manifest}"; then
    fail_verification "SHA256SUMS contains a malformed line."
fi
# The file name starts at column 67; it is compared as a whole string, not as
# a pattern (a "." in a pattern would match any character).
entries="$(awk -v name="${archive_name}" 'substr($0, 67) == name { n++ } END { print n + 0 }' "${manifest}")"
case "${entries}" in
    0) fail_verification "SHA256SUMS has no entry for ${archive_name}." ;;
    1) ;;
    *) fail_verification "SHA256SUMS lists ${archive_name} more than once." ;;
esac
expected_hash="$(awk -v name="${archive_name}" 'substr($0, 67) == name { print tolower(substr($0, 1, 64)) }' "${manifest}")"
if [ -n "${archive_override}" ]; then
    cp "${archive_override}" "${archive}"
elif ! curl --fail --location --show-error --silent --proto '=https' "${releases}/download/${tag}/${archive_name}" --output "${archive}"; then
    echo "Could not download ${archive_name} for ${tag}." >&2
    exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
    actual_hash="$(sha256sum "${archive}" | cut -c1-64)"
elif command -v shasum >/dev/null 2>&1; then
    actual_hash="$(shasum -a 256 "${archive}" | cut -c1-64)"
else
    fail_verification "neither sha256sum nor shasum is available."
fi
actual_hash="$(printf '%s' "${actual_hash}" | tr 'A-F' 'a-f')"
if ! printf '%s\n' "${actual_hash}" | grep -Eq '^[0-9a-f]{64}$'; then
    fail_verification "could not compute the SHA-256 of ${archive_name}."
fi
if [ "${actual_hash}" != "${expected_hash}" ]; then
    fail_verification "${archive_name} has SHA-256 ${actual_hash}, but SHA256SUMS lists ${expected_hash}."
fi
echo "Verified ${archive_name} (SHA-256 ${actual_hash})."

tar -xzf "${archive}" -C "${temporary_dir}"
payload="${temporary_dir}/${package}/arn"
if [ ! -f "${payload}" ]; then payload="${temporary_dir}/arn"; fi
if [ ! -f "${payload}" ]; then
    echo "The package does not contain arn." >&2
    exit 1
fi
chmod 755 "${payload}"
version="$(${payload} --version)"
case "${version}" in arn\ [0-9]*.[0-9]*.[0-9]*) ;; *)
    echo "The downloaded ARN executable failed its version check." >&2
    exit 1
esac

new_version="$(printf '%s\n' "${version}" | awk '{ print $2 }')"
new_commit="$(printf '%s\n' "${version}" | awk '{ c = $3; gsub(/[()]/, "", c); print (c == "" ? "-" : c) }')"
if ! is_semver "${new_version}"; then
    echo "The downloaded ARN executable reports an invalid version ('${new_version}')." >&2
    exit 1
fi
# The version the package itself reports decides (for a local package it is
# the only one known).
check_downgrade "${new_version}" "."
target="${install_dir}/arn"

# Decide what the directory holds before anything in it changes: only files a
# manifest records as ARN's are replaced or removed. (An interrupted earlier
# run was recovered at the start.)
if exists "${install_dir}" && [ ! -d "${install_dir}" ]; then
    echo "${install_dir} exists but is not a directory." >&2
    exit 1
fi
previous_files=""
previous_profile="-"
previous_version="-"
dir_created=0
had_manifest=0
if exists "${manifest_path}"; then
    read_manifest || refuse "Update" "${manifest_problem}"
    had_manifest=1
    previous_files="${owned_files}"
    previous_profile="${recorded_profile}"
    dir_created="${created_dir}"
    if printf '%s\n' "${recorded_version}" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then previous_version="${recorded_version}"; fi
fi
if not_regular "${target}"; then
    refuse "Update" "${target} is a symbolic link or not a regular file, so it is not replaced."
fi
fresh=1
if [ -f "${target}" ]; then
    fresh=0
    if ! printf '%s\n' "${previous_files}" | grep -qx arn; then
        if [ "${adopt_existing}" != "1" ]; then
            refuse "Update" "${install_dir} already has arn, which no ${manifest_name} records as ARN's (installers before this one did not record their files). If it is ARN Code, run the installer again with ARN_ADOPT_EXISTING=1 to replace it."
        fi
        echo "Adopting the existing ${target} as ARN Code's file."
        previous_version="-"
    fi
fi

made_dir=0
if [ ! -d "${install_dir}" ]; then
    mkdir -p "${install_dir}"
    made_dir=1
    dir_created=1
else
    remove_stale_files
fi
work_dir="$(mktemp -d "${install_dir}/.arn-install.XXXXXX")"
work_name="${work_dir##*/}"
profile_change="-"
profile_added=0
journal_written=0
committed=0

# Until the record says "committed", any exit (an error, a signal) undoes
# what this run changed, with the same recovery a later run would use after a
# crash. A rollback never exits with status 0.
on_exit() {
    status=$?
    ignore_signals
    set +e
    test_fault rollback
    cleanup_profile_files
    if [ "${committed}" = 1 ]; then
        rm -f "${journal_path}"
        rm -rf "${work_dir}"
    elif [ "${journal_written}" = 1 ]; then
        [ "${status}" -ne 0 ] || status=1
        if read_journal && recover_transaction; then
            if [ "${fresh}" = 1 ]; then
                echo "ARN Code was not installed." >&2
            elif [ "${recovered_verified}" = 1 ]; then
                echo "The previous installation was kept; ARN Code was not updated." >&2
            else
                echo "The previous arn was put back, but it could not be verified; install ARN Code again to repair it." >&2
            fi
        else
            echo "Restoring the previous state failed: ${journal_problem:-${recovery_problem}}." >&2
            echo "${journal_path} and ${work_dir} were kept; run the installer again to finish restoring." >&2
        fi
    else
        [ "${status}" -ne 0 ] || status=1
        rm -rf "${work_dir}"
        if [ "${made_dir}" = 1 ]; then rmdir "${install_dir}" 2>/dev/null; fi
        if [ "${fresh}" = 1 ]; then
            echo "ARN Code was not installed." >&2
        else
            echo "The previous installation was kept; ARN Code was not updated." >&2
        fi
    fi
    rm -rf "${temporary_dir}"
    exit "${status}"
}
trap on_exit EXIT

write_journal staging
journal_written=1
test_fault after-journal

# Stage next to the target (the same file system), so activation is a rename,
# and keep copies of the previous arn and manifest until the end.
cp "${payload}" "${work_dir}/arn"
chmod 755 "${work_dir}/arn"
if [ "$("${work_dir}/arn" --version 2>/dev/null || true)" != "${version}" ]; then
    echo "The staged ARN executable failed its version check." >&2
    exit 1
fi
if [ "${fresh}" = 0 ]; then
    ln "${target}" "${work_dir}/arn.previous" 2>/dev/null || cp -p "${target}" "${work_dir}/arn.previous"
fi
if [ "${had_manifest}" = 1 ]; then
    cp -p "${manifest_path}" "${work_dir}/manifest.previous"
fi
test_fault after-backup
write_journal activating
test_fault activate
mv -f "${work_dir}/arn" "${target}"
test_fault after-activate
installed_version="$("${target}" --version 2>/dev/null || true)"
if [ "${ARN_INSTALL_TEST_FAULT:-}" = verify ]; then installed_version="injected test failure"; fi
if [ "${installed_version}" != "${version}" ]; then
    echo "The installed ARN executable failed its version check." >&2
    exit 1
fi

path_known=0
case ":${PATH}:" in
    *":${install_dir}:"*) path_known=1 ;;
    *)
        PATH="${install_dir}:${PATH}"
        export PATH
        ;;
esac
if ! command -v arn >/dev/null 2>&1; then
    echo "ARN Code installation verification failed: arn is not found on PATH." >&2
    exit 1
fi
# A profile that cannot be updated does not stop the installation: arn works,
# and the user is told the line to add.
if [ "${path_known}" = 0 ] && [ "${ARN_SKIP_PATH_UPDATE:-0}" != "1" ] && ! has_profile_block "${profile}"; then
    profile_change="${profile}"
    write_journal activating
    if rewrite_profile "${profile}" add; then
        profile_added=1
        echo "Added ${install_dir} to ${profile}."
    else
        echo "Warning: could not add ${install_dir} to ${profile}: ${profile_problem}." >&2
        echo "To run arn from new terminals, add this line to your shell profile: ${export_line}" >&2
    fi
    test_fault after-profile
fi
# The manifest records a profile only while it holds this installer's lines,
# so an uninstall edits nothing else.
if [ "${profile_added}" = 1 ]; then
    recorded="${profile}"
elif [ "${previous_profile}" != "-" ] && has_profile_block "${previous_profile}"; then
    recorded="${previous_profile}"
elif has_profile_block "${profile}"; then
    recorded="${profile}"
else
    recorded="-"
fi
case "${recorded}" in
    *"${newline}"*|*"${carriage_return}"*) recorded="-" ;;
esac

test_fault manifest
write_manifest "${work_dir}/manifest" "${new_version}" "${new_commit}" "${recorded}" "${dir_created}" arn
# Where there was no manifest before, this copy is what lets a rollback tell
# the manifest it is about to put in place from one someone else created.
cp "${work_dir}/manifest" "${work_dir}/manifest.written"
mv -f "${work_dir}/manifest" "${manifest_path}"
test_fault after-manifest
write_journal committed
committed=1
test_fault after-commit
rm -f "${journal_path}"
rm -rf "${work_dir}"

# Files an earlier version owned that this one no longer ships.
for name in ${previous_files}; do
    if [ "${name}" != arn ] && [ -f "${install_dir}/${name}" ] && [ ! -L "${install_dir}/${name}" ]; then
        rm -f "${install_dir}/${name}" || echo "Warning: could not remove ${install_dir}/${name}, a file of the previous ARN Code version." >&2
    fi
done

echo "${installed_version} installed to ${target}"
if [ "${system}" = "Darwin" ]; then
    if ! command -v brew >/dev/null 2>&1 || ! brew --prefix openssl@3 >/dev/null 2>&1; then
        echo "Note: the macOS build requires OpenSSL 3. Install it with: brew install openssl@3"
    fi
fi
echo "Open a new terminal, change to your project directory, and run: arn"
