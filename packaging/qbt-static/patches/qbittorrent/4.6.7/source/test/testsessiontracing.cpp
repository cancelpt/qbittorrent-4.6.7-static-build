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

#include <QTest>

#include "base/global.h"
#include "webui/sessiontracing.h"

class TestSessionTracing final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestSessionTracing)

private slots:
    void traceFingerprintRedactsRawSessionId() const
    {
        const QString raw = u"sid-plain-text-value"_s;
        const QString fingerprint = WebUITrace::makeTraceFingerprint(raw);

        QVERIFY(!fingerprint.isEmpty());
        QCOMPARE(fingerprint.size(), 12);
        QVERIFY(fingerprint != raw);
    }

    void sessionStartDetailsCaptureReasonAndRedactedIds() const
    {
        const QVariantMap details = WebUITrace::buildSessionStartTraceDetails(
            u"auth_bypass_no_cookie"_s,
            u"client-a"_s,
            u"/api/v2/sync/maindata"_s,
            u"GET"_s,
            u"ExampleBrowser/1.0 Example Extension"_s,
            false,
            false,
            {},
            u"new-sid"_s,
            17,
            3600,
            3);

        QCOMPARE(details.value(u"reason"_s).toString(), u"auth_bypass_no_cookie"_s);
        QCOMPARE(details.value(u"client_id"_s).toString(), u"client-a"_s);
        QCOMPARE(details.value(u"request_path"_s).toString(), u"/api/v2/sync/maindata"_s);
        QCOMPARE(details.value(u"request_method"_s).toString(), u"GET"_s);
        QCOMPARE(details.value(u"auth_needed"_s).toBool(), false);
        QCOMPARE(details.value(u"session_cookie_present"_s).toBool(), false);
        QCOMPARE(details.value(u"total_web_sessions"_s).toInt(), 17);
        QCOMPARE(details.value(u"session_timeout_seconds"_s).toInt(), 3600);
        QCOMPARE(details.value(u"expired_sessions_swept"_s).toInt(), 3);
        QVERIFY(details.value(u"created_session_fingerprint"_s).toString() != u"new-sid"_s);
        QVERIFY(!details.contains(u"presented_session_fingerprint"_s));
    }

    void userAgentSummaryIsTrimmedForTrace() const
    {
        const QString longUserAgent =
            u"VeryLongBrowser/1.0 with lots of additional tokens that should be trimmed in trace output"_s;
        const QString summary = WebUITrace::summarizeUserAgentForTrace(longUserAgent);

        QVERIFY(summary.size() <= 80);
        QVERIFY(summary.startsWith(u"VeryLongBrowser/1.0"_s));
    }
};

QTEST_APPLESS_MAIN(TestSessionTracing)
#include "testsessiontracing.moc"
