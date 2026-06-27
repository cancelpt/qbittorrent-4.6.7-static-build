# Local qB 4.6.7 Patch Overlay

This directory contains the qBittorrent deltas applied by the static build wrapper on top of upstream `release-4.6.7`.

## Textual patch chain

`series` defines the functional patch order:

- `0001-Add-session-alert-queue-size-preference-getter-and-t.patch`
- `0002-Harden-preferences-test-isolation-and-clamp-boundari.patch`
- `0003-feat-bound-libtorrent-alert-queue-size.patch`
- `0004-feat-share-WebUI-maindata-state-and-virtualize-torre.patch`
- `0005-feat-narrow-steady-state-payload-policy.patch`

`0005` stays aligned with `packaging/qbt-source-overlays/lt-retention-narrow/`.

`9000-static-qt610-compat.diff` is a separate static-toolchain compatibility diff and is not part of the main functional series.

## `source/` override tree

The upstream builder can copy `patches/qbittorrent/4.6.7/source/` into the upstream qB tree before patch application. Review both the `series` patch chain and the `source/` overrides when auditing the final build inputs.
