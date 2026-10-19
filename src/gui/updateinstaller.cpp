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

#include "updateinstaller.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QWidget>

#include "base/bittorrent/session.h"
#include "base/bittorrent/torrent.h"
#include "base/global.h"
#include "base/logger.h"
#include "base/net/downloadmanager.h"
#include "base/preferences.h"
#include "base/profile.h"
#include "base/utils/fs.h"
#include "base/utils/version.h"
#include "base/version.h"
#include "programupdater.h"

namespace
{
    // An update is a few dozen MB. These limits only stop a wrong release info from filling the disk.
    const qint64 MAX_FILE_SIZE = 200 * 1024 * 1024;
    const qint64 MAX_TOTAL_SIZE = 400 * 1024 * 1024;

    const QString READY_FILE_NAME = u"ready.json"_s;

    bool isSafeRelativePath(const QString &path)
    {
        // "platforms/qwindows.dll": plain names only, so a path can't point outside the program folder
        const QRegularExpression segmentPattern {u"^[A-Za-z0-9_][A-Za-z0-9._-]*$"_s};
        const QStringList segments = path.split(u'/');
        return !segments.isEmpty() && std::ranges::all_of(segments, [&segmentPattern](const QString &segment)
        {
            return segmentPattern.match(segment).hasMatch();
        });
    }

    bool isSafeFileName(const QString &name)
    {
        return QRegularExpression(u"^[A-Za-z0-9_][A-Za-z0-9._-]*$"_s).match(name).hasMatch();
    }

    bool isSha256(const QString &value)
    {
        return QRegularExpression(u"^[0-9a-f]{64}$"_s).match(value).hasMatch();
    }

    // SHA-256 of a file as lowercase hex, empty if the file can't be read
    QString fileSha256(const QString &filePath)
    {
        QFile file {filePath};
        if (!file.open(QIODevice::ReadOnly))
            return {};

        QCryptographicHash hash {QCryptographicHash::Sha256};
        if (!hash.addData(&file))
            return {};

        return QString::fromLatin1(hash.result().toHex());
    }

    Path updateRootFolder()
    {
        return specialFolderLocation(SpecialFolder::Cache) / Path(u"kettle-update"_s);
    }

    // The helper runs from a copy in its own folder, so that it can replace the installed helper too
    const QString HELPER_FOLDER_NAME = u"helper"_s;
    const QString HELPER_FILE_NAME = u"kettle-update-helper.exe"_s;
    const char STATE_ENV_NAME[] = "KETTLE_UPDATE_STATE";  // set by the helper for the program it starts

    const QTime DEFAULT_INSTALL_TIME {4, 0};  // a test can move it with KETTLE_UPDATE_INSTALL_AT=HH:mm:ss
    const int INSTALL_WINDOW_HOURS = 2;  // an update that is not installed by 6:00 AM waits for the next night
    const int INSTALL_RETRY_MINUTES = 30;  // while a torrent is being checked or moved
    const int HEALTHY_SECONDS = 120;  // the new version has to run this long before it counts as working
    const int HEALTHY_TIMEOUT_SECONDS = 300;  // the helper waits this long for that

    QString currentEdition()
    {
        return QStringLiteral(QBT_EDITION_VERSION);
    }

    // The test settings (see ProgramUpdater::isTestMode) can also shorten the waiting times
    int secondsSetting(const char *name, const int defaultValue)
    {
        if (!ProgramUpdater::isTestMode())
            return defaultValue;

        bool ok = false;
        const int value = qEnvironmentVariable(name).toInt(&ok);
        return (ok && (value > 0)) ? value : defaultValue;
    }

    QTime installTime()
    {
        if (ProgramUpdater::isTestMode())
        {
            const QTime time = QTime::fromString(qEnvironmentVariable("KETTLE_UPDATE_INSTALL_AT"), u"HH:mm:ss"_s);
            if (time.isValid())
                return time;
        }
        return DEFAULT_INSTALL_TIME;
    }

    QStringList readTextLines(const Path &path)
    {
        QFile file {path.data()};
        if (!file.open(QIODevice::ReadOnly))
            return {};

        QStringList lines;
        for (const QString &line : QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts))
            lines.append(line.trimmed());
        return lines;
    }

    // "key=value" lines of the state and result files; for a repeated key the last one counts
    QHash<QString, QString> readKeyValues(const Path &path)
    {
        QHash<QString, QString> values;
        for (const QString &line : readTextLines(path))
        {
            const qsizetype equals = line.indexOf(u'=');
            if (equals > 0)
                values.insert(line.left(equals), line.mid(equals + 1));
        }
        return values;
    }

    bool writeTextFile(const Path &path, const QByteArray &content)
    {
        QSaveFile file {path.data()};
        return Utils::Fs::mkpath(path.parentPath()) && file.open(QIODevice::WriteOnly)
                && (file.write(content) == content.size()) && file.commit();
    }

    // versions that failed to install: they are not offered again, which also stops a failing update from being tried every night
    bool isSkipped(const QString &edition)
    {
        return readTextLines(updateRootFolder() / Path(u"skipped.txt"_s)).contains(edition);
    }

    void markSkipped(const QString &edition)
    {
        QFile file {(updateRootFolder() / Path(u"skipped.txt"_s)).data()};
        if (Utils::Fs::mkpath(updateRootFolder()) && file.open(QIODevice::WriteOnly | QIODevice::Append))
            file.write((edition + u'\n').toUtf8());
    }

    // a download that is complete and checked (see UpdateInstaller::finish)
    struct ReadyUpdate
    {
        struct File
        {
            QString path;
            qint64 size = 0;
            QString sha256;
        };

        QString edition;
        QString level;
        QString notes;
        Path folder;
        QList<File> files;
    };

    // the newest checked download that is newer than the running program and not skipped
    std::optional<ReadyUpdate> findReadyUpdate()
    {
        const Utils::Version<3> running {currentEdition()};
        std::optional<ReadyUpdate> best;
        const QDir rootDir {updateRootFolder().data()};
        for (const QString &name : rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        {
            if (name == HELPER_FOLDER_NAME)
                continue;

            const Path folder = updateRootFolder() / Path(name);
            QFile readyFile {(folder / Path(READY_FILE_NAME)).data()};
            if (!readyFile.open(QIODevice::ReadOnly))
                continue;

            const QJsonObject json = QJsonDocument::fromJson(readyFile.readAll()).object();
            const Utils::Version<3> edition {json[u"edition"_s].toString()};
            if (!edition.isValid() || !(edition > running) || isSkipped(edition.toString()))
                continue;

            ReadyUpdate update;
            update.edition = edition.toString();
            update.level = json[u"level"_s].toString();
            update.notes = json[u"notes"_s].toString();
            update.folder = folder;
            bool isComplete = true;
            for (const QJsonValue &value : json[u"files"_s].toArray())
            {
                const QJsonObject item = value.toObject();
                const ReadyUpdate::File file {item[u"path"_s].toString(), static_cast<qint64>(item[u"size"_s].toDouble(-1))
                    , item[u"sha256"_s].toString()};
                isComplete = isComplete && isSafeRelativePath(file.path) && isSha256(file.sha256)
                        && (QFileInfo((folder / Path(file.path)).data()).size() == file.size);
                update.files.append(file);
            }
            if (!isComplete || update.files.isEmpty())
                continue;

            if (!best || (edition > Utils::Version<3>(best->edition)))
                best = update;
        }
        return best;
    }

    // Writes the state file and starts the helper. Returns an empty string on success, else the reason it failed.
    QString startHelper(const ReadyUpdate &update, const QString &windowState)
    {
        const QString programFolder = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
        const QString helperSource = programFolder + u'\\' + HELPER_FILE_NAME;
        if (!QFile::exists(helperSource))
            return UpdateInstaller::tr("%1 is missing").arg(HELPER_FILE_NAME);

        // the helper runs as the user who runs qBittorrent, so that user must be allowed to change the program folder
        QFile probe {programFolder + u"\\kettle-update-write-test.tmp"};
        if (!probe.open(QIODevice::WriteOnly))
            return UpdateInstaller::tr("no permission to change the files in %1").arg(programFolder);
        probe.close();
        probe.remove();

        const Path root = updateRootFolder();
        const Path helperFolder = root / Path(HELPER_FOLDER_NAME);
        Utils::Fs::removeDirRecursively(helperFolder);
        const Path helperCopy = helperFolder / Path(HELPER_FILE_NAME);
        if (!Utils::Fs::mkpath(helperFolder) || !QFile::copy(helperSource, helperCopy.data()))
            return UpdateInstaller::tr("%1 could not be copied").arg(HELPER_FILE_NAME);

        const Path dataFolder = specialFolderLocation(SpecialFolder::Data);
        const Path configFolder = specialFolderLocation(SpecialFolder::Config);
        QStringList lines {
            u"pid=%1"_s.arg(QCoreApplication::applicationPid())
            , u"old=%1"_s.arg(currentEdition())
            , u"new=%1"_s.arg(update.edition)
            , u"level=%1"_s.arg(update.level)
            , u"notes=%1"_s.arg(QString(update.notes).replace(u'\n', u' '))
            , u"program=%1"_s.arg(programFolder)
            , u"exe=%1"_s.arg(QFileInfo(QCoreApplication::applicationFilePath()).fileName())
            , u"update=%1"_s.arg(update.folder.toString())
            , u"root=%1"_s.arg(root.toString())
            , u"window=%1"_s.arg(windowState)
            , u"healthy_timeout=%1"_s.arg(secondsSetting("KETTLE_UPDATE_HEALTHY_TIMEOUT", HEALTHY_TIMEOUT_SECONDS))
            , u"previous=%1"_s.arg((dataFolder / Path(u"kettle-previous"_s)).toString())
            , u"backup=%1-update-backup"_s.arg(dataFolder.toString())
            , u"log=%1"_s.arg((dataFolder / Path(u"update.log"_s)).toString())
            , u"profile=%1"_s.arg(configFolder.toString())
        };
        if (dataFolder != configFolder)
            lines.append(u"profile=%1"_s.arg(dataFolder.toString()));
        // not the cache (it holds this very update), the logs, or the helper's own files
        for (const QString &name : {u"cache"_s, u"logs"_s, u"kettle-previous"_s, u"update.log"_s})
            lines.append(u"exclude=%1"_s.arg(name));

        // only what decides where the profile is; a torrent that was opened with the first start is not opened again
        const QStringList arguments = QCoreApplication::arguments().mid(1);
        for (const QString &argument : arguments)
        {
            if (argument.startsWith(u"--profile="_s) || argument.startsWith(u"--configuration="_s)
                    || argument.startsWith(u"--webui-port="_s) || (argument == u"--confirm-legal-notice"_s))
                lines.append(u"arg=%1"_s.arg(argument));
        }
        for (const ReadyUpdate::File &file : update.files)
            lines.append(u"file=%1|%2|%3"_s.arg(file.path).arg(file.size).arg(file.sha256));

        const Path statePath = root / Path(u"state.txt"_s);
        if (!writeTextFile(statePath, (lines.join(u"\r\n"_s) + u"\r\n"_s).toUtf8()))
            return UpdateInstaller::tr("the state file could not be written");

        if (!QProcess::startDetached(helperCopy.toString(), {statePath.toString()}))
            return UpdateInstaller::tr("%1 could not be started").arg(HELPER_FILE_NAME);

        return {};
    }

    // true if the helper is now installing the update, so that qBittorrent has to exit
    bool handOver(const ReadyUpdate &update, const QString &windowState)
    {
        LogMsg(UpdateInstaller::tr("Installing Kettle Edition %1. qBittorrent restarts now.").arg(update.edition), Log::INFO);
        const QString error = startHelper(update, windowState);
        if (!error.isEmpty())
        {
            LogMsg(UpdateInstaller::tr("Failed to install Kettle Edition %1. Error: \"%2\"").arg(update.edition, error), Log::WARNING);
            return false;
        }
        return true;
    }
}

void UpdateInstaller::start()
{
    connect(this, &UpdateInstaller::updateReady, this, &UpdateInstaller::scheduleInstall);

    const QString statePath = qEnvironmentVariable(STATE_ENV_NAME);
    if (!statePath.isEmpty() && QFile::exists(statePath))
        startHealthyCheck(statePath);
    else
        Utils::Fs::removeDirRecursively(updateRootFolder() / Path(HELPER_FOLDER_NAME));  // the copy of an earlier update

    showLastResult();

    // an update that was downloaded earlier and could not be installed yet
    if (findReadyUpdate())
        scheduleInstall();
}

QString UpdateInstaller::restoredWindowState()
{
    const QString statePath = qEnvironmentVariable(STATE_ENV_NAME);
    return statePath.isEmpty() ? QString() : readKeyValues(Path(statePath)).value(u"window"_s);
}

bool UpdateInstaller::installPendingUpdate(const QString &windowState)
{
    // a program that the helper has just started does not hand over again before it has been checked
    if (!Preferences::instance()->isUpdateCheckEnabled() || qEnvironmentVariableIsSet(STATE_ENV_NAME))
        return false;

    const std::optional<ReadyUpdate> update = findReadyUpdate();
    return update && handOver(*update, windowState);
}

void UpdateInstaller::showLastResult()
{
    const Path resultPath = updateRootFolder() / Path(u"result.txt"_s);
    if (!resultPath.exists())
        return;

    const QHash<QString, QString> result = readKeyValues(resultPath);
    Utils::Fs::removeFile(resultPath);

    const QString edition = result.value(u"edition"_s);
    const QString reason = result.value(u"reason"_s);
    if (result.value(u"status"_s) == u"discarded")
    {
        LogMsg(tr("Kettle Edition %1 was not installed: %2. It is downloaded again later.").arg(edition, reason), Log::WARNING);
        return;
    }

    const QString message = tr("Update to Kettle Edition %1 failed and was undone: %2.").arg(edition, reason);
    LogMsg(message, Log::WARNING);
    QTimer::singleShot(0, this, [this, message]()
    {
        auto *msgBox = new QMessageBox {QMessageBox::Warning, u"qBittorrent"_s
            , message + u"\n\n" + tr("The previous version is running again, with your settings as they were before the update.")
            , QMessageBox::Ok, qobject_cast<QWidget *>(parent())};
        msgBox->setAttribute(Qt::WA_DeleteOnClose);
        msgBox->setWindowModality(Qt::NonModal);
        msgBox->show();
    });
}

void UpdateInstaller::startHealthyCheck(const QString &statePath)
{
    const QHash<QString, QString> state = readKeyValues(Path(statePath));
    m_previousEdition = state.value(u"old"_s);
    m_installedEdition = state.value(u"new"_s);
    m_level = state.value(u"level"_s);
    m_notes = state.value(u"notes"_s);

    // Quitting on purpose before the check is over is no failure; the helper is told so. By the time aboutToQuit
    // reaches this, the cleanup of the application has already removed this object and the profile, so only the
    // application and a path worked out now are used.
    const QString cleanExitPath = (updateRootFolder() / Path(u"clean-exit"_s)).data();
    connect(qApp, &QCoreApplication::aboutToQuit, qApp, [cleanExitPath]()
    {
        QFile file {cleanExitPath};
        if (file.open(QIODevice::WriteOnly))
            file.write("quit");
    });
    QTimer::singleShot(secondsSetting("KETTLE_UPDATE_HEALTHY_SECONDS", HEALTHY_SECONDS) * 1000, this, &UpdateInstaller::markHealthy);
}

void UpdateInstaller::markHealthy()
{
    writeTextFile(updateRootFolder() / Path(u"healthy"_s), m_installedEdition.toUtf8());

    // The release info promised one number and the program reports another: it would be offered again every night
    if (currentEdition() != m_installedEdition)
    {
        markSkipped(m_installedEdition);
        LogMsg(tr("Kettle Edition %1 was installed but reports itself as %2. It will not be offered again.")
                .arg(m_installedEdition, currentEdition()), Log::WARNING);
        return;
    }

    const QString level = m_level.isEmpty() ? QString() : (u" (%1)"_s.arg(m_level));
    const QString message = tr("Updated Kettle Edition %1 → %2%3").arg(m_previousEdition, m_installedEdition, level)
            + (m_notes.isEmpty() ? QString() : (u": "_s + m_notes));
    LogMsg(message, Log::INFO);

    // a big change is the one you want to notice
    if (m_level == u"major")
    {
        auto *msgBox = new QMessageBox {QMessageBox::Information, u"qBittorrent"_s, message, QMessageBox::Ok, qobject_cast<QWidget *>(parent())};
        msgBox->setAttribute(Qt::WA_DeleteOnClose);
        msgBox->setWindowModality(Qt::NonModal);
        msgBox->show();
    }
}

QString UpdateInstaller::currentWindowState() const
{
    const auto *window = qobject_cast<const QWidget *>(parent());
    if (!window || window->isHidden())
        return u"hidden"_s;
    return window->isMinimized() ? u"minimized"_s : u"normal"_s;
}

// The next 4:00 AM, worked out from the clock each time, so that sleep and clock changes don't matter.
// Inside the two hours after 4:00 the install follows in a few seconds.
void UpdateInstaller::scheduleInstall()
{
    if (!m_installTimer)
    {
        m_installTimer = new QTimer(this);
        m_installTimer->setSingleShot(true);
        connect(m_installTimer, &QTimer::timeout, this, &UpdateInstaller::attemptInstall);
    }

    const QDateTime now = QDateTime::currentDateTime();
    QDateTime windowStart {now.date(), installTime()};
    if (now >= windowStart.addSecs(INSTALL_WINDOW_HOURS * 3600))
        windowStart = windowStart.addDays(1);

    const qint64 delay = (now >= windowStart) ? 10000 : now.msecsTo(windowStart);
    m_installTimer->start(static_cast<int>(std::min<qint64>(delay, std::numeric_limits<int>::max())));
}

void UpdateInstaller::attemptInstall()
{
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime windowStart {now.date(), installTime()};
    if ((now < windowStart) || (now >= windowStart.addSecs(INSTALL_WINDOW_HOURS * 3600)))
    {
        // the PC woke up after the window: wait for the next one
        scheduleInstall();
        return;
    }

    const auto *session = BitTorrent::Session::instance();
    const bool isBusy = m_busy || (session && std::ranges::any_of(session->torrents(), [](const BitTorrent::Torrent *torrent)
    {
        return torrent->isChecking() || torrent->isMoving();
    }));
    if (isBusy)
    {
        m_installTimer->start(INSTALL_RETRY_MINUTES * 60 * 1000);
        return;
    }

    const std::optional<ReadyUpdate> update = findReadyUpdate();
    if (update && handOver(*update, currentWindowState()))
        QCoreApplication::exit();
}

void UpdateInstaller::download(const QByteArray &manifest)
{
    if (m_busy)
        return;

    const QJsonObject json = QJsonDocument::fromJson(manifest).object();
    const Utils::Version<3> edition {json[u"edition"_s].toString()};
    const QString tag = json[u"tag"_s].toString();
    if (!edition.isValid() || !isSafeFileName(tag))
    {
        LogMsg(tr("Failed to download the Kettle Edition update. Error: \"%1\"").arg(tr("The release info is not valid")), Log::WARNING);
        return;
    }

    QList<FileEntry> files;
    qint64 totalSize = 0;
    for (const QJsonValue &value : json[u"files"_s].toArray())
    {
        const QJsonObject item = value.toObject();
        const FileEntry file {item[u"path"_s].toString(), item[u"name"_s].toString()
            , static_cast<qint64>(item[u"size"_s].toDouble(-1)), item[u"sha256"_s].toString()};
        if (!isSafeRelativePath(file.path) || !isSafeFileName(file.name) || !isSha256(file.sha256)
                || (file.size <= 0) || (file.size > MAX_FILE_SIZE))
        {
            LogMsg(tr("Failed to download the Kettle Edition update. Error: \"%1\"").arg(tr("The release info is not valid")), Log::WARNING);
            return;
        }
        totalSize += file.size;
        files.append(file);
    }
    if (files.isEmpty() || (totalSize > MAX_TOTAL_SIZE))
    {
        LogMsg(tr("Failed to download the Kettle Edition update. Error: \"%1\"").arg(tr("The release info is not valid")), Log::WARNING);
        return;
    }

    if (isSkipped(edition.toString()))
    {
        LogMsg(tr("Kettle Edition %1 is not downloaded: installing it failed earlier.").arg(edition.toString()), Log::INFO);
        return;
    }

    m_edition = edition.toString();
    m_tag = tag;
    m_folder = updateRootFolder() / Path(m_edition);

    // an older or unfinished download of another version is of no use any more
    const QDir rootDir {updateRootFolder().data()};
    for (const QString &name : rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        if ((name != m_edition) && (name != HELPER_FOLDER_NAME))
            Utils::Fs::removeDirRecursively(updateRootFolder() / Path(name));
    }
    // "ready" is only claimed again after every file has been checked
    Utils::Fs::removeFile(m_folder / Path(READY_FILE_NAME));

    // only the files that are not installed yet and not downloaded yet
    const QString programFolder = QCoreApplication::applicationDirPath();
    m_manifest = manifest;
    m_pendingFiles.clear();
    for (const FileEntry &file : std::as_const(files))
    {
        if (fileSha256(programFolder + u'/' + file.path) == file.sha256)
            continue;

        if (fileSha256((m_folder / Path(file.path)).data()) != file.sha256)
            m_pendingFiles.append(file);
    }

    m_busy = true;
    if (!m_pendingFiles.isEmpty())
        LogMsg(tr("Downloading Kettle Edition %1 (%2 changed files).").arg(m_edition).arg(m_pendingFiles.size()), Log::INFO);
    downloadNext();
}

bool UpdateInstaller::isBusy() const
{
    return m_busy;
}

void UpdateInstaller::downloadNext()
{
    if (m_pendingFiles.isEmpty())
    {
        finish();
        return;
    }

    const FileEntry &file = m_pendingFiles.first();
    Net::DownloadManager::instance()->download(Net::DownloadRequest(ProgramUpdater::editionFileUrl(m_tag, file.name))
            , Preferences::instance()->useProxyForGeneralPurposes(), this
            , [this](const Net::DownloadResult &result) { handleFileDownloaded(result); });
}

void UpdateInstaller::handleFileDownloaded(const Net::DownloadResult &result)
{
    const FileEntry file = m_pendingFiles.first();

    if (result.status != Net::DownloadStatus::Success)
    {
        fail(tr("Failed to download \"%1\". Error: \"%2\"").arg(file.name, result.errorString));
        return;
    }

    // Everything is checked in memory first, so a file that is not what the signed release info lists never reaches the disk
    if (result.data.size() != file.size)
    {
        fail(tr("\"%1\" does not have the expected size.").arg(file.name));
        return;
    }
    if (QString::fromLatin1(QCryptographicHash::hash(result.data, QCryptographicHash::Sha256).toHex()) != file.sha256)
    {
        fail(tr("\"%1\" does not match its checksum.").arg(file.name));
        return;
    }

    const Path destination = m_folder / Path(file.path);
    QSaveFile saveFile {destination.data()};
    if (!Utils::Fs::mkpath(destination.parentPath()) || !saveFile.open(QIODevice::WriteOnly)
            || (saveFile.write(result.data) != result.data.size()) || !saveFile.commit())
    {
        fail(tr("Failed to save \"%1\". Error: \"%2\"").arg(file.name, saveFile.errorString()));
        return;
    }

    m_pendingFiles.removeFirst();
    downloadNext();
}

void UpdateInstaller::fail(const QString &reason)
{
    LogMsg(tr("Failed to download Kettle Edition %1. %2 The update is discarded and checked again later.")
            .arg(m_edition, reason), Log::WARNING);
    m_pendingFiles.clear();
    m_busy = false;
}

void UpdateInstaller::finish()
{
    // Every file in the folder is checked again, whether it was downloaded now or earlier
    const QJsonObject json = QJsonDocument::fromJson(m_manifest).object();
    QJsonArray files;
    for (const QJsonValue &value : json[u"files"_s].toArray())
    {
        const QJsonObject item = value.toObject();
        const QString path = item[u"path"_s].toString();
        const QString sha256 = item[u"sha256"_s].toString();
        const QString downloaded = (m_folder / Path(path)).data();
        if (QFile::exists(downloaded))
        {
            if (fileSha256(downloaded) != sha256)
            {
                fail(tr("\"%1\" does not match its checksum.").arg(path));
                return;
            }
            files.append(QJsonObject {{u"path"_s, path}, {u"size"_s, item[u"size"_s]}, {u"sha256"_s, sha256}});
        }
    }

    const QJsonObject ready {{u"edition"_s, m_edition}, {u"tag"_s, m_tag}, {u"level"_s, json[u"level"_s].toString()}
        , {u"notes"_s, json[u"notes"_s].toString()}, {u"files"_s, files}};
    QSaveFile readyFile {(m_folder / Path(READY_FILE_NAME)).data()};
    if (!Utils::Fs::mkpath(m_folder) || !readyFile.open(QIODevice::WriteOnly)
            || (readyFile.write(QJsonDocument(ready).toJson()) < 0) || !readyFile.commit())
    {
        fail(tr("Failed to save \"%1\". Error: \"%2\"").arg(READY_FILE_NAME, readyFile.errorString()));
        return;
    }

    m_busy = false;
    LogMsg(tr("Kettle Edition %1 is downloaded and ready to install.").arg(m_edition), Log::INFO);
    emit updateReady(m_edition, m_folder.data());
}
