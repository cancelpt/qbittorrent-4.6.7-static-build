/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef QBT_BITTORRENT_TORRENTSTATUSQUERY_H
#define QBT_BITTORRENT_TORRENTSTATUSQUERY_H

#include <memory>
#include <utility>

#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>

namespace BitTorrent::TorrentStatusQuery
{
    inline libtorrent::status_flags_t steadyStateRefreshFlags()
    {
        using TorrentHandle = libtorrent::torrent_handle;

        // Keep the optional fields still consumed on qB's steady-state path,
        // while excluding the heavier payloads that do not need to ride along
        // with every periodic state_update_alert.
        return (TorrentHandle::query_accurate_download_counters
                | TorrentHandle::query_distributed_copies
                | TorrentHandle::query_last_seen_complete
                | TorrentHandle::query_name
                | TorrentHandle::query_pieces
                | TorrentHandle::query_save_path);
    }

    template <typename Loader>
    std::shared_ptr<const libtorrent::torrent_info> resolveNativeTorrentInfo(
            std::weak_ptr<const libtorrent::torrent_info> &cachedTorrentInfo, Loader &&loadTorrentInfo)
    {
        if (std::shared_ptr<const libtorrent::torrent_info> torrentInfo = cachedTorrentInfo.lock())
            return torrentInfo;

        std::shared_ptr<const libtorrent::torrent_info> torrentInfo = std::forward<Loader>(loadTorrentInfo)();
        cachedTorrentInfo = torrentInfo;
        return torrentInfo;
    }
}

#endif // QBT_BITTORRENT_TORRENTSTATUSQUERY_H
