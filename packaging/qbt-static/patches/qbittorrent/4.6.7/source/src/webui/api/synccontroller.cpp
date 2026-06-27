/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2018-2023  Vladimir Golovnev <glassez@yandex.ru>
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

#include "synccontroller.h"

#include <algorithm>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

#include "base/algorithm.h"
#include "base/bittorrent/cachestatus.h"
#include "base/bittorrent/infohash.h"
#include "base/bittorrent/peeraddress.h"
#include "base/bittorrent/peerinfo.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/sessionstatus.h"
#include "base/bittorrent/torrent.h"
#include "base/bittorrent/torrentinfo.h"
#include "base/bittorrent/trackerentry.h"
#include "base/global.h"
#include "base/net/geoipmanager.h"
#include "base/preferences.h"
#include "base/utils/string.h"
#include "base/http/types.h"
#include "apierror.h"
#include "serialize/serialize_torrent.h"

namespace
{
    // Sync main data keys
    const QString KEY_SYNC_MAINDATA_QUEUEING = u"queueing"_s;
    const QString KEY_SYNC_MAINDATA_REFRESH_INTERVAL = u"refresh_interval"_s;
    const QString KEY_SYNC_MAINDATA_USE_ALT_SPEED_LIMITS = u"use_alt_speed_limits"_s;
    const QString KEY_SYNC_MAINDATA_USE_SUBCATEGORIES = u"use_subcategories"_s;

    // Sync torrent peers keys
    const QString KEY_SYNC_TORRENT_PEERS_SHOW_FLAGS = u"show_flags"_s;

    // Peer keys
    const QString KEY_PEER_CLIENT = u"client"_s;
    const QString KEY_PEER_ID_CLIENT = u"peer_id_client"_s;
    const QString KEY_PEER_CONNECTION_TYPE = u"connection"_s;
    const QString KEY_PEER_COUNTRY = u"country"_s;
    const QString KEY_PEER_COUNTRY_CODE = u"country_code"_s;
    const QString KEY_PEER_DOWN_SPEED = u"dl_speed"_s;
    const QString KEY_PEER_FILES = u"files"_s;
    const QString KEY_PEER_FLAGS = u"flags"_s;
    const QString KEY_PEER_FLAGS_DESCRIPTION = u"flags_desc"_s;
    const QString KEY_PEER_IP = u"ip"_s;
    const QString KEY_PEER_PORT = u"port"_s;
    const QString KEY_PEER_PROGRESS = u"progress"_s;
    const QString KEY_PEER_RELEVANCE = u"relevance"_s;
    const QString KEY_PEER_TOT_DOWN = u"downloaded"_s;
    const QString KEY_PEER_TOT_UP = u"uploaded"_s;
    const QString KEY_PEER_UP_SPEED = u"up_speed"_s;

    // TransferInfo keys
    const QString KEY_TRANSFER_CONNECTION_STATUS = u"connection_status"_s;
    const QString KEY_TRANSFER_DHT_NODES = u"dht_nodes"_s;
    const QString KEY_TRANSFER_DLDATA = u"dl_info_data"_s;
    const QString KEY_TRANSFER_DLRATELIMIT = u"dl_rate_limit"_s;
    const QString KEY_TRANSFER_DLSPEED = u"dl_info_speed"_s;
    const QString KEY_TRANSFER_FREESPACEONDISK = u"free_space_on_disk"_s;
    const QString KEY_TRANSFER_UPDATA = u"up_info_data"_s;
    const QString KEY_TRANSFER_UPRATELIMIT = u"up_rate_limit"_s;
    const QString KEY_TRANSFER_UPSPEED = u"up_info_speed"_s;

    // Statistics keys
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

    void processMap(const QVariantMap &prevData, const QVariantMap &data, QVariantMap &syncData);
    void processHash(QVariantHash prevData, const QVariantHash &data, QVariantMap &syncData, QVariantList &removedItems);
    void processList(QVariantList prevData, const QVariantList &data, QVariantList &syncData, QVariantList &removedItems);
    QJsonObject generateSyncData(int acceptedResponseId, const QVariantMap &data, QVariantMap &lastAcceptedData, QVariantMap &lastData);

    // Compare two structures (prevData, data) and calculate difference (syncData).
    // Structures encoded as map.
    void processMap(const QVariantMap &prevData, const QVariantMap &data, QVariantMap &syncData)
    {
        // initialize output variable
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

    // Compare two lists of structures (prevData, data) and calculate difference (syncData, removedItems).
    // Structures encoded as map.
    // Lists are encoded as hash table (indexed by structure key value) to improve ease of searching for removed items.
    void processHash(QVariantHash prevData, const QVariantHash &data, QVariantMap &syncData, QVariantList &removedItems)
    {
        // initialize output variables
        syncData.clear();
        removedItems.clear();

        if (prevData.isEmpty())
        {
            // If list was empty before, then difference is a whole new list.
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
                        // new list item found - append it to syncData
                        syncData[i.key()] = i.value();
                    }
                    else
                    {
                        QVariantMap map;
                        processMap(prevData[i.key()].toMap(), i.value().toMap(), map);
                        // existing list item found - remove it from prevData
                        prevData.remove(i.key());
                        if (!map.isEmpty())
                        {
                            // changed list item found - append its changes to syncData
                            syncData[i.key()] = map;
                        }
                    }
                    break;
                case QVariant::StringList:
                    if (!prevData.contains(i.key()))
                    {
                        // new list item found - append it to syncData
                        syncData[i.key()] = i.value();
                    }
                    else
                    {
                        QVariantList list;
                        QVariantList removedList;
                        processList(prevData[i.key()].toList(), i.value().toList(), list, removedList);
                        // existing list item found - remove it from prevData
                        prevData.remove(i.key());
                        if (!list.isEmpty() || !removedList.isEmpty())
                        {
                            // changed list item found - append entire list to syncData
                            syncData[i.key()] = i.value();
                        }
                    }
                    break;
                default:
                    Q_ASSERT(false);
                    break;
                }
            }

            if (!prevData.isEmpty())
            {
                // prevData contains only items that are missing now -
                // put them in removedItems
                for (auto i = prevData.cbegin(); i != prevData.cend(); ++i)
                    removedItems << i.key();
            }
        }
    }

    // Compare two lists of simple value (prevData, data) and calculate difference (syncData, removedItems).
    void processList(QVariantList prevData, const QVariantList &data, QVariantList &syncData, QVariantList &removedItems)
    {
        // initialize output variables
        syncData.clear();
        removedItems.clear();

        if (prevData.isEmpty())
        {
            // If list was empty before, then difference is a whole new list.
            syncData = data;
        }
        else
        {
            for (const QVariant &item : data)
            {
                if (!prevData.contains(item))
                {
                    // new list item found - append it to syncData
                    syncData.append(item);
                }
                else
                {
                    // unchanged list item found - remove it from prevData
                    prevData.removeOne(item);
                }
            }

            if (!prevData.isEmpty())
            {
                // prevData contains only items that are missing now -
                // put them in removedItems
                removedItems = prevData;
            }
        }
    }

    QJsonObject generateSyncData(int acceptedResponseId, const QVariantMap &data, QVariantMap &lastAcceptedData, QVariantMap &lastData)
    {
        QVariantMap syncData;
        bool fullUpdate = true;
        const int lastResponseId = (acceptedResponseId > 0) ? lastData[KEY_RESPONSE_ID].toInt() : 0;
        if (lastResponseId > 0)
        {
            if (lastResponseId == acceptedResponseId)
                lastAcceptedData = lastData;

            if (const int lastAcceptedResponseId = lastAcceptedData[KEY_RESPONSE_ID].toInt()
                    ; lastAcceptedResponseId == acceptedResponseId)
            {
                fullUpdate = false;
            }
        }

        if (fullUpdate)
        {
            lastAcceptedData.clear();
            syncData = data;
            syncData[KEY_FULL_UPDATE] = true;
        }
        else
        {
            processMap(lastAcceptedData, data, syncData);
        }

        const int responseId = (lastResponseId % 1000000) + 1;  // cycle between 1 and 1000000
        lastData = data;
        lastData[KEY_RESPONSE_ID] = responseId;
        syncData[KEY_RESPONSE_ID] = responseId;

        return QJsonObject::fromVariantMap(syncData);
    }
}

SyncController::SyncController(IApplication *app, WebUISync::MaindataSyncStore *maindataSyncStore, QObject *parent)
    : APIController(app, parent)
    , m_maindataSyncStore(maindataSyncStore)
{
    Q_ASSERT(m_maindataSyncStore);
    m_maindataSyncStore->acquireSyncSession();
}

SyncController::~SyncController()
{
    if (m_maindataSyncStore)
        m_maindataSyncStore->releaseSyncSession();
}

// GET param:
//   - rid (int): last response id
void SyncController::maindataAction()
{
    Q_ASSERT(m_maindataSyncStore);
    const int requestedRid = params()[u"rid"_s].toInt();
    if (const QByteArray initialResponse = m_maindataSyncStore->buildInitialFullUpdateResponse(m_maindataCursor, requestedRid)
        ; !initialResponse.isEmpty())
    {
        setResult(initialResponse, Http::CONTENT_TYPE_JSON);
        m_maindataSyncStore->notifySerializedFullUpdateResponseCommitted(
            requestedRid, m_maindataCursor.lastSentRid, initialResponse.size());
        return;
    }

    QJsonObject response = m_maindataSyncStore->buildResponse(m_maindataCursor, requestedRid);
    if (Preferences::instance()->isExperimentalStatelessWebUIMaindataEnabled())
    {
        const QByteArray responseBytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
        setResult(responseBytes, Http::CONTENT_TYPE_JSON);
        m_maindataSyncStore->notifySerializedFullUpdateResponseCommitted(
            requestedRid, m_maindataCursor.lastSentRid, responseBytes.size());
        return;
    }

    setResult(response);
}

// GET param:
//   - hash (string): torrent hash (ID)
//   - rid (int): last response id
void SyncController::torrentPeersAction()
{
    const auto id = BitTorrent::TorrentID::fromString(params()[u"hash"_s]);
    const BitTorrent::Torrent *torrent = BitTorrent::Session::instance()->getTorrent(id);
    if (!torrent)
        throw APIError(APIErrorType::NotFound);

    QVariantMap data;
    QVariantHash peers;

    const QVector<BitTorrent::PeerInfo> peersList = torrent->peers();

    bool resolvePeerCountries = Preferences::instance()->resolvePeerCountries();

    data[KEY_SYNC_TORRENT_PEERS_SHOW_FLAGS] = resolvePeerCountries;

    for (const BitTorrent::PeerInfo &pi : peersList)
    {
        if (pi.address().ip.isNull()) continue;

        QVariantMap peer =
        {
            {KEY_PEER_IP, pi.address().ip.toString()},
            {KEY_PEER_PORT, pi.address().port},
            {KEY_PEER_CLIENT, pi.client()},
            {KEY_PEER_ID_CLIENT, pi.peerIdClient()},
            {KEY_PEER_PROGRESS, pi.progress()},
            {KEY_PEER_DOWN_SPEED, pi.payloadDownSpeed()},
            {KEY_PEER_UP_SPEED, pi.payloadUpSpeed()},
            {KEY_PEER_TOT_DOWN, pi.totalDownload()},
            {KEY_PEER_TOT_UP, pi.totalUpload()},
            {KEY_PEER_CONNECTION_TYPE, pi.connectionType()},
            {KEY_PEER_FLAGS, pi.flags()},
            {KEY_PEER_FLAGS_DESCRIPTION, pi.flagsDescription()},
            {KEY_PEER_RELEVANCE, pi.relevance()}
        };

        if (torrent->hasMetadata())
        {
            const PathList filePaths = torrent->info().filesForPiece(pi.downloadingPieceIndex());
            QStringList filesForPiece;
            filesForPiece.reserve(filePaths.size());
            for (const Path &filePath : filePaths)
                filesForPiece.append(filePath.toString());
            peer.insert(KEY_PEER_FILES, filesForPiece.join(u'\n'));
        }

        if (resolvePeerCountries)
        {
            peer[KEY_PEER_COUNTRY_CODE] = pi.country().toLower();
            peer[KEY_PEER_COUNTRY] = Net::GeoIPManager::CountryName(pi.country());
        }

        peers[pi.address().toString()] = peer;
    }
    data[u"peers"_s] = peers;

    const int acceptedResponseId = params()[u"rid"_s].toInt();
    setResult(generateSyncData(acceptedResponseId, data, m_lastAcceptedPeersResponse, m_lastPeersResponse));
}
