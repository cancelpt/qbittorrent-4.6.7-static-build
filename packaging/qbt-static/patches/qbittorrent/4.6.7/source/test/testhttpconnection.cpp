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

#include <iostream>
#include <sstream>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringView>
#include "base/global.h"

#define private public
#include "base/http/connection.h"
#undef private

namespace
{
    struct AcceptEncodingCase
    {
        const char *name;
        QString headerValue;
        bool expected;
    };
}

int main()
{
    const AcceptEncodingCase cases[] =
    {
        {"empty", {}, false},
        {"whitespace_only", u"  \t "_s, false},
        {"gzip", u"gzip"_s, true},
        {"gzip_q1", u"gzip;q=1"_s, true},
        {"gzip_qhalf", u"gzip;q=0.5"_s, true},
        {"gzip_q0", u"gzip;q=0"_s, false},
        {"gzip_and_deflate", u"gzip,deflate"_s, true},
        {"star", u"*"_s, true},
        {"identity", u"identity"_s, false},
        {"deflate", u"deflate"_s, false},
        {"identity_q0", u"identity;q=0"_s, false},
        {"identity_q1", u"identity;q=1"_s, false},
        {"spaces_around_tokens", u" gzip ; q=1 , deflate "_s, true},
    };

    for (const AcceptEncodingCase &testCase : cases)
    {
        const bool actual = Http::Connection::acceptsGzipEncoding(testCase.headerValue);
        if (actual != testCase.expected)
        {
            std::cerr << "case failed: " << testCase.name
                << " expected=" << testCase.expected
                << " actual=" << actual << '\n';
            return 1;
        }
    }

    return 0;
}
