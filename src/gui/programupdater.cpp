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

#include "programupdater.h"

#include <algorithm>

#include <QtSystemDetection>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

#include "base/global.h"
#include "base/logger.h"
#include "base/net/downloadmanager.h"
#include "base/preferences.h"
#include "base/utils/signature.h"
#include "base/version.h"

namespace
{
    bool isVersionMoreRecent(const ProgramUpdater::Version &remoteVersion)
    {
        if (!remoteVersion.isValid())
            return false;

        const ProgramUpdater::Version currentVersion {QBT_VERSION_MAJOR, QBT_VERSION_MINOR, QBT_VERSION_BUGFIX, QBT_VERSION_BUILD};
        if (remoteVersion == currentVersion)
        {
            const bool isDevVersion = QStringLiteral(QBT_VERSION_STATUS).contains(
                QRegularExpression(u"(alpha|beta|rc)"_s));
            if (isDevVersion)
                return true;
        }
        return (remoteVersion > currentVersion);
    }

    ProgramUpdater::EditionVersion currentEditionVersion()
    {
        return {QBT_EDITION_VERSION_MAJOR, QBT_EDITION_VERSION_MINOR, QBT_EDITION_VERSION_PATCH};
    }

    // The test suite points the updater at a local server and at its own key with environment variables.
    // They are ignored in the Program Files folders, so they cannot redirect an installed copy.
    bool areTestSettingsAllowed()
    {
        const QString appFolder = QDir::fromNativeSeparators(QCoreApplication::applicationDirPath());
        return !appFolder.contains(u"/Program Files"_s, Qt::CaseInsensitive);
    }

    bool areTestSettingsUsed()
    {
        return areTestSettingsAllowed() && qEnvironmentVariableIsSet("KETTLE_UPDATE_URL");
    }

    // Folder of the Kettle Edition release files (kettle-update.json and its signature)
    QString editionBaseUrl()
    {
        if (areTestSettingsUsed())
            return qEnvironmentVariable("KETTLE_UPDATE_URL");

        return u"https://github.com/Spyhawk00/qbittorrent-kettle-edition/releases/latest/download/"_s;
    }

    // Public half of the key that signs Kettle Edition releases; publish-update.ps1 signs with the private half
    QByteArray editionPublicKey()
    {
        if (areTestSettingsUsed() && qEnvironmentVariableIsSet("KETTLE_UPDATE_PUBLIC_KEY"))
            return QByteArray::fromHex(qEnvironmentVariable("KETTLE_UPDATE_PUBLIC_KEY").toLatin1());

        return QByteArray::fromHex("1ae98e63ba000613a1539de878be6fce49a908857623c9cbf4cfe495a1f95fae");
    }
}

bool ProgramUpdater::isTestMode()
{
    return areTestSettingsUsed();
}

QString ProgramUpdater::editionFileUrl(const QString &tag, const QString &fileName)
{
    // GitHub's "latest" address only serves the two small release info files; the program files come from the tag named in them
    if (areTestSettingsUsed())
        return editionBaseUrl() + u"download/"_s + tag + u'/' + fileName;

    return u"https://github.com/Spyhawk00/qbittorrent-kettle-edition/releases/download/"_s + tag + u'/' + fileName;
}

void ProgramUpdater::checkForUpdates()
{
    const auto EDITION_MANIFEST_URL = editionBaseUrl() + u"kettle-update.json"_s;
    const auto EDITION_SIGNATURE_URL = editionBaseUrl() + u"kettle-update.json.sig"_s;
    const auto QBT_MAIN_URL = areTestSettingsUsed() ? (editionBaseUrl() + u"versions.json"_s) : u"https://www.qbittorrent.org/versions.json"_s;
    const auto QBT_BACKUP_URL = areTestSettingsUsed() ? (editionBaseUrl() + u"versions.json"_s) : u"https://qbittorrent.github.io/qBittorrent-website/versions.json"_s;

    Net::DownloadManager *netManager = Net::DownloadManager::instance();
    const bool useProxy = Preferences::instance()->useProxyForGeneralPurposes();

    m_pendingRequestCount = 4;
    netManager->download(Net::DownloadRequest(EDITION_MANIFEST_URL), useProxy, this, [this](const Net::DownloadResult &result)
    {
        editionDownloadFinished(result, m_manifestData);
    });
    netManager->download(Net::DownloadRequest(EDITION_SIGNATURE_URL), useProxy, this, [this](const Net::DownloadResult &result)
    {
        editionDownloadFinished(result, m_manifestSignature);
    });
    // don't use the custom user agent for the following requests, disguise as a normal browser instead
    netManager->download(Net::DownloadRequest(QBT_MAIN_URL), useProxy, this, [this](const Net::DownloadResult &result)
    {
        fallbackDownloadFinished(result, m_qbtMainVersion);
    });
    netManager->download(Net::DownloadRequest(QBT_BACKUP_URL), useProxy, this, [this](const Net::DownloadResult &result)
    {
        fallbackDownloadFinished(result, m_qbtBackupVersion);
    });
}

ProgramUpdater::Version ProgramUpdater::getNewVersion() const
{
    switch (getLatestRemoteSource())
    {
    case RemoteSource::QbtMain:
        return m_qbtMainVersion;
    case RemoteSource::QbtBackup:
        return m_qbtBackupVersion;
    }
    Q_UNREACHABLE();
}

ProgramUpdater::EditionVersion ProgramUpdater::getNewEditionVersion() const
{
    return m_newEdition;
}

QString ProgramUpdater::getNewEditionLevel() const
{
    return m_newEditionLevel;
}

QString ProgramUpdater::getNewEditionNotes() const
{
    return m_newEditionNotes;
}

QByteArray ProgramUpdater::getNewEditionManifest() const
{
    return m_newEdition.isValid() ? m_manifestData : QByteArray();
}

void ProgramUpdater::editionDownloadFinished(const Net::DownloadResult &result, QByteArray &data)
{
    data.clear();

    if (result.status != Net::DownloadStatus::Success)
    {
        LogMsg(tr("Failed to download the program update info. URL: \"%1\". Error: \"%2\"").arg(result.url, result.errorString) , Log::WARNING);
        handleFinishedRequest();
        return;
    }

    // The release files are small. Anything bigger is not what was asked for.
    // (The size limit of DownloadRequest is not used: it can report a download as finished more than once.)
    const qsizetype maxFileSize = 1024 * 1024;
    if (result.data.size() > maxFileSize)
    {
        LogMsg(tr("Failed to download the program update info. URL: \"%1\". Error: \"%2\"")
            .arg(result.url, tr("The file is larger than expected")), Log::WARNING);
        handleFinishedRequest();
        return;
    }

    data = result.data;
    handleFinishedRequest();
}

void ProgramUpdater::fallbackDownloadFinished(const Net::DownloadResult &result, Version &version)
{
    version = {};

    if (result.status != Net::DownloadStatus::Success)
    {
        LogMsg(tr("Failed to download the program update info. URL: \"%1\". Error: \"%2\"").arg(result.url, result.errorString) , Log::WARNING);
        handleFinishedRequest();
        return;
    }

    const auto json = QJsonDocument::fromJson(result.data);

#if defined(Q_OS_MACOS)
    const QString platformKey = u"macos"_s;
#elif defined(Q_OS_WIN)
    const QString platformKey = u"win"_s;
#endif

    if (const QJsonValue verJSON = json[platformKey][u"version"_s]; verJSON.isString())
    {
        const Version tmpVer {verJSON.toString()};
        if (isVersionMoreRecent(tmpVer))
            version = tmpVer;
    }

    handleFinishedRequest();
}

void ProgramUpdater::handleFinishedRequest()
{
    --m_pendingRequestCount;
    if (m_pendingRequestCount == 0)
    {
        processEditionManifest();
        emit updateCheckFinished();
    }
}

void ProgramUpdater::processEditionManifest()
{
    m_newEdition = {};
    m_newEditionLevel.clear();
    m_newEditionNotes.clear();

    // nothing to check when the release files could not be downloaded, which was logged already
    if (m_manifestData.isEmpty() || m_manifestSignature.isEmpty())
        return;

    if (!Utils::Signature::verifyEd25519(m_manifestData, m_manifestSignature, editionPublicKey()))
    {
        LogMsg(tr("Ignored the Kettle Edition update info because its signature is not valid."), Log::WARNING);
        return;
    }

    const QJsonObject json = QJsonDocument::fromJson(m_manifestData).object();
    const EditionVersion edition {json[u"edition"_s].toString()};
    if (edition > currentEditionVersion())
    {
        m_newEdition = edition;
        m_newEditionLevel = json[u"level"_s].toString();
        m_newEditionNotes = json[u"notes"_s].toString();
    }
}

ProgramUpdater::RemoteSource ProgramUpdater::getLatestRemoteSource() const
{
    const Version max = std::max({m_qbtMainVersion, m_qbtBackupVersion});
    if (max == m_qbtMainVersion)
        return RemoteSource::QbtMain;
    if (max == m_qbtBackupVersion)
        return RemoteSource::QbtBackup;
    Q_UNREACHABLE();
}
