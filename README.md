# qBittorrent 4.6.7 Static Build Wrapper + Patch Overlay

Simplified Chinese: [README.zh-CN.md](README.zh-CN.md)

This repository contains the wrapper, patch set, and source overlays used to build a reproducible Linux `x86_64` static `qbittorrent-nox` for `qBittorrent 4.6.7`. Upstream qBittorrent, libtorrent, Qt, and the static-build toolchain are fetched during the build; this repository keeps the wrapper-owned inputs and tests.

## Prerequisites

- Linux `x86_64`
- `docker`
- `bash` and `rsync`
- `python3` and `pytest` for repository verification

## Repository layout

- `packaging/qbt-static/`
  wrapper scripts, pinned config, release staging, and build notes
- `packaging/qbt-static/patches/qbittorrent/4.6.7/`
  patch series plus file-level overrides applied on top of upstream qB `release-4.6.7`
- `packaging/qbt-source-overlays/lt-retention-narrow/`
  retained narrow-payload source overlay input
- `.github/workflows/`
  CI and release automation
- `tests/test_qbt_static_wrapper.py`
  wrapper and release verification tests
- `artifacts/`
  optional local/reference binaries; release assets belong in GitHub Releases

## Quick start

Run from the repository root:

```bash
bash packaging/qbt-static/run-static-build.sh plan
bash packaging/qbt-static/run-static-build.sh prepare
bash packaging/qbt-static/run-static-build.sh build
```

Pinned defaults live in `packaging/qbt-static/config.env`. If your environment needs different package or GNU mirrors, adjust that file before running `build`.

For an interactive debugging shell inside the pinned container:

```bash
bash packaging/qbt-static/run-static-build.sh shell
```

## Build baseline

- qBittorrent tag: `release-4.6.7`
- preferred libtorrent: `1.2.20`
- fallback libtorrent: `1.2.19`
- Qt line: `6`
- container image: `buildpack-deps:noble`

Expected output:

```text
build/qbt-static/output/qbittorrent-nox
```

## Verification

Repository checks:

```bash
python3 -m pytest tests/test_qbt_static_wrapper.py -q
```

Artifact checks after a build:

```bash
file build/qbt-static/output/qbittorrent-nox
ldd build/qbt-static/output/qbittorrent-nox
build/qbt-static/output/qbittorrent-nox --version
```

Expected results:

- `file` reports `statically linked`
- `ldd` reports `not a dynamic executable`
- `--version` reports `qBittorrent v4.6.7`

## CI and release distribution

Repository automation lives under `.github/workflows/`:

- `ci.yml`
  runs shell syntax checks plus `python3 -m pytest tests/test_qbt_static_wrapper.py -q` on every push and pull request
- `release.yml`
  triggers on `v*` tags or manual `workflow_dispatch`, runs `packaging/qbt-static/run-static-build.sh build`, stages release assets with `packaging/qbt-static/stage-release-assets.sh`, and publishes GitHub Release assets instead of committing binaries to git

Release bundles contain:

- `qbittorrent-nox-4.6.7-linux-x86_64-static`
- `SHA256SUMS`
- `SHA256SUMS.asc` when signing is enabled

Optional release-signing secrets:

- `QBT_GPG_PRIVATE_KEY`
- `QBT_GPG_PASSPHRASE`

Without them, the workflow still publishes the binary and `SHA256SUMS`.

## Patch inputs

qB patch and overlay inputs live under:

```text
packaging/qbt-static/patches/qbittorrent/4.6.7/
packaging/qbt-source-overlays/lt-retention-narrow/
```

See the directory README files for patch ordering, file-level overrides, and narrow-payload overlay details:

- [packaging/qbt-static/README.md](packaging/qbt-static/README.md)
- [packaging/qbt-static/patches/qbittorrent/4.6.7/README.md](packaging/qbt-static/patches/qbittorrent/4.6.7/README.md)

## Known limits

- This repository is focused on building and verifying the static artifact, not on deployment automation.
- The static binary may still emit Qt locale warnings outside the build container.
- The wrapper and docs target Linux `x86_64` only.

## License

This repository is distributed under `GPL-2.0-or-later`. See [LICENSE](LICENSE). Some qBittorrent-derived files retain the upstream OpenSSL linking exception in their file headers.
