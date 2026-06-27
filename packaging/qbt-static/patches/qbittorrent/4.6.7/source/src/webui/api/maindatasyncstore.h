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

#pragma once

#include <QHash>
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>
#include <QVariantMap>
#include <QVariantList>

#include "base/bittorrent/infohash.h"

namespace BitTorrent
{
    class Torrent;
}

namespace WebUISync
{
    struct MaindataSyncData
    {
        QHash<QString, QVariantMap> categories;
        QVariantList tags;
        QHash<QString, QVariantMap> torrents;
        QHash<QString, QStringList> trackers;
        QVariantMap serverState;

        QStringList removedCategories;
        QStringList removedTags;
        QStringList removedTorrents;
        QStringList removedTrackers;

        bool isEmpty() const;
    };

    struct MaindataSessionCursor
    {
        int acceptedRid = 0;
        qint64 acceptedRevision = 0;
        int lastSentRid = 0;
        qint64 lastSentRevision = 0;
    };

    struct MaindataSyncPlan
    {
        bool fullUpdate = true;
        qint64 baseRevision = 0;
        qint64 targetRevision = 0;
        int responseRid = 0;
    };

    struct PreparedCompactSnapshot
    {
        QVariantList fieldDefaults;
        qsizetype fieldDefaultsBytes = 0;
        QList<QStringList> stringPools;
        qsizetype stringPoolsBytes = 0;
        QHash<QString, QByteArray> torrentRows;
        QVariantList descriptiveFieldDefaults;
        qsizetype descriptiveFieldDefaultsBytes = 0;
        QList<QStringList> descriptiveStringPools;
        qsizetype descriptiveStringPoolsBytes = 0;
        QHash<QString, QByteArray> descriptiveTorrentRows;
        QVariantList descriptiveStatusFieldDefaults;
        qsizetype descriptiveStatusFieldDefaultsBytes = 0;
        QList<QStringList> descriptiveStatusStringPools;
        qsizetype descriptiveStatusStringPoolsBytes = 0;
        QHash<QString, QByteArray> descriptiveStatusTorrentRows;
        QVariantList descriptiveLifecycleFieldDefaults;
        qsizetype descriptiveLifecycleFieldDefaultsBytes = 0;
        QList<QStringList> descriptiveLifecycleStringPools;
        qsizetype descriptiveLifecycleStringPoolsBytes = 0;
        QHash<QString, QByteArray> descriptiveLifecycleTorrentRows;
    };

    MaindataSyncPlan planMaindataSync(MaindataSessionCursor &cursor, int requestedRid
        , qint64 currentRevision, qint64 earliestRetainedRevision);
    PreparedCompactSnapshot prepareCompactSnapshot(const QHash<QString, QVariantMap> &rows);

    class MaindataRevisionStore
    {
    public:
        explicit MaindataRevisionStore(qsizetype maxRetainedRevisions = 32
            , qsizetype maxEstimatedPayloadBytes = (16 * 1024 * 1024));

        void initialize(const MaindataSyncData &snapshot);
        void initializeCompactSnapshot(const MaindataSyncData &snapshot, const PreparedCompactSnapshot &preparedSnapshot);
        void reset();
        void absorbDeltaIntoSnapshot(const MaindataSyncData &delta);
        void storeRevision(const MaindataSyncData &delta);

        qint64 currentRevision() const;
        qint64 earliestRetainedRevision() const;
        const MaindataSyncData &snapshot() const;
        QVariantMap snapshotTorrentRow(const QString &torrentID) const;
        bool hasCachedInitialFullUpdateResponse() const;
        qsizetype cachedInitialFullUpdateResponseBytes() const;
        qsizetype estimatedWideSnapshotStoreBytes() const;
        qsizetype retainedRevisionPayloadBytes() const;
        qsizetype compactSnapshotStorageBytes() const;
        qsizetype compactSnapshotPrimaryStorageBytes() const;
        qsizetype compactSnapshotDescriptiveStorageBytes() const;
        qsizetype compactSnapshotDescriptiveStatusStorageBytes() const;
        qsizetype compactSnapshotDescriptiveLifecycleStorageBytes() const;

        QByteArray buildInitialFullUpdateResponse(MaindataSessionCursor &cursor, int requestedRid) const;
        QJsonObject buildResponse(MaindataSessionCursor &cursor, int requestedRid) const;

    private:
        struct RevisionEntry
        {
            qint64 revision = 0;
            MaindataSyncData delta;
            qsizetype estimatedPayloadBytes = 0;
        };

        MaindataSyncData aggregateRange(qint64 baseRevision, qint64 targetRevision) const;
        QVariantMap expandCompactSnapshotTorrentRow(const QString &torrentID, bool materializeDeferredFamilies = true) const;
        QVariantMap materializeDescriptiveNumericTorrentRow(const QString &torrentID) const;
        QVariantMap materializeDescriptiveLifecycleTorrentRow(const QString &torrentID) const;
        bool shouldMaterializeDescriptiveNumericOnDemand() const;
        bool shouldRetainDescriptiveLifecycleRows() const;
        void applyDeltaToCompactSnapshot(const MaindataSyncData &delta);
        QByteArray buildCompactSnapshotInitialFullUpdateResponseBytes(int responseRid) const;
        QJsonObject buildSnapshotResponseObject() const;
        void evictRevisions();

        qsizetype m_maxRetainedRevisions = 0;
        qsizetype m_maxEstimatedPayloadBytes = 0;
        qsizetype m_retainedPayloadBytes = 0;
        qint64 m_currentRevision = 0;
        QList<RevisionEntry> m_revisions;
        MaindataSyncData m_snapshot;
        QVariantList m_compactSnapshotFieldDefaults;
        qsizetype m_compactSnapshotFieldDefaultsBytes = 0;
        QList<QStringList> m_compactSnapshotStringPools;
        qsizetype m_compactSnapshotStringPoolsBytes = 0;
        QHash<QString, QByteArray> m_compactSnapshotTorrentRows;
        QVariantList m_compactSnapshotDescriptiveFieldDefaults;
        qsizetype m_compactSnapshotDescriptiveFieldDefaultsBytes = 0;
        QList<QStringList> m_compactSnapshotDescriptiveStringPools;
        qsizetype m_compactSnapshotDescriptiveStringPoolsBytes = 0;
        QHash<QString, QByteArray> m_compactSnapshotDescriptiveTorrentRows;
        QVariantList m_compactSnapshotDescriptiveStatusFieldDefaults;
        qsizetype m_compactSnapshotDescriptiveStatusFieldDefaultsBytes = 0;
        QList<QStringList> m_compactSnapshotDescriptiveStatusStringPools;
        qsizetype m_compactSnapshotDescriptiveStatusStringPoolsBytes = 0;
        QHash<QString, QByteArray> m_compactSnapshotDescriptiveStatusTorrentRows;
        QVariantList m_compactSnapshotDescriptiveLifecycleFieldDefaults;
        qsizetype m_compactSnapshotDescriptiveLifecycleFieldDefaultsBytes = 0;
        QList<QStringList> m_compactSnapshotDescriptiveLifecycleStringPools;
        qsizetype m_compactSnapshotDescriptiveLifecycleStringPoolsBytes = 0;
        QHash<QString, QByteArray> m_compactSnapshotDescriptiveLifecycleTorrentRows;
        mutable qint64 m_cachedInitialFullUpdateRevision = 0;
        mutable QByteArray m_cachedInitialFullUpdateResponse;
        const bool m_compactTorrentRowsEnabled = false;
        const bool m_directCompactInitialFullResponseEnabled = false;
        const bool m_compactStringPoolEnabled = false;
        const bool m_compactDescriptiveSidecarEnabled = false;
        const bool m_compactDescriptiveSubgroupsEnabled = false;
        const bool m_compactDescriptiveLifecycleOnDemandEnabled = false;
        const bool m_compactDescriptiveNumericOnDemandEnabled = false;
    };

    class MaindataSyncStore final : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(MaindataSyncStore)

    public:
        explicit MaindataSyncStore(QObject *parent = nullptr);

        void acquireSyncSession();
        void releaseSyncSession();
        void recordWebSessionEvent(const QString &event, const QVariantMap &details);
        void updateFreeDiskSpace(qint64 freeDiskSpace);
        QByteArray buildInitialFullUpdateResponse(MaindataSessionCursor &cursor, int requestedRid);
        QJsonObject buildResponse(MaindataSessionCursor &cursor, int requestedRid);
        void notifySerializedFullUpdateResponseCommitted(int requestedRid, int responseRid, qsizetype responseBytes);
        bool hasRetainedSharedState() const;

    private slots:
        void prewarm();
        void onCategoryAdded(const QString &categoryName);
        void onCategoryRemoved(const QString &categoryName);
        void onCategoryOptionsChanged(const QString &categoryName);
        void onSubcategoriesSupportChanged();
        void onTagAdded(const QString &tag);
        void onTagRemoved(const QString &tag);
        void onTorrentAdded(BitTorrent::Torrent *torrent);
        void onTorrentAboutToBeRemoved(BitTorrent::Torrent *torrent);
        void onTorrentCategoryChanged(BitTorrent::Torrent *torrent, const QString &oldCategory);
        void onTorrentMetadataReceived(BitTorrent::Torrent *torrent);
        void onTorrentPaused(BitTorrent::Torrent *torrent);
        void onTorrentResumed(BitTorrent::Torrent *torrent);
        void onTorrentSavePathChanged(BitTorrent::Torrent *torrent);
        void onTorrentSavingModeChanged(BitTorrent::Torrent *torrent);
        void onTorrentTagAdded(BitTorrent::Torrent *torrent, const QString &tag);
        void onTorrentTagRemoved(BitTorrent::Torrent *torrent, const QString &tag);
        void onTorrentsUpdated(const QVector<BitTorrent::Torrent *> &torrents);
        void onTorrentTrackersChanged(BitTorrent::Torrent *torrent);

    private:
        struct FirstFullTraceState
        {
            QString outputPath;
            int requestedRid = 0;
            bool enabled = false;
            bool active = false;
            bool consumed = false;
            int nextEventIndex = 0;
        };

        QVariantMap summarizeDeltaForTrace(const MaindataSyncData &delta, qsizetype updatedTorrentCount) const;
        bool shouldTraceFirstFullRequest(const MaindataSessionCursor &cursor, int requestedRid) const;
        void emitLifecycleTraceEvent(const QString &event, const QVariantMap &details = {}) const;
        void beginFirstFullTrace(int requestedRid);
        void emitFirstFullTracePhase(const QString &phase, const QVariantMap &details = {});
        void finishFirstFullTrace();
        void startMemoryProbeTimer();
        QVariantMap collectMemoryProbeSample() const;
        QVariantMap collectRetainedStateCounters() const;
        void emitMemoryProbeEvent(const QString &event, const QVariantMap &details = {});
        void maybeTraceFirstFullResponseCommitted(int requestedRid, int responseRid, qsizetype responseBytes, qint64 currentRevision);
        QByteArray buildStatelessFullResponseBytes(MaindataSessionCursor &cursor, int requestedRid);
        QJsonObject buildStatelessFullResponse(MaindataSessionCursor &cursor, int requestedRid);
        bool shouldTrackDirtyState() const;
        void ensureInitialized();
        void initializeSnapshot();
        void releaseSharedState();
        void maybeTrimAllocatorNonIdle(const QString &trigger);
        void maybeTrimAllocatorOnSyncRelease();
        void maybeTrimAllocatorOnResponsePath();
        void storePendingRevision();
        void clearDirtyState();

        qint64 m_freeDiskSpace = 0;
        const bool m_statelessModeEnabled = false;
        const bool m_releaseIdleStateEnabled = false;
        const bool m_trimAllocatorAfterIdleReleaseEnabled = false;
        const qsizetype m_nonIdleTrimMaxActiveSyncSessions = 3;
        bool m_hasServedAnyResponse = false;
        qsizetype m_activeSyncSessionCount = 0;
        qint64 m_lastNonIdleTrimAttemptMs = 0;

        QHash<QString, QSet<BitTorrent::TorrentID>> m_knownTrackers;

        QSet<QString> m_updatedCategories;
        QSet<QString> m_removedCategories;
        QSet<QString> m_addedTags;
        QSet<QString> m_removedTags;
        QSet<QString> m_updatedTrackers;
        QSet<QString> m_removedTrackers;
        QSet<BitTorrent::TorrentID> m_updatedTorrents;
        QSet<BitTorrent::TorrentID> m_removedTorrents;

        FirstFullTraceState m_firstFullTrace;
        struct MemoryProbeState
        {
            QString outputPath;
            int intervalMs = 0;
            bool enabled = false;
            int nextEventIndex = 0;
            qint64 firstFullPssAfterSnapshotBytes = -1;
            qint64 firstFullPssAfterMaterializationBytes = -1;
            qint64 firstFullPssAfterCommittedBytes = -1;
        };

        MemoryProbeState m_memoryProbe;
        QTimer *m_memoryProbeTimer = nullptr;
        MaindataRevisionStore m_revisionStore;
    };
}
