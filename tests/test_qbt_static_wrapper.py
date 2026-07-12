#!/usr/bin/env python3

import json
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
WRAPPER = REPO_ROOT / "packaging" / "qbt-static" / "run-static-build.sh"
IN_CONTAINER = REPO_ROOT / "packaging" / "qbt-static" / "in-container-build.sh"
PATCH_SOURCE_DIR = REPO_ROOT / "packaging" / "qbt-static" / "patches" / "qbittorrent" / "4.6.7"
PATCH_SERIES = PATCH_SOURCE_DIR / "series"
SOURCE_OVERRIDE_DIR = PATCH_SOURCE_DIR / "source"
PATCH_STAGE_DIR = REPO_ROOT / "build" / "qbt-static" / "work" / "patches" / "qbittorrent" / "4.6.7"
SOURCE_OVERRIDE_STAGE_DIR = PATCH_STAGE_DIR / "source"
LT_RETENTION_OVERLAY_SOURCE_DIR = REPO_ROOT / "packaging" / "qbt-source-overlays" / "lt-retention-narrow"
LT_RETENTION_OVERLAY_STAGE_DIR = REPO_ROOT / "build" / "qbt-static" / "work" / "source-overlays" / "lt-retention-narrow"
STAGE_RELEASE_ASSETS = REPO_ROOT / "packaging" / "qbt-static" / "stage-release-assets.sh"
COMPAT_PATCH = "9000-static-qt610-compat.diff"
NARROW_PAYLOAD_PATCH = "0005-feat-narrow-steady-state-payload-policy.patch"
FULL_UPDATE_SCROLL_PATCH = "0006-fix-preserve-virtual-scroll-on-full-update.patch"
ALERT_QUEUE_ROLLBACK_PATCH = "0006-revert-alert-queue-to-stock-qb-policy.patch"
PUBLIC_RELEASE_VERSION = "v4.6.7-lt1.2.20-r2"
PUBLIC_RELEASE_ASSET_NAME = f"qbittorrent-nox-{PUBLIC_RELEASE_VERSION}-linux-x86_64-static"
LEGACY_RELEASE_ASSET_NAME = "qbittorrent-nox-4.6.7-linux-x86_64-static"
MEMORY_PROBE_SOURCE_MARKERS = {
    "src/base/preferences.cpp": (
        'value(u"BitTorrent/ExperimentalWebUIMemoryProbeEnabled"_s, false)',
        'value<int>(u"BitTorrent/ExperimentalWebUIMemoryProbeIntervalMs"_s, 5000)',
        'value(u"BitTorrent/ExperimentalWebUIMemoryProbeOutputPath"_s, QString {}).trimmed()',
    ),
    "src/base/preferences.h": (
        "bool isExperimentalWebUIMemoryProbeEnabled() const;",
        "int experimentalWebUIMemoryProbeIntervalMs() const;",
        "QString experimentalWebUIMemoryProbeOutputPath() const;",
    ),
    "src/webui/api/maindatasyncstore.cpp": (
        "m_memoryProbe.outputPath = preferences->experimentalWebUIMemoryProbeOutputPath();",
        "m_memoryProbe.enabled = preferences->isExperimentalWebUIMemoryProbeEnabled() && !m_memoryProbe.outputPath.isEmpty();",
        'emitMemoryProbeEvent(u"tick"_s, {});',
        "QVariantMap WebUISync::MaindataSyncStore::collectMemoryProbeSample() const",
        "void WebUISync::MaindataSyncStore::emitMemoryProbeEvent",
    ),
    "src/webui/api/maindatasyncstore.h": (
        "void startMemoryProbeTimer();",
        "QVariantMap collectMemoryProbeSample() const;",
        "MemoryProbeState m_memoryProbe;",
    ),
}
SCROLLFIX_WEBUI_MARKERS = {
    "src/webui/www/private/scripts/client.js": (
        "captureViewportState",
        "restoreViewportState",
        "filterExistingRowIds",
    ),
    "src/webui/www/private/scripts/dynamicTable.js": (
        "captureViewportState",
        "restoreViewportState",
        "filterExistingRowIds",
    ),
}
STOCK_ALERT_QUEUE_LINE = (
    "settingsPack.set_int(lt::settings_pack::alert_queue_size, "
    "std::numeric_limits<int>::max() / 2);"
)
BOUNDED_ALERT_QUEUE_LINE = (
    "settingsPack.set_int(lt::settings_pack::alert_queue_size, "
    "Preferences::instance()->sessionAlertQueueSize());"
)
SESSIONIMPL_WITH_BOUNDED_ALERT_QUEUE = """lt::settings_pack SessionImpl::loadLTSettings() const
{
    settingsPack.set_int(lt::settings_pack::active_tracker_limit, -1);
    settingsPack.set_int(lt::settings_pack::active_dht_limit, -1);
    settingsPack.set_int(lt::settings_pack::active_lsd_limit, -1);
    settingsPack.set_int(lt::settings_pack::alert_queue_size, Preferences::instance()->sessionAlertQueueSize());

    // Outgoing ports
    settingsPack.set_int(lt::settings_pack::outgoing_port, outgoingPortsMin());
}
"""
UPSTREAM_TEST_CMAKELISTS = """if (QT6)
    find_package(Qt6 REQUIRED COMPONENTS Test)
else()
    find_package(Qt5 REQUIRED COMPONENTS Test)
endif()

enable_testing(true)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure)

include_directories("../src")

set(testFiles
    testalgorithm.cpp
    testbittorrenttrackerentry.cpp
    testglobal.cpp
    testorderedset.cpp
    testpath.cpp
    testutilscompare.cpp
    testutilsbytearray.cpp
    testutilsgzip.cpp
    testutilsio.cpp
    testutilsstring.cpp
    testutilsversion.cpp
)

foreach(testFile ${testFiles})
    get_filename_component(testFilename "${testFile}" NAME_WLE)

    add_executable("${testFilename}" "${testFile}")
    target_link_libraries("${testFilename}" PRIVATE Qt::Test qbt_base)
    add_test(NAME "${testFilename}" COMMAND "${testFilename}")

    add_dependencies(check "${testFilename}")
endforeach()
"""


def extract_patch_for_file(patch_path: Path, relative_path: str) -> str:
    patch_lines = patch_path.read_text(encoding="utf-8").splitlines(keepends=True)
    collected_lines: list[str] = []
    collecting = False

    for line in patch_lines:
        diff_match = re.match(r"^diff --git a/(.+) b/(.+)$", line.rstrip("\n"))
        if diff_match:
            old_path, new_path = diff_match.groups()
            if collecting:
                break
            collecting = (old_path == relative_path) and (new_path == relative_path)

        if collecting:
            collected_lines.append(line)

    if not collected_lines:
        raise AssertionError(f"missing patch block for {relative_path}: {patch_path}")

    return "".join(collected_lines)


class QbtStaticWrapperTest(unittest.TestCase):
    maxDiff = None

    def test_plan_reports_pinned_static_build_inputs(self) -> None:
        self.assertTrue(WRAPPER.is_file(), f"missing wrapper script: {WRAPPER}")

        result = subprocess.run(
            ["bash", str(WRAPPER), "plan"],
            check=True,
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
        )
        plan = json.loads(result.stdout)

        self.assertEqual(plan["container_image"], "buildpack-deps:noble")
        self.assertEqual(plan["qbittorrent_tag"], "release-4.6.7")
        self.assertEqual(plan["public_release_version"], PUBLIC_RELEASE_VERSION)
        self.assertEqual(plan["preferred_libtorrent"], "1.2.20")
        self.assertEqual(plan["fallback_libtorrent"], "1.2.19")
        self.assertEqual(plan["qt_version"], "6")
        self.assertEqual(plan["build_tool"], "cmake")
        self.assertEqual(plan["workflow_files"], "yes")
        self.assertEqual(plan["gnu_mirror"], "https://mirrors.sjtug.sjtu.edu.cn/gnu")
        self.assertEqual(plan["state_dir"], "build/qbt-static")
        self.assertEqual(plan["output_dir"], "build/qbt-static/output")
        self.assertEqual(plan["log_dir"], "build/qbt-static/logs")
        self.assertEqual(plan["cache_dir"], "build/qbt-static/cache")
        self.assertEqual(plan["patch_overlay_dir"], "packaging/qbt-static/patches")
        self.assertEqual(
            plan["lt_retention_overlay_source_dir"],
            "packaging/qbt-source-overlays/lt-retention-narrow",
        )

    def test_prepare_overlay_stages_local_qbittorrent_patch_series(self) -> None:
        self.assertTrue(PATCH_SOURCE_DIR.is_dir(), f"missing patch source dir: {PATCH_SOURCE_DIR}")
        self.assertTrue(
            LT_RETENTION_OVERLAY_SOURCE_DIR.is_dir(),
            f"missing lt-retention source overlay dir: {LT_RETENTION_OVERLAY_SOURCE_DIR}",
        )

        result = subprocess.run(
            ["bash", str(WRAPPER), "prepare-overlay"],
            check=True,
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.stdout.strip(), str(PATCH_STAGE_DIR.relative_to(REPO_ROOT)))
        self.assertTrue(PATCH_STAGE_DIR.is_dir(), f"missing staged patch dir: {PATCH_STAGE_DIR}")

        source_patch_names = sorted(path.name for path in PATCH_SOURCE_DIR.glob("*.patch"))
        self.assertGreaterEqual(len(source_patch_names), 5, "expected exported qB patch series")
        staged_patch_names = sorted(path.name for path in PATCH_STAGE_DIR.glob("*.patch"))
        self.assertEqual(staged_patch_names, source_patch_names)
        self.assertIn(NARROW_PAYLOAD_PATCH, source_patch_names)
        self.assertIn(FULL_UPDATE_SCROLL_PATCH, source_patch_names)
        self.assertIn(ALERT_QUEUE_ROLLBACK_PATCH, source_patch_names)
        source_series = PATCH_SERIES.read_text(encoding="utf-8").splitlines()
        staged_series = (PATCH_STAGE_DIR / "series").read_text(encoding="utf-8").splitlines()
        self.assertEqual(staged_series, source_series)
        self.assertEqual(source_series.count(FULL_UPDATE_SCROLL_PATCH), 1, source_series)
        self.assertEqual(source_series.count(ALERT_QUEUE_ROLLBACK_PATCH), 1, source_series)
        self.assertEqual(
            source_series[-3:],
            [NARROW_PAYLOAD_PATCH, FULL_UPDATE_SCROLL_PATCH, ALERT_QUEUE_ROLLBACK_PATCH],
        )
        self.assertTrue((PATCH_SOURCE_DIR / COMPAT_PATCH).is_file(), "missing isolated static-toolchain compatibility patch")
        self.assertTrue((PATCH_STAGE_DIR / COMPAT_PATCH).is_file(), "missing staged static-toolchain compatibility patch")
        self.assertTrue(SOURCE_OVERRIDE_DIR.is_dir(), "missing qB source override directory")
        self.assertTrue(SOURCE_OVERRIDE_STAGE_DIR.is_dir(), "missing staged qB source override directory")
        self.assertTrue(
            (LT_RETENTION_OVERLAY_STAGE_DIR / "src/base/bittorrent/torrentstatusquery.h").is_file(),
            "missing staged lt-retention source overlay header",
        )

    def test_source_override_keeps_memory_probe_and_stock_alert_queue_policy(self) -> None:
        self.assertTrue(SOURCE_OVERRIDE_DIR.is_dir(), f"missing source override dir: {SOURCE_OVERRIDE_DIR}")

        for relative_path, markers in MEMORY_PROBE_SOURCE_MARKERS.items():
            source_path = SOURCE_OVERRIDE_DIR / relative_path
            self.assertTrue(source_path.is_file(), f"missing memory probe source override: {relative_path}")
            source_text = source_path.read_text(encoding="utf-8")
            for marker in markers:
                self.assertIn(marker, source_text, f"missing memory probe marker in {relative_path}: {marker}")

        sessionimpl_text = (SOURCE_OVERRIDE_DIR / "src/base/bittorrent/sessionimpl.cpp").read_text(encoding="utf-8")
        self.assertIn(STOCK_ALERT_QUEUE_LINE, sessionimpl_text)
        self.assertNotIn(BOUNDED_ALERT_QUEUE_LINE, sessionimpl_text)

    def test_prepare_overlay_stages_memory_probe_source_override_and_stock_alert_queue_policy(self) -> None:
        subprocess.run(
            ["bash", str(WRAPPER), "prepare-overlay"],
            check=True,
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
        )

        self.assertTrue(SOURCE_OVERRIDE_STAGE_DIR.is_dir(), "missing staged source override dir")
        for relative_path, markers in MEMORY_PROBE_SOURCE_MARKERS.items():
            staged_source_path = SOURCE_OVERRIDE_STAGE_DIR / relative_path
            self.assertTrue(staged_source_path.is_file(), f"missing staged memory probe source: {relative_path}")
            staged_source_text = staged_source_path.read_text(encoding="utf-8")
            for marker in markers:
                self.assertIn(marker, staged_source_text, f"missing staged memory probe marker in {relative_path}: {marker}")

        staged_sessionimpl_text = (SOURCE_OVERRIDE_STAGE_DIR / "src/base/bittorrent/sessionimpl.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn(STOCK_ALERT_QUEUE_LINE, staged_sessionimpl_text)
        self.assertNotIn(BOUNDED_ALERT_QUEUE_LINE, staged_sessionimpl_text)

    def test_source_override_keeps_scrollfix_markers_in_webui_scripts(self) -> None:
        self.assertTrue(SOURCE_OVERRIDE_DIR.is_dir(), f"missing source override dir: {SOURCE_OVERRIDE_DIR}")

        for relative_path, markers in SCROLLFIX_WEBUI_MARKERS.items():
            source_path = SOURCE_OVERRIDE_DIR / relative_path
            self.assertTrue(source_path.is_file(), f"missing scrollfix source override: {relative_path}")
            source_text = source_path.read_text(encoding="utf-8")
            for marker in markers:
                self.assertIn(marker, source_text, f"missing scrollfix marker in {relative_path}: {marker}")

    def test_prepare_overlay_stages_scrollfix_markers_in_webui_scripts(self) -> None:
        subprocess.run(
            ["bash", str(WRAPPER), "prepare-overlay"],
            check=True,
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
        )

        self.assertTrue(SOURCE_OVERRIDE_STAGE_DIR.is_dir(), "missing staged source override dir")
        for relative_path, markers in SCROLLFIX_WEBUI_MARKERS.items():
            staged_source_path = SOURCE_OVERRIDE_STAGE_DIR / relative_path
            self.assertTrue(staged_source_path.is_file(), f"missing staged scrollfix source: {relative_path}")
            staged_source_text = staged_source_path.read_text(encoding="utf-8")
            for marker in markers:
                self.assertIn(marker, staged_source_text, f"missing staged scrollfix marker in {relative_path}: {marker}")

    def test_stage_release_assets_uses_canonical_public_release_version_filename(self) -> None:
        self.assertTrue(STAGE_RELEASE_ASSETS.is_file(), f"missing stage release script: {STAGE_RELEASE_ASSETS}")

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_path = Path(tmpdir)
            release_dir = tmp_path / "release"
            source_binary = tmp_path / "qbittorrent-nox"
            source_binary.write_text("fake-binary", encoding="utf-8")

            result = subprocess.run(
                ["bash", str(STAGE_RELEASE_ASSETS), str(release_dir), str(source_binary)],
                check=True,
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.stdout.strip(), str(release_dir))
            staged_binary = release_dir / PUBLIC_RELEASE_ASSET_NAME
            self.assertTrue(staged_binary.is_file(), f"missing staged binary: {staged_binary}")
            self.assertFalse((release_dir / LEGACY_RELEASE_ASSET_NAME).exists())

            checksum_text = (release_dir / "SHA256SUMS").read_text(encoding="utf-8")
            self.assertIn(PUBLIC_RELEASE_ASSET_NAME, checksum_text)
            self.assertNotIn(LEGACY_RELEASE_ASSET_NAME, checksum_text)

    def test_narrow_payload_patch_dry_runs_after_prior_test_cmakelists_patches(self) -> None:
        file_relative_path = "test/CMakeLists.txt"
        prerequisite_patch_names = (
            "0001-Add-session-alert-queue-size-preference-getter-and-t.patch",
            "0004-feat-share-WebUI-maindata-state-and-virtualize-torre.patch",
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            tree_root = Path(tmpdir)
            target_file = tree_root / file_relative_path
            target_file.parent.mkdir(parents=True, exist_ok=True)
            target_file.write_text(UPSTREAM_TEST_CMAKELISTS, encoding="utf-8")

            for patch_name in prerequisite_patch_names:
                patch_text = extract_patch_for_file(PATCH_SOURCE_DIR / patch_name, file_relative_path)
                result = subprocess.run(
                    ["patch", "-p1"],
                    input=patch_text,
                    cwd=tree_root,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            narrow_patch_text = extract_patch_for_file(PATCH_SOURCE_DIR / NARROW_PAYLOAD_PATCH, file_relative_path)
            dry_run = subprocess.run(
                ["patch", "-p1", "--dry-run"],
                input=narrow_patch_text,
                cwd=tree_root,
                capture_output=True,
                text=True,
            )

            self.assertEqual(dry_run.returncode, 0, dry_run.stdout + dry_run.stderr)

    def test_alert_queue_runtime_rollback_patch_is_appended_and_dry_runs(self) -> None:
        rollback_patch_path = PATCH_SOURCE_DIR / ALERT_QUEUE_ROLLBACK_PATCH
        self.assertTrue(rollback_patch_path.is_file(), f"missing rollback patch: {rollback_patch_path}")

        series_lines = PATCH_SERIES.read_text(encoding="utf-8").splitlines()
        self.assertGreaterEqual(len(series_lines), 6, "expected rollback patch appended after existing qB series")
        self.assertEqual(
            series_lines[-3:],
            [NARROW_PAYLOAD_PATCH, FULL_UPDATE_SCROLL_PATCH, ALERT_QUEUE_ROLLBACK_PATCH],
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            tree_root = Path(tmpdir)
            target_file = tree_root / "src/base/bittorrent/sessionimpl.cpp"
            target_file.parent.mkdir(parents=True, exist_ok=True)
            target_file.write_text(SESSIONIMPL_WITH_BOUNDED_ALERT_QUEUE, encoding="utf-8")

            rollback_patch_text = extract_patch_for_file(rollback_patch_path, "src/base/bittorrent/sessionimpl.cpp")
            dry_run = subprocess.run(
                ["patch", "-p1", "--dry-run"],
                input=rollback_patch_text,
                cwd=tree_root,
                capture_output=True,
                text=True,
            )

            self.assertEqual(dry_run.returncode, 0, dry_run.stdout + dry_run.stderr)

    def test_python_rewrite_snippet_pins_glibc_libtorrent_and_qb_tags(self) -> None:
        in_container_text = IN_CONTAINER.read_text(encoding="utf-8")
        match = re.search(r"<<'PY'\n(.*)\nPY", in_container_text, re.S)
        self.assertIsNotNone(match, "missing embedded python rewrite snippet")
        rewrite_script = match.group(1)

        upstream_stub = "\n".join(
            [
                "https://ftpmirror.gnu.org/gnu",
                "--retry-max-time 25",
                '\t\tgithub_tag[glibc]="$(_git_git ls-remote -q -t --refs "${github_url[glibc]}" | awk \'/glibc-/{sub("refs/tags/", "");sub("(.*)(cvs|fedora)(.*)", ""); if($2 ~ /^glibc-[0-9]+\\.[0-9]+$/) print $2 }\' | awk \'!/^$/\' | sort -rV | head -n1)"',
                '\tgithub_tag[libtorrent]="$(_git_git ls-remote -q -t --refs "${github_url[libtorrent]}" | awk \'/v1\\.2\\./{print $2}\' | sort -rV | head -n1)"',
                '\t\t\t\tgithub_tag[libtorrent]="$(_git "${github_url[libtorrent]}" -t "$2")"',
                '\t\t\t\tgithub_tag[qbittorrent]="$(_git "${github_url[qbittorrent]}" -t "$2")"',
                '\t\t\t-D QT_FEATURE_gui=off -D QT_FEATURE_openssl_linked=on -D QT_FEATURE_dbus=off \\',
            ]
        ) + "\n"

        with tempfile.TemporaryDirectory() as tmpdir:
            stub_path = Path(tmpdir) / "qbt-nox-static.bash"
            stub_path.write_text(upstream_stub, encoding="utf-8")

            result = subprocess.run(
                ["python3", "-", str(stub_path), "https://mirror.example/gnu", "glibc-2.43"],
                input=rewrite_script,
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            rewritten = stub_path.read_text(encoding="utf-8")

        self.assertIn("https://mirror.example/gnu", rewritten)
        self.assertIn("--retry-max-time 300", rewritten)
        self.assertIn('github_tag[glibc]="${QBT_STATIC_GLIBC_TAG:-glibc-2.43}"', rewritten)
        self.assertIn('if [[ -n ${qbt_libtorrent_tag} ]]; then', rewritten)
        self.assertIn('github_tag[libtorrent]="${qbt_libtorrent_tag}"', rewritten)
        self.assertIn('github_tag[libtorrent]="$2"', rewritten)
        self.assertIn('github_tag[qbittorrent]="$2"', rewritten)
        self.assertIn("FEATURE_icu=OFF", rewritten)
        self.assertIn("FEATURE_system_proxies=OFF", rewritten)
        self.assertIn("FEATURE_sql_mysql=OFF", rewritten)
        self.assertIn("FEATURE_sql_psql=OFF", rewritten)

    def test_container_entrypoint_bootstraps_dependencies_as_root_and_restores_host_ownership(self) -> None:
        wrapper_text = WRAPPER.read_text(encoding="utf-8")
        in_container_text = IN_CONTAINER.read_text(encoding="utf-8")

        self.assertNotIn('--user "$(id -u):$(id -g)"', wrapper_text)
        self.assertNotIn("-w /workspace", wrapper_text)
        self.assertIn("-w /tmp", wrapper_text)
        self.assertNotIn("docker run --rm -it", wrapper_text)
        self.assertIn('local -a docker_flags=(--rm -i)', wrapper_text)
        self.assertIn('if [[ "${mode}" == "shell" ]]; then', wrapper_text)
        self.assertIn("docker_flags+=(-t)", wrapper_text)
        self.assertIn('-e HOST_UID="$(id -u)"', wrapper_text)
        self.assertIn('-e HOST_GID="$(id -g)"', wrapper_text)
        self.assertIn("-e QBT_STATIC_WORKFLOW_FILES", wrapper_text)
        self.assertIn("-e QBT_STATIC_APT_MIRROR", wrapper_text)
        self.assertIn("-e QBT_STATIC_GNU_MIRROR", wrapper_text)
        self.assertIn("bootstrap_deps", in_container_text)
        self.assertIn("restore_ownership", in_container_text)
        self.assertIn("trap restore_ownership EXIT", in_container_text)
        self.assertIn("safe.directory", in_container_text)
        self.assertIn("QBT_STATIC_APT_MIRROR", in_container_text)
        self.assertIn("QBT_STATIC_GNU_MIRROR", in_container_text)
        self.assertIn("ftpmirror.gnu.org/gnu", in_container_text)
        self.assertIn('github_tag[libtorrent]="$2"', in_container_text)
        self.assertIn('github_tag[qbittorrent]="$2"', in_container_text)
        self.assertIn("missing expected default libtorrent probe", in_container_text)
        self.assertIn("missing expected qbittorrent tag override branch", in_container_text)
        self.assertIn("--retry-max-time 300", in_container_text)
        self.assertIn("Acquire::ForceIPv4", in_container_text)
        self.assertIn("resolve_libtorrent_inputs", in_container_text)
        self.assertIn("FEATURE_icu=OFF", in_container_text)
        self.assertIn("FEATURE_glib=OFF", in_container_text)
        self.assertIn("FEATURE_zstd=OFF", in_container_text)
        self.assertIn("FEATURE_brotli=OFF", in_container_text)
        self.assertIn("FEATURE_gssapi=OFF", in_container_text)
        self.assertIn("FEATURE_system_proxies=OFF", in_container_text)
        self.assertIn("FEATURE_sql_mysql=OFF", in_container_text)
        self.assertIn("FEATURE_sql_psql=OFF", in_container_text)
        self.assertIn("libqsqlmysql.a", in_container_text)
        self.assertIn("libqsqlpsql.a", in_container_text)
        self.assertIn("QMYSQLDriverPlugin_init", in_container_text)
        self.assertIn("QPSQLDriverPlugin_init", in_container_text)
        self.assertIn("Qt6QMYSQLDriverPluginConfig.cmake", in_container_text)
        self.assertIn("Qt6QPSQLDriverPluginConfig.cmake", in_container_text)
        self.assertIn('mapfile -t lt_inputs < <(resolve_libtorrent_inputs "${QBT_STATIC_LT_VERSION}")', in_container_text)
        self.assertIn('export qbt_libtorrent_version="${lt_series}"', in_container_text)
        self.assertIn('export qbt_libtorrent_tag="${lt_tag}"', in_container_text)
        self.assertIn('export qbt_workflow_files="${QBT_STATIC_WORKFLOW_FILES}"', in_container_text)
        self.assertRegex(wrapper_text, r"build\)\s+shift\s+stage_overlay(?: > /dev/null)?\s+run_container build")

    def test_in_container_script_sets_git_ceiling_to_avoid_broken_worktree_gitdir_discovery(self) -> None:
        in_container_text = IN_CONTAINER.read_text(encoding="utf-8")

        self.assertIn('export GIT_CEILING_DIRECTORIES="${repo_root}"', in_container_text)

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_path = Path(tmpdir)
            fake_root = tmp_path / "fake-root"
            fake_root.mkdir()
            (fake_root / ".git").write_text("gitdir: /no/such/path\n", encoding="utf-8")
            work_dir = fake_root / "work" / "subdir"
            work_dir.mkdir(parents=True)
            remote_src = tmp_path / "remote-src"

            subprocess.run(["git", "init", "-q", str(remote_src)], check=True)

            without_ceiling = subprocess.run(
                ["git", "ls-remote", "-q", "--symref", str(remote_src), "HEAD"],
                cwd=work_dir,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(without_ceiling.returncode, 0)
            self.assertIn("not a git repository", without_ceiling.stderr)

            with_ceiling = subprocess.run(
                ["git", "ls-remote", "-q", "--symref", str(remote_src), "HEAD"],
                cwd=work_dir,
                env={**os.environ, "GIT_CEILING_DIRECTORIES": str(fake_root)},
                capture_output=True,
                text=True,
            )
            self.assertEqual(with_ceiling.returncode, 0, with_ceiling.stderr)


if __name__ == "__main__":
    unittest.main()
