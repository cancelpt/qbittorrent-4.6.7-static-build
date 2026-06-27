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

#include "torrentpathstate.h"

#include <QtGlobal>

#include "base/bittorrent/common.h"
#include "base/path.h"

using namespace BitTorrent;

namespace
{
    PathList applyContentLayout(PathList filePaths, const TorrentContentLayout contentLayout)
    {
        if (contentLayout == TorrentContentLayout::Original)
            return filePaths;

        const Path originalRootFolder = Path::findRootFolder(filePaths);
        const auto originalContentLayout = (originalRootFolder.isEmpty()
                                            ? TorrentContentLayout::NoSubfolder
                                            : TorrentContentLayout::Subfolder);
        if (contentLayout == originalContentLayout)
            return filePaths;

        if (contentLayout == TorrentContentLayout::NoSubfolder)
        {
            Path::stripRootFolder(filePaths);
        }
        else
        {
            Q_ASSERT(!filePaths.isEmpty());
            if (!filePaths.isEmpty())
                Path::addRootFolder(filePaths, filePaths.at(0).removedExtension());
        }

        return filePaths;
    }
}

PathList BitTorrent::defaultLogicalFilePaths(const TorrentInfo &torrentInfo, const TorrentContentLayout contentLayout)
{
    return applyContentLayout(torrentInfo.filePaths(), contentLayout);
}

PathList BitTorrent::mappedFilePaths(const TorrentInfo &torrentInfo, const TorrentContentLayout contentLayout
        , const std::map<lt::file_index_t, std::string> &renamedFiles)
{
    PathList filePaths = defaultLogicalFilePaths(torrentInfo, contentLayout);
    const auto nativeIndexes = torrentInfo.nativeIndexes();

    for (int i = 0; i < filePaths.size(); ++i)
    {
        if (const auto it = renamedFiles.find(nativeIndexes.at(i)); it != renamedFiles.cend())
            filePaths[i] = Path(it->second);
    }

    return filePaths;
}

std::map<lt::file_index_t, std::string> BitTorrent::makeSparseRenamedFiles(const TorrentInfo &torrentInfo
        , const TorrentContentLayout contentLayout, const PathList &actualFilePaths)
{
    Q_ASSERT(actualFilePaths.size() == torrentInfo.filesCount());
    if (actualFilePaths.size() != torrentInfo.filesCount())
        return {};

    const PathList defaultPaths = defaultLogicalFilePaths(torrentInfo, contentLayout);
    const auto nativeIndexes = torrentInfo.nativeIndexes();

    std::map<lt::file_index_t, std::string> renamedFiles;
    for (int i = 0; i < actualFilePaths.size(); ++i)
    {
        if (actualFilePaths.at(i) != defaultPaths.at(i))
            renamedFiles[nativeIndexes.at(i)] = actualFilePaths.at(i).toString().toStdString();
    }

    return renamedFiles;
}

std::map<lt::file_index_t, std::string> BitTorrent::storedRenamedFiles(const TorrentInfo &torrentInfo
        , const TorrentContentLayout contentLayout, const PathList &actualFilePaths, const bool useSparseRenamedFiles)
{
    return useSparseRenamedFiles
            ? makeSparseRenamedFiles(torrentInfo, contentLayout, actualFilePaths)
            : expandedRenamedFiles(torrentInfo, contentLayout
                    , makeSparseRenamedFiles(torrentInfo, contentLayout, actualFilePaths));
}

std::map<lt::file_index_t, std::string> BitTorrent::expandedRenamedFiles(const TorrentInfo &torrentInfo
        , const TorrentContentLayout contentLayout, const std::map<lt::file_index_t, std::string> &renamedFiles)
{
    std::map<lt::file_index_t, std::string> expanded;
    const PathList filePaths = mappedFilePaths(torrentInfo, contentLayout, renamedFiles);
    const auto nativeIndexes = torrentInfo.nativeIndexes();
    for (int i = 0; i < filePaths.size(); ++i)
        expanded[nativeIndexes.at(i)] = filePaths.at(i).toString().toStdString();

    return expanded;
}

void BitTorrent::updateStoredRenamedFile(const TorrentInfo &torrentInfo, const TorrentContentLayout contentLayout
        , std::map<lt::file_index_t, std::string> &renamedFiles, const int fileIndex, const Path &actualFilePath
        , const bool useSparseRenamedFiles)
{
    Q_ASSERT(fileIndex >= 0);
    Q_ASSERT(fileIndex < torrentInfo.filesCount());
    if ((fileIndex < 0) || (fileIndex >= torrentInfo.filesCount()))
        return;

    const lt::file_index_t nativeIndex = torrentInfo.nativeIndexes().at(fileIndex);
    if (!useSparseRenamedFiles)
    {
        renamedFiles[nativeIndex] = actualFilePath.toString().toStdString();
        return;
    }

    const Path defaultPath = defaultLogicalFilePaths(torrentInfo, contentLayout).at(fileIndex);
    if (actualFilePath == defaultPath)
        renamedFiles.erase(nativeIndex);
    else
        renamedFiles[nativeIndex] = actualFilePath.toString().toStdString();
}

Path BitTorrent::resolvedLogicalFilePath(const TorrentInfo &torrentInfo, const TorrentContentLayout contentLayout
        , const std::map<lt::file_index_t, std::string> &renamedFiles, const int fileIndex)
{
    Q_ASSERT(fileIndex >= 0);
    Q_ASSERT(fileIndex < torrentInfo.filesCount());
    if ((fileIndex < 0) || (fileIndex >= torrentInfo.filesCount()))
        return {};

    const auto nativeIndex = torrentInfo.nativeIndexes().at(fileIndex);
    if (const auto it = renamedFiles.find(nativeIndex); it != renamedFiles.cend())
        return Path(it->second).removedExtension(QB_EXT);

    return defaultLogicalFilePaths(torrentInfo, contentLayout).at(fileIndex);
}
