/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  Spyhawk
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

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace Access
{
    struct ActivityName
    {
        QString id;
        QString label;
    };

    // Categories and results of activity log entries, in display order.
    const QList<ActivityName> &activityCategories();
    const QList<ActivityName> &activityOutcomes();
    QString activityCategoryLabel(const QString &id);
    QString activityOutcomeLabel(const QString &id);

    // One activity log entry (as returned by AccessStore) as a CSV line, without the line break.
    QString activityCsvHeader();
    QString activityCsvLine(const QJsonObject &entry);

    // Torrent hashes named by a request ("hashes" or "hash"); {"all"} for every torrent.
    QStringList torrentHashes(const QHash<QString, QString> &params);

    // One readable line describing a WebAPI request, for the activity log.
    QString describe(const QString &endpoint, const QHash<QString, QString> &params
            , const QHash<QString, QString> &torrentNames, const QStringList &fileNames = {});

    // Request parameters stored with a log entry: long values shortened, secrets hidden.
    QJsonObject safeDetails(const QString &endpoint, const QHash<QString, QString> &params);
}
