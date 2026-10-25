/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  Kettle Edition
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

#include <QDate>
#include <QList>
#include <QMap>
#include <QtTypes>

namespace BitTorrent
{
    class Torrent;

    // Data moved on one day or in one month, counted like the all-time totals
    struct UsageEntry
    {
        qint64 downloaded = 0;
        qint64 uploaded = 0;
    };

    // Daily entries are kept for about a year, monthly ones (keyed by the first day of the month) for good.
    // "since" is the day tracking began; everything moved before that is only in the all-time totals.
    struct UsageHistory
    {
        QDate since;
        QMap<QDate, UsageEntry> days;
        QMap<QDate, UsageEntry> months;
    };

    // The torrents in the same states as the status filter of the transfer list
    struct TorrentCounts
    {
        int downloading = 0;
        int seeding = 0;
        int stopped = 0;
        int errored = 0;
    };

    TorrentCounts countTorrents(const QList<Torrent *> &torrents);
}
