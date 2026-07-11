# Local qB 4.6.7 Patch Overlay

Primary patch sources:

- repository-owned qB WebUI candidate export captured in this directory's `0001`-`0006` patch chain and `source/` override tree
- `packaging/qbt-source-overlays/lt-retention-narrow`

Source commits, in apply order, for the exported clean-worktree chain:

- `6d85fda` Add session alert queue size preference getter and tests
- `958f312` Harden preferences test isolation and clamp boundaries
- `ac37db5` feat: bound libtorrent alert queue size
- `ea6058c` feat: share WebUI maindata state and virtualize torrent list

`0005-feat-narrow-steady-state-payload-policy.patch` is intentionally not
exported from the mixed WebUI tree. It reconstructs the narrow steady-state
payload policy from the repository-owned canonical overlay under
`packaging/qbt-source-overlays/lt-retention-narrow` so the static build flow
uses the same minimal source of truth as the live `lt-retention` compare
tooling.
`0006-fix-preserve-virtual-scroll-on-full-update.patch` preserves the
virtualized WebUI torrent list scroll position across `full_update` rebuilds.

`0006-revert-alert-queue-to-stock-qb-policy.patch` is a repository-local
rollback candidate that restores qB's stock `alert_queue_size` runtime policy
without reverting the surrounding preference plumbing or the later `0004/0005`
WebUI payload changes.

The `source/` override tree is retained for the productized WebUI memory probe
implementation. Because the upstream static builder gives this directory
priority over textual patches, `source/src/base/bittorrent/sessionimpl.cpp`
also carries the rollback's stock qB `alert_queue_size` line directly; do not
reintroduce the bounded `Preferences::sessionAlertQueueSize()` runtime policy
there.

The wrapper stages both inputs into `build/qbt-static/work/` before invoking
the upstream builder:

- `patches/qbittorrent/4.6.7/` for the patch chain that the upstream
  `local-patches` path applies
- `patches/qbittorrent/4.6.7/source/` for the productized WebUI memory probe
  source override used by the upstream builder's highest-priority source mode
- `source-overlays/lt-retention-narrow/` for auditable source-level provenance
  of `torrentstatusquery.h`

`9000-static-qt610-compat.diff` remains an isolated static-toolchain
compatibility diff and is not part of the functional qB feature/export chain.
