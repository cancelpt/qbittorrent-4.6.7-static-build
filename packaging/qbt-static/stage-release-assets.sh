#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CONFIG_PATH="${SCRIPT_DIR}/config.env"

if [[ ! -f "${CONFIG_PATH}" ]]; then
    printf 'missing config: %s\n' "${CONFIG_PATH}" >&2
    exit 1
fi

set -a
# shellcheck disable=SC1090
source "${CONFIG_PATH}"
set +a

version="${QBT_STATIC_QB_TAG#release-}"
release_dir="${1:-${REPO_ROOT}/${QBT_STATIC_STATE_DIR}/release}"
source_binary="${2:-${REPO_ROOT}/${QBT_STATIC_OUTPUT_DIR}/qbittorrent-nox}"
staged_binary_name="qbittorrent-nox-${version}-linux-x86_64-static"
staged_binary_path="${release_dir}/${staged_binary_name}"
checksum_path="${release_dir}/SHA256SUMS"
signature_path="${release_dir}/SHA256SUMS.asc"

if [[ ! -f "${source_binary}" ]]; then
    printf 'missing built artifact: %s\n' "${source_binary}" >&2
    exit 1
fi

mkdir -p "${release_dir}"
rm -f "${staged_binary_path}" "${checksum_path}" "${signature_path}"
install -m 0755 "${source_binary}" "${staged_binary_path}"

(
    cd "${release_dir}"
    sha256sum "${staged_binary_name}" > "SHA256SUMS"
)

if [[ -n "${QBT_RELEASE_GPG_KEY_ID:-}" ]]; then
    if ! command -v gpg > /dev/null 2>&1; then
        printf 'gpg is required when QBT_RELEASE_GPG_KEY_ID is set\n' >&2
        exit 1
    fi

    gpg_args=(
        --batch
        --yes
        --armor
        --detach-sign
        --local-user "${QBT_RELEASE_GPG_KEY_ID}"
    )

    if [[ -n "${QBT_RELEASE_GPG_PASSPHRASE:-}" ]]; then
        gpg_args+=(
            --pinentry-mode loopback
            --passphrase "${QBT_RELEASE_GPG_PASSPHRASE}"
        )
    fi

    gpg "${gpg_args[@]}" --output "${signature_path}" "${checksum_path}"
fi

printf '%s\n' "${release_dir}"
