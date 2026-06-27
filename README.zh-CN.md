# qBittorrent 4.6.7 静态构建 Wrapper + Patch Overlay

English: [README.md](README.md)

这个仓库保存的是一套用于构建 `qBittorrent 4.6.7` Linux `x86_64` 静态 `qbittorrent-nox` 的 wrapper、patch 集和 source overlay。上游 qBittorrent、libtorrent、Qt 以及静态构建工具链会在构建时拉取；仓库本身只保留 wrapper 自己维护的输入和测试。

## 前置依赖

- Linux `x86_64`
- `docker`
- `bash` 和 `rsync`
- 用于仓库内验证的 `python3` 与 `pytest`

## 仓库结构

- `packaging/qbt-static/`
  wrapper 脚本、固定配置、release 打包辅助脚本和构建说明
- `packaging/qbt-static/patches/qbittorrent/4.6.7/`
  应用在 upstream qB `release-4.6.7` 之上的 patch 序列和文件级覆盖
- `packaging/qbt-source-overlays/lt-retention-narrow/`
  narrow-payload 保留的 source overlay 输入
- `.github/workflows/`
  CI 和 release 自动化
- `tests/test_qbt_static_wrapper.py`
  wrapper / release 验证测试
- `artifacts/`
  可选的本地参考二进制；正式分发走 GitHub Releases

## 快速开始

在仓库根目录执行：

```bash
bash packaging/qbt-static/run-static-build.sh plan
bash packaging/qbt-static/run-static-build.sh prepare
bash packaging/qbt-static/run-static-build.sh build
```

固定默认值位于 `packaging/qbt-static/config.env`。如果你的环境需要替换软件源或 GNU mirror，请在执行 `build` 之前修改该文件。

如果要进入固定容器环境做交互式排查：

```bash
bash packaging/qbt-static/run-static-build.sh shell
```

## 构建基线

- qBittorrent tag: `release-4.6.7`
- 首选 libtorrent: `1.2.20`
- 回退 libtorrent: `1.2.19`
- Qt 主线: `6`
- 容器镜像: `buildpack-deps:noble`

期望产物路径：

```text
build/qbt-static/output/qbittorrent-nox
```

## 验证方式

仓库内验证：

```bash
python3 -m pytest tests/test_qbt_static_wrapper.py -q
```

构建后验证产物：

```bash
file build/qbt-static/output/qbittorrent-nox
ldd build/qbt-static/output/qbittorrent-nox
build/qbt-static/output/qbittorrent-nox --version
```

期望结果：

- `file` 输出包含 `statically linked`
- `ldd` 输出 `not a dynamic executable`
- `--version` 输出 `qBittorrent v4.6.7`

## CI 与 Release 分发

仓库自动化位于 `.github/workflows/`：

- `ci.yml`
  在每次 push 和 pull request 上运行 shell 语法检查，以及 `python3 -m pytest tests/test_qbt_static_wrapper.py -q`
- `release.yml`
  在 `v*` 标签或手动 `workflow_dispatch` 时触发，运行 `packaging/qbt-static/run-static-build.sh build`，通过 `packaging/qbt-static/stage-release-assets.sh` 暂存 release 附件，并发布 GitHub Release assets，而不是把二进制提交进 git

Release 产物包含：

- `qbittorrent-nox-4.6.7-linux-x86_64-static`
- `SHA256SUMS`
- 配置签名时生成的 `SHA256SUMS.asc`

可选签名 secrets：

- `QBT_GPG_PRIVATE_KEY`
- `QBT_GPG_PASSPHRASE`

不配置时，release 仍会发布二进制和 `SHA256SUMS`。

## Patch 输入

qB patch 和 overlay 输入位于：

```text
packaging/qbt-static/patches/qbittorrent/4.6.7/
packaging/qbt-source-overlays/lt-retention-narrow/
```

patch 顺序、文件级覆盖和 narrow-payload overlay 细节见：

- [packaging/qbt-static/README.md](packaging/qbt-static/README.md)
- [packaging/qbt-static/patches/qbittorrent/4.6.7/README.md](packaging/qbt-static/patches/qbittorrent/4.6.7/README.md)

## 当前限制

- 这个仓库只聚焦静态构建与产物验证，不覆盖部署自动化。
- 静态二进制在容器外运行时，仍可能出现 Qt locale warning。
- 当前 wrapper 和文档只面向 Linux `x86_64`。

## 许可证

这个仓库采用 `GPL-2.0-or-later`。见 [LICENSE](LICENSE)。部分 qBittorrent 派生文件仍在文件头中保留上游的 OpenSSL linking exception。
