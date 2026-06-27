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

#include "sessiontracing.h"

#include <QCryptographicHash>

namespace
{
    constexpr qsizetype TRACE_FINGERPRINT_LENGTH = 12;
    constexpr qsizetype USER_AGENT_SUMMARY_MAX_LENGTH = 80;

    QString buildTraceFingerprint(const QString &value)
    {
        if (value.isEmpty())
            return {};

        const QByteArray hash = QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha1);
        return QString::fromLatin1(hash.toHex().left(TRACE_FINGERPRINT_LENGTH));
    }

    QString buildUserAgentSummary(QString value)
    {
        value = value.simplified();
        if (value.size() > USER_AGENT_SUMMARY_MAX_LENGTH)
            value.truncate(USER_AGENT_SUMMARY_MAX_LENGTH);
        return value;
    }

    void insertIfNotEmpty(QVariantMap &details, const QString &key, const QString &value)
    {
        if (!value.isEmpty())
            details.insert(key, value);
    }

    void appendCommonTraceDetails(QVariantMap &details, const QString &reason, const QString &clientId
        , const QString &requestPath, const QString &requestMethod, const QString &userAgent)
    {
        details.insert(u"reason"_s, reason);
        details.insert(u"client_id"_s, clientId);
        details.insert(u"request_path"_s, requestPath);
        details.insert(u"request_method"_s, requestMethod);

        const QString userAgentSummary = buildUserAgentSummary(userAgent);
        insertIfNotEmpty(details, u"user_agent_summary"_s, userAgentSummary);
        insertIfNotEmpty(details, u"user_agent_fingerprint"_s, buildTraceFingerprint(userAgent));
    }
}

QString WebUITrace::makeTraceFingerprint(const QString &value)
{
    return buildTraceFingerprint(value);
}

QString WebUITrace::summarizeUserAgentForTrace(const QString &userAgent)
{
    return buildUserAgentSummary(userAgent);
}

QVariantMap WebUITrace::buildSessionStartTraceDetails(const QString &reason, const QString &clientId
    , const QString &requestPath, const QString &requestMethod, const QString &userAgent
    , const bool authNeeded, const bool sessionCookiePresent, const QString &presentedSessionId
    , const QString &createdSessionId, const qsizetype totalWebSessions, const qint64 sessionTimeoutSeconds
    , const qsizetype expiredSessionsSwept)
{
    QVariantMap details;
    appendCommonTraceDetails(details, reason, clientId, requestPath, requestMethod, userAgent);
    details.insert(u"auth_needed"_s, authNeeded);
    details.insert(u"session_cookie_present"_s, sessionCookiePresent);
    details.insert(u"total_web_sessions"_s, totalWebSessions);
    details.insert(u"session_timeout_seconds"_s, sessionTimeoutSeconds);
    details.insert(u"expired_sessions_swept"_s, expiredSessionsSwept);

    insertIfNotEmpty(details, u"presented_session_fingerprint"_s, buildTraceFingerprint(presentedSessionId));
    insertIfNotEmpty(details, u"created_session_fingerprint"_s, buildTraceFingerprint(createdSessionId));

    return details;
}

QVariantMap WebUITrace::buildSessionEndTraceDetails(const QString &reason, const QString &clientId
    , const QString &requestPath, const QString &requestMethod, const QString &userAgent
    , const QString &sessionId, const qsizetype totalWebSessions, const qint64 sessionIdleMs)
{
    QVariantMap details;
    appendCommonTraceDetails(details, reason, clientId, requestPath, requestMethod, userAgent);
    details.insert(u"total_web_sessions"_s, totalWebSessions);
    details.insert(u"session_idle_ms"_s, sessionIdleMs);

    insertIfNotEmpty(details, u"session_fingerprint"_s, buildTraceFingerprint(sessionId));

    return details;
}
