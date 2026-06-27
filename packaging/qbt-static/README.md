# qBittorrent Static Build Wrapper

This directory contains the wrapper around `userdocs/qbittorrent-nox-static` used to build this repository's pinned `qBittorrent 4.6.7 + libtorrent 1.2.x` line. Upstream sources are fetched during the build.

## Commands

Use the wrapper from the repository root:

```bash
bash packaging/qbt-static/run-static-build.sh plan
bash packaging/qbt-static/run-static-build.sh prepare
bash packaging/qbt-static/run-static-build.sh build
```

Use `shell` only for interactive debugging inside the pinned container:

```bash
bash packaging/qbt-static/run-static-build.sh shell
```

After a successful build, stage release assets locally with:

```bash
bash packaging/qbt-static/stage-release-assets.sh
```

That command copies the built binary into `build/qbt-static/release/`, generates `SHA256SUMS`, and writes `SHA256SUMS.asc` when `QBT_RELEASE_GPG_KEY_ID` is available in the environment.

## Pinned inputs

Current defaults live in [config.env](config.env):

- Container image: `buildpack-deps:noble`
- Upstream repo: `https://github.com/userdocs/qbittorrent-nox-static.git`
- Upstream ref: `a67e830`
- qB tag: `release-4.6.7`
- Preferred libtorrent: `1.2.20`
- Fallback libtorrent: `1.2.19`
- Qt line: `6`
- Build tool: `cmake`
- Upstream workflow-files mode: `yes`
- Pinned glibc tag for workflow-files mode: `glibc-2.43`

`1.2.20` is the default because the wrapper drives upstream in `workflow-files` mode with pinned glibc and libtorrent inputs.

## Repository state paths

All local state stays under `build/qbt-static/`:

- Output binary: `build/qbt-static/output/qbittorrent-nox`
- Release assets: `build/qbt-static/release/`
- Build logs: `build/qbt-static/logs/`
- Cached upstream checkout: `build/qbt-static/cache/qbittorrent-nox-static`
- Working prefix and staged patches: `build/qbt-static/work/`
- Patch overlay: `packaging/qbt-static/patches/`
- Narrow source overlay: `packaging/qbt-source-overlays/lt-retention-narrow/`

## qB inputs

The wrapper stages three qB input sets on top of upstream `release-4.6.7`:

- `patches/qbittorrent/4.6.7/*.patch`
- `patches/qbittorrent/4.6.7/source/`
- `../qbt-source-overlays/lt-retention-narrow/`

Review both the patch series and the `source/` overrides when auditing the final build inputs.

## Wrapper behavior

The wrapper keeps the upstream toolchain but constrains the parts that must stay stable for this repository:

- Runs the container as `root` so the upstream `bootstrap_deps` path works, then restores host ownership with `HOST_UID` / `HOST_GID`.
- Uses `docker run --rm -i` for `build`, and only adds `-t` for interactive `shell` sessions so non-interactive automation does not fail with `stdin is not a terminal`.
- Rewrites Ubuntu APT sources to `QBT_STATIC_APT_MIRROR` and forces IPv4 for the configured build environment.
- Exports `GIT_CEILING_DIRECTORIES=/workspace` inside the container so upstream `git ls-remote` calls do not walk into a mounted parent git-worktree `.git` file whose absolute host path is invalid inside the container.
- Rewrites the upstream build script at runtime to pin the GNU mirror, extend retry timeouts, force the selected glibc tag, and honor an exact libtorrent tag such as `v1.2.20`.
- Disables Qt6 features that otherwise pull host dynamic dependencies into the qmake/toolchain path: `icu`, `glib`, `zstd`, `brotli`, `gssapi`, `system_proxies`, `sql_mysql`, and `sql_psql`.
- Cleans stale Qt SQL plugin archives and CMake package files from the prefix before both `build` and `shell` runs so old `mysql` / `psql` artifacts cannot leak back into the final qB link step.
- Stages the patch overlay into `build/qbt-static/work/patches/` and the `lt-retention-narrow` overlay into `build/qbt-static/work/source-overlays/lt-retention-narrow/` before invoking the upstream builder.
- Stages release assets with the stable filename `qbittorrent-nox-4.6.7-linux-x86_64-static`, plus `SHA256SUMS`, and signs the checksum file when `QBT_RELEASE_GPG_KEY_ID` is configured.

## CI and release automation

The repository automation keeps release mechanics inside this wrapper repo:

- `.github/workflows/ci.yml`
  runs shell syntax checks and the repository pytest suite for every push and pull request
- `.github/workflows/release.yml`
  builds with `run-static-build.sh build`, optionally imports `QBT_GPG_PRIVATE_KEY` and `QBT_GPG_PASSPHRASE`, stages assets with `stage-release-assets.sh`, then publishes GitHub Release assets

## Verification

### Binary type and static-link check

Run:

```bash
file build/qbt-static/output/qbittorrent-nox
ldd build/qbt-static/output/qbittorrent-nox
build/qbt-static/output/qbittorrent-nox --version
```

Expected result:

- `file` reports an ELF `x86-64` executable that is `statically linked`
- `ldd` reports `not a dynamic executable`
- `--version` reports `qBittorrent v4.6.7`

### Fresh-profile smoke test

One minimal smoke test is:

```bash
profile_dir="$(mktemp -d /tmp/qbt-static-smoke.XXXXXX)"
log_file="${profile_dir}/qbt-smoke.log"
(
  env LANG=C.UTF-8 LC_ALL=C.UTF-8 \
    build/qbt-static/output/qbittorrent-nox \
      --profile="${profile_dir}" \
      --webui-port=18081
) >"${log_file}" 2>&1 &
qbt_pid=$!
for _ in $(seq 1 40); do
  if curl -fsS -o /dev/null http://127.0.0.1:18081/; then
    break
  fi
  sleep 0.5
done
kill "${qbt_pid}"
wait "${qbt_pid}" || true
tail -n 20 "${log_file}"
```

Expected result:

- the WebUI answers on `http://127.0.0.1:18081/`
- qB prints the first-run WebUI notice and temporary admin password without requiring stdin input

## Known caveats

- The current static glibc/Qt artifact still emits a Qt locale warning outside the build container. `strace` shows that Qt falls back to the build-time embedded locale path under `/workspace/.../lib/locale/locale-archive`, which does not exist on the host after the single-file binary is copied out, so the process ultimately reports locale `C` even when `LANG=C.utf8` is requested.
- The static link still emits the usual SQLite `dlopen` warning during the link step; this is a linker warning, not a final link failure.
- This repository proves the static artifact path, the pinned inputs, and fresh-profile startup. Remote deployment, systemd integration, and populated live profile behavior remain separate work.
