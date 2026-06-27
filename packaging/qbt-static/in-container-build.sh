#!/usr/bin/env bash
set -euo pipefail

mode="${1:-}"
shift || true

repo_root="/workspace"
state_dir="${repo_root}/${QBT_STATIC_STATE_DIR}"
cache_dir="${repo_root}/${QBT_STATIC_CACHE_DIR}"
work_dir="${repo_root}/${QBT_STATIC_WORK_DIR}"
output_dir="${repo_root}/${QBT_STATIC_OUTPUT_DIR}"
log_dir="${repo_root}/${QBT_STATIC_LOG_DIR}"
upstream_dir="${cache_dir}/qbittorrent-nox-static"
host_uid="${HOST_UID:-}"
host_gid="${HOST_GID:-}"
apt_mirror="${QBT_STATIC_APT_MIRROR:-}"
gnu_mirror="${QBT_STATIC_GNU_MIRROR:-}"
glibc_tag="${QBT_STATIC_GLIBC_TAG:-}"

export GIT_CEILING_DIRECTORIES="${repo_root}"

restore_ownership() {
    local path

    if [[ "$(id -u)" -ne 0 ]]; then
        return 0
    fi

    if [[ -z "${host_uid}" || -z "${host_gid}" ]]; then
        return 0
    fi

    for path in "${state_dir}" "${cache_dir}" "${work_dir}" "${output_dir}" "${log_dir}"; do
        if [[ -e "${path}" ]]; then
            chown -R "${host_uid}:${host_gid}" "${path}" || true
        fi
    done
}

trap restore_ownership EXIT

configure_apt_access() {
    if [[ -n "${apt_mirror}" ]]; then
        cat > /etc/apt/sources.list.d/ubuntu.sources <<EOF
Types: deb
URIs: ${apt_mirror}
Suites: noble noble-updates noble-backports noble-security
Components: main universe restricted multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
    fi

    cat > /etc/apt/apt.conf.d/99qbt-force-ipv4 <<'EOF'
Acquire::ForceIPv4 "true";
EOF
}

require_host_deps() {
    local missing=0
    local tools=(
        bash
        curl
        file
        gcc
        g++
        git
        make
        patch
        perl
        pkg-config
        python3
    )

    for tool in "${tools[@]}"; do
        if ! command -v "${tool}" > /dev/null 2>&1; then
            printf 'missing required container tool: %s\n' "${tool}" >&2
            missing=1
        fi
    done

    if [[ ${missing} -ne 0 ]]; then
        exit 1
    fi
}

prepare_upstream() {
    mkdir -p "${cache_dir}" "${work_dir}" "${output_dir}" "${log_dir}"
    git config --file /root/.gitconfig --add safe.directory "${upstream_dir}"

    if [[ ! -d "${upstream_dir}/.git" ]]; then
        git clone "${QBT_STATIC_UPSTREAM_REPO}" "${upstream_dir}"
    fi

    git -C "${upstream_dir}" fetch --tags --force origin
    git -C "${upstream_dir}" checkout --force "${QBT_STATIC_UPSTREAM_REF}"

    python3 - "${upstream_dir}/qbt-nox-static.bash" "${gnu_mirror}" "${glibc_tag}" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
gnu_mirror = sys.argv[2]
glibc_tag = sys.argv[3]
lines = path.read_text(encoding="utf-8").splitlines()

if gnu_mirror:
    lines = [line.replace("https://ftpmirror.gnu.org/gnu", gnu_mirror) for line in lines]

lines = [line.replace("--retry-max-time 25", "--retry-max-time 300") for line in lines]

glibc_idx = next(
    (
        idx
        for idx, line in enumerate(lines)
        if line == '\t\tgithub_tag[glibc]="$(_git_git ls-remote -q -t --refs "${github_url[glibc]}" | awk \'/glibc-/{sub("refs/tags/", "");sub("(.*)(cvs|fedora)(.*)", ""); if($2 ~ /^glibc-[0-9]+\\.[0-9]+$/) print $2 }\' | awk \'!/^$/\' | sort -rV | head -n1)"'
    ),
    None,
)
if glibc_idx is None:
    raise SystemExit("missing expected glibc tag probe in upstream script")
lines[glibc_idx] = '\t\tgithub_tag[glibc]="${QBT_STATIC_GLIBC_TAG:-' + glibc_tag + '}"'

default_idx = next(
    (
        idx
        for idx, line in enumerate(lines)
        if line.startswith('\tgithub_tag[libtorrent]="$(_git_git ls-remote -q -t --refs "${github_url[libtorrent]}"')
    ),
    None,
)
if default_idx is None:
    raise SystemExit("missing expected default libtorrent probe in upstream script")
default_line = lines[default_idx]
lines[default_idx:default_idx + 1] = [
    '\tif [[ -n ${qbt_libtorrent_tag} ]]; then',
    '\t\tgithub_tag[libtorrent]="${qbt_libtorrent_tag}"',
    '\telse',
    default_line,
    '\tfi',
]

tag_idx = next(
    (
        idx
        for idx, line in enumerate(lines)
        if line == '\t\t\t\tgithub_tag[libtorrent]="$(_git "${github_url[libtorrent]}" -t "$2")"'
    ),
    None,
)
if tag_idx is None:
    raise SystemExit("missing expected libtorrent tag override branch in upstream script")
lines[tag_idx] = '\t\t\t\tgithub_tag[libtorrent]="$2"'

qtbase_feature_idx = next(
    (
        idx
        for idx, line in enumerate(lines)
        if line == '\t\t\t-D QT_FEATURE_gui=off -D QT_FEATURE_openssl_linked=on -D QT_FEATURE_dbus=off \\'
    ),
    None,
)
if qtbase_feature_idx is None:
    raise SystemExit("missing expected qtbase cmake feature line in upstream script")
lines[qtbase_feature_idx:qtbase_feature_idx + 1] = [
    lines[qtbase_feature_idx],
    '\t\t\t-D FEATURE_icu=OFF -D FEATURE_glib=OFF -D FEATURE_zstd=OFF \\',
    '\t\t\t-D FEATURE_brotli=OFF -D FEATURE_gssapi=OFF -D FEATURE_system_proxies=OFF \\',
    '\t\t\t-D FEATURE_sql_mysql=OFF -D FEATURE_sql_psql=OFF \\',
]

path.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY
}

bootstrap_build_dependencies() {
    if [[ "$(id -u)" -ne 0 ]]; then
        printf 'container must run as root to bootstrap build dependencies\n' >&2
        exit 1
    fi

    export DEBIAN_FRONTEND=noninteractive
    configure_apt_access

    cd "${upstream_dir}"
    bash ./qbt-nox-static.bash bootstrap_deps | tee "${log_dir}/qbt-static-bootstrap.log"
}

resolve_libtorrent_inputs() {
    local requested="${1}"
    local normalized="${requested#v}"
    local series="${normalized}"
    local tag=""

    if [[ ${normalized} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        series="${normalized%.*}"
        tag="v${normalized}"
    elif [[ ${normalized} =~ ^[0-9]+\.[0-9]+$ ]]; then
        series="${normalized}"
    elif [[ ${requested} =~ ^RC_[0-9_]+$ ]]; then
        series="${requested#RC_}"
        series="${series//_/.}"
        tag="${requested}"
    else
        printf 'unsupported pinned libtorrent input: %s\n' "${requested}" >&2
        exit 1
    fi

    printf '%s\n%s\n' "${series}" "${tag}"
}

clean_stale_qt_sql_driver_artifacts() {
    local prefix_dir="${1}"
    local sqldrivers_dir="${prefix_dir}/plugins/sqldrivers"
    local qt_sql_cmake_dir="${prefix_dir}/lib/cmake/Qt6Sql"

    rm -f \
        "${sqldrivers_dir}/libqsqlmysql.a" \
        "${sqldrivers_dir}/libqsqlmysql.prl" \
        "${sqldrivers_dir}/libqsqlpsql.a" \
        "${sqldrivers_dir}/libqsqlpsql.prl" \
        "${qt_sql_cmake_dir}/Qt6QMYSQLDriverPluginAdditionalTargetInfo.cmake" \
        "${qt_sql_cmake_dir}/Qt6QMYSQLDriverPluginConfig.cmake" \
        "${qt_sql_cmake_dir}/Qt6QMYSQLDriverPluginConfigVersion.cmake" \
        "${qt_sql_cmake_dir}/Qt6QMYSQLDriverPluginConfigVersionImpl.cmake" \
        "${qt_sql_cmake_dir}/Qt6QMYSQLDriverPluginDependencies.cmake" \
        "${qt_sql_cmake_dir}/Qt6QMYSQLDriverPluginTargets-release.cmake" \
        "${qt_sql_cmake_dir}/Qt6QMYSQLDriverPluginTargets.cmake" \
        "${qt_sql_cmake_dir}/Qt6QPSQLDriverPluginAdditionalTargetInfo.cmake" \
        "${qt_sql_cmake_dir}/Qt6QPSQLDriverPluginConfig.cmake" \
        "${qt_sql_cmake_dir}/Qt6QPSQLDriverPluginConfigVersion.cmake" \
        "${qt_sql_cmake_dir}/Qt6QPSQLDriverPluginConfigVersionImpl.cmake" \
        "${qt_sql_cmake_dir}/Qt6QPSQLDriverPluginDependencies.cmake" \
        "${qt_sql_cmake_dir}/Qt6QPSQLDriverPluginTargets-release.cmake" \
        "${qt_sql_cmake_dir}/Qt6QPSQLDriverPluginTargets.cmake"
    rm -rf \
        "${sqldrivers_dir}/objects-Release/QMYSQLDriverPlugin_init" \
        "${sqldrivers_dir}/objects-Release/QPSQLDriverPlugin_init"
}

run_build() {
    local prefix_dir="${work_dir}/prefix"
    local lt_series
    local lt_tag
    local -a lt_inputs

    prepare_upstream
    if [[ -d "${work_dir}/patches" ]]; then
        mkdir -p "${prefix_dir}/patches"
        cp -a "${work_dir}/patches/." "${prefix_dir}/patches/"
    fi
    clean_stale_qt_sql_driver_artifacts "${prefix_dir}"

    mapfile -t lt_inputs < <(resolve_libtorrent_inputs "${QBT_STATIC_LT_VERSION}")
    lt_series="${lt_inputs[0]}"
    lt_tag="${lt_inputs[1]}"

    export qbt_build_dir="${prefix_dir}"
    export qbt_qbittorrent_tag="${QBT_STATIC_QB_TAG}"
    export qbt_libtorrent_version="${lt_series}"
    export qbt_libtorrent_tag="${lt_tag}"
    export qbt_qt_version="${QBT_STATIC_QT_VERSION}"
    export qbt_build_tool="${QBT_STATIC_BUILD_TOOL}"
    export qbt_workflow_files="${QBT_STATIC_WORKFLOW_FILES}"
    export qbt_legacy_mode="no"
    export qbt_advanced_view="yes"
    export qbt_optimise_strip="yes"

    cd "${upstream_dir}"
    bootstrap_build_dependencies
    bash ./qbt-nox-static.bash all | tee "${log_dir}/qbt-static-build.log"

    if [[ -f "${prefix_dir}/completed/qbittorrent-nox" ]]; then
        cp -f "${prefix_dir}/completed/qbittorrent-nox" "${output_dir}/qbittorrent-nox"
    fi
}

case "${mode}" in
    shell)
        require_host_deps
        prepare_upstream
        clean_stale_qt_sql_driver_artifacts "${work_dir}/prefix"
        bootstrap_build_dependencies
        exec bash "$@"
        ;;
    build)
        require_host_deps
        run_build
        ;;
    *)
        printf 'unknown container mode: %s\n' "${mode}" >&2
        exit 1
        ;;
esac
