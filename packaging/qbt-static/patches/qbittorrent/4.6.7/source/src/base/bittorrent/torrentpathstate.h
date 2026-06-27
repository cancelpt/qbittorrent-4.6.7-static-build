/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#pragma once

#include <libtorrent/add_torrent_params.hpp>

#include <map>

#include "base/pathfwd.h"
#include "torrentcontentlayout.h"
#include "torrentinfo.h"

namespace BitTorrent
{
    PathList defaultLogicalFilePaths(const TorrentInfo &torrentInfo, TorrentContentLayout contentLayout);
    PathList mappedFilePaths(const TorrentInfo &torrentInfo, TorrentContentLayout contentLayout
            , const std::map<lt::file_index_t, std::string> &renamedFiles);
    std::map<lt::file_index_t, std::string> makeSparseRenamedFiles(const TorrentInfo &torrentInfo
            , TorrentContentLayout contentLayout, const PathList &actualFilePaths);
    std::map<lt::file_index_t, std::string> storedRenamedFiles(const TorrentInfo &torrentInfo
            , TorrentContentLayout contentLayout, const PathList &actualFilePaths, bool useSparseRenamedFiles);
    std::map<lt::file_index_t, std::string> expandedRenamedFiles(const TorrentInfo &torrentInfo
            , TorrentContentLayout contentLayout, const std::map<lt::file_index_t, std::string> &renamedFiles);
    void updateStoredRenamedFile(const TorrentInfo &torrentInfo, TorrentContentLayout contentLayout
            , std::map<lt::file_index_t, std::string> &renamedFiles, int fileIndex, const Path &actualFilePath
            , bool useSparseRenamedFiles);
    Path resolvedLogicalFilePath(const TorrentInfo &torrentInfo, TorrentContentLayout contentLayout
            , const std::map<lt::file_index_t, std::string> &renamedFiles, int fileIndex);
}
