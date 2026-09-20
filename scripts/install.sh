#!/usr/bin/env sh

set -eu

repository="${ARN_REPOSITORY:-arnecto/arn}"
install_dir="${ARN_INSTALL_DIR:-${HOME}/.local/bin}"
system="$(uname -s)"
machine="$(uname -m)"

case "${system}:${machine}" in
    Linux:x86_64|Linux:amd64)
        package="arn-linux-x64"
        ;;
    Darwin:arm64|Darwin:aarch64)
        package="arn-macos-arm64"
        ;;
    Darwin:x86_64|Darwin:amd64)
        package="arn-macos-x64"
        ;;
    *)
        echo "ARN does not have a prebuilt release for ${system} ${machine}." >&2
        exit 1
        ;;
esac

if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required to install ARN." >&2
    exit 1
fi

temporary_dir="$(mktemp -d)"
trap 'rm -rf "${temporary_dir}"' EXIT HUP INT TERM

archive="${temporary_dir}/${package}.tar.gz"
url="https://github.com/${repository}/releases/latest/download/${package}.tar.gz"

echo "Downloading ${package}..."
curl --fail --location --show-error --silent "${url}" --output "${archive}"
tar -xzf "${archive}" -C "${temporary_dir}"

mkdir -p "${install_dir}"
install -m 755 "${temporary_dir}/${package}/arn" "${install_dir}/arn"

echo "ARN installed to ${install_dir}/arn"
case ":${PATH}:" in
    *":${install_dir}:"*) ;;
    *)
        echo "Add ${install_dir} to PATH, then open a new terminal."
        echo "For example: export PATH=\"${install_dir}:\$PATH\""
        ;;
esac

if [ "${system}" = "Darwin" ]; then
    if ! command -v brew >/dev/null 2>&1 || ! brew --prefix openssl@3 >/dev/null 2>&1; then
        echo "Note: the macOS build requires OpenSSL 3. Install it with: brew install openssl@3"
    fi
fi

echo "Run: arn"
