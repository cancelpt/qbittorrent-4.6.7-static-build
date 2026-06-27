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

#include <memory>

#include <QTemporaryDir>
#include <QTest>

#include "base/global.h"
#include "base/path.h"
#include "base/preferences.h"
#include "base/profile.h"
#include "base/settingsstorage.h"

class TestPreferences final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestPreferences)

public:
    TestPreferences() = default;

private slots:
    void init()
    {
        m_profileDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_profileDir->isValid());

        Profile::initInstance(Path(m_profileDir->path()), u"testpreferences"_s, false);
        SettingsStorage::initInstance();
        Preferences::initInstance();
    }

    void cleanup()
    {
        Preferences::freeInstance();
        SettingsStorage::freeInstance();
        Profile::freeInstance();
        m_profileDir.reset();
    }

    void missingSessionAlertQueueSizeUsesDefault() const
    {
        QCOMPARE(Preferences::instance()->sessionAlertQueueSize(), 10'000);
    }

    void configuredSessionAlertQueueSizeIsUsed() const
    {
        SettingsStorage::instance()->storeValue(u"BitTorrent/SessionAlertQueueSize"_s, 4321);
        QCOMPARE(Preferences::instance()->sessionAlertQueueSize(), 4321);
    }

    void zeroSessionAlertQueueSizeIsClamped() const
    {
        SettingsStorage::instance()->storeValue(u"BitTorrent/SessionAlertQueueSize"_s, 0);
        QCOMPARE(Preferences::instance()->sessionAlertQueueSize(), 100);
    }

    void belowMinimumSessionAlertQueueSizeIsClamped() const
    {
        SettingsStorage::instance()->storeValue(u"BitTorrent/SessionAlertQueueSize"_s, 99);
        QCOMPARE(Preferences::instance()->sessionAlertQueueSize(), 100);
    }

    void minimumSessionAlertQueueSizeIsPreserved() const
    {
        SettingsStorage::instance()->storeValue(u"BitTorrent/SessionAlertQueueSize"_s, 100);
        QCOMPARE(Preferences::instance()->sessionAlertQueueSize(), 100);
    }

    void negativeSessionAlertQueueSizeIsClamped() const
    {
        SettingsStorage::instance()->storeValue(u"BitTorrent/SessionAlertQueueSize"_s, -3);
        QCOMPARE(Preferences::instance()->sessionAlertQueueSize(), 100);
    }

    void missingExperimentalShareTorrentInfoNativeInfoUsesSafeDefault() const
    {
        QCOMPARE(Preferences::instance()->isExperimentalShareTorrentInfoNativeInfoEnabled(), false);
    }

    void configuredExperimentalShareTorrentInfoNativeInfoIsIgnored() const
    {
        SettingsStorage::instance()->storeValue(u"BitTorrent/ExperimentalShareTorrentInfoNativeInfo"_s, true);
        QCOMPARE(Preferences::instance()->isExperimentalShareTorrentInfoNativeInfoEnabled(), false);
    }

    void missingExperimentalDropAtpTorrentInfoAfterMetadataReadyUsesSafeDefault() const
    {
        QCOMPARE(Preferences::instance()->isExperimentalDropAtpTorrentInfoAfterMetadataReadyEnabled(), false);
    }

    void configuredExperimentalDropAtpTorrentInfoAfterMetadataReadyIsIgnored() const
    {
        SettingsStorage::instance()->storeValue(u"BitTorrent/ExperimentalDropAtpTorrentInfoAfterMetadataReady"_s, true);
        QCOMPARE(Preferences::instance()->isExperimentalDropAtpTorrentInfoAfterMetadataReadyEnabled(), false);
    }

private:
    std::unique_ptr<QTemporaryDir> m_profileDir;
};

QTEST_APPLESS_MAIN(TestPreferences)
#include "testpreferences.moc"
