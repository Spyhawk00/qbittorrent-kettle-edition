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

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include "base/path.h"

class QTimer;

namespace Net
{
    struct DownloadResult;
}

// Downloads the files of a Kettle Edition release that differ from the installed ones and checks each one
// against the SHA-256 in the (already signature-checked) release info. Once they are all there, the update is
// installed by kettle-update-helper.exe: at 4:00 AM, or when qBittorrent starts and an update is already waiting.
// Never when the user quits. The helper replaces the files after qBittorrent has exited, and puts the old version
// back if the new one does not run properly.
class UpdateInstaller final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(UpdateInstaller)

public:
    using QObject::QObject;

    // Call once the main window exists: shows the outcome of an earlier update, reports a healthy update to the helper
    void start();
    // `manifest` must be the content of a kettle-update.json whose signature was verified
    void download(const QByteArray &manifest);
    bool isBusy() const;

    // Install on start: if a verified update is waiting, starts the helper and returns true. The caller then quits at once.
    // `windowState` ("hidden", "minimized" or "normal") is how the new version shows its window.
    static bool installPendingUpdate(const QString &windowState);
    // How the window was before an update restarted the program ("hidden", "minimized", "normal"), empty after a normal start
    static QString restoredWindowState();

signals:
    // all changed files of the release are in the folder and match their SHA-256
    void updateReady(const QString &edition, const QString &folder);

private:
    struct FileEntry
    {
        QString path;  // relative to the program folder, with "/" as separator
        QString name;  // name of the file in the GitHub release
        qint64 size = 0;
        QString sha256;
    };

    void showLastResult();
    void startHealthyCheck(const QString &statePath);
    void markHealthy();
    void scheduleInstall();
    void attemptInstall();
    QString currentWindowState() const;

    void downloadNext();
    void handleFileDownloaded(const Net::DownloadResult &result);
    void fail(const QString &reason);
    void finish();

    bool m_busy = false;
    QString m_edition;
    QString m_tag;
    Path m_folder;
    QByteArray m_manifest;
    QList<FileEntry> m_pendingFiles;

    QTimer *m_installTimer = nullptr;
    // set when the helper started this process: "healthy" is reported once it has run well for a while
    QString m_previousEdition;
    QString m_installedEdition;
    QString m_level;
    QString m_notes;
};
