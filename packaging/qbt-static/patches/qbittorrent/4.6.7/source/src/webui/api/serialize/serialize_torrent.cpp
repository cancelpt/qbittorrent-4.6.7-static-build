/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2018  Vladimir Golovnev <glassez@yandex.ru>
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

#include "serialize_torrent.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonValue>
#include <QVector>

#include "base/preferences.h"
#include "base/bittorrent/infohash.h"
#include "base/bittorrent/torrent.h"
#include "base/bittorrent/trackerentry.h"
#include "base/path.h"
#include "base/tagset.h"
#include "base/utils/fs.h"

namespace
{
    QString torrentStateToString(const BitTorrent::TorrentState state)
    {
        switch (state)
        {
        case BitTorrent::TorrentState::Error:
            return u"error"_s;
        case BitTorrent::TorrentState::MissingFiles:
            return u"missingFiles"_s;
        case BitTorrent::TorrentState::Uploading:
            return u"uploading"_s;
        case BitTorrent::TorrentState::PausedUploading:
            return u"pausedUP"_s;
        case BitTorrent::TorrentState::QueuedUploading:
            return u"queuedUP"_s;
        case BitTorrent::TorrentState::StalledUploading:
            return u"stalledUP"_s;
        case BitTorrent::TorrentState::CheckingUploading:
            return u"checkingUP"_s;
        case BitTorrent::TorrentState::ForcedUploading:
            return u"forcedUP"_s;
        case BitTorrent::TorrentState::Downloading:
            return u"downloading"_s;
        case BitTorrent::TorrentState::DownloadingMetadata:
            return u"metaDL"_s;
        case BitTorrent::TorrentState::ForcedDownloadingMetadata:
            return u"forcedMetaDL"_s;
        case BitTorrent::TorrentState::PausedDownloading:
            return u"pausedDL"_s;
        case BitTorrent::TorrentState::QueuedDownloading:
            return u"queuedDL"_s;
        case BitTorrent::TorrentState::StalledDownloading:
            return u"stalledDL"_s;
        case BitTorrent::TorrentState::CheckingDownloading:
            return u"checkingDL"_s;
        case BitTorrent::TorrentState::ForcedDownloading:
            return u"forcedDL"_s;
        case BitTorrent::TorrentState::CheckingResumeData:
            return u"checkingResumeData"_s;
        case BitTorrent::TorrentState::Moving:
            return u"moving"_s;
        default:
            return u"unknown"_s;
        }
    }

    template <typename MapType, typename ValueType>
    void insertField(MapType &map, const QString &key, ValueType &&value)
    {
        map.insert(key, std::forward<ValueType>(value));
    }

    template <typename MapType>
    MapType serializeImpl(const BitTorrent::Torrent &torrent)
    {
        const auto *preferences = Preferences::instance();

        if (preferences->isExperimentalMinimalWebUIMaindataTorrentObjectEnabled())
        {
            MapType serialized;
            insertField(serialized, KEY_TORRENT_ID, torrent.id().toString());
            return serialized;
        }

        const auto adjustQueuePosition = [](const int position) -> int
        {
            return (position < 0) ? 0 : (position + 1);
        };

        const auto adjustRatio = [](const qreal ratio) -> qreal
        {
            return (ratio > BitTorrent::Torrent::MAX_RATIO) ? -1 : ratio;
        };

        const auto getLastActivityTime = [&torrent]() -> qlonglong
        {
            const qlonglong timeSinceActivity = torrent.timeSinceActivity();
            return (timeSinceActivity < 0)
                ? torrent.addedTime().toSecsSinceEpoch()
                : (QDateTime::currentDateTime().toSecsSinceEpoch() - timeSinceActivity);
        };

        MapType serialized;
        insertField(serialized, KEY_TORRENT_ID, torrent.id().toString());

        if (!preferences->isExperimentalOmitWebUIMaindataTorrentIdentityFieldsEnabled())
        {
            insertField(serialized, KEY_TORRENT_INFOHASHV1, torrent.infoHash().v1().toString());
            insertField(serialized, KEY_TORRENT_INFOHASHV2, torrent.infoHash().v2().toString());
            insertField(serialized, KEY_TORRENT_NAME, torrent.name());
        }

        if (!preferences->isExperimentalOmitWebUIMaindataTorrentMagnetUriEnabled())
            insertField(serialized, KEY_TORRENT_MAGNET_URI, torrent.createMagnetURI());

        if (!preferences->isExperimentalOmitWebUIMaindataTorrentNumericFieldsEnabled())
        {
            insertField(serialized, KEY_TORRENT_SIZE, torrent.wantedSize());
            insertField(serialized, KEY_TORRENT_PROGRESS, torrent.progress());
            insertField(serialized, KEY_TORRENT_DLSPEED, torrent.downloadPayloadRate());
            insertField(serialized, KEY_TORRENT_UPSPEED, torrent.uploadPayloadRate());
            insertField(serialized, KEY_TORRENT_QUEUE_POSITION, adjustQueuePosition(torrent.queuePosition()));
            insertField(serialized, KEY_TORRENT_SEEDS, torrent.seedsCount());
            insertField(serialized, KEY_TORRENT_NUM_COMPLETE, torrent.totalSeedsCount());
            insertField(serialized, KEY_TORRENT_LEECHS, torrent.leechsCount());
            insertField(serialized, KEY_TORRENT_NUM_INCOMPLETE, torrent.totalLeechersCount());
            insertField(serialized, KEY_TORRENT_DL_LIMIT, torrent.downloadLimit());
            insertField(serialized, KEY_TORRENT_UP_LIMIT, torrent.uploadLimit());
            insertField(serialized, KEY_TORRENT_AMOUNT_DOWNLOADED, torrent.totalDownload());
            insertField(serialized, KEY_TORRENT_AMOUNT_UPLOADED, torrent.totalUpload());
            insertField(serialized, KEY_TORRENT_AMOUNT_DOWNLOADED_SESSION, torrent.totalPayloadDownload());
            insertField(serialized, KEY_TORRENT_AMOUNT_UPLOADED_SESSION, torrent.totalPayloadUpload());
            insertField(serialized, KEY_TORRENT_AMOUNT_LEFT, torrent.remainingSize());
            insertField(serialized, KEY_TORRENT_AMOUNT_COMPLETED, torrent.completedSize());
            insertField(serialized, KEY_TORRENT_RATIO, adjustRatio(torrent.realRatio()));
            insertField(serialized, KEY_TORRENT_AVAILABILITY, torrent.distributedCopies());
            insertField(serialized, KEY_TORRENT_TOTAL_SIZE, torrent.totalSize());
        }

        if (!preferences->isExperimentalOmitWebUIMaindataTorrentDescriptiveFieldsEnabled())
        {
            insertField(serialized, KEY_TORRENT_STATE, torrentStateToString(torrent.state()));
            insertField(serialized, KEY_TORRENT_ETA, torrent.eta());
            insertField(serialized, KEY_TORRENT_SEQUENTIAL_DOWNLOAD, torrent.isSequentialDownload());
            insertField(serialized, KEY_TORRENT_FIRST_LAST_PIECE_PRIO, torrent.hasFirstLastPiecePriority());
            insertField(serialized, KEY_TORRENT_CATEGORY, torrent.category());
            insertField(serialized, KEY_TORRENT_TAGS, torrent.tags().join(u", "_s));
            insertField(serialized, KEY_TORRENT_SUPER_SEEDING, torrent.superSeeding());
            insertField(serialized, KEY_TORRENT_FORCE_START, torrent.isForced());
            insertField(serialized, KEY_TORRENT_ADDED_ON, torrent.addedTime().toSecsSinceEpoch());
            insertField(serialized, KEY_TORRENT_COMPLETION_ON, torrent.completedTime().toSecsSinceEpoch());
            insertField(serialized, KEY_TORRENT_TRACKER, torrent.currentTracker());
            insertField(serialized, KEY_TORRENT_TRACKERS_COUNT, torrent.trackers().size());
            insertField(serialized, KEY_TORRENT_MAX_RATIO, torrent.maxRatio());
            insertField(serialized, KEY_TORRENT_MAX_SEEDING_TIME, torrent.maxSeedingTime());
            insertField(serialized, KEY_TORRENT_MAX_INACTIVE_SEEDING_TIME, torrent.maxInactiveSeedingTime());
            insertField(serialized, KEY_TORRENT_RATIO_LIMIT, torrent.ratioLimit());
            insertField(serialized, KEY_TORRENT_SEEDING_TIME_LIMIT, torrent.seedingTimeLimit());
            insertField(serialized, KEY_TORRENT_INACTIVE_SEEDING_TIME_LIMIT, torrent.inactiveSeedingTimeLimit());
            insertField(serialized, KEY_TORRENT_LAST_SEEN_COMPLETE_TIME, torrent.lastSeenComplete().toSecsSinceEpoch());
            insertField(serialized, KEY_TORRENT_AUTO_TORRENT_MANAGEMENT, torrent.isAutoTMMEnabled());
            insertField(serialized, KEY_TORRENT_TIME_ACTIVE, torrent.activeTime());
            insertField(serialized, KEY_TORRENT_SEEDING_TIME, torrent.finishedTime());
            insertField(serialized, KEY_TORRENT_LAST_ACTIVITY_TIME, getLastActivityTime());
        }

        if (!preferences->isExperimentalOmitWebUIMaindataTorrentPathFieldsEnabled())
            insertField(serialized, KEY_TORRENT_DOWNLOAD_PATH, torrent.downloadPath().toString());

        if (!preferences->isExperimentalOmitWebUIMaindataTorrentPathFieldsEnabled()
            && !preferences->isExperimentalOmitWebUISavePathEnabled())
        {
            insertField(serialized, KEY_TORRENT_SAVE_PATH, torrent.savePath().toString());
        }

        if (!preferences->isExperimentalOmitWebUIMaindataTorrentPathFieldsEnabled()
            && preferences->isExperimentalTouchWebUIContentPathNoStoreEnabled())
        {
            torrent.contentPath();
        }
        else if (!preferences->isExperimentalOmitWebUIMaindataTorrentPathFieldsEnabled()
                 && !preferences->isExperimentalOmitWebUIContentPathEnabled())
        {
            insertField(serialized, KEY_TORRENT_CONTENT_PATH, torrent.contentPath().toString());
        }

        return serialized;
    }
}

QVariantMap serialize(const BitTorrent::Torrent &torrent)
{
    return serializeImpl<QVariantMap>(torrent);
}

QVariantMap serializeDescriptiveFields(const BitTorrent::Torrent &torrent)
{
    const auto *preferences = Preferences::instance();
    if (preferences->isExperimentalMinimalWebUIMaindataTorrentObjectEnabled()
        || preferences->isExperimentalOmitWebUIMaindataTorrentDescriptiveFieldsEnabled())
    {
        return {};
    }

    const auto getLastActivityTime = [&torrent]() -> qlonglong
    {
        const qlonglong timeSinceActivity = torrent.timeSinceActivity();
        return (timeSinceActivity < 0)
            ? torrent.addedTime().toSecsSinceEpoch()
            : (QDateTime::currentDateTime().toSecsSinceEpoch() - timeSinceActivity);
    };

    QVariantMap serialized;
    insertField(serialized, KEY_TORRENT_STATE, torrentStateToString(torrent.state()));
    insertField(serialized, KEY_TORRENT_ETA, torrent.eta());
    insertField(serialized, KEY_TORRENT_SEQUENTIAL_DOWNLOAD, torrent.isSequentialDownload());
    insertField(serialized, KEY_TORRENT_FIRST_LAST_PIECE_PRIO, torrent.hasFirstLastPiecePriority());
    insertField(serialized, KEY_TORRENT_CATEGORY, torrent.category());
    insertField(serialized, KEY_TORRENT_TAGS, torrent.tags().join(u", "_s));
    insertField(serialized, KEY_TORRENT_SUPER_SEEDING, torrent.superSeeding());
    insertField(serialized, KEY_TORRENT_FORCE_START, torrent.isForced());
    insertField(serialized, KEY_TORRENT_ADDED_ON, torrent.addedTime().toSecsSinceEpoch());
    insertField(serialized, KEY_TORRENT_COMPLETION_ON, torrent.completedTime().toSecsSinceEpoch());
    insertField(serialized, KEY_TORRENT_TRACKER, torrent.currentTracker());
    insertField(serialized, KEY_TORRENT_TRACKERS_COUNT, torrent.trackers().size());
    insertField(serialized, KEY_TORRENT_MAX_RATIO, torrent.maxRatio());
    insertField(serialized, KEY_TORRENT_MAX_SEEDING_TIME, torrent.maxSeedingTime());
    insertField(serialized, KEY_TORRENT_MAX_INACTIVE_SEEDING_TIME, torrent.maxInactiveSeedingTime());
    insertField(serialized, KEY_TORRENT_RATIO_LIMIT, torrent.ratioLimit());
    insertField(serialized, KEY_TORRENT_SEEDING_TIME_LIMIT, torrent.seedingTimeLimit());
    insertField(serialized, KEY_TORRENT_INACTIVE_SEEDING_TIME_LIMIT, torrent.inactiveSeedingTimeLimit());
    insertField(serialized, KEY_TORRENT_LAST_SEEN_COMPLETE_TIME, torrent.lastSeenComplete().toSecsSinceEpoch());
    insertField(serialized, KEY_TORRENT_AUTO_TORRENT_MANAGEMENT, torrent.isAutoTMMEnabled());
    insertField(serialized, KEY_TORRENT_TIME_ACTIVE, torrent.activeTime());
    insertField(serialized, KEY_TORRENT_SEEDING_TIME, torrent.finishedTime());
    insertField(serialized, KEY_TORRENT_LAST_ACTIVITY_TIME, getLastActivityTime());
    return serialized;
}

QVariantMap serializeDescriptiveLifecycleFields(const BitTorrent::Torrent &torrent)
{
    const auto *preferences = Preferences::instance();
    if (preferences->isExperimentalMinimalWebUIMaindataTorrentObjectEnabled()
        || preferences->isExperimentalOmitWebUIMaindataTorrentDescriptiveFieldsEnabled())
    {
        return {};
    }

    const auto getLastActivityTime = [&torrent]() -> qlonglong
    {
        const qlonglong timeSinceActivity = torrent.timeSinceActivity();
        return (timeSinceActivity < 0)
            ? torrent.addedTime().toSecsSinceEpoch()
            : (QDateTime::currentDateTime().toSecsSinceEpoch() - timeSinceActivity);
    };

    QVariantMap serialized;
    insertField(serialized, KEY_TORRENT_ADDED_ON, torrent.addedTime().toSecsSinceEpoch());
    insertField(serialized, KEY_TORRENT_COMPLETION_ON, torrent.completedTime().toSecsSinceEpoch());
    insertField(serialized, KEY_TORRENT_MAX_RATIO, torrent.maxRatio());
    insertField(serialized, KEY_TORRENT_MAX_SEEDING_TIME, torrent.maxSeedingTime());
    insertField(serialized, KEY_TORRENT_MAX_INACTIVE_SEEDING_TIME, torrent.maxInactiveSeedingTime());
    insertField(serialized, KEY_TORRENT_RATIO_LIMIT, torrent.ratioLimit());
    insertField(serialized, KEY_TORRENT_SEEDING_TIME_LIMIT, torrent.seedingTimeLimit());
    insertField(serialized, KEY_TORRENT_INACTIVE_SEEDING_TIME_LIMIT, torrent.inactiveSeedingTimeLimit());
    insertField(serialized, KEY_TORRENT_LAST_SEEN_COMPLETE_TIME, torrent.lastSeenComplete().toSecsSinceEpoch());
    insertField(serialized, KEY_TORRENT_LAST_ACTIVITY_TIME, getLastActivityTime());
    insertField(serialized, KEY_TORRENT_TIME_ACTIVE, torrent.activeTime());
    insertField(serialized, KEY_TORRENT_SEEDING_TIME, torrent.finishedTime());
    return serialized;
}

QVariantMap serializeNumericFields(const BitTorrent::Torrent &torrent)
{
    const auto *preferences = Preferences::instance();
    if (preferences->isExperimentalMinimalWebUIMaindataTorrentObjectEnabled()
        || preferences->isExperimentalOmitWebUIMaindataTorrentNumericFieldsEnabled())
    {
        return {};
    }

    const auto adjustQueuePosition = [](const int position) -> int
    {
        return (position < 0) ? 0 : (position + 1);
    };

    const auto adjustRatio = [](const qreal ratio) -> qreal
    {
        return (ratio > BitTorrent::Torrent::MAX_RATIO) ? -1 : ratio;
    };

    QVariantMap serialized;
    insertField(serialized, KEY_TORRENT_SIZE, torrent.wantedSize());
    insertField(serialized, KEY_TORRENT_PROGRESS, torrent.progress());
    insertField(serialized, KEY_TORRENT_DLSPEED, torrent.downloadPayloadRate());
    insertField(serialized, KEY_TORRENT_UPSPEED, torrent.uploadPayloadRate());
    insertField(serialized, KEY_TORRENT_QUEUE_POSITION, adjustQueuePosition(torrent.queuePosition()));
    insertField(serialized, KEY_TORRENT_SEEDS, torrent.seedsCount());
    insertField(serialized, KEY_TORRENT_NUM_COMPLETE, torrent.totalSeedsCount());
    insertField(serialized, KEY_TORRENT_LEECHS, torrent.leechsCount());
    insertField(serialized, KEY_TORRENT_NUM_INCOMPLETE, torrent.totalLeechersCount());
    insertField(serialized, KEY_TORRENT_DL_LIMIT, torrent.downloadLimit());
    insertField(serialized, KEY_TORRENT_UP_LIMIT, torrent.uploadLimit());
    insertField(serialized, KEY_TORRENT_AMOUNT_DOWNLOADED, torrent.totalDownload());
    insertField(serialized, KEY_TORRENT_AMOUNT_UPLOADED, torrent.totalUpload());
    insertField(serialized, KEY_TORRENT_AMOUNT_DOWNLOADED_SESSION, torrent.totalPayloadDownload());
    insertField(serialized, KEY_TORRENT_AMOUNT_UPLOADED_SESSION, torrent.totalPayloadUpload());
    insertField(serialized, KEY_TORRENT_AMOUNT_LEFT, torrent.remainingSize());
    insertField(serialized, KEY_TORRENT_AMOUNT_COMPLETED, torrent.completedSize());
    insertField(serialized, KEY_TORRENT_RATIO, adjustRatio(torrent.realRatio()));
    insertField(serialized, KEY_TORRENT_AVAILABILITY, torrent.distributedCopies());
    insertField(serialized, KEY_TORRENT_TOTAL_SIZE, torrent.totalSize());
    return serialized;
}

QJsonObject serializeToJson(const BitTorrent::Torrent &torrent)
{
    return serializeImpl<QJsonObject>(torrent);
}

QByteArray serializeToJsonBytes(const BitTorrent::Torrent &torrent, const bool includeTorrentId)
{
    QJsonObject serialized = serializeToJson(torrent);
    if (!includeTorrentId)
        serialized.remove(KEY_TORRENT_ID);

    return QJsonDocument(serialized).toJson(QJsonDocument::Compact);
}
