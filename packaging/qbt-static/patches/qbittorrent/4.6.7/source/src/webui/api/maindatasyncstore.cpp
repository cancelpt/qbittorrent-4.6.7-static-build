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
 * License in all respects for all of the code used other than "OpenSSL". If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "maindatasyncstore.h"

#include <algorithm>

#include <QDateTime>
#include <QDataStream>
#include <QFile>
#include <QMetaType>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>

#if defined(Q_OS_LINUX) && defined(__GLIBC__)
#include <malloc.h>
#endif

#include "base/algorithm.h"
#include "base/bittorrent/cachestatus.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/sessionstatus.h"
#include "base/bittorrent/torrent.h"
#include "base/bittorrent/trackerentry.h"
#include "base/global.h"
#include "base/preferences.h"
#include "base/utils/string.h"
#include "serialize/serialize_torrent.h"

namespace
{
    const QString FIRST_FULL_TRACE_FILE_ENV = u"QBT_WEBUI_FIRST_FULL_TRACE_FILE"_s;
    const QString LIFECYCLE_TRACE_FILE_ENV = u"QBT_WEBUI_LIFECYCLE_TRACE_FILE"_s;
    const QString MEMORY_PROBE_FILE_ENV = u"QBT_WEBUI_MEMORY_PROBE_FILE"_s;
    constexpr int MEMORY_PROBE_TOP_RESIDENT_TORRENTS = 20;
    constexpr int MEMORY_PROBE_TOP_ANON_UNNAMED_MAPPINGS = 12;
    constexpr qint64 NON_IDLE_TRIM_MIN_INTERVAL_MS = (30 * 1000);

    const QString KEY_SYNC_MAINDATA_QUEUEING = u"queueing"_s;
    const QString KEY_SYNC_MAINDATA_REFRESH_INTERVAL = u"refresh_interval"_s;
    const QString KEY_SYNC_MAINDATA_USE_ALT_SPEED_LIMITS = u"use_alt_speed_limits"_s;
    const QString KEY_SYNC_MAINDATA_USE_SUBCATEGORIES = u"use_subcategories"_s;

    const QString KEY_TRANSFER_CONNECTION_STATUS = u"connection_status"_s;
    const QString KEY_TRANSFER_DHT_NODES = u"dht_nodes"_s;
    const QString KEY_TRANSFER_DLDATA = u"dl_info_data"_s;
    const QString KEY_TRANSFER_DLRATELIMIT = u"dl_rate_limit"_s;
    const QString KEY_TRANSFER_DLSPEED = u"dl_info_speed"_s;
    const QString KEY_TRANSFER_FREESPACEONDISK = u"free_space_on_disk"_s;
    const QString KEY_TRANSFER_UPDATA = u"up_info_data"_s;
    const QString KEY_TRANSFER_UPRATELIMIT = u"up_rate_limit"_s;
    const QString KEY_TRANSFER_UPSPEED = u"up_info_speed"_s;
    const QString KEY_TRANSFER_ALLTIME_DL = u"alltime_dl"_s;
    const QString KEY_TRANSFER_ALLTIME_UL = u"alltime_ul"_s;
    const QString KEY_TRANSFER_AVERAGE_TIME_QUEUE = u"average_time_queue"_s;
    const QString KEY_TRANSFER_GLOBAL_RATIO = u"global_ratio"_s;
    const QString KEY_TRANSFER_QUEUED_IO_JOBS = u"queued_io_jobs"_s;
    const QString KEY_TRANSFER_READ_CACHE_HITS = u"read_cache_hits"_s;
    const QString KEY_TRANSFER_READ_CACHE_OVERLOAD = u"read_cache_overload"_s;
    const QString KEY_TRANSFER_TOTAL_BUFFERS_SIZE = u"total_buffers_size"_s;
    const QString KEY_TRANSFER_TOTAL_PEER_CONNECTIONS = u"total_peer_connections"_s;
    const QString KEY_TRANSFER_TOTAL_QUEUED_SIZE = u"total_queued_size"_s;
    const QString KEY_TRANSFER_TOTAL_WASTE_SESSION = u"total_wasted_session"_s;
    const QString KEY_TRANSFER_WRITE_CACHE_OVERLOAD = u"write_cache_overload"_s;

    const QString KEY_SUFFIX_REMOVED = u"_removed"_s;

    const QString KEY_CATEGORIES = u"categories"_s;
    const QString KEY_CATEGORIES_REMOVED = KEY_CATEGORIES + KEY_SUFFIX_REMOVED;
    const QString KEY_TAGS = u"tags"_s;
    const QString KEY_TAGS_REMOVED = KEY_TAGS + KEY_SUFFIX_REMOVED;
    const QString KEY_TORRENTS = u"torrents"_s;
    const QString KEY_TORRENTS_REMOVED = KEY_TORRENTS + KEY_SUFFIX_REMOVED;
    const QString KEY_TRACKERS = u"trackers"_s;
    const QString KEY_TRACKERS_REMOVED = KEY_TRACKERS + KEY_SUFFIX_REMOVED;
    const QString KEY_SERVER_STATE = u"server_state"_s;
    const QString KEY_FULL_UPDATE = u"full_update"_s;
    const QString KEY_RESPONSE_ID = u"rid"_s;

    qint64 extractFieldValueKb(const QByteArray &content, const QByteArray &fieldName)
    {
        const QList<QByteArray> lines = content.split('\n');
        for (const QByteArray &line : lines)
        {
            if (!line.startsWith(fieldName))
                continue;

            const QList<QByteArray> parts = line.mid(fieldName.size()).simplified().split(' ');
            if (parts.isEmpty())
                return -1;

            bool ok = false;
            const qint64 value = parts.constFirst().toLongLong(&ok);
            return ok ? value : -1;
        }

        return -1;
    }

    enum class SmapsMappingClass
    {
        Heap,
        Anon,
        File,
        Stack,
        Other,
    };

    struct SmapsMappingTotalsKb
    {
        qint64 pss = 0;
        qint64 privateDirty = 0;
    };

    struct SmapsMappingEntry
    {
        QString label;
        qint64 pss = 0;
        qint64 privateDirty = 0;
    };

    struct SmapsAnonUnnamedMappingEntry
    {
        QString addressRange;
        QString permissions;
        qint64 sizeKb = -1;
        qint64 pss = 0;
        qint64 privateDirty = 0;
    };

    struct SmapsMappingHeaderInfo
    {
        QByteArray addressRange;
        QByteArray permissions;
        QByteArray path;
        qint64 sizeKb = -1;
    };

    struct SmapsClassifiedKb
    {
        qint64 pssHeap = 0;
        qint64 pssAnon = 0;
        qint64 pssAnonUnnamed = 0;
        qint64 pssAnonNamed = 0;
        qint64 pssFile = 0;
        qint64 pssStack = 0;
        qint64 pssOther = 0;
        qint64 privateDirtyHeap = 0;
        qint64 privateDirtyAnon = 0;
        qint64 privateDirtyAnonUnnamed = 0;
        qint64 privateDirtyAnonNamed = 0;
        qint64 privateDirtyFile = 0;
        qint64 privateDirtyStack = 0;
        qint64 privateDirtyOther = 0;
        QHash<QString, SmapsMappingTotalsKb> mappingTotalsByLabel;
        QList<SmapsAnonUnnamedMappingEntry> anonUnnamedMappings;
    };

    QByteArray smapsMappingPath(const QByteArray &headerLine)
    {
        const QList<QByteArray> parts = headerLine.simplified().split(' ');
        return (parts.size() > 5) ? parts.mid(5).join(" ") : QByteArray {};
    }

    bool parseHexAddress(const QByteArray &value, quint64 &result)
    {
        bool ok = false;
        const quint64 parsed = value.toULongLong(&ok, 16);
        if (!ok)
            return false;

        result = parsed;
        return true;
    }

    SmapsMappingHeaderInfo parseSmapsMappingHeader(const QByteArray &headerLine)
    {
        SmapsMappingHeaderInfo header;
        const QList<QByteArray> parts = headerLine.simplified().split(' ');
        if (parts.isEmpty())
            return header;

        header.addressRange = parts.constFirst();
        header.permissions = (parts.size() > 1) ? parts.at(1) : QByteArray {};
        header.path = smapsMappingPath(headerLine);

        const int dashPos = header.addressRange.indexOf('-');
        if (dashPos <= 0)
            return header;

        quint64 start = 0;
        quint64 end = 0;
        if (!parseHexAddress(header.addressRange.left(dashPos), start)
            || !parseHexAddress(header.addressRange.mid(dashPos + 1), end)
            || (end <= start))
        {
            return header;
        }

        header.sizeKb = static_cast<qint64>((end - start) / 1024);
        return header;
    }

    SmapsMappingClass classifySmapsMappingHeader(const QByteArray &headerLine)
    {
        const QByteArray path = smapsMappingPath(headerLine);

        if (path == "[heap]")
            return SmapsMappingClass::Heap;
        if (path.startsWith("[stack"))
            return SmapsMappingClass::Stack;
        if (path.isEmpty() || path.startsWith("[anon"))
            return SmapsMappingClass::Anon;
        if (path.startsWith('/'))
            return SmapsMappingClass::File;

        return SmapsMappingClass::Other;
    }

    QString smapsMappingLabel(const QByteArray &headerLine, const SmapsMappingClass mappingClass)
    {
        const QByteArray path = smapsMappingPath(headerLine);
        switch (mappingClass)
        {
        case SmapsMappingClass::Heap:
            return u"[heap]"_s;
        case SmapsMappingClass::Stack:
            return u"[stack]"_s;
        case SmapsMappingClass::Anon:
            if (path.isEmpty())
                return u"[anon:unnamed]"_s;
            return QString::fromUtf8(path.left(96));
        case SmapsMappingClass::File:
        {
            const int slash = path.lastIndexOf('/');
            const QByteArray fileName = (slash >= 0) ? path.mid(slash + 1) : path;
            return u"[file]/"_s + QString::fromUtf8(fileName.left(96));
        }
        case SmapsMappingClass::Other:
            if (path.isEmpty())
                return u"[other:unnamed]"_s;
            return QString::fromUtf8(path.left(96));
        }

        return u"[unknown]"_s;
    }

    QList<SmapsMappingEntry> smapsTopMappingsByPss(const QHash<QString, SmapsMappingTotalsKb> &mappingTotalsByLabel)
    {
        QList<SmapsMappingEntry> entries;
        entries.reserve(mappingTotalsByLabel.size());
        for (auto it = mappingTotalsByLabel.cbegin(); it != mappingTotalsByLabel.cend(); ++it)
        {
            const SmapsMappingTotalsKb totals = it.value();
            if ((totals.pss <= 0) && (totals.privateDirty <= 0))
                continue;
            entries.append({it.key(), totals.pss, totals.privateDirty});
        }

        std::sort(entries.begin(), entries.end(), [](const SmapsMappingEntry &left, const SmapsMappingEntry &right)
        {
            if (left.pss != right.pss)
                return (left.pss > right.pss);
            if (left.privateDirty != right.privateDirty)
                return (left.privateDirty > right.privateDirty);
            return (left.label < right.label);
        });

        const int topCount = qMin(entries.size(), MEMORY_PROBE_TOP_RESIDENT_TORRENTS);
        entries.resize(topCount);
        return entries;
    }

    QList<SmapsAnonUnnamedMappingEntry> smapsTopAnonUnnamedMappings(const QList<SmapsAnonUnnamedMappingEntry> &mappings)
    {
        QList<SmapsAnonUnnamedMappingEntry> topMappings = mappings;
        std::sort(topMappings.begin(), topMappings.end(), [](const SmapsAnonUnnamedMappingEntry &left
                , const SmapsAnonUnnamedMappingEntry &right)
        {
            if (left.pss != right.pss)
                return (left.pss > right.pss);
            if (left.privateDirty != right.privateDirty)
                return (left.privateDirty > right.privateDirty);
            if (left.sizeKb != right.sizeKb)
                return (left.sizeKb > right.sizeKb);
            if (left.addressRange != right.addressRange)
                return (left.addressRange < right.addressRange);
            return (left.permissions < right.permissions);
        });

        const int topCount = qMin(topMappings.size(), MEMORY_PROBE_TOP_ANON_UNNAMED_MAPPINGS);
        topMappings.resize(topCount);
        return topMappings;
    }

    SmapsClassifiedKb classifySmapsKb(const QByteArray &content)
    {
        SmapsClassifiedKb classified;
        SmapsMappingClass currentClass = SmapsMappingClass::Other;
        QString currentLabel = u"[other:unnamed]"_s;
        SmapsMappingHeaderInfo currentHeader;
        qint64 currentMappingPssKb = 0;
        qint64 currentMappingPrivateDirtyKb = 0;
        bool hasCurrentMappingHeader = false;

        const auto flushCurrentMapping = [&classified, &currentClass, &currentHeader
                , &currentMappingPssKb, &currentMappingPrivateDirtyKb, &hasCurrentMappingHeader]
        {
            if (!hasCurrentMappingHeader)
                return;

            if ((currentClass == SmapsMappingClass::Anon)
                && currentHeader.path.isEmpty()
                && ((currentMappingPssKb > 0) || (currentMappingPrivateDirtyKb > 0)))
            {
                classified.anonUnnamedMappings.append({
                    QString::fromUtf8(currentHeader.addressRange),
                    QString::fromUtf8(currentHeader.permissions),
                    currentHeader.sizeKb,
                    currentMappingPssKb,
                    currentMappingPrivateDirtyKb,
                });
            }

            currentMappingPssKb = 0;
            currentMappingPrivateDirtyKb = 0;
        };

        const QList<QByteArray> lines = content.split('\n');
        for (const QByteArray &line : lines)
        {
            if (line.isEmpty())
                continue;

            const bool looksLikeHeader = !line.startsWith(' ')
                && !line.startsWith('\t')
                && line.contains('-')
                && !line.startsWith("Pss:")
                && !line.startsWith("Private_Dirty:");
            if (looksLikeHeader)
            {
                flushCurrentMapping();
                currentClass = classifySmapsMappingHeader(line);
                currentLabel = smapsMappingLabel(line, currentClass);
                currentHeader = parseSmapsMappingHeader(line);
                hasCurrentMappingHeader = true;
                continue;
            }

            const qint64 pssKb = line.startsWith("Pss:") ? extractFieldValueKb(line, "Pss:") : -1;
            const qint64 privateDirtyKb = line.startsWith("Private_Dirty:")
                ? extractFieldValueKb(line, "Private_Dirty:") : -1;
            if ((pssKb < 0) && (privateDirtyKb < 0))
                continue;

            switch (currentClass)
            {
            case SmapsMappingClass::Heap:
                if (pssKb >= 0)
                    classified.pssHeap += pssKb;
                if (privateDirtyKb >= 0)
                    classified.privateDirtyHeap += privateDirtyKb;
                break;
            case SmapsMappingClass::Anon:
                if (pssKb >= 0)
                {
                    classified.pssAnon += pssKb;
                    if (currentHeader.path.isEmpty())
                        classified.pssAnonUnnamed += pssKb;
                    else
                        classified.pssAnonNamed += pssKb;
                }
                if (privateDirtyKb >= 0)
                {
                    classified.privateDirtyAnon += privateDirtyKb;
                    if (currentHeader.path.isEmpty())
                        classified.privateDirtyAnonUnnamed += privateDirtyKb;
                    else
                        classified.privateDirtyAnonNamed += privateDirtyKb;
                }
                break;
            case SmapsMappingClass::File:
                if (pssKb >= 0)
                    classified.pssFile += pssKb;
                if (privateDirtyKb >= 0)
                    classified.privateDirtyFile += privateDirtyKb;
                break;
            case SmapsMappingClass::Stack:
                if (pssKb >= 0)
                    classified.pssStack += pssKb;
                if (privateDirtyKb >= 0)
                    classified.privateDirtyStack += privateDirtyKb;
                break;
            case SmapsMappingClass::Other:
                if (pssKb >= 0)
                    classified.pssOther += pssKb;
                if (privateDirtyKb >= 0)
                    classified.privateDirtyOther += privateDirtyKb;
                break;
            }

            SmapsMappingTotalsKb &totals = classified.mappingTotalsByLabel[currentLabel];
            if (pssKb >= 0)
            {
                totals.pss += pssKb;
                currentMappingPssKb += pssKb;
            }
            if (privateDirtyKb >= 0)
            {
                totals.privateDirty += privateDirtyKb;
                currentMappingPrivateDirtyKb += privateDirtyKb;
            }
        }

        flushCurrentMapping();

        return classified;
    }

    qint64 extractFileInteger(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return -1;

        bool ok = false;
        const qint64 value = QString::fromUtf8(file.readAll()).trimmed().toLongLong(&ok);
        return ok ? value : -1;
    }

    qint64 readMemoryCurrentBytesFromCgroupV2()
    {
        QFile cgroupFile(u"/proc/self/cgroup"_s);
        if (!cgroupFile.open(QIODevice::ReadOnly | QIODevice::Text))
            return -1;

        const QList<QByteArray> lines = cgroupFile.readAll().split('\n');
        for (const QByteArray &line : lines)
        {
            if (line.isEmpty())
                continue;

            const QList<QByteArray> parts = line.split(':');
            if (parts.size() != 3)
                continue;
            if (parts.constFirst() != "0")
                continue;

            QString cgroupPath = QString::fromUtf8(parts.constLast()).trimmed();
            if (cgroupPath.isEmpty())
                cgroupPath = u"/"_s;

            QString memoryCurrentPath = u"/sys/fs/cgroup"_s;
            if (cgroupPath != u"/"_s)
                memoryCurrentPath += cgroupPath;
            memoryCurrentPath += u"/memory.current"_s;
            return extractFileInteger(memoryCurrentPath);
        }

        return -1;
    }

    qint64 variantToInt64(const QVariantMap &values, const QString &key)
    {
        const auto it = values.constFind(key);
        if (it == values.cend())
            return -1;

        bool ok = false;
        const qint64 value = it.value().toLongLong(&ok);
        return ok ? value : -1;
    }

    using CompactTorrentValue = std::variant<std::monostate, QString, qlonglong, int, bool, double>;

    struct CompactTorrentFieldSpec
    {
        QString key;
        CompactTorrentValue defaultValue;
    };

    struct PreparedCompactRowSet
    {
        QVariantList fieldDefaults;
        qsizetype fieldDefaultsBytes = 0;
        QList<QStringList> stringPools;
        qsizetype stringPoolsBytes = 0;
        QHash<QString, QByteArray> torrentRows;
    };

    const QList<CompactTorrentFieldSpec> &compactTorrentFieldSpecs()
    {
        static const QList<CompactTorrentFieldSpec> specs {
            {KEY_TORRENT_INFOHASHV1, QString {}},
            {KEY_TORRENT_INFOHASHV2, QString {}},
            {KEY_TORRENT_NAME, QString {}},
            {KEY_TORRENT_MAGNET_URI, QString {}},
            {KEY_TORRENT_SIZE, qlonglong {}},
            {KEY_TORRENT_PROGRESS, double {}},
            {KEY_TORRENT_DLSPEED, int {}},
            {KEY_TORRENT_UPSPEED, int {}},
            {KEY_TORRENT_QUEUE_POSITION, int {}},
            {KEY_TORRENT_SEEDS, int {}},
            {KEY_TORRENT_NUM_COMPLETE, int {}},
            {KEY_TORRENT_LEECHS, int {}},
            {KEY_TORRENT_NUM_INCOMPLETE, int {}},
            {KEY_TORRENT_RATIO, double {}},
            {KEY_TORRENT_ETA, qlonglong {}},
            {KEY_TORRENT_STATE, QString {}},
            {KEY_TORRENT_SEQUENTIAL_DOWNLOAD, bool {}},
            {KEY_TORRENT_FIRST_LAST_PIECE_PRIO, bool {}},
            {KEY_TORRENT_CATEGORY, QString {}},
            {KEY_TORRENT_TAGS, QString {}},
            {KEY_TORRENT_SUPER_SEEDING, bool {}},
            {KEY_TORRENT_FORCE_START, bool {}},
            {KEY_TORRENT_SAVE_PATH, QString {}},
            {KEY_TORRENT_DOWNLOAD_PATH, QString {}},
            {KEY_TORRENT_CONTENT_PATH, QString {}},
            {KEY_TORRENT_ADDED_ON, qlonglong {}},
            {KEY_TORRENT_COMPLETION_ON, qlonglong {}},
            {KEY_TORRENT_TRACKER, QString {}},
            {KEY_TORRENT_TRACKERS_COUNT, int {}},
            {KEY_TORRENT_DL_LIMIT, int {}},
            {KEY_TORRENT_UP_LIMIT, int {}},
            {KEY_TORRENT_AMOUNT_DOWNLOADED, qlonglong {}},
            {KEY_TORRENT_AMOUNT_UPLOADED, qlonglong {}},
            {KEY_TORRENT_AMOUNT_DOWNLOADED_SESSION, qlonglong {}},
            {KEY_TORRENT_AMOUNT_UPLOADED_SESSION, qlonglong {}},
            {KEY_TORRENT_AMOUNT_LEFT, qlonglong {}},
            {KEY_TORRENT_AMOUNT_COMPLETED, qlonglong {}},
            {KEY_TORRENT_MAX_RATIO, double {}},
            {KEY_TORRENT_MAX_SEEDING_TIME, int {}},
            {KEY_TORRENT_MAX_INACTIVE_SEEDING_TIME, int {}},
            {KEY_TORRENT_RATIO_LIMIT, double {}},
            {KEY_TORRENT_SEEDING_TIME_LIMIT, int {}},
            {KEY_TORRENT_INACTIVE_SEEDING_TIME_LIMIT, int {}},
            {KEY_TORRENT_LAST_SEEN_COMPLETE_TIME, qlonglong {}},
            {KEY_TORRENT_LAST_ACTIVITY_TIME, qlonglong {}},
            {KEY_TORRENT_TOTAL_SIZE, qlonglong {}},
            {KEY_TORRENT_AUTO_TORRENT_MANAGEMENT, bool {}},
            {KEY_TORRENT_TIME_ACTIVE, qlonglong {}},
            {KEY_TORRENT_SEEDING_TIME, qlonglong {}},
            {KEY_TORRENT_AVAILABILITY, double {}},
        };

        return specs;
    }

    const QSet<QString> &descriptiveCompactTorrentFieldKeys()
    {
        static const QSet<QString> keys = []
        {
            QSet<QString> result {
                KEY_TORRENT_STATE,
                KEY_TORRENT_ETA,
                KEY_TORRENT_SEQUENTIAL_DOWNLOAD,
                KEY_TORRENT_FIRST_LAST_PIECE_PRIO,
                KEY_TORRENT_CATEGORY,
                KEY_TORRENT_TAGS,
                KEY_TORRENT_SUPER_SEEDING,
                KEY_TORRENT_FORCE_START,
                KEY_TORRENT_TRACKER,
                KEY_TORRENT_TRACKERS_COUNT,
                KEY_TORRENT_AUTO_TORRENT_MANAGEMENT,
            };
            result.unite({
                KEY_TORRENT_ADDED_ON,
                KEY_TORRENT_COMPLETION_ON,
                KEY_TORRENT_MAX_RATIO,
                KEY_TORRENT_MAX_SEEDING_TIME,
                KEY_TORRENT_MAX_INACTIVE_SEEDING_TIME,
                KEY_TORRENT_RATIO_LIMIT,
                KEY_TORRENT_SEEDING_TIME_LIMIT,
                KEY_TORRENT_INACTIVE_SEEDING_TIME_LIMIT,
                KEY_TORRENT_LAST_SEEN_COMPLETE_TIME,
                KEY_TORRENT_LAST_ACTIVITY_TIME,
                KEY_TORRENT_TIME_ACTIVE,
                KEY_TORRENT_SEEDING_TIME,
            });
            return result;
        }();

        return keys;
    }

    const QSet<QString> &descriptiveStatusCompactTorrentFieldKeys()
    {
        static const QSet<QString> keys {
            KEY_TORRENT_STATE,
            KEY_TORRENT_ETA,
            KEY_TORRENT_SEQUENTIAL_DOWNLOAD,
            KEY_TORRENT_FIRST_LAST_PIECE_PRIO,
            KEY_TORRENT_CATEGORY,
            KEY_TORRENT_TAGS,
            KEY_TORRENT_SUPER_SEEDING,
            KEY_TORRENT_FORCE_START,
            KEY_TORRENT_TRACKER,
            KEY_TORRENT_TRACKERS_COUNT,
            KEY_TORRENT_AUTO_TORRENT_MANAGEMENT,
        };

        return keys;
    }

    const QSet<QString> &descriptiveLifecycleCompactTorrentFieldKeys()
    {
        static const QSet<QString> keys {
            KEY_TORRENT_ADDED_ON,
            KEY_TORRENT_COMPLETION_ON,
            KEY_TORRENT_MAX_RATIO,
            KEY_TORRENT_MAX_SEEDING_TIME,
            KEY_TORRENT_MAX_INACTIVE_SEEDING_TIME,
            KEY_TORRENT_RATIO_LIMIT,
            KEY_TORRENT_SEEDING_TIME_LIMIT,
            KEY_TORRENT_INACTIVE_SEEDING_TIME_LIMIT,
            KEY_TORRENT_LAST_SEEN_COMPLETE_TIME,
            KEY_TORRENT_LAST_ACTIVITY_TIME,
            KEY_TORRENT_TIME_ACTIVE,
            KEY_TORRENT_SEEDING_TIME,
        };

        return keys;
    }

    const QSet<QString> &numericCompactTorrentFieldKeys()
    {
        static const QSet<QString> keys {
            KEY_TORRENT_SIZE,
            KEY_TORRENT_PROGRESS,
            KEY_TORRENT_DLSPEED,
            KEY_TORRENT_UPSPEED,
            KEY_TORRENT_QUEUE_POSITION,
            KEY_TORRENT_SEEDS,
            KEY_TORRENT_NUM_COMPLETE,
            KEY_TORRENT_LEECHS,
            KEY_TORRENT_NUM_INCOMPLETE,
            KEY_TORRENT_DL_LIMIT,
            KEY_TORRENT_UP_LIMIT,
            KEY_TORRENT_AMOUNT_DOWNLOADED,
            KEY_TORRENT_AMOUNT_UPLOADED,
            KEY_TORRENT_AMOUNT_DOWNLOADED_SESSION,
            KEY_TORRENT_AMOUNT_UPLOADED_SESSION,
            KEY_TORRENT_AMOUNT_LEFT,
            KEY_TORRENT_AMOUNT_COMPLETED,
            KEY_TORRENT_RATIO,
            KEY_TORRENT_AVAILABILITY,
            KEY_TORRENT_TOTAL_SIZE,
        };

        return keys;
    }

    const QList<CompactTorrentFieldSpec> &primaryCompactTorrentFieldSpecs()
    {
        static const QList<CompactTorrentFieldSpec> specs = []
        {
            QList<CompactTorrentFieldSpec> result;
            const QSet<QString> &descriptiveKeys = descriptiveCompactTorrentFieldKeys();
            for (const CompactTorrentFieldSpec &spec : asConst(compactTorrentFieldSpecs()))
            {
                if (!descriptiveKeys.contains(spec.key))
                    result.append(spec);
            }
            return result;
        }();

        return specs;
    }

    const QList<CompactTorrentFieldSpec> &descriptiveCompactTorrentFieldSpecs()
    {
        static const QList<CompactTorrentFieldSpec> specs = []
        {
            QList<CompactTorrentFieldSpec> result;
            const QSet<QString> &descriptiveKeys = descriptiveCompactTorrentFieldKeys();
            for (const CompactTorrentFieldSpec &spec : asConst(compactTorrentFieldSpecs()))
            {
                if (descriptiveKeys.contains(spec.key))
                    result.append(spec);
            }
            return result;
        }();

        return specs;
    }

    const QList<CompactTorrentFieldSpec> &descriptiveStatusCompactTorrentFieldSpecs()
    {
        static const QList<CompactTorrentFieldSpec> specs = []
        {
            QList<CompactTorrentFieldSpec> result;
            const QSet<QString> &descriptiveKeys = descriptiveStatusCompactTorrentFieldKeys();
            for (const CompactTorrentFieldSpec &spec : asConst(compactTorrentFieldSpecs()))
            {
                if (descriptiveKeys.contains(spec.key))
                    result.append(spec);
            }
            return result;
        }();

        return specs;
    }

    const QList<CompactTorrentFieldSpec> &descriptiveLifecycleCompactTorrentFieldSpecs()
    {
        static const QList<CompactTorrentFieldSpec> specs = []
        {
            QList<CompactTorrentFieldSpec> result;
            const QSet<QString> &descriptiveKeys = descriptiveLifecycleCompactTorrentFieldKeys();
            for (const CompactTorrentFieldSpec &spec : asConst(compactTorrentFieldSpecs()))
            {
                if (descriptiveKeys.contains(spec.key))
                    result.append(spec);
            }
            return result;
        }();

        return specs;
    }

    const QList<CompactTorrentFieldSpec> &numericCompactTorrentFieldSpecs()
    {
        static const QList<CompactTorrentFieldSpec> specs = []
        {
            QList<CompactTorrentFieldSpec> result;
            const QSet<QString> &numericKeys = numericCompactTorrentFieldKeys();
            for (const CompactTorrentFieldSpec &spec : asConst(compactTorrentFieldSpecs()))
            {
                if (numericKeys.contains(spec.key))
                    result.append(spec);
            }
            return result;
        }();

        return specs;
    }

    const QList<CompactTorrentFieldSpec> &deferredDescriptiveNumericPrimaryCompactTorrentFieldSpecs()
    {
        static const QList<CompactTorrentFieldSpec> specs = []
        {
            QList<CompactTorrentFieldSpec> result;
            const QSet<QString> &descriptiveKeys = descriptiveCompactTorrentFieldKeys();
            const QSet<QString> &numericKeys = numericCompactTorrentFieldKeys();
            for (const CompactTorrentFieldSpec &spec : asConst(compactTorrentFieldSpecs()))
            {
                if (!descriptiveKeys.contains(spec.key) && !numericKeys.contains(spec.key))
                    result.append(spec);
            }
            return result;
        }();

        return specs;
    }

    const QList<CompactTorrentFieldSpec> &retainedPrimaryCompactTorrentFieldSpecs(
        const bool enableDescriptiveSidecar, const bool enableDeferredDescriptiveNumericOnDemand)
    {
        if (enableDeferredDescriptiveNumericOnDemand)
            return deferredDescriptiveNumericPrimaryCompactTorrentFieldSpecs();

        if (enableDescriptiveSidecar)
            return primaryCompactTorrentFieldSpecs();

        return compactTorrentFieldSpecs();
    }

    QVariant compactTorrentVariantToQVariant(const CompactTorrentValue &value)
    {
        if (std::holds_alternative<QString>(value))
            return QVariant(std::get<QString>(value));
        if (std::holds_alternative<qlonglong>(value))
            return QVariant(std::get<qlonglong>(value));
        if (std::holds_alternative<int>(value))
            return QVariant(std::get<int>(value));
        if (std::holds_alternative<bool>(value))
            return QVariant(std::get<bool>(value));
        if (std::holds_alternative<double>(value))
            return QVariant(std::get<double>(value));
        return {};
    }

    QVariantMap compactTorrentStaticDefaultsToRow(const QList<CompactTorrentFieldSpec> &specs)
    {
        QVariantMap row;
        for (const CompactTorrentFieldSpec &spec : specs)
            row.insert(spec.key, compactTorrentVariantToQVariant(spec.defaultValue));
        return row;
    }

    QVariantMap compactTorrentDefaultsToRow(const QList<CompactTorrentFieldSpec> &specs
        , const QVariantList &defaults)
    {
        QVariantMap row;
        const int count = qMin(specs.size(), defaults.size());
        for (int i = 0; i < count; ++i)
        {
            const QVariant value = defaults.at(i);
            if (value.isValid())
                row.insert(specs.at(i).key, value);
        }
        return row;
    }

    QDataStream::Version compactTorrentRowStreamVersion()
    {
        return QDataStream::Qt_5_15;
    }

    QByteArray compactTorrentRow(const QVariantMap &row, const QList<CompactTorrentFieldSpec> &specs
        , const QVariantList &defaults, const QList<QStringList> &stringPools);
    QVariantMap expandCompactTorrentRow(const QByteArray &row, const QList<CompactTorrentFieldSpec> &specs
        , const QVariantList &defaults, const QList<QStringList> &stringPools);
    QJsonObject expandCompactTorrentRowObject(const QByteArray &row, const QList<CompactTorrentFieldSpec> &specs
        , const QVariantList &defaults, const QList<QStringList> &stringPools);
    QByteArray serializeCompactTorrentRowToJsonBytes(const QByteArray &row
        , const QList<CompactTorrentFieldSpec> &specs, const QVariantList &defaults
        , const QList<QStringList> &stringPools);

    struct CompactTorrentFieldDefaultAccumulator
    {
        QHash<QString, int> counts;
        QHash<QString, QVariant> valuesByKey;
        QVariant bestValue;
        int bestCount = 0;
    };

    struct CompactTorrentStringPoolAccumulator
    {
        QHash<QString, int> counts;
    };

    qsizetype estimateCompactValueBytes(const QVariant &value)
    {
        switch (value.type())
        {
        case QVariant::String:
            return value.toString().toUtf8().size();
        case QVariant::LongLong:
        case QVariant::ULongLong:
            return sizeof(qlonglong);
        case QVariant::Int:
        case QVariant::UInt:
            return sizeof(int);
        case QVariant::Bool:
            return sizeof(bool);
        case QVariant::Double:
            return sizeof(double);
        default:
            return 0;
        }
    }

    enum class WideSnapshotTorrentFieldFamily
    {
        Descriptive,
        Numeric,
        Identity,
        Path,
        Magnet,
        Other
    };

    struct WideSnapshotFamilyBytes
    {
        qint64 descriptive = 0;
        qint64 numeric = 0;
        qint64 identity = 0;
        qint64 path = 0;
        qint64 magnet = 0;
        qint64 other = 0;

        qint64 total() const
        {
            return (descriptive + numeric + identity + path + magnet + other);
        }
    };

    WideSnapshotTorrentFieldFamily classifyWideSnapshotTorrentFieldFamily(const QString &key)
    {
        static const QSet<QString> identityKeys {
            KEY_TORRENT_INFOHASHV1,
            KEY_TORRENT_INFOHASHV2,
            KEY_TORRENT_NAME,
        };
        static const QSet<QString> pathKeys {
            KEY_TORRENT_SAVE_PATH,
            KEY_TORRENT_DOWNLOAD_PATH,
            KEY_TORRENT_CONTENT_PATH,
        };

        if (descriptiveCompactTorrentFieldKeys().contains(key))
            return WideSnapshotTorrentFieldFamily::Descriptive;
        if (numericCompactTorrentFieldKeys().contains(key))
            return WideSnapshotTorrentFieldFamily::Numeric;
        if (identityKeys.contains(key))
            return WideSnapshotTorrentFieldFamily::Identity;
        if (pathKeys.contains(key))
            return WideSnapshotTorrentFieldFamily::Path;
        if (key == KEY_TORRENT_MAGNET_URI)
            return WideSnapshotTorrentFieldFamily::Magnet;

        return WideSnapshotTorrentFieldFamily::Other;
    }

    qint64 estimateWideSnapshotTorrentFieldBytes(const QString &key, const QVariant &value)
    {
        // Rough per-entry bytes: "key":<value>, excluding map-level comma/brace overhead.
        return (key.toUtf8().size() + 3 + estimateCompactValueBytes(value));
    }

    WideSnapshotFamilyBytes estimateWideSnapshotFamilyBytes(const WebUISync::MaindataSyncData &snapshot
        , const qint64 wideSnapshotEstimatedBytes)
    {
        WideSnapshotFamilyBytes bytes;

        for (auto rowIt = snapshot.torrents.cbegin(); rowIt != snapshot.torrents.cend(); ++rowIt)
        {
            const QVariantMap &row = rowIt.value();
            for (auto fieldIt = row.cbegin(); fieldIt != row.cend(); ++fieldIt)
            {
                const qint64 fieldBytes = estimateWideSnapshotTorrentFieldBytes(fieldIt.key(), fieldIt.value());
                switch (classifyWideSnapshotTorrentFieldFamily(fieldIt.key()))
                {
                case WideSnapshotTorrentFieldFamily::Descriptive:
                    bytes.descriptive += fieldBytes;
                    break;
                case WideSnapshotTorrentFieldFamily::Numeric:
                    bytes.numeric += fieldBytes;
                    break;
                case WideSnapshotTorrentFieldFamily::Identity:
                    bytes.identity += fieldBytes;
                    break;
                case WideSnapshotTorrentFieldFamily::Path:
                    bytes.path += fieldBytes;
                    break;
                case WideSnapshotTorrentFieldFamily::Magnet:
                    bytes.magnet += fieldBytes;
                    break;
                case WideSnapshotTorrentFieldFamily::Other:
                    bytes.other += fieldBytes;
                    break;
                }
            }
        }

        const qint64 unattributedRemainder = qMax<qint64>(0, wideSnapshotEstimatedBytes - bytes.total());
        bytes.other += unattributedRemainder;
        return bytes;
    }

    QList<CompactTorrentFieldDefaultAccumulator> makeCompactTorrentFieldDefaultAccumulators(
        const QList<CompactTorrentFieldSpec> &specs)
    {
        QList<CompactTorrentFieldDefaultAccumulator> accumulators;
        accumulators.reserve(specs.size());
        for (int i = 0; i < specs.size(); ++i)
            accumulators.append(CompactTorrentFieldDefaultAccumulator {});
        return accumulators;
    }

    QList<CompactTorrentStringPoolAccumulator> makeCompactTorrentStringPoolAccumulators(
        const QList<CompactTorrentFieldSpec> &specs)
    {
        QList<CompactTorrentStringPoolAccumulator> accumulators;
        accumulators.reserve(specs.size());
        for (int i = 0; i < specs.size(); ++i)
            accumulators.append(CompactTorrentStringPoolAccumulator {});
        return accumulators;
    }

    void accumulateCompactTorrentFieldDefaults(QList<CompactTorrentFieldDefaultAccumulator> &accumulators
        , const QVariantMap &row, const QList<CompactTorrentFieldSpec> &specs)
    {
        Q_ASSERT(accumulators.size() == specs.size());

        for (int i = 0; i < specs.size(); ++i)
        {
            const QVariant value = row.value(specs.at(i).key);
            if (!value.isValid())
                continue;

            CompactTorrentFieldDefaultAccumulator &accumulator = accumulators[i];
            const QString valueKey = value.toString();
            accumulator.valuesByKey.insert(valueKey, value);
            const int count = ++accumulator.counts[valueKey];
            if (count > accumulator.bestCount)
            {
                accumulator.bestCount = count;
                accumulator.bestValue = accumulator.valuesByKey.value(valueKey);
            }
        }
    }

    void accumulateCompactTorrentStringPools(QList<CompactTorrentStringPoolAccumulator> &accumulators
        , const QVariantMap &row, const QList<CompactTorrentFieldSpec> &specs
        , const QVariantList &defaults)
    {
        Q_ASSERT(accumulators.size() == specs.size());

        for (int i = 0; i < specs.size(); ++i)
        {
            if (!std::holds_alternative<QString>(specs.at(i).defaultValue))
                continue;

            const QVariant value = row.value(specs.at(i).key);
            if (!value.isValid())
                continue;

            const QVariant defaultValue = defaults.value(i);
            if (defaultValue.isValid() && (value == defaultValue))
                continue;

            ++accumulators[i].counts[value.toString()];
        }
    }

    QVariantList finalizeCompactTorrentFieldDefaults(const QList<CompactTorrentFieldDefaultAccumulator> &accumulators)
    {
        QVariantList defaults;
        defaults.reserve(accumulators.size());
        for (const CompactTorrentFieldDefaultAccumulator &accumulator : accumulators)
            defaults.append(accumulator.bestValue);
        return defaults;
    }

    QList<QStringList> finalizeCompactTorrentStringPools(
        const QList<CompactTorrentStringPoolAccumulator> &accumulators)
    {
        QList<QStringList> pools;
        pools.reserve(accumulators.size());

        for (const CompactTorrentStringPoolAccumulator &accumulator : accumulators)
        {
            QStringList pool;
            QList<QPair<QString, int>> entries;
            entries.reserve(accumulator.counts.size());
            for (auto it = accumulator.counts.cbegin(); it != accumulator.counts.cend(); ++it)
            {
                if (it.value() < 2)
                    continue;
                entries.append({it.key(), it.value()});
            }

            std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right)
            {
                if (left.second != right.second)
                    return (left.second > right.second);
                return (left.first < right.first);
            });

            for (const auto &entry : asConst(entries))
                pool.append(entry.first);

            pools.append(pool);
        }

        return pools;
    }

    qsizetype estimateCompactStringPoolsBytes(const QList<QStringList> &stringPools)
    {
        qsizetype total = 0;
        for (const QStringList &pool : stringPools)
        {
            for (const QString &value : pool)
                total += value.toUtf8().size();
        }
        return total;
    }

    QVariantList buildCompactTorrentFieldDefaults(const QHash<QString, QVariantMap> &rows
        , const QList<CompactTorrentFieldSpec> &specs)
    {
        QList<CompactTorrentFieldDefaultAccumulator> accumulators
            = makeCompactTorrentFieldDefaultAccumulators(specs);
        for (auto it = rows.cbegin(); it != rows.cend(); ++it)
            accumulateCompactTorrentFieldDefaults(accumulators, it.value(), specs);
        return finalizeCompactTorrentFieldDefaults(accumulators);
    }

    PreparedCompactRowSet prepareCompactRowSet(const QHash<QString, QVariantMap> &rows
        , const QList<CompactTorrentFieldSpec> &specs, const bool enableStringPool)
    {
        PreparedCompactRowSet prepared;
        prepared.fieldDefaults = buildCompactTorrentFieldDefaults(rows, specs);
        for (const QVariant &value : asConst(prepared.fieldDefaults))
            prepared.fieldDefaultsBytes += estimateCompactValueBytes(value);

        if (enableStringPool)
        {
            QList<CompactTorrentStringPoolAccumulator> stringPoolAccumulators
                = makeCompactTorrentStringPoolAccumulators(specs);
            for (auto it = rows.cbegin(); it != rows.cend(); ++it)
                accumulateCompactTorrentStringPools(stringPoolAccumulators, it.value(), specs
                    , prepared.fieldDefaults);
            prepared.stringPools = finalizeCompactTorrentStringPools(stringPoolAccumulators);
            prepared.stringPoolsBytes = estimateCompactStringPoolsBytes(prepared.stringPools);
        }

        prepared.torrentRows.reserve(rows.size());
        for (auto it = rows.cbegin(); it != rows.cend(); ++it)
        {
            prepared.torrentRows.insert(it.key(), compactTorrentRow(it.value(), specs
                , prepared.fieldDefaults, prepared.stringPools));
        }

        return prepared;
    }

    qsizetype compactRowSetStorageBytes(const QVariantList &fieldDefaults, const qsizetype fieldDefaultsBytes
        , const QList<QStringList> &stringPools, const qsizetype stringPoolsBytes
        , const QHash<QString, QByteArray> &torrentRows)
    {
        Q_UNUSED(fieldDefaults);
        Q_UNUSED(stringPools);

        qsizetype total = fieldDefaultsBytes + stringPoolsBytes;
        for (auto it = torrentRows.cbegin(); it != torrentRows.cend(); ++it)
            total += it.value().size();
        return total;
    }

    WebUISync::PreparedCompactSnapshot prepareCompactSnapshotImpl(const QHash<QString, QVariantMap> &rows
        , const bool enableStringPool, const bool enableDescriptiveSidecar
        , const bool enableDescriptiveSubgroups
        , const bool enableDeferredDescriptiveNumericOnDemand)
    {
        WebUISync::PreparedCompactSnapshot prepared;
        const bool useDescriptiveSubgroups = enableDescriptiveSidecar && enableDescriptiveSubgroups;
        const PreparedCompactRowSet primary = prepareCompactRowSet(rows
            , retainedPrimaryCompactTorrentFieldSpecs(enableDescriptiveSidecar
                , enableDeferredDescriptiveNumericOnDemand)
            , enableStringPool);
        prepared.fieldDefaults = primary.fieldDefaults;
        prepared.fieldDefaultsBytes = primary.fieldDefaultsBytes;
        prepared.stringPools = primary.stringPools;
        prepared.stringPoolsBytes = primary.stringPoolsBytes;
        prepared.torrentRows = primary.torrentRows;

        if (useDescriptiveSubgroups)
        {
            const PreparedCompactRowSet descriptiveStatus = prepareCompactRowSet(rows
                , descriptiveStatusCompactTorrentFieldSpecs(), enableStringPool);
            prepared.descriptiveStatusFieldDefaults = descriptiveStatus.fieldDefaults;
            prepared.descriptiveStatusFieldDefaultsBytes = descriptiveStatus.fieldDefaultsBytes;
            prepared.descriptiveStatusStringPools = descriptiveStatus.stringPools;
            prepared.descriptiveStatusStringPoolsBytes = descriptiveStatus.stringPoolsBytes;
            prepared.descriptiveStatusTorrentRows = descriptiveStatus.torrentRows;

            const PreparedCompactRowSet descriptiveLifecycle = prepareCompactRowSet(rows
                , descriptiveLifecycleCompactTorrentFieldSpecs(), enableStringPool);
            prepared.descriptiveLifecycleFieldDefaults = descriptiveLifecycle.fieldDefaults;
            prepared.descriptiveLifecycleFieldDefaultsBytes = descriptiveLifecycle.fieldDefaultsBytes;
            prepared.descriptiveLifecycleStringPools = descriptiveLifecycle.stringPools;
            prepared.descriptiveLifecycleStringPoolsBytes = descriptiveLifecycle.stringPoolsBytes;
            prepared.descriptiveLifecycleTorrentRows = descriptiveLifecycle.torrentRows;
        }
        else if (enableDescriptiveSidecar)
        {
            const PreparedCompactRowSet descriptive = prepareCompactRowSet(rows
                , descriptiveCompactTorrentFieldSpecs(), enableStringPool);
            prepared.descriptiveFieldDefaults = descriptive.fieldDefaults;
            prepared.descriptiveFieldDefaultsBytes = descriptive.fieldDefaultsBytes;
            prepared.descriptiveStringPools = descriptive.stringPools;
            prepared.descriptiveStringPoolsBytes = descriptive.stringPoolsBytes;
            prepared.descriptiveTorrentRows = descriptive.torrentRows;
        }

        return prepared;
    }

    WebUISync::PreparedCompactSnapshot prepareCompactSnapshotFromTorrents(
        const QVector<BitTorrent::Torrent *> &torrents, const bool enableStringPool
        , const bool enableDescriptiveSidecar, const bool enableDescriptiveSubgroups
        , const bool enableDeferredDescriptiveNumericOnDemand)
    {
        if (enableDeferredDescriptiveNumericOnDemand && !enableDescriptiveSidecar)
        {
            const QList<CompactTorrentFieldSpec> &specs = deferredDescriptiveNumericPrimaryCompactTorrentFieldSpecs();
            QList<CompactTorrentFieldDefaultAccumulator> accumulators
                = makeCompactTorrentFieldDefaultAccumulators(specs);
            for (const BitTorrent::Torrent *torrent : torrents)
            {
                QVariantMap row = serialize(*torrent);
                row.remove(KEY_TORRENT_ID);
                accumulateCompactTorrentFieldDefaults(accumulators, row, specs);
            }

            WebUISync::PreparedCompactSnapshot prepared;
            prepared.fieldDefaults = finalizeCompactTorrentFieldDefaults(accumulators);
            for (const QVariant &value : asConst(prepared.fieldDefaults))
                prepared.fieldDefaultsBytes += estimateCompactValueBytes(value);

            if (enableStringPool)
            {
                QList<CompactTorrentStringPoolAccumulator> stringPoolAccumulators
                    = makeCompactTorrentStringPoolAccumulators(specs);
                for (const BitTorrent::Torrent *torrent : torrents)
                {
                    QVariantMap row = serialize(*torrent);
                    row.remove(KEY_TORRENT_ID);
                    accumulateCompactTorrentStringPools(stringPoolAccumulators, row
                        , specs, prepared.fieldDefaults);
                }
                prepared.stringPools = finalizeCompactTorrentStringPools(stringPoolAccumulators);
                prepared.stringPoolsBytes = estimateCompactStringPoolsBytes(prepared.stringPools);
            }

            prepared.torrentRows.reserve(torrents.size());
            for (const BitTorrent::Torrent *torrent : torrents)
            {
                QVariantMap row = serialize(*torrent);
                row.remove(KEY_TORRENT_ID);
                prepared.torrentRows.insert(torrent->id().toString()
                    , compactTorrentRow(row, specs, prepared.fieldDefaults, prepared.stringPools));
            }

            return prepared;
        }

        if (enableDescriptiveSidecar || enableDeferredDescriptiveNumericOnDemand)
        {
            QHash<QString, QVariantMap> rows;
            rows.reserve(torrents.size());
            for (const BitTorrent::Torrent *torrent : torrents)
            {
                QVariantMap row = serialize(*torrent);
                row.remove(KEY_TORRENT_ID);
                rows.insert(torrent->id().toString(), row);
            }

            return prepareCompactSnapshotImpl(rows, enableStringPool, enableDescriptiveSidecar
                , enableDescriptiveSubgroups, enableDeferredDescriptiveNumericOnDemand);
        }

        QList<CompactTorrentFieldDefaultAccumulator> accumulators
            = makeCompactTorrentFieldDefaultAccumulators(compactTorrentFieldSpecs());
        for (const BitTorrent::Torrent *torrent : torrents)
        {
            QVariantMap row = serialize(*torrent);
            row.remove(KEY_TORRENT_ID);
            accumulateCompactTorrentFieldDefaults(accumulators, row, compactTorrentFieldSpecs());
        }

        WebUISync::PreparedCompactSnapshot prepared;
        prepared.fieldDefaults = finalizeCompactTorrentFieldDefaults(accumulators);
        for (const QVariant &value : asConst(prepared.fieldDefaults))
            prepared.fieldDefaultsBytes += estimateCompactValueBytes(value);

        if (enableStringPool)
        {
            QList<CompactTorrentStringPoolAccumulator> stringPoolAccumulators
                = makeCompactTorrentStringPoolAccumulators(compactTorrentFieldSpecs());
            for (const BitTorrent::Torrent *torrent : torrents)
            {
                QVariantMap row = serialize(*torrent);
                row.remove(KEY_TORRENT_ID);
                accumulateCompactTorrentStringPools(stringPoolAccumulators, row
                    , compactTorrentFieldSpecs(), prepared.fieldDefaults);
            }
            prepared.stringPools = finalizeCompactTorrentStringPools(stringPoolAccumulators);
            prepared.stringPoolsBytes = estimateCompactStringPoolsBytes(prepared.stringPools);
        }

        prepared.torrentRows.reserve(torrents.size());
        for (const BitTorrent::Torrent *torrent : torrents)
        {
            QVariantMap row = serialize(*torrent);
            row.remove(KEY_TORRENT_ID);
            prepared.torrentRows.insert(torrent->id().toString()
                , compactTorrentRow(row, compactTorrentFieldSpecs(), prepared.fieldDefaults
                    , prepared.stringPools));
        }

        return prepared;
    }

    QByteArray compactTorrentRow(const QVariantMap &row, const QList<CompactTorrentFieldSpec> &specs
        , const QVariantList &defaults, const QList<QStringList> &stringPools)
    {
        QByteArray bytes;
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out.setVersion(compactTorrentRowStreamVersion());
        out << quint16(specs.size());

        for (int i = 0; i < specs.size(); ++i)
        {
            const CompactTorrentFieldSpec &spec = specs.at(i);
            const QVariant value = row.value(spec.key);
            const QVariant defaultValue = defaults.value(i);
            if (!value.isValid() || (defaultValue.isValid() && (value == defaultValue)))
            {
                out << false;
                continue;
            }

            out << true;
            if (std::holds_alternative<QString>(spec.defaultValue))
            {
                const QString stringValue = value.toString();
                const QStringList pool = stringPools.value(i);
                const int poolIndex = pool.indexOf(stringValue);
                out << qint32(poolIndex);
                if (poolIndex < 0)
                    out << stringValue;
            }
            else if (std::holds_alternative<qlonglong>(spec.defaultValue))
                out << value.toLongLong();
            else if (std::holds_alternative<int>(spec.defaultValue))
                out << value.toInt();
            else if (std::holds_alternative<bool>(spec.defaultValue))
                out << value.toBool();
            else if (std::holds_alternative<double>(spec.defaultValue))
                out << value.toDouble();
        }

        return bytes;
    }

    QVariantMap expandCompactTorrentRow(const QByteArray &row, const QList<CompactTorrentFieldSpec> &specs
        , const QVariantList &defaults, const QList<QStringList> &stringPools)
    {
        if (row.isEmpty())
            return {};

        QVariantMap expanded;
        QDataStream in(row);
        in.setVersion(compactTorrentRowStreamVersion());

        quint16 fieldCount = 0;
        in >> fieldCount;
        const int count = qMin<int>(fieldCount, specs.size());
        for (int i = 0; i < count; ++i)
        {
            bool present = false;
            in >> present;
            const CompactTorrentFieldSpec &spec = specs.at(i);
            const QVariant defaultValue = defaults.value(i);
            if (!present)
            {
                if (defaultValue.isValid())
                    expanded.insert(spec.key, defaultValue);
                continue;
            }

            if (std::holds_alternative<QString>(spec.defaultValue))
            {
                qint32 poolIndex = -1;
                in >> poolIndex;
                QString value;
                if ((poolIndex >= 0) && (poolIndex < stringPools.value(i).size()))
                    value = stringPools.value(i).at(poolIndex);
                else
                    in >> value;
                expanded.insert(spec.key, value);
            }
            else if (std::holds_alternative<qlonglong>(spec.defaultValue))
            {
                qlonglong value = 0;
                in >> value;
                expanded.insert(spec.key, value);
            }
            else if (std::holds_alternative<int>(spec.defaultValue))
            {
                int value = 0;
                in >> value;
                expanded.insert(spec.key, value);
            }
            else if (std::holds_alternative<bool>(spec.defaultValue))
            {
                bool value = false;
                in >> value;
                expanded.insert(spec.key, value);
            }
            else if (std::holds_alternative<double>(spec.defaultValue))
            {
                double value = 0;
                in >> value;
                expanded.insert(spec.key, value);
            }
        }

        return expanded;
    }

    QJsonObject expandCompactTorrentRowObject(const QByteArray &row, const QList<CompactTorrentFieldSpec> &specs
        , const QVariantList &defaults, const QList<QStringList> &stringPools)
    {
        return QJsonObject::fromVariantMap(expandCompactTorrentRow(row, specs, defaults, stringPools));
    }

    QByteArray serializeCompactTorrentRowToJsonBytes(const QByteArray &row
        , const QList<CompactTorrentFieldSpec> &specs, const QVariantList &defaults
        , const QList<QStringList> &stringPools)
    {
        return QJsonDocument(expandCompactTorrentRowObject(row, specs, defaults, stringPools))
            .toJson(QJsonDocument::Compact);
    }

    QVariantMap getTransferInfo()
    {
        QVariantMap map;
        const auto *session = BitTorrent::Session::instance();

        const BitTorrent::SessionStatus &sessionStatus = session->status();
        const BitTorrent::CacheStatus &cacheStatus = session->cacheStatus();
        map[KEY_TRANSFER_DLSPEED] = sessionStatus.payloadDownloadRate;
        map[KEY_TRANSFER_DLDATA] = sessionStatus.totalPayloadDownload;
        map[KEY_TRANSFER_UPSPEED] = sessionStatus.payloadUploadRate;
        map[KEY_TRANSFER_UPDATA] = sessionStatus.totalPayloadUpload;
        map[KEY_TRANSFER_DLRATELIMIT] = session->downloadSpeedLimit();
        map[KEY_TRANSFER_UPRATELIMIT] = session->uploadSpeedLimit();

        const qint64 atd = sessionStatus.allTimeDownload;
        const qint64 atu = sessionStatus.allTimeUpload;
        map[KEY_TRANSFER_ALLTIME_DL] = atd;
        map[KEY_TRANSFER_ALLTIME_UL] = atu;
        map[KEY_TRANSFER_TOTAL_WASTE_SESSION] = sessionStatus.totalWasted;
        map[KEY_TRANSFER_GLOBAL_RATIO] = ((atd > 0) && (atu > 0))
            ? Utils::String::fromDouble(static_cast<qreal>(atu) / atd, 2)
            : u"-"_s;
        map[KEY_TRANSFER_TOTAL_PEER_CONNECTIONS] = sessionStatus.peersCount;

        const qreal readRatio = cacheStatus.readRatio;
        map[KEY_TRANSFER_READ_CACHE_HITS] = (readRatio > 0)
            ? Utils::String::fromDouble(100 * readRatio, 2)
            : u"0"_s;
        map[KEY_TRANSFER_TOTAL_BUFFERS_SIZE] = cacheStatus.totalUsedBuffers * 16 * 1024;

        map[KEY_TRANSFER_WRITE_CACHE_OVERLOAD] = ((sessionStatus.diskWriteQueue > 0) && (sessionStatus.peersCount > 0))
            ? Utils::String::fromDouble((100. * sessionStatus.diskWriteQueue / sessionStatus.peersCount), 2)
            : u"0"_s;
        map[KEY_TRANSFER_READ_CACHE_OVERLOAD] = ((sessionStatus.diskReadQueue > 0) && (sessionStatus.peersCount > 0))
            ? Utils::String::fromDouble((100. * sessionStatus.diskReadQueue / sessionStatus.peersCount), 2)
            : u"0"_s;

        map[KEY_TRANSFER_QUEUED_IO_JOBS] = cacheStatus.jobQueueLength;
        map[KEY_TRANSFER_AVERAGE_TIME_QUEUE] = cacheStatus.averageJobTime;
        map[KEY_TRANSFER_TOTAL_QUEUED_SIZE] = cacheStatus.queuedBytes;

        map[KEY_TRANSFER_DHT_NODES] = sessionStatus.dhtNodes;
        map[KEY_TRANSFER_CONNECTION_STATUS] = session->isListening()
            ? (sessionStatus.hasIncomingConnections ? u"connected"_s : u"firewalled"_s)
            : u"disconnected"_s;

        return map;
    }

    void processList(QVariantList prevData, const QVariantList &data, QVariantList &syncData, QVariantList &removedItems);
    void processHash(QVariantHash prevData, const QVariantHash &data, QVariantMap &syncData, QVariantList &removedItems);

    void processMap(const QVariantMap &prevData, const QVariantMap &data, QVariantMap &syncData)
    {
        syncData.clear();

        for (auto i = data.cbegin(); i != data.cend(); ++i)
        {
            const QString &key = i.key();
            const QVariant &value = i.value();
            QVariantList removedItems;

            switch (static_cast<QMetaType::Type>(value.type()))
            {
            case QMetaType::QVariantMap:
                {
                    QVariantMap map;
                    processMap(prevData[key].toMap(), value.toMap(), map);
                    if (!map.isEmpty())
                        syncData[key] = map;
                }
                break;
            case QMetaType::QVariantHash:
                {
                    QVariantMap map;
                    processHash(prevData[key].toHash(), value.toHash(), map, removedItems);
                    if (!map.isEmpty())
                        syncData[key] = map;
                    if (!removedItems.isEmpty())
                        syncData[key + KEY_SUFFIX_REMOVED] = removedItems;
                }
                break;
            case QMetaType::QVariantList:
                {
                    QVariantList list;
                    processList(prevData[key].toList(), value.toList(), list, removedItems);
                    if (!list.isEmpty())
                        syncData[key] = list;
                    if (!removedItems.isEmpty())
                        syncData[key + KEY_SUFFIX_REMOVED] = removedItems;
                }
                break;
            case QMetaType::QString:
            case QMetaType::LongLong:
            case QMetaType::Float:
            case QMetaType::Int:
            case QMetaType::Bool:
            case QMetaType::Double:
            case QMetaType::ULongLong:
            case QMetaType::UInt:
            case QMetaType::QDateTime:
            case QMetaType::Nullptr:
                if (prevData[key] != value)
                    syncData[key] = value;
                break;
            default:
                Q_ASSERT_X(false, "processMap"
                           , u"Unexpected type: %1"_s
                           .arg(QString::fromLatin1(QMetaType::typeName(static_cast<QMetaType::Type>(value.type()))))
                           .toUtf8().constData());
            }
        }
    }

    void processHash(QVariantHash prevData, const QVariantHash &data, QVariantMap &syncData, QVariantList &removedItems)
    {
        syncData.clear();
        removedItems.clear();

        if (prevData.isEmpty())
        {
            for (auto i = data.cbegin(); i != data.cend(); ++i)
                syncData[i.key()] = i.value();
        }
        else
        {
            for (auto i = data.cbegin(); i != data.cend(); ++i)
            {
                switch (i.value().type())
                {
                case QVariant::Map:
                    if (!prevData.contains(i.key()))
                    {
                        syncData[i.key()] = i.value();
                    }
                    else
                    {
                        QVariantMap map;
                        processMap(prevData[i.key()].toMap(), i.value().toMap(), map);
                        prevData.remove(i.key());
                        if (!map.isEmpty())
                            syncData[i.key()] = map;
                    }
                    break;
                case QVariant::StringList:
                    if (!prevData.contains(i.key()))
                    {
                        syncData[i.key()] = i.value();
                    }
                    else
                    {
                        QVariantList list;
                        QVariantList removedList;
                        processList(prevData[i.key()].toList(), i.value().toList(), list, removedList);
                        prevData.remove(i.key());
                        if (!list.isEmpty() || !removedList.isEmpty())
                            syncData[i.key()] = i.value();
                    }
                    break;
                default:
                    Q_ASSERT(false);
                    break;
                }
            }

            if (!prevData.isEmpty())
            {
                for (auto i = prevData.cbegin(); i != prevData.cend(); ++i)
                    removedItems << i.key();
            }
        }
    }

    void processList(QVariantList prevData, const QVariantList &data, QVariantList &syncData, QVariantList &removedItems)
    {
        syncData.clear();
        removedItems.clear();

        if (prevData.isEmpty())
        {
            syncData = data;
        }
        else
        {
            for (const QVariant &item : data)
            {
                if (!prevData.contains(item))
                    syncData.append(item);
                else
                    prevData.removeOne(item);
            }

            if (!prevData.isEmpty())
                removedItems = prevData;
        }
    }

    int nextRid(const int currentRid)
    {
        return ((currentRid % 1000000) + 1);
    }

    void mergeVariantMap(QVariantMap &target, const QVariantMap &delta)
    {
        for (auto it = delta.cbegin(); it != delta.cend(); ++it)
        {
            if ((it.value().type() == QVariant::Map) && (target.value(it.key()).type() == QVariant::Map))
            {
                QVariantMap nestedTarget = target.value(it.key()).toMap();
                mergeVariantMap(nestedTarget, it.value().toMap());
                target[it.key()] = nestedTarget;
                continue;
            }

            target[it.key()] = it.value();
        }
    }

    void appendUnique(QVariantList &list, const QVariant &item)
    {
        if (!list.contains(item))
            list.append(item);
    }

    void appendUnique(QStringList &list, const QString &item)
    {
        if (!list.contains(item))
            list.append(item);
    }

    void applyDeltaToSnapshot(WebUISync::MaindataSyncData &snapshot, const WebUISync::MaindataSyncData &delta)
    {
        for (auto it = delta.categories.cbegin(); it != delta.categories.cend(); ++it)
            mergeVariantMap(snapshot.categories[it.key()], it.value());
        for (const QString &category : asConst(delta.removedCategories))
            snapshot.categories.remove(category);

        for (const QVariant &tag : asConst(delta.tags))
            appendUnique(snapshot.tags, tag);
        for (const QString &tag : asConst(delta.removedTags))
            snapshot.tags.removeOne(tag);

        for (auto it = delta.torrents.cbegin(); it != delta.torrents.cend(); ++it)
            mergeVariantMap(snapshot.torrents[it.key()], it.value());
        for (const QString &torrentID : asConst(delta.removedTorrents))
            snapshot.torrents.remove(torrentID);

        for (auto it = delta.trackers.cbegin(); it != delta.trackers.cend(); ++it)
            snapshot.trackers[it.key()] = it.value();
        for (const QString &tracker : asConst(delta.removedTrackers))
            snapshot.trackers.remove(tracker);

        mergeVariantMap(snapshot.serverState, delta.serverState);
    }

    void mergeDeltaIntoAggregate(WebUISync::MaindataSyncData &aggregate, const WebUISync::MaindataSyncData &delta)
    {
        for (auto it = delta.categories.cbegin(); it != delta.categories.cend(); ++it)
        {
            aggregate.removedCategories.removeOne(it.key());
            mergeVariantMap(aggregate.categories[it.key()], it.value());
        }
        for (const QString &category : asConst(delta.removedCategories))
        {
            aggregate.categories.remove(category);
            appendUnique(aggregate.removedCategories, category);
        }

        for (const QVariant &tag : asConst(delta.tags))
        {
            aggregate.removedTags.removeOne(tag.toString());
            appendUnique(aggregate.tags, tag);
        }
        for (const QString &tag : asConst(delta.removedTags))
        {
            aggregate.tags.removeOne(tag);
            appendUnique(aggregate.removedTags, tag);
        }

        for (auto it = delta.torrents.cbegin(); it != delta.torrents.cend(); ++it)
        {
            aggregate.removedTorrents.removeOne(it.key());
            mergeVariantMap(aggregate.torrents[it.key()], it.value());
        }
        for (const QString &torrentID : asConst(delta.removedTorrents))
        {
            aggregate.torrents.remove(torrentID);
            appendUnique(aggregate.removedTorrents, torrentID);
        }

        for (auto it = delta.trackers.cbegin(); it != delta.trackers.cend(); ++it)
        {
            aggregate.removedTrackers.removeOne(it.key());
            aggregate.trackers[it.key()] = it.value();
        }
        for (const QString &tracker : asConst(delta.removedTrackers))
        {
            aggregate.trackers.remove(tracker);
            appendUnique(aggregate.removedTrackers, tracker);
        }

        mergeVariantMap(aggregate.serverState, delta.serverState);
    }

    QJsonObject toJsonObject(const WebUISync::MaindataSyncData &data)
    {
        QJsonObject syncData;

        if (!data.categories.isEmpty())
        {
            QJsonObject categories;
            for (auto it = data.categories.cbegin(); it != data.categories.cend(); ++it)
                categories[it.key()] = QJsonObject::fromVariantMap(it.value());
            syncData[KEY_CATEGORIES] = categories;
        }
        if (!data.removedCategories.isEmpty())
            syncData[KEY_CATEGORIES_REMOVED] = QJsonArray::fromStringList(data.removedCategories);

        if (!data.tags.isEmpty())
            syncData[KEY_TAGS] = QJsonArray::fromVariantList(data.tags);
        if (!data.removedTags.isEmpty())
            syncData[KEY_TAGS_REMOVED] = QJsonArray::fromStringList(data.removedTags);

        if (!data.torrents.isEmpty())
        {
            QJsonObject torrents;
            for (auto it = data.torrents.cbegin(); it != data.torrents.cend(); ++it)
                torrents[it.key()] = QJsonObject::fromVariantMap(it.value());
            syncData[KEY_TORRENTS] = torrents;
        }
        if (!data.removedTorrents.isEmpty())
            syncData[KEY_TORRENTS_REMOVED] = QJsonArray::fromStringList(data.removedTorrents);

        if (!data.trackers.isEmpty())
        {
            QJsonObject trackers;
            for (auto it = data.trackers.cbegin(); it != data.trackers.cend(); ++it)
                trackers[it.key()] = QJsonArray::fromStringList(it.value());
            syncData[KEY_TRACKERS] = trackers;
        }
        if (!data.removedTrackers.isEmpty())
            syncData[KEY_TRACKERS_REMOVED] = QJsonArray::fromStringList(data.removedTrackers);

        if (!data.serverState.isEmpty())
            syncData[KEY_SERVER_STATE] = QJsonObject::fromVariantMap(data.serverState);

        return syncData;
    }

    QByteArray serializeResponse(const WebUISync::MaindataSyncData &data, const bool fullUpdate, const int responseRid)
    {
        QJsonObject response = toJsonObject(data);
        if (fullUpdate)
            response[KEY_FULL_UPDATE] = true;
        response[KEY_RESPONSE_ID] = responseRid;
        return QJsonDocument(response).toJson(QJsonDocument::Compact);
    }

    QByteArray serializeJsonObject(const QJsonObject &object)
    {
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    }

    QByteArray serializeJsonArray(const QJsonArray &array)
    {
        return QJsonDocument(array).toJson(QJsonDocument::Compact);
    }

    void appendJsonFieldPrefix(QByteArray &buffer, const QString &key, bool &firstField)
    {
        if (!firstField)
            buffer += ',';
        firstField = false;
        buffer += '"';
        buffer += key.toUtf8();
        buffer += "\":";
    }

    void appendJsonLiteralField(QByteArray &buffer, const QString &key, const QByteArray &valueBytes, bool &firstField)
    {
        appendJsonFieldPrefix(buffer, key, firstField);
        buffer += valueBytes;
    }

    qsizetype estimatePayloadBytes(const WebUISync::MaindataSyncData &data)
    {
        return QJsonDocument(toJsonObject(data)).toJson(QJsonDocument::Compact).size();
    }
}

bool WebUISync::MaindataSyncData::isEmpty() const
{
    return categories.isEmpty()
        && tags.isEmpty()
        && torrents.isEmpty()
        && trackers.isEmpty()
        && serverState.isEmpty()
        && removedCategories.isEmpty()
        && removedTags.isEmpty()
        && removedTorrents.isEmpty()
        && removedTrackers.isEmpty();
}

WebUISync::MaindataSyncPlan WebUISync::planMaindataSync(MaindataSessionCursor &cursor
    , const int requestedRid, const qint64 currentRevision, const qint64 earliestRetainedRevision)
{
    bool fullUpdate = true;
    if ((requestedRid > 0) && (cursor.lastSentRid > 0))
    {
        if (cursor.lastSentRid == requestedRid)
        {
            cursor.acceptedRid = requestedRid;
            cursor.acceptedRevision = cursor.lastSentRevision;
        }

        const qint64 earliestIncrementalBaseRevision = qMax<qint64>(0, (earliestRetainedRevision - 1));
        if ((cursor.acceptedRid == requestedRid) && (cursor.acceptedRevision >= earliestIncrementalBaseRevision))
            fullUpdate = false;
    }

    const MaindataSyncPlan plan
    {
        fullUpdate,
        (fullUpdate ? 0 : cursor.acceptedRevision),
        currentRevision,
        nextRid(cursor.lastSentRid)
    };

    cursor.lastSentRid = plan.responseRid;
    cursor.lastSentRevision = currentRevision;
    return plan;
}

WebUISync::MaindataRevisionStore::MaindataRevisionStore(const qsizetype maxRetainedRevisions
    , const qsizetype maxEstimatedPayloadBytes)
    : m_maxRetainedRevisions(maxRetainedRevisions)
    , m_maxEstimatedPayloadBytes(maxEstimatedPayloadBytes)
    , m_compactTorrentRowsEnabled(Preferences::instance()->isExperimentalCompactWebUIMaindataTorrentRowsEnabled())
    , m_directCompactInitialFullResponseEnabled(Preferences::instance()->isExperimentalDirectCompactWebUIInitialFullResponseEnabled())
    , m_compactStringPoolEnabled(Preferences::instance()->isExperimentalCompactWebUIMaindataStringPoolEnabled())
    , m_compactDescriptiveSidecarEnabled(Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveSidecarEnabled())
    , m_compactDescriptiveSubgroupsEnabled(Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveSubgroupsEnabled())
    , m_compactDescriptiveLifecycleOnDemandEnabled(Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveLifecycleOnDemandEnabled())
    , m_compactDescriptiveNumericOnDemandEnabled(Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveNumericOnDemandEnabled())
{
}

WebUISync::PreparedCompactSnapshot WebUISync::prepareCompactSnapshot(const QHash<QString, QVariantMap> &rows)
{
    return prepareCompactSnapshotImpl(rows
        , Preferences::instance()->isExperimentalCompactWebUIMaindataStringPoolEnabled()
        , Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveSidecarEnabled()
        , Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveSubgroupsEnabled()
        , Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveNumericOnDemandEnabled());
}

void WebUISync::MaindataRevisionStore::initialize(const MaindataSyncData &snapshot)
{
    m_snapshot = snapshot;
    m_compactSnapshotFieldDefaults.clear();
    m_compactSnapshotFieldDefaultsBytes = 0;
    m_compactSnapshotStringPools.clear();
    m_compactSnapshotStringPoolsBytes = 0;
    m_compactSnapshotTorrentRows.clear();
    m_compactSnapshotDescriptiveFieldDefaults.clear();
    m_compactSnapshotDescriptiveFieldDefaultsBytes = 0;
    m_compactSnapshotDescriptiveStringPools.clear();
    m_compactSnapshotDescriptiveStringPoolsBytes = 0;
    m_compactSnapshotDescriptiveTorrentRows.clear();
    m_compactSnapshotDescriptiveStatusFieldDefaults.clear();
    m_compactSnapshotDescriptiveStatusFieldDefaultsBytes = 0;
    m_compactSnapshotDescriptiveStatusStringPools.clear();
    m_compactSnapshotDescriptiveStatusStringPoolsBytes = 0;
    m_compactSnapshotDescriptiveStatusTorrentRows.clear();
    m_compactSnapshotDescriptiveLifecycleFieldDefaults.clear();
    m_compactSnapshotDescriptiveLifecycleFieldDefaultsBytes = 0;
    m_compactSnapshotDescriptiveLifecycleStringPools.clear();
    m_compactSnapshotDescriptiveLifecycleStringPoolsBytes = 0;
    m_compactSnapshotDescriptiveLifecycleTorrentRows.clear();
    if (m_compactTorrentRowsEnabled)
    {
        initializeCompactSnapshot(snapshot, prepareCompactSnapshotImpl(snapshot.torrents
            , m_compactStringPoolEnabled, m_compactDescriptiveSidecarEnabled
            , m_compactDescriptiveSubgroupsEnabled
            , m_compactDescriptiveNumericOnDemandEnabled));
        return;
    }

    m_revisions.clear();
    m_retainedPayloadBytes = 0;
    m_currentRevision = 1;
}

void WebUISync::MaindataRevisionStore::initializeCompactSnapshot(const MaindataSyncData &snapshot
    , const PreparedCompactSnapshot &preparedSnapshot)
{
    m_snapshot = snapshot;
    m_snapshot.torrents.clear();
    m_compactSnapshotFieldDefaults = preparedSnapshot.fieldDefaults;
    m_compactSnapshotFieldDefaultsBytes = preparedSnapshot.fieldDefaultsBytes;
    m_compactSnapshotStringPools = preparedSnapshot.stringPools;
    m_compactSnapshotStringPoolsBytes = preparedSnapshot.stringPoolsBytes;
    m_compactSnapshotTorrentRows = preparedSnapshot.torrentRows;
    m_compactSnapshotDescriptiveFieldDefaults = preparedSnapshot.descriptiveFieldDefaults;
    m_compactSnapshotDescriptiveFieldDefaultsBytes = preparedSnapshot.descriptiveFieldDefaultsBytes;
    m_compactSnapshotDescriptiveStringPools = preparedSnapshot.descriptiveStringPools;
    m_compactSnapshotDescriptiveStringPoolsBytes = preparedSnapshot.descriptiveStringPoolsBytes;
    m_compactSnapshotDescriptiveTorrentRows = preparedSnapshot.descriptiveTorrentRows;
    m_compactSnapshotDescriptiveStatusFieldDefaults = preparedSnapshot.descriptiveStatusFieldDefaults;
    m_compactSnapshotDescriptiveStatusFieldDefaultsBytes = preparedSnapshot.descriptiveStatusFieldDefaultsBytes;
    m_compactSnapshotDescriptiveStatusStringPools = preparedSnapshot.descriptiveStatusStringPools;
    m_compactSnapshotDescriptiveStatusStringPoolsBytes = preparedSnapshot.descriptiveStatusStringPoolsBytes;
    m_compactSnapshotDescriptiveStatusTorrentRows = preparedSnapshot.descriptiveStatusTorrentRows;
    m_compactSnapshotDescriptiveLifecycleFieldDefaults = preparedSnapshot.descriptiveLifecycleFieldDefaults;
    m_compactSnapshotDescriptiveLifecycleFieldDefaultsBytes = preparedSnapshot.descriptiveLifecycleFieldDefaultsBytes;
    m_compactSnapshotDescriptiveLifecycleStringPools = preparedSnapshot.descriptiveLifecycleStringPools;
    m_compactSnapshotDescriptiveLifecycleStringPoolsBytes = preparedSnapshot.descriptiveLifecycleStringPoolsBytes;
    m_compactSnapshotDescriptiveLifecycleTorrentRows = preparedSnapshot.descriptiveLifecycleTorrentRows;
    if (!shouldRetainDescriptiveLifecycleRows())
        m_compactSnapshotDescriptiveLifecycleTorrentRows.clear();
    m_revisions.clear();
    m_retainedPayloadBytes = 0;
    m_currentRevision = 1;
    m_cachedInitialFullUpdateRevision = 0;
    m_cachedInitialFullUpdateResponse.clear();
}

void WebUISync::MaindataRevisionStore::reset()
{
    m_snapshot = {};
    m_compactSnapshotFieldDefaults.clear();
    m_compactSnapshotFieldDefaultsBytes = 0;
    m_compactSnapshotStringPools.clear();
    m_compactSnapshotStringPoolsBytes = 0;
    m_compactSnapshotTorrentRows.clear();
    m_compactSnapshotDescriptiveFieldDefaults.clear();
    m_compactSnapshotDescriptiveFieldDefaultsBytes = 0;
    m_compactSnapshotDescriptiveStringPools.clear();
    m_compactSnapshotDescriptiveStringPoolsBytes = 0;
    m_compactSnapshotDescriptiveTorrentRows.clear();
    m_compactSnapshotDescriptiveStatusFieldDefaults.clear();
    m_compactSnapshotDescriptiveStatusFieldDefaultsBytes = 0;
    m_compactSnapshotDescriptiveStatusStringPools.clear();
    m_compactSnapshotDescriptiveStatusStringPoolsBytes = 0;
    m_compactSnapshotDescriptiveStatusTorrentRows.clear();
    m_compactSnapshotDescriptiveLifecycleFieldDefaults.clear();
    m_compactSnapshotDescriptiveLifecycleFieldDefaultsBytes = 0;
    m_compactSnapshotDescriptiveLifecycleStringPools.clear();
    m_compactSnapshotDescriptiveLifecycleStringPoolsBytes = 0;
    m_compactSnapshotDescriptiveLifecycleTorrentRows.clear();
    m_revisions.clear();
    m_retainedPayloadBytes = 0;
    m_currentRevision = 0;
    m_cachedInitialFullUpdateRevision = 0;
    m_cachedInitialFullUpdateResponse.clear();
}

void WebUISync::MaindataRevisionStore::absorbDeltaIntoSnapshot(const MaindataSyncData &delta)
{
    Q_ASSERT(m_currentRevision > 0);
    if (delta.isEmpty())
        return;

    applyDeltaToSnapshot(m_snapshot, delta);
    if (m_compactTorrentRowsEnabled)
    {
        m_snapshot.torrents.clear();
        applyDeltaToCompactSnapshot(delta);
    }
}

void WebUISync::MaindataRevisionStore::storeRevision(const MaindataSyncData &delta)
{
    Q_ASSERT(m_currentRevision > 0);
    if (delta.isEmpty())
        return;

    applyDeltaToSnapshot(m_snapshot, delta);
    if (m_compactTorrentRowsEnabled)
        m_snapshot.torrents.clear();

    RevisionEntry entry;
    entry.revision = ++m_currentRevision;
    entry.delta = delta;
    entry.estimatedPayloadBytes = estimatePayloadBytes(delta);
    m_revisions.append(entry);
    m_retainedPayloadBytes += entry.estimatedPayloadBytes;

    if (m_compactTorrentRowsEnabled)
        applyDeltaToCompactSnapshot(delta);

    evictRevisions();
}

qint64 WebUISync::MaindataRevisionStore::currentRevision() const
{
    return m_currentRevision;
}

qint64 WebUISync::MaindataRevisionStore::earliestRetainedRevision() const
{
    if (m_currentRevision == 0)
        return 1;

    if (m_revisions.isEmpty())
        return (m_currentRevision + 1);

    return m_revisions.constFirst().revision;
}

const WebUISync::MaindataSyncData &WebUISync::MaindataRevisionStore::snapshot() const
{
    return m_snapshot;
}

QVariantMap WebUISync::MaindataRevisionStore::snapshotTorrentRow(const QString &torrentID) const
{
    if (!m_compactTorrentRowsEnabled)
        return m_snapshot.torrents.value(torrentID);

    return expandCompactSnapshotTorrentRow(torrentID, !shouldMaterializeDescriptiveNumericOnDemand());
}

bool WebUISync::MaindataRevisionStore::hasCachedInitialFullUpdateResponse() const
{
    return !m_cachedInitialFullUpdateResponse.isEmpty();
}

qsizetype WebUISync::MaindataRevisionStore::cachedInitialFullUpdateResponseBytes() const
{
    return m_cachedInitialFullUpdateResponse.size();
}

qsizetype WebUISync::MaindataRevisionStore::estimatedWideSnapshotStoreBytes() const
{
    // In non-compact mode we retain wide QVariant snapshot rows in memory.
    if (m_compactTorrentRowsEnabled || (m_currentRevision <= 0) || m_snapshot.isEmpty())
        return 0;

    return estimatePayloadBytes(m_snapshot);
}

qsizetype WebUISync::MaindataRevisionStore::retainedRevisionPayloadBytes() const
{
    return m_retainedPayloadBytes;
}

qsizetype WebUISync::MaindataRevisionStore::compactSnapshotStorageBytes() const
{
    return compactSnapshotPrimaryStorageBytes() + compactSnapshotDescriptiveStorageBytes();
}

qsizetype WebUISync::MaindataRevisionStore::compactSnapshotPrimaryStorageBytes() const
{
    return compactRowSetStorageBytes(m_compactSnapshotFieldDefaults, m_compactSnapshotFieldDefaultsBytes
        , m_compactSnapshotStringPools, m_compactSnapshotStringPoolsBytes, m_compactSnapshotTorrentRows);
}

qsizetype WebUISync::MaindataRevisionStore::compactSnapshotDescriptiveStorageBytes() const
{
    return compactRowSetStorageBytes(m_compactSnapshotDescriptiveFieldDefaults
            , m_compactSnapshotDescriptiveFieldDefaultsBytes
            , m_compactSnapshotDescriptiveStringPools
            , m_compactSnapshotDescriptiveStringPoolsBytes
            , m_compactSnapshotDescriptiveTorrentRows)
        + compactSnapshotDescriptiveStatusStorageBytes()
        + compactSnapshotDescriptiveLifecycleStorageBytes();
}

qsizetype WebUISync::MaindataRevisionStore::compactSnapshotDescriptiveStatusStorageBytes() const
{
    return compactRowSetStorageBytes(m_compactSnapshotDescriptiveStatusFieldDefaults
        , m_compactSnapshotDescriptiveStatusFieldDefaultsBytes
        , m_compactSnapshotDescriptiveStatusStringPools
        , m_compactSnapshotDescriptiveStatusStringPoolsBytes
        , m_compactSnapshotDescriptiveStatusTorrentRows);
}

qsizetype WebUISync::MaindataRevisionStore::compactSnapshotDescriptiveLifecycleStorageBytes() const
{
    return compactRowSetStorageBytes(m_compactSnapshotDescriptiveLifecycleFieldDefaults
        , m_compactSnapshotDescriptiveLifecycleFieldDefaultsBytes
        , m_compactSnapshotDescriptiveLifecycleStringPools
        , m_compactSnapshotDescriptiveLifecycleStringPoolsBytes
        , m_compactSnapshotDescriptiveLifecycleTorrentRows);
}

QByteArray WebUISync::MaindataRevisionStore::buildInitialFullUpdateResponse(MaindataSessionCursor &cursor
    , const int requestedRid) const
{
    if (Preferences::instance()->isExperimentalDisableWebUIInitialFullResponseCacheEnabled())
        return {};

    Q_ASSERT(m_currentRevision > 0);

    const bool isFreshSession = (requestedRid == 0)
        && (cursor.acceptedRid == 0)
        && (cursor.acceptedRevision == 0)
        && (cursor.lastSentRid == 0)
        && (cursor.lastSentRevision == 0);
    if (!isFreshSession)
        return {};

    const MaindataSyncPlan plan = planMaindataSync(cursor, requestedRid, m_currentRevision, earliestRetainedRevision());
    Q_ASSERT(plan.fullUpdate);
    Q_ASSERT(plan.responseRid == 1);
    Q_ASSERT(plan.targetRevision == m_currentRevision);

    if (m_compactTorrentRowsEnabled)
    {
        if (m_directCompactInitialFullResponseEnabled)
            return buildCompactSnapshotInitialFullUpdateResponseBytes(plan.responseRid);

        QJsonObject response = buildSnapshotResponseObject();
        response[KEY_FULL_UPDATE] = true;
        response[KEY_RESPONSE_ID] = plan.responseRid;
        return QJsonDocument(response).toJson(QJsonDocument::Compact);
    }

    if (m_cachedInitialFullUpdateRevision != m_currentRevision)
    {
        m_cachedInitialFullUpdateResponse = serializeResponse(m_snapshot, true, plan.responseRid);
        m_cachedInitialFullUpdateRevision = m_currentRevision;
    }

    return m_cachedInitialFullUpdateResponse;
}

QJsonObject WebUISync::MaindataRevisionStore::buildResponse(MaindataSessionCursor &cursor, const int requestedRid) const
{
    Q_ASSERT(m_currentRevision > 0);

    const MaindataSyncPlan plan = planMaindataSync(cursor, requestedRid, m_currentRevision, earliestRetainedRevision());

    QJsonObject response = (plan.fullUpdate && m_compactTorrentRowsEnabled)
        ? buildSnapshotResponseObject()
        : toJsonObject(plan.fullUpdate
            ? m_snapshot
            : aggregateRange(plan.baseRevision, plan.targetRevision));

    if (plan.fullUpdate)
        response[KEY_FULL_UPDATE] = true;
    response[KEY_RESPONSE_ID] = plan.responseRid;

    return response;
}

WebUISync::MaindataSyncData WebUISync::MaindataRevisionStore::aggregateRange(const qint64 baseRevision
    , const qint64 targetRevision) const
{
    MaindataSyncData aggregate;

    for (const RevisionEntry &entry : asConst(m_revisions))
    {
        if ((entry.revision <= baseRevision) || (entry.revision > targetRevision))
            continue;

        mergeDeltaIntoAggregate(aggregate, entry.delta);
    }

    return aggregate;
}

QVariantMap WebUISync::MaindataRevisionStore::expandCompactSnapshotTorrentRow(const QString &torrentID
    , const bool materializeDeferredFamilies) const
{
    QVariantMap row = expandCompactTorrentRow(m_compactSnapshotTorrentRows.value(torrentID)
        , retainedPrimaryCompactTorrentFieldSpecs(m_compactDescriptiveSidecarEnabled
            , shouldMaterializeDescriptiveNumericOnDemand())
        , m_compactSnapshotFieldDefaults, m_compactSnapshotStringPools);

    if (materializeDeferredFamilies && shouldMaterializeDescriptiveNumericOnDemand())
    {
        mergeVariantMap(row, materializeDescriptiveNumericTorrentRow(torrentID));
        return row;
    }

    if (!m_compactDescriptiveSidecarEnabled)
        return row;

    if (m_compactDescriptiveSubgroupsEnabled)
    {
        mergeVariantMap(row, expandCompactTorrentRow(m_compactSnapshotDescriptiveStatusTorrentRows.value(torrentID)
            , descriptiveStatusCompactTorrentFieldSpecs(), m_compactSnapshotDescriptiveStatusFieldDefaults
            , m_compactSnapshotDescriptiveStatusStringPools));
        mergeVariantMap(row, materializeDescriptiveLifecycleTorrentRow(torrentID));
    }
    else
    {
        mergeVariantMap(row, expandCompactTorrentRow(m_compactSnapshotDescriptiveTorrentRows.value(torrentID)
            , descriptiveCompactTorrentFieldSpecs(), m_compactSnapshotDescriptiveFieldDefaults
            , m_compactSnapshotDescriptiveStringPools));
    }
    return row;
}

QVariantMap WebUISync::MaindataRevisionStore::materializeDescriptiveNumericTorrentRow(const QString &torrentID) const
{
    if (const auto *session = BitTorrent::Session::instance())
    {
        if (const BitTorrent::Torrent *torrent = session->getTorrent(BitTorrent::TorrentID::fromString(torrentID)))
        {
            QVariantMap row = serializeDescriptiveFields(*torrent);
            mergeVariantMap(row, serializeNumericFields(*torrent));
            return row;
        }
    }

    QVariantMap row = compactTorrentStaticDefaultsToRow(descriptiveCompactTorrentFieldSpecs());
    mergeVariantMap(row, compactTorrentStaticDefaultsToRow(numericCompactTorrentFieldSpecs()));
    return row;
}

QVariantMap WebUISync::MaindataRevisionStore::materializeDescriptiveLifecycleTorrentRow(const QString &torrentID) const
{
    if (shouldRetainDescriptiveLifecycleRows())
    {
        return expandCompactTorrentRow(m_compactSnapshotDescriptiveLifecycleTorrentRows.value(torrentID)
            , descriptiveLifecycleCompactTorrentFieldSpecs()
            , m_compactSnapshotDescriptiveLifecycleFieldDefaults
            , m_compactSnapshotDescriptiveLifecycleStringPools);
    }

    if (const auto *session = BitTorrent::Session::instance())
    {
        if (const BitTorrent::Torrent *torrent = session->getTorrent(BitTorrent::TorrentID::fromString(torrentID)))
        {
            return serializeDescriptiveLifecycleFields(*torrent);
        }
    }

    return compactTorrentDefaultsToRow(descriptiveLifecycleCompactTorrentFieldSpecs()
        , m_compactSnapshotDescriptiveLifecycleFieldDefaults);
}

bool WebUISync::MaindataRevisionStore::shouldMaterializeDescriptiveNumericOnDemand() const
{
    return !m_compactDescriptiveSidecarEnabled
        && m_compactDescriptiveNumericOnDemandEnabled;
}

bool WebUISync::MaindataRevisionStore::shouldRetainDescriptiveLifecycleRows() const
{
    return !m_compactDescriptiveSidecarEnabled
        || !m_compactDescriptiveSubgroupsEnabled
        || !m_compactDescriptiveLifecycleOnDemandEnabled;
}

void WebUISync::MaindataRevisionStore::applyDeltaToCompactSnapshot(const MaindataSyncData &delta)
{
    for (auto it = delta.torrents.cbegin(); it != delta.torrents.cend(); ++it)
    {
        QVariantMap row = expandCompactSnapshotTorrentRow(it.key(), !shouldMaterializeDescriptiveNumericOnDemand());
        mergeVariantMap(row, it.value());

        if (m_compactDescriptiveSidecarEnabled)
        {
            m_compactSnapshotTorrentRows.insert(it.key(), compactTorrentRow(row
                , primaryCompactTorrentFieldSpecs(), m_compactSnapshotFieldDefaults
                , m_compactSnapshotStringPools));
            if (m_compactDescriptiveSubgroupsEnabled)
            {
                m_compactSnapshotDescriptiveStatusTorrentRows.insert(it.key(), compactTorrentRow(row
                    , descriptiveStatusCompactTorrentFieldSpecs()
                    , m_compactSnapshotDescriptiveStatusFieldDefaults
                    , m_compactSnapshotDescriptiveStatusStringPools));
                if (shouldRetainDescriptiveLifecycleRows())
                {
                    m_compactSnapshotDescriptiveLifecycleTorrentRows.insert(it.key(), compactTorrentRow(row
                        , descriptiveLifecycleCompactTorrentFieldSpecs()
                        , m_compactSnapshotDescriptiveLifecycleFieldDefaults
                        , m_compactSnapshotDescriptiveLifecycleStringPools));
                }
            }
            else
            {
                m_compactSnapshotDescriptiveTorrentRows.insert(it.key(), compactTorrentRow(row
                    , descriptiveCompactTorrentFieldSpecs(), m_compactSnapshotDescriptiveFieldDefaults
                    , m_compactSnapshotDescriptiveStringPools));
            }
        }
        else
        {
            m_compactSnapshotTorrentRows.insert(it.key()
                , compactTorrentRow(row
                    , retainedPrimaryCompactTorrentFieldSpecs(m_compactDescriptiveSidecarEnabled
                        , shouldMaterializeDescriptiveNumericOnDemand())
                    , m_compactSnapshotFieldDefaults
                    , m_compactSnapshotStringPools));
        }
    }

    for (const QString &torrentID : asConst(delta.removedTorrents))
    {
        m_compactSnapshotTorrentRows.remove(torrentID);
        m_compactSnapshotDescriptiveTorrentRows.remove(torrentID);
        m_compactSnapshotDescriptiveStatusTorrentRows.remove(torrentID);
        m_compactSnapshotDescriptiveLifecycleTorrentRows.remove(torrentID);
    }
}

QByteArray WebUISync::MaindataRevisionStore::buildCompactSnapshotInitialFullUpdateResponseBytes(const int responseRid) const
{
    QByteArray response;
    response += '{';
    bool firstField = true;

    if (!m_snapshot.categories.isEmpty())
    {
        QJsonObject categories;
        for (auto it = m_snapshot.categories.cbegin(); it != m_snapshot.categories.cend(); ++it)
            categories.insert(it.key(), QJsonObject::fromVariantMap(it.value()));
        appendJsonLiteralField(response, KEY_CATEGORIES, serializeJsonObject(categories), firstField);
    }
    if (!m_snapshot.removedCategories.isEmpty())
        appendJsonLiteralField(response, KEY_CATEGORIES_REMOVED
                , serializeJsonArray(QJsonArray::fromStringList(m_snapshot.removedCategories)), firstField);

    if (!m_snapshot.tags.isEmpty())
        appendJsonLiteralField(response, KEY_TAGS
                , serializeJsonArray(QJsonArray::fromVariantList(m_snapshot.tags)), firstField);
    if (!m_snapshot.removedTags.isEmpty())
        appendJsonLiteralField(response, KEY_TAGS_REMOVED
                , serializeJsonArray(QJsonArray::fromStringList(m_snapshot.removedTags)), firstField);

    if (!m_compactSnapshotTorrentRows.isEmpty())
    {
        appendJsonFieldPrefix(response, KEY_TORRENTS, firstField);
        response += '{';

        bool firstTorrent = true;
        for (auto it = m_compactSnapshotTorrentRows.cbegin(); it != m_compactSnapshotTorrentRows.cend(); ++it)
        {
            if (!firstTorrent)
                response += ',';
            firstTorrent = false;

            response += '"';
            response += it.key().toUtf8();
            response += "\":";
            if (m_compactDescriptiveSidecarEnabled)
            {
                response += QJsonDocument(QJsonObject::fromVariantMap(
                    expandCompactSnapshotTorrentRow(it.key()))).toJson(QJsonDocument::Compact);
            }
            else if (shouldMaterializeDescriptiveNumericOnDemand())
            {
                response += QJsonDocument(QJsonObject::fromVariantMap(
                    expandCompactSnapshotTorrentRow(it.key()))).toJson(QJsonDocument::Compact);
            }
            else
            {
                response += serializeCompactTorrentRowToJsonBytes(it.value(), compactTorrentFieldSpecs()
                    , m_compactSnapshotFieldDefaults, m_compactSnapshotStringPools);
            }
        }

        response += '}';
    }
    if (!m_snapshot.removedTorrents.isEmpty())
        appendJsonLiteralField(response, KEY_TORRENTS_REMOVED
                , serializeJsonArray(QJsonArray::fromStringList(m_snapshot.removedTorrents)), firstField);

    if (!m_snapshot.trackers.isEmpty())
    {
        QJsonObject trackers;
        for (auto it = m_snapshot.trackers.cbegin(); it != m_snapshot.trackers.cend(); ++it)
            trackers.insert(it.key(), QJsonArray::fromStringList(it.value()));
        appendJsonLiteralField(response, KEY_TRACKERS, serializeJsonObject(trackers), firstField);
    }
    if (!m_snapshot.removedTrackers.isEmpty())
        appendJsonLiteralField(response, KEY_TRACKERS_REMOVED
                , serializeJsonArray(QJsonArray::fromStringList(m_snapshot.removedTrackers)), firstField);

    if (!m_snapshot.serverState.isEmpty())
        appendJsonLiteralField(response, KEY_SERVER_STATE
                , serializeJsonObject(QJsonObject::fromVariantMap(m_snapshot.serverState)), firstField);

    appendJsonLiteralField(response, KEY_FULL_UPDATE, "true", firstField);
    appendJsonLiteralField(response, KEY_RESPONSE_ID, QByteArray::number(responseRid), firstField);
    response += '}';
    return response;
}

QJsonObject WebUISync::MaindataRevisionStore::buildSnapshotResponseObject() const
{
    QJsonObject response = toJsonObject(m_snapshot);
    if (!m_compactSnapshotTorrentRows.isEmpty())
    {
        QJsonObject torrents;
        for (auto it = m_compactSnapshotTorrentRows.cbegin(); it != m_compactSnapshotTorrentRows.cend(); ++it)
            torrents.insert(it.key(), QJsonObject::fromVariantMap(expandCompactSnapshotTorrentRow(it.key())));
        response[KEY_TORRENTS] = torrents;
    }

    return response;
}

void WebUISync::MaindataRevisionStore::evictRevisions()
{
    while (!m_revisions.isEmpty()
        && ((m_revisions.size() > m_maxRetainedRevisions)
            || (m_retainedPayloadBytes > m_maxEstimatedPayloadBytes)))
    {
        m_retainedPayloadBytes -= m_revisions.constFirst().estimatedPayloadBytes;
        m_revisions.removeFirst();
    }
}

WebUISync::MaindataSyncStore::MaindataSyncStore(QObject *parent)
    : QObject(parent)
    , m_statelessModeEnabled(Preferences::instance()->isExperimentalStatelessWebUIMaindataEnabled())
    , m_releaseIdleStateEnabled(Preferences::instance()->isExperimentalReleaseIdleWebUIMaindataStateEnabled())
    , m_trimAllocatorAfterIdleReleaseEnabled(Preferences::instance()->isExperimentalTrimAllocatorAfterIdleWebUIMaindataReleaseEnabled())
    , m_nonIdleTrimMaxActiveSyncSessions(static_cast<qsizetype>(Preferences::instance()->experimentalWebUINonIdleTrimMaxActiveSyncSessions()))
{
    const auto *preferences = Preferences::instance();
    m_firstFullTrace.outputPath = qEnvironmentVariable(FIRST_FULL_TRACE_FILE_ENV.toLocal8Bit().constData()).trimmed();
    m_firstFullTrace.enabled = !m_firstFullTrace.outputPath.isEmpty();

    m_memoryProbe.outputPath = preferences->experimentalWebUIMemoryProbeOutputPath();
    if (m_memoryProbe.outputPath.isEmpty())
        m_memoryProbe.outputPath = qEnvironmentVariable(MEMORY_PROBE_FILE_ENV.toLocal8Bit().constData()).trimmed();
    m_memoryProbe.intervalMs = preferences->experimentalWebUIMemoryProbeIntervalMs();
    m_memoryProbe.enabled = preferences->isExperimentalWebUIMemoryProbeEnabled() && !m_memoryProbe.outputPath.isEmpty();

    emitLifecycleTraceEvent(u"store_config"_s, {
        {u"stateless_mode_enabled"_s, m_statelessModeEnabled},
        {u"release_idle_state_enabled"_s, m_releaseIdleStateEnabled},
        {u"trim_allocator_after_idle_release_enabled"_s, m_trimAllocatorAfterIdleReleaseEnabled},
        {u"nonidle_trim_max_active_sync_sessions"_s, m_nonIdleTrimMaxActiveSyncSessions},
    });
    emitMemoryProbeEvent(u"store_config"_s, {
        {u"stateless_mode_enabled"_s, m_statelessModeEnabled},
        {u"release_idle_state_enabled"_s, m_releaseIdleStateEnabled},
        {u"trim_allocator_after_idle_release_enabled"_s, m_trimAllocatorAfterIdleReleaseEnabled},
        {u"nonidle_trim_max_active_sync_sessions"_s, m_nonIdleTrimMaxActiveSyncSessions},
    });

    startMemoryProbeTimer();

    const auto *session = BitTorrent::Session::instance();
    if (!m_statelessModeEnabled)
    {
        connect(session, &BitTorrent::Session::restored, this, &MaindataSyncStore::prewarm);
        connect(session, &BitTorrent::Session::categoryAdded, this, &MaindataSyncStore::onCategoryAdded);
        connect(session, &BitTorrent::Session::categoryRemoved, this, &MaindataSyncStore::onCategoryRemoved);
        connect(session, &BitTorrent::Session::categoryOptionsChanged, this, &MaindataSyncStore::onCategoryOptionsChanged);
        connect(session, &BitTorrent::Session::subcategoriesSupportChanged, this, &MaindataSyncStore::onSubcategoriesSupportChanged);
        connect(session, &BitTorrent::Session::tagAdded, this, &MaindataSyncStore::onTagAdded);
        connect(session, &BitTorrent::Session::tagRemoved, this, &MaindataSyncStore::onTagRemoved);
        connect(session, &BitTorrent::Session::torrentAdded, this, &MaindataSyncStore::onTorrentAdded);
        connect(session, &BitTorrent::Session::torrentAboutToBeRemoved, this, &MaindataSyncStore::onTorrentAboutToBeRemoved);
        connect(session, &BitTorrent::Session::torrentCategoryChanged, this, &MaindataSyncStore::onTorrentCategoryChanged);
        connect(session, &BitTorrent::Session::torrentMetadataReceived, this, &MaindataSyncStore::onTorrentMetadataReceived);
        connect(session, &BitTorrent::Session::torrentPaused, this, &MaindataSyncStore::onTorrentPaused);
        connect(session, &BitTorrent::Session::torrentResumed, this, &MaindataSyncStore::onTorrentResumed);
        connect(session, &BitTorrent::Session::torrentSavePathChanged, this, &MaindataSyncStore::onTorrentSavePathChanged);
        connect(session, &BitTorrent::Session::torrentSavingModeChanged, this, &MaindataSyncStore::onTorrentSavingModeChanged);
        connect(session, &BitTorrent::Session::torrentTagAdded, this, &MaindataSyncStore::onTorrentTagAdded);
        connect(session, &BitTorrent::Session::torrentTagRemoved, this, &MaindataSyncStore::onTorrentTagRemoved);
        connect(session, &BitTorrent::Session::torrentsUpdated, this, &MaindataSyncStore::onTorrentsUpdated);
        connect(session, &BitTorrent::Session::trackersChanged, this, &MaindataSyncStore::onTorrentTrackersChanged);

        if (session->isRestored())
            prewarm();
    }
}

void WebUISync::MaindataSyncStore::acquireSyncSession()
{
    if (m_statelessModeEnabled)
        return;

    ++m_activeSyncSessionCount;
    const QVariantMap details {
        {u"active_sync_sessions"_s, m_activeSyncSessionCount},
        {u"current_revision"_s, m_revisionStore.currentRevision()},
        {u"release_idle_state_enabled"_s, m_releaseIdleStateEnabled},
    };
    emitLifecycleTraceEvent(u"sync_session_acquired"_s, details);
    emitMemoryProbeEvent(u"sync_session_acquired"_s, details);
}

void WebUISync::MaindataSyncStore::releaseSyncSession()
{
    if (m_statelessModeEnabled)
        return;

    Q_ASSERT(m_activeSyncSessionCount > 0);
    if (Q_UNLIKELY(m_activeSyncSessionCount == 0))
        return;

    --m_activeSyncSessionCount;
    const QVariantMap details {
        {u"active_sync_sessions"_s, m_activeSyncSessionCount},
        {u"current_revision"_s, m_revisionStore.currentRevision()},
        {u"release_idle_state_enabled"_s, m_releaseIdleStateEnabled},
    };
    emitLifecycleTraceEvent(u"sync_session_released"_s, details);
    emitMemoryProbeEvent(u"sync_session_released"_s, details);
    if (m_releaseIdleStateEnabled && (m_activeSyncSessionCount == 0))
    {
        releaseSharedState();
        return;
    }

    maybeTrimAllocatorOnSyncRelease();
}

void WebUISync::MaindataSyncStore::recordWebSessionEvent(const QString &event, const QVariantMap &details)
{
    QVariantMap enrichedDetails = details;
    enrichedDetails.insert(u"active_sync_sessions"_s, m_activeSyncSessionCount);
    enrichedDetails.insert(u"current_revision"_s, m_revisionStore.currentRevision());

    emitLifecycleTraceEvent(event, enrichedDetails);
    emitMemoryProbeEvent(event, enrichedDetails);
}

void WebUISync::MaindataSyncStore::updateFreeDiskSpace(const qint64 freeDiskSpace)
{
    m_freeDiskSpace = freeDiskSpace;
}

bool WebUISync::MaindataSyncStore::hasRetainedSharedState() const
{
    return (m_revisionStore.currentRevision() > 0);
}

void WebUISync::MaindataSyncStore::emitLifecycleTraceEvent(const QString &event, const QVariantMap &details) const
{
    const QString outputPath = qEnvironmentVariable(LIFECYCLE_TRACE_FILE_ENV.toLocal8Bit().constData()).trimmed();
    if (outputPath.isEmpty())
        return;

    QFile traceFile(outputPath);
    if (!traceFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QJsonObject marker;
    marker[u"trace"_s] = u"maindata_lifecycle"_s;
    marker[u"event"_s] = event;
    marker[u"timestamp"_s] = QDateTime::currentDateTime().toString(u"yyyy-MM-ddTHH:mm:ss.zzzt"_s);

    const QJsonObject detailObject = QJsonObject::fromVariantMap(details);
    for (auto it = detailObject.begin(); it != detailObject.end(); ++it)
        marker[it.key()] = it.value();

    traceFile.write(QJsonDocument(marker).toJson(QJsonDocument::Compact));
    traceFile.write("\n");
}

void WebUISync::MaindataSyncStore::startMemoryProbeTimer()
{
    if (!m_memoryProbe.enabled)
        return;

    m_memoryProbeTimer = new QTimer(this);
    m_memoryProbeTimer->setInterval(m_memoryProbe.intervalMs);
    connect(m_memoryProbeTimer, &QTimer::timeout, this, [this]
    {
        emitMemoryProbeEvent(u"tick"_s, {});
    });
    m_memoryProbeTimer->start();
}

QVariantMap WebUISync::MaindataSyncStore::collectMemoryProbeSample() const
{
    QVariantMap sample;

    QFile statusFile(u"/proc/self/status"_s);
    if (statusFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        const QByteArray content = statusFile.readAll();
        const qint64 vmRssKb = extractFieldValueKb(content, "VmRSS:");
        const qint64 vmHwmKb = extractFieldValueKb(content, "VmHWM:");
        if (vmRssKb >= 0)
            sample[u"vmrss_kb"_s] = vmRssKb;
        if (vmHwmKb >= 0)
            sample[u"vmhwm_kb"_s] = vmHwmKb;
    }

    QFile smapsRollupFile(u"/proc/self/smaps_rollup"_s);
    if (smapsRollupFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        const QByteArray content = smapsRollupFile.readAll();
        const struct SampleField
        {
            const char *name = nullptr;
            const QString key;
        } fields[] = {
            {"Rss:", u"rss_kb"_s},
            {"Pss:", u"pss_kb"_s},
            {"Anonymous:", u"anonymous_kb"_s},
            {"Private_Dirty:", u"private_dirty_kb"_s},
            {"Private_Clean:", u"private_clean_kb"_s},
            {"Shared_Clean:", u"shared_clean_kb"_s},
            {"Shared_Dirty:", u"shared_dirty_kb"_s},
            {"SwapPss:", u"swap_pss_kb"_s},
        };

        for (const SampleField &field : fields)
        {
            const qint64 valueKb = extractFieldValueKb(content, field.name);
            if (valueKb >= 0)
                sample[field.key] = valueKb;
        }
    }

    QFile smapsFile(u"/proc/self/smaps"_s);
    if (smapsFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        const SmapsClassifiedKb classified = classifySmapsKb(smapsFile.readAll());
        sample[u"smaps_class_pss_kb.heap"_s] = classified.pssHeap;
        sample[u"smaps_class_pss_kb.anon"_s] = classified.pssAnon;
        sample[u"smaps_class_pss_kb.anon_unnamed"_s] = classified.pssAnonUnnamed;
        sample[u"smaps_class_pss_kb.anon_named"_s] = classified.pssAnonNamed;
        sample[u"smaps_class_pss_kb.file"_s] = classified.pssFile;
        sample[u"smaps_class_pss_kb.stack"_s] = classified.pssStack;
        sample[u"smaps_class_pss_kb.other"_s] = classified.pssOther;
        sample[u"smaps_class_pss_kb.sum"_s]
            = (classified.pssHeap + classified.pssAnon + classified.pssFile + classified.pssStack + classified.pssOther);
        sample[u"smaps_class_private_dirty_kb.heap"_s] = classified.privateDirtyHeap;
        sample[u"smaps_class_private_dirty_kb.anon"_s] = classified.privateDirtyAnon;
        sample[u"smaps_class_private_dirty_kb.anon_unnamed"_s] = classified.privateDirtyAnonUnnamed;
        sample[u"smaps_class_private_dirty_kb.anon_named"_s] = classified.privateDirtyAnonNamed;
        sample[u"smaps_class_private_dirty_kb.file"_s] = classified.privateDirtyFile;
        sample[u"smaps_class_private_dirty_kb.stack"_s] = classified.privateDirtyStack;
        sample[u"smaps_class_private_dirty_kb.other"_s] = classified.privateDirtyOther;
        sample[u"smaps_class_private_dirty_kb.sum"_s]
            = (classified.privateDirtyHeap + classified.privateDirtyAnon + classified.privateDirtyFile
                + classified.privateDirtyStack + classified.privateDirtyOther);

        const QList<SmapsMappingEntry> topMappings = smapsTopMappingsByPss(classified.mappingTotalsByLabel);
        sample[u"smaps_top_mappings_count"_s] = topMappings.size();
        for (int i = 0; i < topMappings.size(); ++i)
        {
            const SmapsMappingEntry &entry = topMappings.at(i);
            const QString keyPrefix = QString(u"smaps_top_mapping_%1."_s).arg(i + 1);
            sample[keyPrefix + u"label"_s] = entry.label;
            sample[keyPrefix + u"pss_kb"_s] = entry.pss;
            sample[keyPrefix + u"private_dirty_kb"_s] = entry.privateDirty;
        }

        sample[u"smaps_anon_unnamed_mappings_count"_s] = classified.anonUnnamedMappings.size();
        const QList<SmapsAnonUnnamedMappingEntry> topAnonUnnamedMappings
            = smapsTopAnonUnnamedMappings(classified.anonUnnamedMappings);
        sample[u"smaps_top_anon_unnamed_mappings_count"_s] = topAnonUnnamedMappings.size();
        for (int i = 0; i < topAnonUnnamedMappings.size(); ++i)
        {
            const SmapsAnonUnnamedMappingEntry &entry = topAnonUnnamedMappings.at(i);
            const QString keyPrefix = QString(u"smaps_top_anon_unnamed_mapping_%1."_s).arg(i + 1);
            sample[keyPrefix + u"address_range"_s] = entry.addressRange;
            sample[keyPrefix + u"permissions"_s] = entry.permissions;
            sample[keyPrefix + u"size_kb"_s] = entry.sizeKb;
            sample[keyPrefix + u"pss_kb"_s] = entry.pss;
            sample[keyPrefix + u"private_dirty_kb"_s] = entry.privateDirty;
        }
    }

#if defined(Q_OS_LINUX) && defined(__GLIBC__)
    const struct mallinfo2 mi = mallinfo2();
    const qint64 glibcArenaBytes = static_cast<qint64>(mi.arena);
    const qint64 glibcHblkhdBytes = static_cast<qint64>(mi.hblkhd);
    const qint64 glibcUordblksBytes = static_cast<qint64>(mi.uordblks);
    const qint64 glibcFordblksBytes = static_cast<qint64>(mi.fordblks);
    const qint64 glibcKeepcostBytes = static_cast<qint64>(mi.keepcost);
    const qint64 glibcManagedBytes = glibcArenaBytes + glibcHblkhdBytes;
    const qint64 glibcInuseEstimatedBytes = glibcUordblksBytes + glibcHblkhdBytes;

    sample[u"glibc_mallinfo2.arena_bytes"_s] = glibcArenaBytes;
    sample[u"glibc_mallinfo2.ordblks"_s] = static_cast<qint64>(mi.ordblks);
    sample[u"glibc_mallinfo2.hblks"_s] = static_cast<qint64>(mi.hblks);
    sample[u"glibc_mallinfo2.hblkhd_bytes"_s] = glibcHblkhdBytes;
    sample[u"glibc_mallinfo2.uordblks_bytes"_s] = glibcUordblksBytes;
    sample[u"glibc_mallinfo2.fordblks_bytes"_s] = glibcFordblksBytes;
    sample[u"glibc_mallinfo2.keepcost_bytes"_s] = glibcKeepcostBytes;

    const qint64 anonHeapPssBytes = (variantToInt64(sample, u"smaps_class_pss_kb.heap"_s)
        + variantToInt64(sample, u"smaps_class_pss_kb.anon"_s)) * 1024;
    const qint64 fileStackOtherPssBytes = (variantToInt64(sample, u"smaps_class_pss_kb.file"_s)
        + variantToInt64(sample, u"smaps_class_pss_kb.stack"_s)
        + variantToInt64(sample, u"smaps_class_pss_kb.other"_s)) * 1024;
    sample[u"resident_anon_heap_pss_bytes_total"_s] = anonHeapPssBytes;
    sample[u"resident_file_stack_other_pss_bytes_total"_s] = fileStackOtherPssBytes;
    sample[u"resident_glibc_managed_bytes_total"_s] = glibcManagedBytes;
    sample[u"resident_glibc_inuse_estimated_bytes_total"_s] = glibcInuseEstimatedBytes;
    sample[u"resident_glibc_free_in_arenas_bytes_total"_s] = glibcFordblksBytes;
    sample[u"resident_glibc_keepcost_bytes_total"_s] = glibcKeepcostBytes;
    sample[u"resident_glibc_managed_minus_inuse_estimated_bytes"_s]
        = qMax<qint64>(0, glibcManagedBytes - glibcInuseEstimatedBytes);
    sample[u"resident_anon_heap_minus_glibc_managed_bytes"_s] = qMax<qint64>(0, anonHeapPssBytes - glibcManagedBytes);
#endif

    const qint64 memoryCurrentBytes = readMemoryCurrentBytesFromCgroupV2();
    if (memoryCurrentBytes >= 0)
        sample[u"cgroup_memory_current_bytes"_s] = memoryCurrentBytes;

    sample[u"current_revision"_s] = m_revisionStore.currentRevision();
    return sample;
}

QVariantMap WebUISync::MaindataSyncStore::collectRetainedStateCounters() const
{
    QVariantMap counters {
        {u"has_retained_shared_state"_s, hasRetainedSharedState()},
        {u"active_sync_sessions"_s, m_activeSyncSessionCount},
        {u"compact_snapshot_storage_bytes"_s, m_revisionStore.compactSnapshotStorageBytes()},
        {u"compact_snapshot_primary_storage_bytes"_s, m_revisionStore.compactSnapshotPrimaryStorageBytes()},
        {u"compact_snapshot_descriptive_storage_bytes"_s, m_revisionStore.compactSnapshotDescriptiveStorageBytes()},
        {u"compact_snapshot_descriptive_status_storage_bytes"_s, m_revisionStore.compactSnapshotDescriptiveStatusStorageBytes()},
        {u"compact_snapshot_descriptive_lifecycle_storage_bytes"_s, m_revisionStore.compactSnapshotDescriptiveLifecycleStorageBytes()},
    };

    const qint64 snapshotStoreCompactBytes = m_revisionStore.compactSnapshotStorageBytes();
    const qint64 snapshotStoreWideEstimatedBytes = m_revisionStore.estimatedWideSnapshotStoreBytes();
    const qint64 snapshotStoreBytes = (snapshotStoreCompactBytes + snapshotStoreWideEstimatedBytes);
    const WideSnapshotFamilyBytes wideSnapshotFamilyBytes
        = estimateWideSnapshotFamilyBytes(m_revisionStore.snapshot(), snapshotStoreWideEstimatedBytes);
    const qint64 initialFullCacheBytes = m_revisionStore.cachedInitialFullUpdateResponseBytes();
    const qint64 retainedRevisionPayloadEstimatedBytes = m_revisionStore.retainedRevisionPayloadBytes();
    counters[u"resident_non5_bytes_total.snapshot_store"_s] = snapshotStoreBytes;
    counters[u"resident_non5_bytes_total.snapshot_store_compact"_s] = snapshotStoreCompactBytes;
    counters[u"resident_non5_bytes_total.snapshot_store_wide_estimated"_s] = snapshotStoreWideEstimatedBytes;
    counters[u"resident_non5_bytes_total.snapshot_store_wide_family.descriptive"_s] = wideSnapshotFamilyBytes.descriptive;
    counters[u"resident_non5_bytes_total.snapshot_store_wide_family.numeric"_s] = wideSnapshotFamilyBytes.numeric;
    counters[u"resident_non5_bytes_total.snapshot_store_wide_family.identity"_s] = wideSnapshotFamilyBytes.identity;
    counters[u"resident_non5_bytes_total.snapshot_store_wide_family.path"_s] = wideSnapshotFamilyBytes.path;
    counters[u"resident_non5_bytes_total.snapshot_store_wide_family.magnet"_s] = wideSnapshotFamilyBytes.magnet;
    counters[u"resident_non5_bytes_total.snapshot_store_wide_family.other"_s] = wideSnapshotFamilyBytes.other;
    counters[u"resident_non5_bytes_total.snapshot_store_wide_family.sum"_s] = wideSnapshotFamilyBytes.total();
    counters[u"resident_non5_bytes_total.initial_full_response_cache"_s] = initialFullCacheBytes;
    counters[u"resident_non5_bytes_total.retained_revisions_estimated"_s] = retainedRevisionPayloadEstimatedBytes;
    counters[u"resident_non5_bytes_total.shared_state_known"_s] = (snapshotStoreBytes + initialFullCacheBytes);
    counters[u"resident_non5_bytes_total.shared_state_known_plus_retained"_s]
        = (snapshotStoreBytes + initialFullCacheBytes + retainedRevisionPayloadEstimatedBytes);

    const auto *session = BitTorrent::Session::instance();
    struct TorrentResidentEntry
    {
        QString torrentID;
        BitTorrent::ResidentMemberFamilyBytes bytes;
    };

    qint64 totalFilePathsBytes = 0;
    qint64 totalIndexMapBytes = 0;
    qint64 totalFilePrioritiesBytes = 0;
    qint64 totalCompletedFilesBytes = 0;
    qint64 totalFilesProgressBytes = 0;
    qint64 totalPiecesBitfieldBytes = 0;
    qint64 totalAtpFilePrioritiesBytes = 0;
    qint64 totalAtpRenamedFilesBytes = 0;
    qint64 totalAtpTrackersBytes = 0;
    qint64 totalAtpTrackerTiersBytes = 0;
    qint64 totalAtpUrlSeedsBytes = 0;
    qint64 totalAtpHavePiecesBytes = 0;
    qint64 totalAtpVerifiedPiecesBytes = 0;
    qint64 totalAtpUnfinishedPiecesBytes = 0;
    qint64 totalAtpSavePathBytes = 0;
    qint64 totalPersistentNameBytes = 0;
    qint64 totalPersistentSavePathBytes = 0;
    qint64 totalPersistentDownloadPathBytes = 0;
    qint64 totalPersistentCategoryBytes = 0;
    qint64 totalTrackerEntriesBytes = 0;
    qint64 totalUrlSeedsBytes = 0;
    qint64 totalNativeStatusCoreStringsBytes = 0;
    qint64 totalNativeStatusPiecesBitfieldBytes = 0;
    qint64 totalTorrentInfoMetadataBytes = 0;
    qint64 totalTorrentInfoMetadataAliasedBytes = 0;
    qint64 totalTorrentInfoMetadataUniqueBytes = 0;
    qint64 totalAtpTorrentInfoMetadataBytes = 0;
    qint64 totalAtpTorrentInfoMetadataAliasedBytes = 0;
    qint64 totalAtpTorrentInfoMetadataUniqueBytes = 0;
    QList<TorrentResidentEntry> topResidents;
    for (const BitTorrent::Torrent *torrent : asConst(session->torrents()))
    {
        const BitTorrent::ResidentMemberFamilyBytes bytes = torrent->residentMemberFamilyBytes();
        totalFilePathsBytes += bytes.filePaths;
        totalIndexMapBytes += bytes.indexMap;
        totalFilePrioritiesBytes += bytes.filePriorities;
        totalCompletedFilesBytes += bytes.completedFiles;
        totalFilesProgressBytes += bytes.filesProgress;
        totalPiecesBitfieldBytes += bytes.piecesBitfield;
        totalAtpFilePrioritiesBytes += bytes.atpFilePriorities;
        totalAtpRenamedFilesBytes += bytes.atpRenamedFiles;
        totalAtpTrackersBytes += bytes.atpTrackers;
        totalAtpTrackerTiersBytes += bytes.atpTrackerTiers;
        totalAtpUrlSeedsBytes += bytes.atpUrlSeeds;
        totalAtpHavePiecesBytes += bytes.atpHavePieces;
        totalAtpVerifiedPiecesBytes += bytes.atpVerifiedPieces;
        totalAtpUnfinishedPiecesBytes += bytes.atpUnfinishedPieces;
        totalAtpSavePathBytes += bytes.atpSavePath;
        totalPersistentNameBytes += bytes.persistentName;
        totalPersistentSavePathBytes += bytes.persistentSavePath;
        totalPersistentDownloadPathBytes += bytes.persistentDownloadPath;
        totalPersistentCategoryBytes += bytes.persistentCategory;
        totalTrackerEntriesBytes += bytes.trackerEntries;
        totalUrlSeedsBytes += bytes.urlSeeds;
        totalNativeStatusCoreStringsBytes += bytes.nativeStatusCoreStrings;
        totalNativeStatusPiecesBitfieldBytes += bytes.nativeStatusPiecesBitfield;
        totalTorrentInfoMetadataBytes += bytes.torrentInfoMetadata;
        totalTorrentInfoMetadataAliasedBytes += bytes.torrentInfoMetadataAliased;
        totalTorrentInfoMetadataUniqueBytes += bytes.torrentInfoMetadataUnique;
        totalAtpTorrentInfoMetadataBytes += bytes.atpTorrentInfoMetadata;
        totalAtpTorrentInfoMetadataAliasedBytes += bytes.atpTorrentInfoMetadataAliased;
        totalAtpTorrentInfoMetadataUniqueBytes += bytes.atpTorrentInfoMetadataUnique;

        if (bytes.extendedTotal() <= 0)
            continue;

        topResidents.append({torrent->id().toString(), bytes});
    }

    counters[u"resident_family_bytes_total.filePaths"_s] = totalFilePathsBytes;
    counters[u"resident_family_bytes_total.indexMap"_s] = totalIndexMapBytes;
    counters[u"resident_family_bytes_total.filePriorities"_s] = totalFilePrioritiesBytes;
    counters[u"resident_family_bytes_total.completedFiles"_s] = totalCompletedFilesBytes;
    counters[u"resident_family_bytes_total.filesProgress"_s] = totalFilesProgressBytes;
    counters[u"resident_family_bytes_total.piecesBitfield"_s] = totalPiecesBitfieldBytes;
    counters[u"resident_family_bytes_total.atpFilePriorities"_s] = totalAtpFilePrioritiesBytes;
    counters[u"resident_family_bytes_total.atpRenamedFiles"_s] = totalAtpRenamedFilesBytes;
    counters[u"resident_family_bytes_total.atpTrackers"_s] = totalAtpTrackersBytes;
    counters[u"resident_family_bytes_total.atpTrackerTiers"_s] = totalAtpTrackerTiersBytes;
    counters[u"resident_family_bytes_total.atpUrlSeeds"_s] = totalAtpUrlSeedsBytes;
    counters[u"resident_family_bytes_total.atpHavePieces"_s] = totalAtpHavePiecesBytes;
    counters[u"resident_family_bytes_total.atpVerifiedPieces"_s] = totalAtpVerifiedPiecesBytes;
    counters[u"resident_family_bytes_total.atpUnfinishedPieces"_s] = totalAtpUnfinishedPiecesBytes;
    counters[u"resident_family_bytes_total.atpSavePath"_s] = totalAtpSavePathBytes;
    counters[u"resident_family_bytes_total.persistentName"_s] = totalPersistentNameBytes;
    counters[u"resident_family_bytes_total.persistentSavePath"_s] = totalPersistentSavePathBytes;
    counters[u"resident_family_bytes_total.persistentDownloadPath"_s] = totalPersistentDownloadPathBytes;
    counters[u"resident_family_bytes_total.persistentCategory"_s] = totalPersistentCategoryBytes;
    counters[u"resident_family_bytes_total.trackerEntries"_s] = totalTrackerEntriesBytes;
    counters[u"resident_family_bytes_total.urlSeeds"_s] = totalUrlSeedsBytes;
    counters[u"resident_family_bytes_total.nativeStatusCoreStrings"_s] = totalNativeStatusCoreStringsBytes;
    counters[u"resident_family_bytes_total.nativeStatusPiecesBitfield"_s] = totalNativeStatusPiecesBitfieldBytes;
    counters[u"resident_family_bytes_total.torrentInfoMetadata"_s] = totalTorrentInfoMetadataBytes;
    counters[u"resident_family_bytes_total.torrentInfoMetadataAliased"_s] = totalTorrentInfoMetadataAliasedBytes;
    counters[u"resident_family_bytes_total.torrentInfoMetadataUnique"_s] = totalTorrentInfoMetadataUniqueBytes;
    counters[u"resident_family_bytes_total.atpTorrentInfoMetadata"_s] = totalAtpTorrentInfoMetadataBytes;
    counters[u"resident_family_bytes_total.atpTorrentInfoMetadataAliased"_s] = totalAtpTorrentInfoMetadataAliasedBytes;
    counters[u"resident_family_bytes_total.atpTorrentInfoMetadataUnique"_s] = totalAtpTorrentInfoMetadataUniqueBytes;
    const qint64 residentFamilyTotalBytes = (totalFilePathsBytes
        + totalIndexMapBytes
        + totalFilePrioritiesBytes
        + totalCompletedFilesBytes
        + totalFilesProgressBytes
        + totalPiecesBitfieldBytes);
    const qint64 residentFamilyExtendedTotalBytes = residentFamilyTotalBytes
        + totalAtpFilePrioritiesBytes
        + totalAtpRenamedFilesBytes
        + totalAtpTrackersBytes
        + totalAtpTrackerTiersBytes
        + totalAtpUrlSeedsBytes
        + totalAtpHavePiecesBytes
        + totalAtpVerifiedPiecesBytes
        + totalAtpUnfinishedPiecesBytes
        + totalAtpSavePathBytes
        + totalPersistentNameBytes
        + totalPersistentSavePathBytes
        + totalPersistentDownloadPathBytes
        + totalPersistentCategoryBytes
        + totalTrackerEntriesBytes
        + totalUrlSeedsBytes
        + totalNativeStatusCoreStringsBytes
        + totalNativeStatusPiecesBitfieldBytes
        + totalTorrentInfoMetadataBytes
        + totalAtpTorrentInfoMetadataBytes;
    counters[u"resident_family_bytes_total.all"_s] = residentFamilyTotalBytes;
    counters[u"resident_family_bytes_total.extended_all"_s] = residentFamilyExtendedTotalBytes;

    std::sort(topResidents.begin(), topResidents.end(), [](const TorrentResidentEntry &left
            , const TorrentResidentEntry &right)
    {
        return left.bytes.extendedTotal() > right.bytes.extendedTotal();
    });

    QVariantList topTorrents;
    const int topCount = qMin(topResidents.size(), MEMORY_PROBE_TOP_RESIDENT_TORRENTS);
    for (int i = 0; i < topCount; ++i)
    {
        const TorrentResidentEntry &entry = topResidents.at(i);
        topTorrents.append(QVariantMap {
            {u"id"_s, entry.torrentID},
            {u"total_bytes"_s, entry.bytes.total()},
            {u"extended_total_bytes"_s, entry.bytes.extendedTotal()},
            {u"filePaths_bytes"_s, entry.bytes.filePaths},
            {u"indexMap_bytes"_s, entry.bytes.indexMap},
            {u"filePriorities_bytes"_s, entry.bytes.filePriorities},
            {u"completedFiles_bytes"_s, entry.bytes.completedFiles},
            {u"filesProgress_bytes"_s, entry.bytes.filesProgress},
            {u"piecesBitfield_bytes"_s, entry.bytes.piecesBitfield},
            {u"atpFilePriorities_bytes"_s, entry.bytes.atpFilePriorities},
            {u"atpRenamedFiles_bytes"_s, entry.bytes.atpRenamedFiles},
            {u"atpTrackers_bytes"_s, entry.bytes.atpTrackers},
            {u"atpTrackerTiers_bytes"_s, entry.bytes.atpTrackerTiers},
            {u"atpUrlSeeds_bytes"_s, entry.bytes.atpUrlSeeds},
            {u"atpHavePieces_bytes"_s, entry.bytes.atpHavePieces},
            {u"atpVerifiedPieces_bytes"_s, entry.bytes.atpVerifiedPieces},
            {u"atpUnfinishedPieces_bytes"_s, entry.bytes.atpUnfinishedPieces},
            {u"atpSavePath_bytes"_s, entry.bytes.atpSavePath},
            {u"persistentName_bytes"_s, entry.bytes.persistentName},
            {u"persistentSavePath_bytes"_s, entry.bytes.persistentSavePath},
            {u"persistentDownloadPath_bytes"_s, entry.bytes.persistentDownloadPath},
            {u"persistentCategory_bytes"_s, entry.bytes.persistentCategory},
            {u"trackerEntries_bytes"_s, entry.bytes.trackerEntries},
            {u"urlSeeds_bytes"_s, entry.bytes.urlSeeds},
            {u"nativeStatusCoreStrings_bytes"_s, entry.bytes.nativeStatusCoreStrings},
            {u"nativeStatusPiecesBitfield_bytes"_s, entry.bytes.nativeStatusPiecesBitfield},
            {u"torrentInfoMetadata_bytes"_s, entry.bytes.torrentInfoMetadata},
            {u"torrentInfoMetadataAliased_bytes"_s, entry.bytes.torrentInfoMetadataAliased},
            {u"torrentInfoMetadataUnique_bytes"_s, entry.bytes.torrentInfoMetadataUnique},
            {u"atpTorrentInfoMetadata_bytes"_s, entry.bytes.atpTorrentInfoMetadata},
            {u"atpTorrentInfoMetadataAliased_bytes"_s, entry.bytes.atpTorrentInfoMetadataAliased},
            {u"atpTorrentInfoMetadataUnique_bytes"_s, entry.bytes.atpTorrentInfoMetadataUnique},
        });
    }
    counters[u"resident_top_torrents_by_resident"_s] = topTorrents;
    return counters;
}

void WebUISync::MaindataSyncStore::emitMemoryProbeEvent(const QString &event, const QVariantMap &details)
{
    if (!m_memoryProbe.enabled)
        return;

    QFile traceFile(m_memoryProbe.outputPath);
    if (!traceFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QVariantMap merged = collectMemoryProbeSample();
    const QVariantMap counters = collectRetainedStateCounters();
    for (auto it = counters.cbegin(); it != counters.cend(); ++it)
        merged.insert(it.key(), it.value());
    for (auto it = details.cbegin(); it != details.cend(); ++it)
        merged.insert(it.key(), it.value());

    const qint64 pssKb = variantToInt64(merged, u"pss_kb"_s);
    const qint64 pssBytes = (pssKb >= 0) ? (pssKb * 1024) : -1;
    if (pssBytes >= 0)
    {
        const qint64 residentFamilyBytes = variantToInt64(merged, u"resident_family_bytes_total.all"_s);
        const qint64 residentFamilyExtendedBytes = variantToInt64(merged, u"resident_family_bytes_total.extended_all"_s);
        const qint64 sharedKnownBytes = variantToInt64(merged, u"resident_non5_bytes_total.shared_state_known"_s);
        const qint64 sharedKnownPlusRetainedBytes
            = variantToInt64(merged, u"resident_non5_bytes_total.shared_state_known_plus_retained"_s);
        const qint64 glibcManagedBytes = variantToInt64(merged, u"resident_glibc_managed_bytes_total"_s);
        const qint64 glibcInuseEstimatedBytes = variantToInt64(merged, u"resident_glibc_inuse_estimated_bytes_total"_s);
        const qint64 fileStackOtherPssBytes = variantToInt64(merged, u"resident_file_stack_other_pss_bytes_total"_s);
        const qint64 piecesBitfieldBytes = variantToInt64(merged, u"resident_family_bytes_total.piecesBitfield"_s);

        if ((sharedKnownPlusRetainedBytes >= 0) && (glibcInuseEstimatedBytes >= 0) && (fileStackOtherPssBytes >= 0))
        {
            const qint64 attributedNonOverlapBaselineBytes
                = sharedKnownPlusRetainedBytes + glibcInuseEstimatedBytes + fileStackOtherPssBytes;
            merged[u"resident_attributed_bytes_total.plus_retained_plus_glibc_inuse_plus_file_stack_other"_s]
                = attributedNonOverlapBaselineBytes;
            merged[u"resident_non5_bytes_total.residual_unattributed_plus_retained_plus_glibc_inuse_plus_file_stack_other"_s]
                = qMax<qint64>(0, pssBytes - attributedNonOverlapBaselineBytes);
            merged[u"resident_non5_bytes_total.overattributed_plus_retained_plus_glibc_inuse_plus_file_stack_other"_s]
                = qMax<qint64>(0, attributedNonOverlapBaselineBytes - pssBytes);
        }

        if ((sharedKnownPlusRetainedBytes >= 0) && (glibcManagedBytes >= 0) && (fileStackOtherPssBytes >= 0))
        {
            const qint64 attributedManagedBaselineBytes
                = sharedKnownPlusRetainedBytes + glibcManagedBytes + fileStackOtherPssBytes;
            merged[u"resident_attributed_bytes_total.plus_retained_plus_glibc_managed_plus_file_stack_other"_s]
                = attributedManagedBaselineBytes;
            merged[u"resident_non5_bytes_total.residual_unattributed_plus_retained_plus_glibc_managed_plus_file_stack_other"_s]
                = qMax<qint64>(0, pssBytes - attributedManagedBaselineBytes);
            merged[u"resident_non5_bytes_total.overattributed_plus_retained_plus_glibc_managed_plus_file_stack_other"_s]
                = qMax<qint64>(0, attributedManagedBaselineBytes - pssBytes);
        }
        if ((residentFamilyBytes >= 0) && (sharedKnownBytes >= 0))
        {
            const qint64 attributedKnownBytes = residentFamilyBytes + sharedKnownBytes;
            const qint64 residualBytes = qMax<qint64>(0, pssBytes - attributedKnownBytes);
            merged[u"resident_non5_bytes_total.residual_unattributed"_s] = residualBytes;
        }

        if ((residentFamilyBytes >= 0) && (sharedKnownPlusRetainedBytes >= 0))
        {
            const qint64 attributedKnownBytes = residentFamilyBytes + sharedKnownPlusRetainedBytes;
            const qint64 residualBytes = qMax<qint64>(0, pssBytes - attributedKnownBytes);
            merged[u"resident_non5_bytes_total.residual_unattributed_plus_retained"_s] = residualBytes;
        }

        if ((residentFamilyExtendedBytes >= 0) && (sharedKnownPlusRetainedBytes >= 0))
        {
        const qint64 attributedKnownBytes = residentFamilyExtendedBytes + sharedKnownPlusRetainedBytes;
        const qint64 residualBytes = qMax<qint64>(0, pssBytes - attributedKnownBytes);
        merged[u"resident_non5_bytes_total.residual_unattributed_plus_retained_extended"_s] = residualBytes;

        if (glibcManagedBytes >= 0)
        {
            const qint64 attributedWithGlibcBytes = attributedKnownBytes + glibcManagedBytes;
            merged[u"resident_attributed_bytes_total.plus_retained_extended_plus_glibc"_s] = attributedWithGlibcBytes;
                merged[u"resident_non5_bytes_total.residual_unattributed_plus_retained_extended_plus_glibc"_s]
                    = qMax<qint64>(0, pssBytes - attributedWithGlibcBytes);

                if (fileStackOtherPssBytes >= 0)
                {
                    const qint64 attributedWithGlibcAndSmapsClassesBytes = attributedWithGlibcBytes + fileStackOtherPssBytes;
                    merged[u"resident_attributed_bytes_total.plus_retained_extended_plus_glibc_plus_file_stack_other"_s]
                        = attributedWithGlibcAndSmapsClassesBytes;
                    merged[u"resident_non5_bytes_total.residual_unattributed_plus_retained_extended_plus_glibc_plus_file_stack_other"_s]
                        = qMax<qint64>(0, pssBytes - attributedWithGlibcAndSmapsClassesBytes);
                }
            }

            if ((glibcInuseEstimatedBytes >= 0) && (fileStackOtherPssBytes >= 0))
            {
                const qint64 attributedWithGlibcInuseAndSmapsClassesBytes
                    = attributedKnownBytes + glibcInuseEstimatedBytes + fileStackOtherPssBytes;
                merged[u"resident_attributed_bytes_total.plus_retained_extended_plus_glibc_inuse_plus_file_stack_other"_s]
                    = attributedWithGlibcInuseAndSmapsClassesBytes;
                merged[u"resident_non5_bytes_total.residual_unattributed_plus_retained_extended_plus_glibc_inuse_plus_file_stack_other"_s]
                    = qMax<qint64>(0, pssBytes - attributedWithGlibcInuseAndSmapsClassesBytes);
                merged[u"resident_non5_bytes_total.overattributed_plus_retained_extended_plus_glibc_inuse_plus_file_stack_other"_s]
                    = qMax<qint64>(0, attributedWithGlibcInuseAndSmapsClassesBytes - pssBytes);
            }

            if ((glibcInuseEstimatedBytes >= 0) && (fileStackOtherPssBytes >= 0) && (piecesBitfieldBytes >= 0))
            {
                const qint64 attributedWithGlibcInuseFileStackAndPiecesBytes
                    = attributedKnownBytes + glibcInuseEstimatedBytes + fileStackOtherPssBytes + piecesBitfieldBytes;
                merged[u"resident_attributed_bytes_total.plus_retained_extended_plus_glibc_inuse_plus_file_stack_other_plus_pieces"_s]
                    = attributedWithGlibcInuseFileStackAndPiecesBytes;
                merged[u"resident_non5_bytes_total.residual_unattributed_plus_retained_extended_plus_glibc_inuse_plus_file_stack_other_plus_pieces"_s]
                    = qMax<qint64>(0, pssBytes - attributedWithGlibcInuseFileStackAndPiecesBytes);
                merged[u"resident_non5_bytes_total.overattributed_plus_retained_extended_plus_glibc_inuse_plus_file_stack_other_plus_pieces"_s]
                    = qMax<qint64>(0, attributedWithGlibcInuseFileStackAndPiecesBytes - pssBytes);
            }
        }
    }

    if (event == u"begin_first_full_trace"_s)
    {
        m_memoryProbe.firstFullPssAfterSnapshotBytes = -1;
        m_memoryProbe.firstFullPssAfterMaterializationBytes = -1;
        m_memoryProbe.firstFullPssAfterCommittedBytes = -1;
    }
    else if ((event == u"after_shared_snapshot_refresh"_s) && (pssBytes >= 0))
    {
        m_memoryProbe.firstFullPssAfterSnapshotBytes = pssBytes;
    }
    else if ((event == u"after_full_response_materialization"_s) && (pssBytes >= 0))
    {
        m_memoryProbe.firstFullPssAfterMaterializationBytes = pssBytes;
        if (m_memoryProbe.firstFullPssAfterSnapshotBytes >= 0)
        {
            merged[u"resident_non5_phase_delta_bytes.snapshot_to_materialization"_s]
                = (m_memoryProbe.firstFullPssAfterMaterializationBytes - m_memoryProbe.firstFullPssAfterSnapshotBytes);
        }
    }
    else if ((event == u"after_response_committed"_s) && (pssBytes >= 0))
    {
        m_memoryProbe.firstFullPssAfterCommittedBytes = pssBytes;
        if ((m_memoryProbe.firstFullPssAfterSnapshotBytes >= 0)
            && (m_memoryProbe.firstFullPssAfterMaterializationBytes >= 0))
        {
            merged[u"resident_non5_phase_delta_bytes.snapshot_to_materialization"_s]
                = (m_memoryProbe.firstFullPssAfterMaterializationBytes - m_memoryProbe.firstFullPssAfterSnapshotBytes);
            merged[u"resident_non5_phase_delta_bytes.materialization_to_committed"_s]
                = (m_memoryProbe.firstFullPssAfterCommittedBytes - m_memoryProbe.firstFullPssAfterMaterializationBytes);
        }
    }

    QJsonObject marker;
    marker[u"trace"_s] = u"webui_memory_probe"_s;
    marker[u"event"_s] = event;
    marker[u"event_index"_s] = ++m_memoryProbe.nextEventIndex;
    marker[u"timestamp"_s] = QDateTime::currentDateTime().toString(u"yyyy-MM-ddTHH:mm:ss.zzzt"_s);

    const QJsonObject detailObject = QJsonObject::fromVariantMap(merged);
    for (auto it = detailObject.begin(); it != detailObject.end(); ++it)
        marker[it.key()] = it.value();

    traceFile.write(QJsonDocument(marker).toJson(QJsonDocument::Compact));
    traceFile.write("\n");
}

void WebUISync::MaindataSyncStore::prewarm()
{
    if (!shouldTrackDirtyState())
        return;

    if (m_firstFullTrace.active)
    {
        const QVariantMap details {
            {u"current_revision"_s, m_revisionStore.currentRevision()},
        };
        emitFirstFullTracePhase(u"before_shared_snapshot_refresh"_s, details);
        emitMemoryProbeEvent(u"before_shared_snapshot_refresh"_s, details);
    }

    ensureInitialized();
    storePendingRevision();

    if (m_firstFullTrace.active)
    {
        const QVariantMap details {
            {u"current_revision"_s, m_revisionStore.currentRevision()},
        };
        emitFirstFullTracePhase(u"after_shared_snapshot_refresh"_s, details);
        emitMemoryProbeEvent(u"after_shared_snapshot_refresh"_s, details);
    }
}

QByteArray WebUISync::MaindataSyncStore::buildInitialFullUpdateResponse(MaindataSessionCursor &cursor
    , const int requestedRid)
{
    if (shouldTraceFirstFullRequest(cursor, requestedRid))
        beginFirstFullTrace(requestedRid);

    if (m_statelessModeEnabled)
    {
        if (!Preferences::instance()->isExperimentalDirectWebUIStatelessMaindataResponseEnabled())
            return {};

        const QByteArray response = buildStatelessFullResponseBytes(cursor, requestedRid);
        if (!response.isEmpty())
            m_hasServedAnyResponse = true;
        return response;
    }

    prewarm();
    const QByteArray response = m_revisionStore.buildInitialFullUpdateResponse(cursor, requestedRid);
    if (m_firstFullTrace.active)
    {
        QString responseBuildMode = u"snapshot_object"_s;
        if (Preferences::instance()->isExperimentalCompactWebUIMaindataTorrentRowsEnabled())
            responseBuildMode = Preferences::instance()->isExperimentalDirectCompactWebUIInitialFullResponseEnabled()
                ? u"compact_snapshot_row_bytes"_s
                : u"compact_snapshot_object"_s;

        const QVariantMap details {
            {u"response_bytes"_s, response.size()},
            {u"response_rid"_s, cursor.lastSentRid},
            {u"current_revision"_s, m_revisionStore.currentRevision()},
            {u"response_build_mode"_s, responseBuildMode},
        };
        emitFirstFullTracePhase(u"after_full_response_materialization"_s, details);
        emitMemoryProbeEvent(u"after_full_response_materialization"_s, details);
    }
    if (!response.isEmpty())
    {
        m_hasServedAnyResponse = true;
        maybeTrimAllocatorOnResponsePath();
    }
    return response;
}

QJsonObject WebUISync::MaindataSyncStore::buildResponse(MaindataSessionCursor &cursor, const int requestedRid)
{
    if (m_statelessModeEnabled)
        return buildStatelessFullResponse(cursor, requestedRid);

    prewarm();
    QJsonObject response = m_revisionStore.buildResponse(cursor, requestedRid);
    m_hasServedAnyResponse = true;
    maybeTrimAllocatorOnResponsePath();
    return response;
}

void WebUISync::MaindataSyncStore::notifySerializedFullUpdateResponseCommitted(const int requestedRid
    , const int responseRid, const qsizetype responseBytes)
{
    maybeTraceFirstFullResponseCommitted(requestedRid, responseRid, responseBytes, m_revisionStore.currentRevision());
}

void WebUISync::MaindataSyncStore::maybeTraceFirstFullResponseCommitted(const int requestedRid
    , const int responseRid, const qsizetype responseBytes, const qint64 currentRevision)
{
    if (!m_firstFullTrace.active || (requestedRid != m_firstFullTrace.requestedRid))
        return;

    emitFirstFullTracePhase(u"after_response_committed"_s, {
        {u"response_bytes"_s, responseBytes},
        {u"response_rid"_s, responseRid},
        {u"current_revision"_s, currentRevision},
    });
    emitMemoryProbeEvent(u"after_response_committed"_s, {
        {u"response_bytes"_s, responseBytes},
        {u"response_rid"_s, responseRid},
        {u"current_revision"_s, currentRevision},
    });
    finishFirstFullTrace();
}

QByteArray WebUISync::MaindataSyncStore::buildStatelessFullResponseBytes(MaindataSessionCursor &cursor
    , const int requestedRid)
{
    Q_UNUSED(requestedRid);

    const auto *session = BitTorrent::Session::instance();
    const bool omitTorrents = Preferences::instance()->isExperimentalOmitWebUIMaindataTorrentsEnabled();
    QByteArray response;
    response += '{';
    bool firstField = true;

    if (!omitTorrents)
    {
        const QVector<BitTorrent::Torrent *> torrentsList = session->torrents();
        if (!torrentsList.isEmpty())
        {
            appendJsonFieldPrefix(response, KEY_TORRENTS, firstField);
            response += '{';

            bool firstTorrent = true;
            for (const BitTorrent::Torrent *torrent : torrentsList)
            {
                if (!firstTorrent)
                    response += ',';
                firstTorrent = false;

                response += '"';
                response += torrent->id().toString().toUtf8();
                response += "\":";
                response += serializeToJsonBytes(*torrent, false);
            }

            response += '}';
        }
    }

    const QStringList categoriesList = session->categories();
    if (!categoriesList.isEmpty())
    {
        QJsonObject categories;
        for (const QString &categoryName : categoriesList)
        {
            QJsonObject category = session->categoryOptions(categoryName).toJSON();
            category[u"savePath"_s] = category.take(u"save_path"_s);
            category.insert(u"name"_s, categoryName);
            categories.insert(categoryName, category);
        }
        appendJsonLiteralField(response, KEY_CATEGORIES, serializeJsonObject(categories), firstField);
    }

    const QStringList tags = session->tags().values();
    if (!tags.isEmpty())
        appendJsonLiteralField(response, KEY_TAGS, serializeJsonArray(QJsonArray::fromStringList(tags)), firstField);

    QHash<QString, QSet<BitTorrent::TorrentID>> trackersByUrl;
    for (const BitTorrent::Torrent *torrent : asConst(session->torrents()))
    {
        const BitTorrent::TorrentID torrentID = torrent->id();
        for (const BitTorrent::TrackerEntry &tracker : asConst(torrent->trackers()))
            trackersByUrl[tracker.url].insert(torrentID);
    }

    if (!trackersByUrl.isEmpty())
    {
        QJsonObject trackers;
        for (auto trackersIter = trackersByUrl.cbegin(); trackersIter != trackersByUrl.cend(); ++trackersIter)
        {
            QStringList torrentIDs;
            torrentIDs.reserve(trackersIter.value().size());
            for (const BitTorrent::TorrentID &torrentID : asConst(trackersIter.value()))
                torrentIDs.append(torrentID.toString());

            trackers.insert(trackersIter.key(), QJsonArray::fromStringList(torrentIDs));
        }
        appendJsonLiteralField(response, KEY_TRACKERS, serializeJsonObject(trackers), firstField);
    }

    QVariantMap serverState = getTransferInfo();
    serverState[KEY_TRANSFER_FREESPACEONDISK] = m_freeDiskSpace;
    serverState[KEY_SYNC_MAINDATA_QUEUEING] = session->isQueueingSystemEnabled();
    serverState[KEY_SYNC_MAINDATA_USE_ALT_SPEED_LIMITS] = session->isAltGlobalSpeedLimitEnabled();
    serverState[KEY_SYNC_MAINDATA_REFRESH_INTERVAL] = session->refreshInterval();
    serverState[KEY_SYNC_MAINDATA_USE_SUBCATEGORIES] = session->isSubcategoriesEnabled();
    appendJsonLiteralField(response, KEY_SERVER_STATE
            , serializeJsonObject(QJsonObject::fromVariantMap(serverState)), firstField);

    const int responseRid = nextRid(cursor.lastSentRid);
    cursor.lastSentRid = responseRid;
    cursor.lastSentRevision = 0;
    appendJsonLiteralField(response, KEY_FULL_UPDATE, "true", firstField);
    appendJsonLiteralField(response, KEY_RESPONSE_ID, QByteArray::number(responseRid), firstField);
    response += '}';

    if (m_firstFullTrace.active)
    {
        const QVariantMap details {
            {u"response_bytes"_s, response.size()},
            {u"response_rid"_s, responseRid},
            {u"current_revision"_s, 0},
        };
        emitFirstFullTracePhase(u"after_full_response_materialization"_s, details);
        emitMemoryProbeEvent(u"after_full_response_materialization"_s, details);
    }

    return response;
}

bool WebUISync::MaindataSyncStore::shouldTraceFirstFullRequest(const MaindataSessionCursor &cursor
    , const int requestedRid) const
{
    return m_firstFullTrace.enabled
        && !m_firstFullTrace.consumed
        && !m_firstFullTrace.active
        && (requestedRid == 0)
        && (cursor.acceptedRid == 0)
        && (cursor.acceptedRevision == 0)
        && (cursor.lastSentRid == 0)
        && (cursor.lastSentRevision == 0);
}

void WebUISync::MaindataSyncStore::beginFirstFullTrace(const int requestedRid)
{
    m_firstFullTrace.requestedRid = requestedRid;
    m_firstFullTrace.active = true;
    m_memoryProbe.firstFullPssAfterSnapshotBytes = -1;
    m_memoryProbe.firstFullPssAfterMaterializationBytes = -1;
    m_memoryProbe.firstFullPssAfterCommittedBytes = -1;
    emitMemoryProbeEvent(u"begin_first_full_trace"_s, {
        {u"requested_rid"_s, requestedRid},
    });
}

void WebUISync::MaindataSyncStore::emitFirstFullTracePhase(const QString &phase, const QVariantMap &details)
{
    if (!m_firstFullTrace.active)
        return;

    QFile traceFile(m_firstFullTrace.outputPath);
    if (!traceFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QJsonObject marker;
    marker[u"trace"_s] = u"first_full_retained_floor"_s;
    marker[u"event_index"_s] = ++m_firstFullTrace.nextEventIndex;
    marker[u"phase"_s] = phase;
    marker[u"timestamp"_s] = QDateTime::currentDateTime().toString(u"yyyy-MM-ddTHH:mm:ss.zzzt"_s);
    marker[u"requested_rid"_s] = m_firstFullTrace.requestedRid;

    const QJsonObject detailObject = QJsonObject::fromVariantMap(details);
    for (auto it = detailObject.begin(); it != detailObject.end(); ++it)
        marker[it.key()] = it.value();

    traceFile.write(QJsonDocument(marker).toJson(QJsonDocument::Compact));
    traceFile.write("\n");
}

void WebUISync::MaindataSyncStore::finishFirstFullTrace()
{
    emitMemoryProbeEvent(u"finish_first_full_trace"_s, {
        {u"requested_rid"_s, m_firstFullTrace.requestedRid},
    });
    m_firstFullTrace.active = false;
    m_firstFullTrace.consumed = true;
}

QVariantMap WebUISync::MaindataSyncStore::summarizeDeltaForTrace(const MaindataSyncData &delta
    , const qsizetype updatedTorrentCount) const
{
    qsizetype torrentDeltaFieldCount = 0;
    for (auto it = delta.torrents.cbegin(); it != delta.torrents.cend(); ++it)
        torrentDeltaFieldCount += it.value().size();

    return {
        {u"updated_torrents_seen"_s, updatedTorrentCount},
        {u"delta_categories_count"_s, delta.categories.size()},
        {u"delta_tags_count"_s, delta.tags.size()},
        {u"delta_torrents_count"_s, delta.torrents.size()},
        {u"delta_torrent_field_count"_s, torrentDeltaFieldCount},
        {u"delta_trackers_count"_s, delta.trackers.size()},
        {u"delta_server_state_field_count"_s, delta.serverState.size()},
        {u"delta_removed_categories_count"_s, delta.removedCategories.size()},
        {u"delta_removed_tags_count"_s, delta.removedTags.size()},
        {u"delta_removed_torrents_count"_s, delta.removedTorrents.size()},
        {u"delta_removed_trackers_count"_s, delta.removedTrackers.size()},
        {u"delta_estimated_payload_bytes"_s, estimatePayloadBytes(delta)},
    };
}

QJsonObject WebUISync::MaindataSyncStore::buildStatelessFullResponse(MaindataSessionCursor &cursor
    , const int requestedRid)
{
    Q_UNUSED(requestedRid);

    MaindataSyncData fullData;
    const auto *session = BitTorrent::Session::instance();
    const bool omitTorrents = Preferences::instance()->isExperimentalOmitWebUIMaindataTorrentsEnabled();

    if (!omitTorrents)
    {
        for (const BitTorrent::Torrent *torrent : asConst(session->torrents()))
        {
            QVariantMap serializedTorrent = serialize(*torrent);
            const QString torrentID = serializedTorrent.take(KEY_TORRENT_ID).toString();
            fullData.torrents[torrentID] = serializedTorrent;
        }
    }

    const QStringList categoriesList = session->categories();
    for (const QString &categoryName : categoriesList)
    {
        QJsonObject category = session->categoryOptions(categoryName).toJSON();
        category[u"savePath"_s] = category.take(u"save_path"_s);
        category.insert(u"name"_s, categoryName);
        fullData.categories[categoryName] = category.toVariantMap();
    }

    for (const QString &tag : asConst(session->tags()))
        fullData.tags.append(tag);

    QHash<QString, QSet<BitTorrent::TorrentID>> trackersByUrl;
    for (const BitTorrent::Torrent *torrent : asConst(session->torrents()))
    {
        const BitTorrent::TorrentID torrentID = torrent->id();
        for (const BitTorrent::TrackerEntry &tracker : asConst(torrent->trackers()))
            trackersByUrl[tracker.url].insert(torrentID);
    }

    for (auto trackersIter = trackersByUrl.cbegin(); trackersIter != trackersByUrl.cend(); ++trackersIter)
    {
        QStringList torrentIDs;
        torrentIDs.reserve(trackersIter.value().size());
        for (const BitTorrent::TorrentID &torrentID : asConst(trackersIter.value()))
            torrentIDs.append(torrentID.toString());

        fullData.trackers[trackersIter.key()] = torrentIDs;
    }

    fullData.serverState = getTransferInfo();
    fullData.serverState[KEY_TRANSFER_FREESPACEONDISK] = m_freeDiskSpace;
    fullData.serverState[KEY_SYNC_MAINDATA_QUEUEING] = session->isQueueingSystemEnabled();
    fullData.serverState[KEY_SYNC_MAINDATA_USE_ALT_SPEED_LIMITS] = session->isAltGlobalSpeedLimitEnabled();
    fullData.serverState[KEY_SYNC_MAINDATA_REFRESH_INTERVAL] = session->refreshInterval();
    fullData.serverState[KEY_SYNC_MAINDATA_USE_SUBCATEGORIES] = session->isSubcategoriesEnabled();

    const int responseRid = nextRid(cursor.lastSentRid);
    cursor.lastSentRid = responseRid;
    cursor.lastSentRevision = 0;

    QJsonObject response = toJsonObject(fullData);
    response[KEY_FULL_UPDATE] = true;
    response[KEY_RESPONSE_ID] = responseRid;

    if (m_firstFullTrace.active)
    {
        const QVariantMap details {
            {u"response_rid"_s, responseRid},
            {u"current_revision"_s, 0},
        };
        emitFirstFullTracePhase(u"after_full_response_materialization"_s, details);
        emitMemoryProbeEvent(u"after_full_response_materialization"_s, details);
    }

    return response;
}

bool WebUISync::MaindataSyncStore::shouldTrackDirtyState() const
{
    return !m_releaseIdleStateEnabled || (m_activeSyncSessionCount > 0);
}

void WebUISync::MaindataSyncStore::ensureInitialized()
{
    if (m_revisionStore.currentRevision() > 0)
        return;

    initializeSnapshot();
}

void WebUISync::MaindataSyncStore::initializeSnapshot()
{
    m_knownTrackers.clear();
    clearDirtyState();

    MaindataSyncData snapshot;
    const auto *session = BitTorrent::Session::instance();
    const QVector<BitTorrent::Torrent *> torrents = session->torrents();
    const bool compactDescriptiveNumericOnDemandEnabled
        = Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveNumericOnDemandEnabled();
    const bool directCompactSnapshotEnabled
        = Preferences::instance()->isExperimentalCompactWebUIMaindataTorrentRowsEnabled()
        && (Preferences::instance()->isExperimentalDirectCompactWebUIMaindataSnapshotEnabled()
            || compactDescriptiveNumericOnDemandEnabled);

    PreparedCompactSnapshot preparedSnapshot;
    if (directCompactSnapshotEnabled)
    {
        preparedSnapshot = prepareCompactSnapshotFromTorrents(torrents
            , Preferences::instance()->isExperimentalCompactWebUIMaindataStringPoolEnabled()
            , Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveSidecarEnabled()
            , Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveSubgroupsEnabled()
            , Preferences::instance()->isExperimentalCompactWebUIMaindataDescriptiveNumericOnDemandEnabled());
        for (const BitTorrent::Torrent *torrent : torrents)
        {
            const BitTorrent::TorrentID torrentID = torrent->id();
            for (const BitTorrent::TrackerEntry &tracker : asConst(torrent->trackers()))
                m_knownTrackers[tracker.url].insert(torrentID);
        }
    }
    else for (const BitTorrent::Torrent *torrent : asConst(torrents))
    {
        const BitTorrent::TorrentID torrentID = torrent->id();

        QVariantMap serializedTorrent = serialize(*torrent);
        serializedTorrent.remove(KEY_TORRENT_ID);

        for (const BitTorrent::TrackerEntry &tracker : asConst(torrent->trackers()))
            m_knownTrackers[tracker.url].insert(torrentID);

        snapshot.torrents[torrentID.toString()] = serializedTorrent;
    }

    const QStringList categoriesList = session->categories();
    for (const QString &categoryName : categoriesList)
    {
        QJsonObject category = session->categoryOptions(categoryName).toJSON();
        category[u"savePath"_s] = category.take(u"save_path"_s);
        category.insert(u"name"_s, categoryName);
        snapshot.categories[categoryName] = category.toVariantMap();
    }

    for (const QString &tag : asConst(session->tags()))
        snapshot.tags.append(tag);

    for (auto trackersIter = m_knownTrackers.cbegin(); trackersIter != m_knownTrackers.cend(); ++trackersIter)
    {
        QStringList torrentIDs;
        for (const BitTorrent::TorrentID &torrentID : asConst(trackersIter.value()))
            torrentIDs.append(torrentID.toString());

        snapshot.trackers[trackersIter.key()] = torrentIDs;
    }

    snapshot.serverState = getTransferInfo();
    snapshot.serverState[KEY_TRANSFER_FREESPACEONDISK] = m_freeDiskSpace;
    snapshot.serverState[KEY_SYNC_MAINDATA_QUEUEING] = session->isQueueingSystemEnabled();
    snapshot.serverState[KEY_SYNC_MAINDATA_USE_ALT_SPEED_LIMITS] = session->isAltGlobalSpeedLimitEnabled();
    snapshot.serverState[KEY_SYNC_MAINDATA_REFRESH_INTERVAL] = session->refreshInterval();
    snapshot.serverState[KEY_SYNC_MAINDATA_USE_SUBCATEGORIES] = session->isSubcategoriesEnabled();

    if (directCompactSnapshotEnabled)
    {
        m_revisionStore.initializeCompactSnapshot(snapshot, preparedSnapshot);
        return;
    }

    m_revisionStore.initialize(snapshot);
}

void WebUISync::MaindataSyncStore::releaseSharedState()
{
    const QVariantMap beforeDetails {
        {u"active_sync_sessions"_s, m_activeSyncSessionCount},
        {u"current_revision"_s, m_revisionStore.currentRevision()},
    };
    emitLifecycleTraceEvent(u"release_shared_state_before"_s, beforeDetails);
    emitMemoryProbeEvent(u"release_shared_state_before"_s, beforeDetails);
    m_knownTrackers.clear();
    clearDirtyState();
    m_hasServedAnyResponse = false;
    m_revisionStore.reset();

    bool allocatorTrimAttempted = false;
    bool allocatorTrimResult = false;
#if defined(Q_OS_LINUX) && defined(__GLIBC__)
    if (m_trimAllocatorAfterIdleReleaseEnabled)
    {
        allocatorTrimAttempted = true;
        allocatorTrimResult = (malloc_trim(0) != 0);
    }
#endif

    const QVariantMap afterDetails {
        {u"active_sync_sessions"_s, m_activeSyncSessionCount},
        {u"current_revision"_s, m_revisionStore.currentRevision()},
        {u"allocator_trim_attempted"_s, allocatorTrimAttempted},
        {u"allocator_trim_result"_s, allocatorTrimResult},
    };
    emitLifecycleTraceEvent(u"release_shared_state_after"_s, afterDetails);
    emitMemoryProbeEvent(u"release_shared_state_after"_s, afterDetails);
}

void WebUISync::MaindataSyncStore::maybeTrimAllocatorNonIdle(const QString &trigger)
{
#if defined(Q_OS_LINUX) && defined(__GLIBC__)
    if (!m_trimAllocatorAfterIdleReleaseEnabled)
        return;

    if (m_activeSyncSessionCount > m_nonIdleTrimMaxActiveSyncSessions)
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if ((m_lastNonIdleTrimAttemptMs > 0)
        && ((nowMs - m_lastNonIdleTrimAttemptMs) < NON_IDLE_TRIM_MIN_INTERVAL_MS))
    {
        return;
    }

    m_lastNonIdleTrimAttemptMs = nowMs;

    const bool allocatorTrimResult = (malloc_trim(0) != 0);
    const QVariantMap details {
        {u"active_sync_sessions"_s, m_activeSyncSessionCount},
        {u"nonidle_trim_max_active_sync_sessions"_s, m_nonIdleTrimMaxActiveSyncSessions},
        {u"current_revision"_s, m_revisionStore.currentRevision()},
        {u"allocator_trim_attempted"_s, true},
        {u"allocator_trim_result"_s, allocatorTrimResult},
        {u"trigger"_s, trigger},
    };
    emitLifecycleTraceEvent(u"allocator_trim_nonidle"_s, details);
    emitMemoryProbeEvent(u"allocator_trim_nonidle"_s, details);
#else
    Q_UNUSED(trigger);
#endif
}

void WebUISync::MaindataSyncStore::maybeTrimAllocatorOnSyncRelease()
{
    maybeTrimAllocatorNonIdle(u"sync_session_release_nonidle"_s);
}

void WebUISync::MaindataSyncStore::maybeTrimAllocatorOnResponsePath()
{
    maybeTrimAllocatorNonIdle(u"maindata_response_nonidle"_s);
}

void WebUISync::MaindataSyncStore::storePendingRevision()
{
    const MaindataSyncData &snapshot = m_revisionStore.snapshot();
    const auto *session = BitTorrent::Session::instance();
    const qsizetype updatedTorrentCount = m_updatedTorrents.size();

    MaindataSyncData delta;

    for (const QString &categoryName : asConst(m_updatedCategories))
    {
        auto category = session->categoryOptions(categoryName).toJSON().toVariantMap();
        category[u"savePath"_s] = category.take(u"save_path"_s);
        category.insert(u"name"_s, categoryName);
        processMap(snapshot.categories.value(categoryName), category, delta.categories[categoryName]);
    }
    m_updatedCategories.clear();

    for (const QString &category : asConst(m_removedCategories))
        delta.removedCategories.append(category);
    m_removedCategories.clear();

    for (const QString &tag : asConst(m_addedTags))
        delta.tags.append(tag);
    m_addedTags.clear();

    for (const QString &tag : asConst(m_removedTags))
        delta.removedTags.append(tag);
    m_removedTags.clear();

    for (const BitTorrent::TorrentID &torrentID : asConst(m_updatedTorrents))
    {
        const BitTorrent::Torrent *torrent = session->getTorrent(torrentID);
        Q_ASSERT(torrent);
        if (Q_UNLIKELY(!torrent))
            continue;

        QVariantMap serializedTorrent = serialize(*torrent);
        serializedTorrent.remove(KEY_TORRENT_ID);
        processMap(m_revisionStore.snapshotTorrentRow(torrentID.toString()), serializedTorrent, delta.torrents[torrentID.toString()]);
    }
    m_updatedTorrents.clear();

    for (const BitTorrent::TorrentID &torrentID : asConst(m_removedTorrents))
        delta.removedTorrents.append(torrentID.toString());
    m_removedTorrents.clear();

    for (const QString &tracker : asConst(m_updatedTrackers))
    {
        const QSet<BitTorrent::TorrentID> torrentIDs = m_knownTrackers.value(tracker);
        QStringList serializedTorrentIDs;
        serializedTorrentIDs.reserve(torrentIDs.size());
        for (const BitTorrent::TorrentID &torrentID : asConst(torrentIDs))
            serializedTorrentIDs.append(torrentID.toString());

        delta.trackers[tracker] = serializedTorrentIDs;
    }
    m_updatedTrackers.clear();

    for (const QString &tracker : asConst(m_removedTrackers))
        delta.removedTrackers.append(tracker);
    m_removedTrackers.clear();

    QVariantMap serverState = getTransferInfo();
    serverState[KEY_TRANSFER_FREESPACEONDISK] = m_freeDiskSpace;
    serverState[KEY_SYNC_MAINDATA_QUEUEING] = session->isQueueingSystemEnabled();
    serverState[KEY_SYNC_MAINDATA_USE_ALT_SPEED_LIMITS] = session->isAltGlobalSpeedLimitEnabled();
    serverState[KEY_SYNC_MAINDATA_REFRESH_INTERVAL] = session->refreshInterval();
    serverState[KEY_SYNC_MAINDATA_USE_SUBCATEGORIES] = session->isSubcategoriesEnabled();
    processMap(snapshot.serverState, serverState, delta.serverState);

    if (m_firstFullTrace.active)
    {
        const QVariantMap details = summarizeDeltaForTrace(delta, updatedTorrentCount);
        emitFirstFullTracePhase(u"pending_revision_delta_built"_s, details);
        emitMemoryProbeEvent(u"pending_revision_delta_built"_s, details);
    }

    if (!m_hasServedAnyResponse)
    {
        m_revisionStore.absorbDeltaIntoSnapshot(delta);
        return;
    }

    m_revisionStore.storeRevision(delta);
}

void WebUISync::MaindataSyncStore::clearDirtyState()
{
    m_updatedCategories.clear();
    m_removedCategories.clear();
    m_addedTags.clear();
    m_removedTags.clear();
    m_updatedTrackers.clear();
    m_removedTrackers.clear();
    m_updatedTorrents.clear();
    m_removedTorrents.clear();
}

void WebUISync::MaindataSyncStore::onCategoryAdded(const QString &categoryName)
{
    if (!shouldTrackDirtyState())
        return;

    m_removedCategories.remove(categoryName);
    m_updatedCategories.insert(categoryName);
}

void WebUISync::MaindataSyncStore::onCategoryRemoved(const QString &categoryName)
{
    if (!shouldTrackDirtyState())
        return;

    m_updatedCategories.remove(categoryName);
    m_removedCategories.insert(categoryName);
}

void WebUISync::MaindataSyncStore::onCategoryOptionsChanged(const QString &categoryName)
{
    if (!shouldTrackDirtyState())
        return;

    Q_ASSERT(!m_removedCategories.contains(categoryName));

    m_updatedCategories.insert(categoryName);
}

void WebUISync::MaindataSyncStore::onSubcategoriesSupportChanged()
{
    if (!shouldTrackDirtyState())
        return;

    const QStringList categoriesList = BitTorrent::Session::instance()->categories();
    const QHash<QString, QVariantMap> &categories = m_revisionStore.snapshot().categories;
    for (const QString &categoryName : categoriesList)
    {
        if (!categories.contains(categoryName))
        {
            m_removedCategories.remove(categoryName);
            m_updatedCategories.insert(categoryName);
        }
    }
}

void WebUISync::MaindataSyncStore::onTagAdded(const QString &tag)
{
    if (!shouldTrackDirtyState())
        return;

    m_removedTags.remove(tag);
    m_addedTags.insert(tag);
}

void WebUISync::MaindataSyncStore::onTagRemoved(const QString &tag)
{
    if (!shouldTrackDirtyState())
        return;

    m_addedTags.remove(tag);
    m_removedTags.insert(tag);
}

void WebUISync::MaindataSyncStore::onTorrentAdded(BitTorrent::Torrent *torrent)
{
    if (!shouldTrackDirtyState())
        return;

    const BitTorrent::TorrentID torrentID = torrent->id();

    m_removedTorrents.remove(torrentID);
    m_updatedTorrents.insert(torrentID);

    for (const BitTorrent::TrackerEntry &trackerEntry : asConst(torrent->trackers()))
    {
        m_knownTrackers[trackerEntry.url].insert(torrentID);
        m_updatedTrackers.insert(trackerEntry.url);
        m_removedTrackers.remove(trackerEntry.url);
    }
}

void WebUISync::MaindataSyncStore::onTorrentAboutToBeRemoved(BitTorrent::Torrent *torrent)
{
    if (!shouldTrackDirtyState())
        return;

    const BitTorrent::TorrentID torrentID = torrent->id();

    m_updatedTorrents.remove(torrentID);
    m_removedTorrents.insert(torrentID);

    for (const BitTorrent::TrackerEntry &trackerEntry : asConst(torrent->trackers()))
    {
        auto iter = m_knownTrackers.find(trackerEntry.url);
        Q_ASSERT(iter != m_knownTrackers.end());
        if (Q_UNLIKELY(iter == m_knownTrackers.end()))
            continue;

        QSet<BitTorrent::TorrentID> &torrentIDs = iter.value();
        torrentIDs.remove(torrentID);
        if (torrentIDs.isEmpty())
        {
            m_knownTrackers.erase(iter);
            m_updatedTrackers.remove(trackerEntry.url);
            m_removedTrackers.insert(trackerEntry.url);
        }
        else
        {
            m_updatedTrackers.insert(trackerEntry.url);
        }
    }
}

void WebUISync::MaindataSyncStore::onTorrentCategoryChanged(BitTorrent::Torrent *torrent
        , [[maybe_unused]] const QString &oldCategory)
{
    if (!shouldTrackDirtyState())
        return;

    m_updatedTorrents.insert(torrent->id());
}

void WebUISync::MaindataSyncStore::onTorrentMetadataReceived(BitTorrent::Torrent *torrent)
{
    if (!shouldTrackDirtyState())
        return;

    m_updatedTorrents.insert(torrent->id());
}

void WebUISync::MaindataSyncStore::onTorrentPaused(BitTorrent::Torrent *torrent)
{
    if (!shouldTrackDirtyState())
        return;

    m_updatedTorrents.insert(torrent->id());
}

void WebUISync::MaindataSyncStore::onTorrentResumed(BitTorrent::Torrent *torrent)
{
    if (!shouldTrackDirtyState())
        return;

    m_updatedTorrents.insert(torrent->id());
}

void WebUISync::MaindataSyncStore::onTorrentSavePathChanged(BitTorrent::Torrent *torrent)
{
    if (!shouldTrackDirtyState())
        return;

    m_updatedTorrents.insert(torrent->id());
}

void WebUISync::MaindataSyncStore::onTorrentSavingModeChanged(BitTorrent::Torrent *torrent)
{
    if (!shouldTrackDirtyState())
        return;

    m_updatedTorrents.insert(torrent->id());
}

void WebUISync::MaindataSyncStore::onTorrentTagAdded(BitTorrent::Torrent *torrent, [[maybe_unused]] const QString &tag)
{
    if (!shouldTrackDirtyState())
        return;

    m_updatedTorrents.insert(torrent->id());
}

void WebUISync::MaindataSyncStore::onTorrentTagRemoved(BitTorrent::Torrent *torrent, [[maybe_unused]] const QString &tag)
{
    if (!shouldTrackDirtyState())
        return;

    m_updatedTorrents.insert(torrent->id());
}

void WebUISync::MaindataSyncStore::onTorrentsUpdated(const QVector<BitTorrent::Torrent *> &torrents)
{
    if (!shouldTrackDirtyState())
        return;

    for (const BitTorrent::Torrent *torrent : torrents)
        m_updatedTorrents.insert(torrent->id());
}

void WebUISync::MaindataSyncStore::onTorrentTrackersChanged(BitTorrent::Torrent *torrent)
{
    if (!shouldTrackDirtyState())
        return;

    using namespace BitTorrent;

    const QVector<TrackerEntry> currentTrackerEntries = torrent->trackers();
    QSet<QString> currentTrackers;
    currentTrackers.reserve(currentTrackerEntries.size());
    for (const TrackerEntry &currentTrackerEntry : currentTrackerEntries)
        currentTrackers.insert(currentTrackerEntry.url);

    const TorrentID torrentID = torrent->id();
    Algorithm::removeIf(m_knownTrackers
            , [this, torrentID, currentTrackers]
                    (const QString &knownTracker, QSet<TorrentID> &torrentIDs)
    {
        if (auto idIter = torrentIDs.find(torrentID)
                ; (idIter != torrentIDs.end()) && !currentTrackers.contains(knownTracker))
        {
            torrentIDs.erase(idIter);
            if (torrentIDs.isEmpty())
            {
                m_updatedTrackers.remove(knownTracker);
                m_removedTrackers.insert(knownTracker);
                return true;
            }

            m_updatedTrackers.insert(knownTracker);
            return false;
        }

        if (currentTrackers.contains(knownTracker) && !torrentIDs.contains(torrentID))
        {
            torrentIDs.insert(torrentID);
            m_updatedTrackers.insert(knownTracker);
            return false;
        }

        return false;
    });

    for (const QString &currentTracker : asConst(currentTrackers))
    {
        if (!m_knownTrackers.contains(currentTracker))
        {
            m_knownTrackers.insert(currentTracker, {torrentID});
            m_updatedTrackers.insert(currentTracker);
            m_removedTrackers.remove(currentTracker);
        }
    }
}
