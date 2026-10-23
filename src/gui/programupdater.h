/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2021  Mike Tzou (Chocobo1)
 * Copyright (C) 2010  Christophe Dumez <chris@qbittorrent.org>
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

#include <QByteArray>
#include <QObject>
#include <QString>

#include "base/utils/version.h"

namespace Net
{
    struct DownloadResult;
}

class ProgramUpdater final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(ProgramUpdater)

public:
    // Kettle Edition has its own number: major.minor.patch
    using EditionVersion = Utils::Version<3>;

    using QObject::QObject;

    void checkForUpdates();
    // true when the Kettle Edition release info was downloaded and its signature is valid (even if nothing newer was found)
    bool isEditionCheckVerified() const;
    // newest Kettle Edition release with a valid signature, if it is newer than this one
    EditionVersion getNewEditionVersion() const;
    QString getNewEditionLevel() const;
    QString getNewEditionNotes() const;
    // content of kettle-update.json of that release (its signature was verified)
    QByteArray getNewEditionManifest() const;
    // where a file of the release `tag` is downloaded from
    static QString editionFileUrl(const QString &tag, const QString &fileName);
    // true when the test settings (KETTLE_UPDATE_*) are in use: only outside the Program Files folders
    static bool isTestMode();

signals:
    void updateCheckFinished();

private slots:
    void editionDownloadFinished(const Net::DownloadResult &result, QByteArray &data);

private:
    void handleFinishedRequest();
    void processEditionManifest();

    int m_pendingRequestCount = 0;
    QByteArray m_manifestData;
    QByteArray m_manifestSignature;
    bool m_isEditionCheckVerified = false;
    EditionVersion m_newEdition;
    QString m_newEditionLevel;
    QString m_newEditionNotes;
};
