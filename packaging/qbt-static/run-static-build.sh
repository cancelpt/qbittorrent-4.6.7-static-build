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

usage() {
    cat <<'EOF'
Usage: packaging/qbt-static/run-static-build.sh <command>

Commands:
  plan      Print resolved pinned build inputs as JSON
  prepare   Create build/cache/log/output directories
  prepare-overlay  Stage repository patch overlays into the build work area
  build     Run the static build inside the pinned container
  shell     Open an interactive shell inside the pinned container
EOF
}

print_plan() {
    python3 - <<'PY'
import json
import os

plan = {
    "container_image": os.environ["QBT_STATIC_CONTAINER_IMAGE"],
    "container_user": "root",
    "dependency_bootstrap": "upstream-bootstrap_deps",
    "ownership_restore": "HOST_UID/HOST_GID",
    "upstream_repo": os.environ["QBT_STATIC_UPSTREAM_REPO"],
    "upstream_ref": os.environ["QBT_STATIC_UPSTREAM_REF"],
    "qbittorrent_tag": os.environ["QBT_STATIC_QB_TAG"],
    "preferred_libtorrent": os.environ["QBT_STATIC_LT_VERSION"],
    "fallback_libtorrent": os.environ["QBT_STATIC_LT_FALLBACK_VERSION"],
    "qt_version": os.environ["QBT_STATIC_QT_VERSION"],
    "build_tool": os.environ["QBT_STATIC_BUILD_TOOL"],
    "workflow_files": os.environ["QBT_STATIC_WORKFLOW_FILES"],
    "glibc_tag": os.environ["QBT_STATIC_GLIBC_TAG"],
    "apt_mirror": os.environ["QBT_STATIC_APT_MIRROR"],
    "gnu_mirror": os.environ["QBT_STATIC_GNU_MIRROR"],
    "state_dir": os.environ["QBT_STATIC_STATE_DIR"],
    "output_dir": os.environ["QBT_STATIC_OUTPUT_DIR"],
    "log_dir": os.environ["QBT_STATIC_LOG_DIR"],
    "cache_dir": os.environ["QBT_STATIC_CACHE_DIR"],
    "work_dir": os.environ["QBT_STATIC_WORK_DIR"],
    "patch_overlay_dir": os.environ["QBT_STATIC_PATCH_OVERLAY_DIR"],
    "lt_retention_overlay_source_dir": os.environ["QBT_STATIC_LT_RETENTION_OVERLAY_SOURCE_DIR"],
}
print(json.dumps(plan, indent=2, sort_keys=True))
PY
}

prepare_dirs() {
    mkdir -p \
        "${REPO_ROOT}/${QBT_STATIC_STATE_DIR}" \
        "${REPO_ROOT}/${QBT_STATIC_OUTPUT_DIR}" \
        "${REPO_ROOT}/${QBT_STATIC_LOG_DIR}" \
        "${REPO_ROOT}/${QBT_STATIC_CACHE_DIR}" \
        "${REPO_ROOT}/${QBT_STATIC_WORK_DIR}" \
        "${REPO_ROOT}/${QBT_STATIC_PATCH_OVERLAY_DIR}"
}

stage_overlay() {
    local source_dir="${REPO_ROOT}/${QBT_STATIC_PATCH_OVERLAY_DIR}"
    local source_overlay_dir="${REPO_ROOT}/${QBT_STATIC_LT_RETENTION_OVERLAY_SOURCE_DIR}"
    local stage_root="${REPO_ROOT}/${QBT_STATIC_WORK_DIR}/patches"
    local qb_stage_dir="${QBT_STATIC_WORK_DIR}/patches/qbittorrent/4.6.7"
    local source_overlay_stage_root="${REPO_ROOT}/${QBT_STATIC_WORK_DIR}/source-overlays"
    local source_overlay_stage_dir="${source_overlay_stage_root}/lt-retention-narrow"

    prepare_dirs
    rm -rf "${stage_root}"
    rm -rf "${source_overlay_stage_root}"
    mkdir -p "${stage_root}"
    rsync -a --delete "${source_dir}/" "${stage_root}/"
    mkdir -p "${source_overlay_stage_root}"
    rsync -a --delete "${source_overlay_dir}/" "${source_overlay_stage_dir}/"
    printf '%s\n' "${qb_stage_dir}"
}

run_container() {
    local mode="${1}"
    local -a docker_flags=(--rm -i)
    shift || true

    prepare_dirs

    if [[ "${mode}" == "shell" ]]; then
        docker_flags+=(-t)
    fi

    docker run "${docker_flags[@]}" \
        --network=host \
        -e HOST_UID="$(id -u)" \
        -e HOST_GID="$(id -g)" \
        -e QBT_STATIC_UPSTREAM_REPO \
        -e QBT_STATIC_UPSTREAM_REF \
        -e QBT_STATIC_QB_TAG \
        -e QBT_STATIC_LT_VERSION \
        -e QBT_STATIC_LT_FALLBACK_VERSION \
        -e QBT_STATIC_QT_VERSION \
        -e QBT_STATIC_BUILD_TOOL \
        -e QBT_STATIC_WORKFLOW_FILES \
        -e QBT_STATIC_GLIBC_TAG \
        -e QBT_STATIC_APT_MIRROR \
        -e QBT_STATIC_GNU_MIRROR \
        -e QBT_STATIC_STATE_DIR \
        -e QBT_STATIC_OUTPUT_DIR \
        -e QBT_STATIC_LOG_DIR \
        -e QBT_STATIC_CACHE_DIR \
        -e QBT_STATIC_WORK_DIR \
        -e QBT_STATIC_PATCH_OVERLAY_DIR \
        -e QBT_STATIC_LT_RETENTION_OVERLAY_SOURCE_DIR \
        -v "${REPO_ROOT}:/workspace" \
        -w /tmp \
        "${QBT_STATIC_CONTAINER_IMAGE}" \
        bash "/workspace/packaging/qbt-static/in-container-build.sh" "${mode}" "$@"
}

main() {
    local command="${1:-}"

    case "${command}" in
        plan)
            print_plan
            ;;
        prepare)
            prepare_dirs
            ;;
        prepare-overlay)
            stage_overlay
            ;;
        build)
            shift
            stage_overlay > /dev/null
            run_container build "$@"
            ;;
        shell)
            shift
            stage_overlay > /dev/null
            run_container shell "$@"
            ;;
        ""|-h|--help|help)
            usage
            ;;
        *)
            printf 'unknown command: %s\n\n' "${command}" >&2
            usage >&2
            exit 1
            ;;
    esac
}

main "$@"
