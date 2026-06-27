#!/usr/bin/env python3

import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
WRAPPER = REPO_ROOT / "packaging" / "qbt-static" / "run-static-build.sh"
IN_CONTAINER = REPO_ROOT / "packaging" / "qbt-static" / "in-container-build.sh"
RELEASE_STAGE = REPO_ROOT / "packaging" / "qbt-static" / "stage-release-assets.sh"
PRE_COMMIT_HOOK = REPO_ROOT / ".githooks" / "pre-commit"
PRE_PUSH_HOOK = REPO_ROOT / ".githooks" / "pre-push"
PATCH_SOURCE_DIR = REPO_ROOT / "packaging" / "qbt-static" / "patches" / "qbittorrent" / "4.6.7"
PATCH_SERIES = PATCH_SOURCE_DIR / "series"
PATCH_STAGE_DIR = REPO_ROOT / "build" / "qbt-static" / "work" / "patches" / "qbittorrent" / "4.6.7"
LT_RETENTION_OVERLAY_SOURCE_DIR = REPO_ROOT / "packaging" / "qbt-source-overlays" / "lt-retention-narrow"
LT_RETENTION_OVERLAY_STAGE_DIR = REPO_ROOT / "build" / "qbt-static" / "work" / "source-overlays" / "lt-retention-narrow"
CI_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "ci.yml"
RELEASE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release.yml"
COMPAT_PATCH = "9000-static-qt610-compat.diff"
NARROW_PAYLOAD_PATCH = "0005-feat-narrow-steady-state-payload-policy.patch"
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

    def test_public_repository_docs_and_ignore_rules_match_wrapper_scope(self) -> None:
        readme_text = (REPO_ROOT / "README.md").read_text(encoding="utf-8")
        readme_zh_text = (REPO_ROOT / "README.zh-CN.md").read_text(encoding="utf-8")
        patch_readme_text = (PATCH_SOURCE_DIR / "README.md").read_text(encoding="utf-8")
        lt_overlay_readme_text = (LT_RETENTION_OVERLAY_SOURCE_DIR / "README.md").read_text(encoding="utf-8")
        artifacts_readme_text = (REPO_ROOT / "artifacts" / "README.md").read_text(encoding="utf-8")
        gitignore_text = (REPO_ROOT / ".gitignore").read_text(encoding="utf-8")

        self.assertIn("wrapper, patch set, and source overlays", readme_text)
        self.assertIn("wrapper、patch 集和 source overlay", readme_zh_text)
        self.assertIn("qBittorrent deltas applied by the static build wrapper", patch_readme_text)
        self.assertIn("retained `torrentstatusquery.h` override", lt_overlay_readme_text)
        self.assertIn("GitHub Release assets", artifacts_readme_text)

        self.assertIn("artifacts/*", gitignore_text)
        self.assertIn("!artifacts/README.md", gitignore_text)
        self.assertIn(".pytest_cache/", gitignore_text)
        self.assertIn(".omx/", gitignore_text)
        self.assertIn(".github/workflows/", readme_text)
        self.assertIn("SHA256SUMS", readme_text)
        self.assertIn("QBT_GPG_PRIVATE_KEY", readme_text)
        self.assertIn("SHA256SUMS", readme_zh_text)
        self.assertIn("QBT_GPG_PRIVATE_KEY", readme_zh_text)
        self.assertIn("SHA256SUMS.asc", artifacts_readme_text)

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
        self.assertTrue((PATCH_SOURCE_DIR / COMPAT_PATCH).is_file(), "missing isolated static-toolchain compatibility patch")
        self.assertTrue((PATCH_STAGE_DIR / COMPAT_PATCH).is_file(), "missing staged static-toolchain compatibility patch")
        self.assertTrue(
            (LT_RETENTION_OVERLAY_STAGE_DIR / "src/base/bittorrent/torrentstatusquery.h").is_file(),
            "missing staged lt-retention source overlay header",
        )

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

    def test_python_rewrite_snippet_pins_glibc_and_libtorrent_tags(self) -> None:
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
        self.assertIn("missing expected default libtorrent probe", in_container_text)
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

    def test_release_stage_script_requires_built_binary(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            release_dir = Path(tmpdir) / "release"
            missing_binary = Path(tmpdir) / "missing" / "qbittorrent-nox"
            result = subprocess.run(
                ["bash", str(RELEASE_STAGE), str(release_dir), str(missing_binary)],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing built artifact", result.stderr)

    def test_release_stage_script_stages_binary_and_sha256sums(self) -> None:
        payload = b"static-qbt-test-payload\n"

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_path = Path(tmpdir)
            source_binary = tmp_path / "qbittorrent-nox"
            release_dir = tmp_path / "release"
            source_binary.write_bytes(payload)

            result = subprocess.run(
                ["bash", str(RELEASE_STAGE), str(release_dir), str(source_binary)],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            staged_binary = release_dir / "qbittorrent-nox-4.6.7-linux-x86_64-static"
            checksum_path = release_dir / "SHA256SUMS"

            self.assertEqual(result.stdout.strip(), str(release_dir))
            self.assertTrue(staged_binary.is_file(), f"missing staged binary: {staged_binary}")
            self.assertEqual(staged_binary.read_bytes(), payload)
            self.assertTrue(checksum_path.is_file(), f"missing checksum file: {checksum_path}")
            self.assertFalse((release_dir / "SHA256SUMS.asc").exists())

            expected_hash = hashlib.sha256(payload).hexdigest()
            checksum_text = checksum_path.read_text(encoding="utf-8")
            self.assertIn(f"{expected_hash}  {staged_binary.name}", checksum_text)

    def test_release_stage_script_optionally_signs_checksums(self) -> None:
        if shutil.which("gpg") is None:
            self.skipTest("gpg is not installed")

        payload = b"static-qbt-test-payload\n"

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_path = Path(tmpdir)
            gnupg_home = tmp_path / "gnupg"
            source_binary = tmp_path / "qbittorrent-nox"
            release_dir = tmp_path / "release"
            source_binary.write_bytes(payload)
            gnupg_home.mkdir(mode=0o700)

            generate_key = subprocess.run(
                [
                    "gpg",
                    "--homedir",
                    str(gnupg_home),
                    "--batch",
                    "--pinentry-mode",
                    "loopback",
                    "--passphrase",
                    "",
                    "--quick-generate-key",
                    "QBT Release Test <release@example.com>",
                    "default",
                    "sign",
                    "0",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(generate_key.returncode, 0, generate_key.stdout + generate_key.stderr)

            list_keys = subprocess.run(
                ["gpg", "--homedir", str(gnupg_home), "--list-secret-keys", "--with-colons"],
                capture_output=True,
                text=True,
                check=True,
            )
            key_id = next(
                line.split(":")[4]
                for line in list_keys.stdout.splitlines()
                if line.startswith("sec:")
            )

            result = subprocess.run(
                ["bash", str(RELEASE_STAGE), str(release_dir), str(source_binary)],
                cwd=REPO_ROOT,
                env={
                    **os.environ,
                    "GNUPGHOME": str(gnupg_home),
                    "QBT_RELEASE_GPG_KEY_ID": key_id,
                },
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            checksum_path = release_dir / "SHA256SUMS"
            signature_path = release_dir / "SHA256SUMS.asc"
            self.assertTrue(signature_path.is_file(), f"missing checksum signature: {signature_path}")

            verify = subprocess.run(
                [
                    "gpg",
                    "--homedir",
                    str(gnupg_home),
                    "--verify",
                    str(signature_path),
                    str(checksum_path),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(verify.returncode, 0, verify.stdout + verify.stderr)

    def test_github_actions_workflows_cover_ci_and_release_distribution(self) -> None:
        ci_text = CI_WORKFLOW.read_text(encoding="utf-8")
        release_text = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("pull_request:", ci_text)
        self.assertIn("push:", ci_text)
        self.assertIn("python3 -m pytest tests/test_qbt_static_wrapper.py -q", ci_text)
        self.assertIn("bash -n packaging/qbt-static/run-static-build.sh", ci_text)
        self.assertIn("bash -n packaging/qbt-static/in-container-build.sh", ci_text)
        self.assertIn("bash -n packaging/qbt-static/stage-release-assets.sh", ci_text)

        self.assertIn("workflow_dispatch:", release_text)
        self.assertIn("tags:", release_text)
        self.assertIn("- 'v*'", release_text)
        self.assertIn("contents: write", release_text)
        self.assertIn("run-static-build.sh build", release_text)
        self.assertIn("stage-release-assets.sh", release_text)
        self.assertIn("SHA256SUMS", release_text)
        self.assertIn("QBT_GPG_PRIVATE_KEY", release_text)
        self.assertIn("gh release create", release_text)
        self.assertIn("gh release upload", release_text)

    def test_release_workflow_checks_out_resolved_release_ref_before_build(self) -> None:
        release_text = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("fetch-depth: 0", release_text)
        self.assertIn('printf \'QBT_RELEASE_TAG=%s\\n\' "${release_tag}" >> "${GITHUB_ENV}"', release_text)
        self.assertIn('git fetch --force --tags origin "${QBT_RELEASE_TAG}"', release_text)
        self.assertIn('git checkout --force "${QBT_RELEASE_TAG}"', release_text)
        self.assertIn("if: ${{ env.QBT_GPG_PRIVATE_KEY != '' }}", release_text)
        self.assertNotIn("if: ${{ secrets.QBT_GPG_PRIVATE_KEY != '' }}", release_text)

    def test_githooks_privacy_scan_is_limited_to_exact_repo_local_leak_markers(self) -> None:
        pre_commit_text = PRE_COMMIT_HOOK.read_text(encoding="utf-8")
        pre_push_text = PRE_PUSH_HOOK.read_text(encoding="utf-8")
        scan_section = pre_commit_text.split("local -a patterns=", 1)[1]
        exact_markers = [
            '"' + "/home/" + "ryan" + '"',
            '"' + "/mnt/c/" + "Users/" + '"',
            '"' + "C:" + "\\\\\\\\" + "Users" + "\\\\\\\\" + '"',
            '"' + "/" + "Users/" + '"',
            '"' + "ryan" + "@localhost" + '"',
            '"' + "Codex <" + "codex@local" + "\\\\.invalid>" + '"',
            '"' + "qi" + "he" + '"',
            '"' + "qdirected" + "-bench" + '"',
            '"' + "browser" + "-forge" + '"',
            '"' + "dut-upstream" + "-corpus" + '"',
        ]

        self.assertIn(".githooks/*", pre_commit_text)
        self.assertIn("exec", pre_push_text)
        for marker in exact_markers:
            self.assertIn(marker, pre_commit_text)

        self.assertNotIn("placeholder" + " identity", pre_commit_text)
        self.assertNotIn("${HOME}", scan_section)
        self.assertNotIn("${repo_root}", scan_section)
        self.assertNotIn("patterns+=(", pre_push_text)


if __name__ == "__main__":
    unittest.main()
