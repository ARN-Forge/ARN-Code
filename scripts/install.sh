#!/usr/bin/env sh

set -eu

repository="${ARN_REPOSITORY:-ARN-Forge/ARN-Code}"
install_dir="${ARN_INSTALL_DIR:-${HOME}/.local/bin}"
archive_override="${ARN_INSTALL_ARCHIVE:-}"
system="$(uname -s)"
machine="$(uname -m)"

case "${system}:${machine}" in
    Linux:x86_64|Linux:amd64) package="arn-linux-x64" ;;
    Darwin:arm64|Darwin:aarch64) package="arn-macos-arm64" ;;
    Darwin:x86_64|Darwin:amd64) package="arn-macos-x64" ;;
    *)
        echo "ARN Code does not have a prebuilt release for ${system} ${machine}." >&2
        exit 1
        ;;
esac

if ! command -v curl >/dev/null 2>&1 && [ -z "${archive_override}" ]; then
    echo "curl is required to install ARN Code." >&2
    exit 1
fi

temporary_dir="$(mktemp -d)"
trap 'rm -rf "${temporary_dir}"' EXIT HUP INT TERM
archive="${temporary_dir}/${package}.tar.gz"

if [ -n "${archive_override}" ]; then
    cp "${archive_override}" "${archive}"
    echo "Installing ARN Code from local package..."
else
    url="https://github.com/${repository}/releases/latest/download/${package}.tar.gz"
    echo "Downloading ${package}..."
    curl --fail --location --show-error --silent "${url}" --output "${archive}"
fi

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

mkdir -p "${install_dir}"
staged="${install_dir}/.arn-new-$$"
install -m 755 "${payload}" "${staged}"
mv -f "${staged}" "${install_dir}/arn"

case ":${PATH}:" in
    *":${install_dir}:"*) ;;
    *)
        if [ "${ARN_SKIP_PATH_UPDATE:-0}" != "1" ]; then
            profile="${ARN_PROFILE:-${HOME}/.profile}"
            case "${SHELL:-}" in
                */zsh) profile="${ARN_PROFILE:-${HOME}/.zprofile}" ;;
            esac
            marker="# Added by ARN Code installer: ${install_dir}"
            if [ ! -f "${profile}" ] || ! grep -Fq "${marker}" "${profile}"; then
                {
                    printf '\n%s\n' "${marker}"
                    printf 'export PATH="%s:$PATH"\n' "${install_dir}"
                } >> "${profile}"
            fi
            echo "Added ${install_dir} to ${profile}."
        fi
        PATH="${install_dir}:${PATH}"
        export PATH
        ;;
esac

installed_version="$(${install_dir}/arn --version)"
if [ "${installed_version}" != "${version}" ] || ! command -v arn >/dev/null 2>&1; then
    echo "ARN Code installation verification failed." >&2
    exit 1
fi

echo "${installed_version} installed to ${install_dir}/arn"
if [ "${system}" = "Darwin" ]; then
    if ! command -v brew >/dev/null 2>&1 || ! brew --prefix openssl@3 >/dev/null 2>&1; then
        echo "Note: the macOS build requires OpenSSL 3. Install it with: brew install openssl@3"
    fi
fi
echo "Open a new terminal, change to your project directory, and run: arn"
