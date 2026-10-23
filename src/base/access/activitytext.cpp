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

#include "activitytext.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include "base/global.h"
#include "permissions.h"

namespace
{
    const int MAX_NAMES = 5;

    const QHash<QString, QString> TORRENT_VERBS
    {
        {u"torrents/start"_s, u"Started"_s},
        {u"torrents/stop"_s, u"Stopped"_s},
        {u"torrents/recheck"_s, u"Rechecked"_s},
        {u"torrents/reannounce"_s, u"Reannounced"_s},
        {u"torrents/increasePrio"_s, u"Moved up in queue:"_s},
        {u"torrents/decreasePrio"_s, u"Moved down in queue:"_s},
        {u"torrents/topPrio"_s, u"Moved to top of queue:"_s},
        {u"torrents/bottomPrio"_s, u"Moved to bottom of queue:"_s},
        {u"torrents/setComment"_s, u"Changed comment of"_s},
        {u"torrents/addTrackers"_s, u"Added trackers to"_s},
        {u"torrents/editTracker"_s, u"Edited a tracker of"_s},
        {u"torrents/removeTrackers"_s, u"Removed trackers from"_s},
        {u"torrents/addPeers"_s, u"Added peers to"_s},
        {u"torrents/addWebSeeds"_s, u"Added web seeds to"_s},
        {u"torrents/editWebSeed"_s, u"Edited a web seed of"_s},
        {u"torrents/removeWebSeeds"_s, u"Removed web seeds from"_s},
        {u"torrents/filePrio"_s, u"Changed file priorities of"_s},
        {u"torrents/setShareLimits"_s, u"Changed share limits of"_s},
        {u"torrents/renameFile"_s, u"Renamed a file in"_s},
        {u"torrents/renameFolder"_s, u"Renamed a folder in"_s},
        {u"torrents/setSSLParameters"_s, u"Changed SSL parameters of"_s},
        {u"torrents/setSavePath"_s, u"Changed save path of"_s},
        {u"torrents/setDownloadPath"_s, u"Changed incomplete-download path of"_s},
        {u"torrents/toggleSequentialDownload"_s, u"Toggled sequential download for"_s},
        {u"torrents/toggleFirstLastPiecePrio"_s, u"Toggled first/last piece priority for"_s},
    };

    const QHash<QString, QString> TOGGLES
    {
        {u"torrents/setForceStart"_s, u"force start"_s},
        {u"torrents/setAutoManagement"_s, u"automatic torrent management"_s},
        {u"torrents/setSuperSeeding"_s, u"super seeding"_s},
    };

    QString quoted(const QString &text)
    {
        return u"'%1'"_s.arg(text);
    }

    QString joinNames(QStringList values)
    {
        values.removeIf([](const QString &value) { return value.trimmed().isEmpty(); });
        if (values.isEmpty())
            return u"(none)"_s;
        if (values.size() > MAX_NAMES)
            return values.first(MAX_NAMES).join(u", "_s) + u" and %1 more"_s.arg(values.size() - MAX_NAMES);
        return values.join(u", "_s);
    }

    QString targets(const QHash<QString, QString> &params, const QHash<QString, QString> &names)
    {
        const QStringList hashes = Access::torrentHashes(params);
        if (hashes == QStringList {u"all"_s})
            return u"all torrents"_s;
        if (hashes.isEmpty())
            return u"(no torrent)"_s;

        QStringList labels;
        for (const QString &hash : hashes)
        {
            const QString name = names.value(hash);
            labels.append(quoted(name.isEmpty() ? (u"torrent "_s + hash.left(8)) : name));
        }
        return joinNames(labels);
    }

    QString speed(const QString &value)
    {
        bool ok = false;
        double number = value.toDouble(&ok);
        if (!ok)
            return value;
        if (number <= 0)
            return u"unlimited"_s;
        if (number < 1024)
            return u"%1 B/s"_s.arg(number);
        const QStringList units {u"KiB/s"_s, u"MiB/s"_s, u"GiB/s"_s};
        for (int i = 0; i < units.size(); ++i)
        {
            number /= 1024;
            if ((number < 1024) || (i == (units.size() - 1)))
                return u"%1 %2"_s.arg(QString::number(number, 'f', 1), units[i]);
        }
        return value;
    }

    // Private trackers and feeds put a personal key in the address ("announce?passkey=...", "/rss/<token>"), and the
    // log can be read by other people and exported. Only the site of an address is kept; a magnet link keeps just
    // its info hash and name.
    QString redactUrl(const QString &address)
    {
        if (address.startsWith(u"magnet:", Qt::CaseInsensitive))
        {
            QString rawQuery = QUrl(address).query(QUrl::FullyEncoded);
            rawQuery.replace(u'+', u"%20"_s);
            const QUrlQuery query {rawQuery};
            QStringList parts;
            for (const QString &key : {u"xt"_s, u"dn"_s})
            {
                if (const QString value = query.queryItemValue(key, QUrl::FullyEncoded); !value.isEmpty())
                    parts.append(key + u"="_s + value);
            }
            return u"magnet:?"_s + parts.join(u'&');
        }

        const QUrl url {address};
        if (url.host().isEmpty())
            return address;
        const bool hasMore = !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
            || (!url.path().isEmpty() && (url.path() != u"/"));
        return url.scheme() + u"://"_s + url.host() + (url.port() > 0 ? (u":"_s + QString::number(url.port())) : QString())
            + (hasMore ? u"/..."_s : QString());
    }

    // redactUrl for every address inside a longer text
    QString redactUrls(const QString &text)
    {
        static const QRegularExpression pattern {u"(?:[A-Za-z][A-Za-z0-9+.-]*://|magnet:\\?)[^\\s|]*"_s};
        QString result;
        qsizetype last = 0;
        for (auto it = pattern.globalMatch(text); it.hasNext();)
        {
            const QRegularExpressionMatch match = it.next();
            result += text.mid(last, (match.capturedStart() - last)) + redactUrl(match.captured());
            last = match.capturedEnd();
        }
        return result + text.mid(last);
    }

    QString torrentSource(const QString &text)
    {
        const QString trimmed = text.trimmed();
        if (trimmed.startsWith(u"magnet:", Qt::CaseInsensitive))
        {
            // In magnet links "+" means a space, as in HTML forms.
            QString rawQuery = QUrl(trimmed).query(QUrl::FullyEncoded);
            rawQuery.replace(u'+', u"%20"_s);
            const QUrlQuery query {rawQuery};
            const QString name = query.queryItemValue(u"dn"_s, QUrl::FullyDecoded);
            if (!name.isEmpty())
                return quoted(name);
            const QString xt = query.queryItemValue(u"xt"_s);
            if (!xt.isEmpty())
                return u"magnet "_s + xt.section(u':', -1).left(12);
            return u"magnet link"_s;
        }
        return redactUrl(trimmed).left(200);
    }

    bool isHiddenPreference(const QString &key)
    {
        return Access::redactedPreferences().contains(key) || key.contains(u"password");
    }

    QString preferenceChanges(const QString &json)
    {
        const QJsonObject changes = QJsonDocument::fromJson(json.toUtf8()).object();
        if (changes.isEmpty())
            return u"nothing"_s;

        QStringList parts;
        for (auto iter = changes.constBegin(); iter != changes.constEnd(); ++iter)
        {
            if (isHiddenPreference(iter.key()))
            {
                parts.append(u"%1 (hidden)"_s.arg(iter.key()));
                continue;
            }
            QString text = QString::fromUtf8(QJsonDocument(QJsonArray {iter.value()}).toJson(QJsonDocument::Compact));
            text = text.mid(1, text.size() - 2);  // strip the surrounding [ ]
            if (text.size() > 60)
                text = text.left(57) + u"..."_s;
            parts.append(u"%1 = %2"_s.arg(iter.key(), text));
        }
        return parts.join(u", "_s);
    }

    QStringList split(const QString &text, const QChar separator)
    {
        return text.split(separator, Qt::SkipEmptyParts);
    }

    QString labelOf(const QList<Access::ActivityName> &names, const QString &id)
    {
        for (const Access::ActivityName &name : names)
        {
            if (name.id == id)
                return name.label;
        }
        return id;
    }

    QString csvCell(QString text)
    {
        // Stop spreadsheet programs from treating a value as a formula.
        if (!text.isEmpty() && QStringView(u"=+-@\t\r").contains(text.at(0)))
            text.prepend(u'\'');
        if (text.contains(u',') || text.contains(u'"') || text.contains(u'\n') || text.contains(u'\r'))
            text = u'"' + text.replace(u'"', u"\"\""_s) + u'"';
        return text;
    }
}

const QList<Access::ActivityName> &Access::activityCategories()
{
    static const QList<ActivityName> list
    {
        {u"torrent"_s, u"Torrents"_s},
        {u"speed"_s, u"Speed limits"_s},
        {u"settings"_s, u"qBittorrent settings"_s},
        {u"rss"_s, u"RSS"_s},
        {u"search"_s, u"Search"_s},
        {u"creator"_s, u"Torrent creator"_s},
        {u"login"_s, u"Logins"_s},
        {u"admin"_s, u"User management"_s},
        {u"security"_s, u"Security"_s},
        {u"other"_s, u"Other"_s},
    };
    return list;
}

const QList<Access::ActivityName> &Access::activityOutcomes()
{
    static const QList<ActivityName> list
    {
        {u"ok"_s, u"Done"_s},
        {u"failed"_s, u"Failed"_s},
        {u"denied"_s, u"Blocked"_s},
    };
    return list;
}

QString Access::activityCategoryLabel(const QString &id)
{
    return labelOf(activityCategories(), id);
}

QString Access::activityOutcomeLabel(const QString &id)
{
    return labelOf(activityOutcomes(), id);
}

QString Access::activityCsvHeader()
{
    return u"Time,User,Role,IP address,Category,Action,Result,API action"_s;
}

QString Access::activityCsvLine(const QJsonObject &entry)
{
    const QString time = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(entry[u"ts"_s].toDouble() * 1000))
        .toString(u"yyyy-MM-dd HH:mm:ss"_s);
    const QStringList cells
    {
        time, entry[u"username"_s].toString(), entry[u"role"_s].toString(), entry[u"ip"_s].toString(),
        activityCategoryLabel(entry[u"category"_s].toString()), entry[u"summary"_s].toString(),
        activityOutcomeLabel(entry[u"outcome"_s].toString()), entry[u"endpoint"_s].toString()
    };
    QStringList escaped;
    for (const QString &cell : cells)
        escaped.append(csvCell(cell));
    return escaped.join(u',');
}

QStringList Access::torrentHashes(const QHash<QString, QString> &params)
{
    QString value = params.value(u"hashes"_s);
    if (value.isEmpty())
        value = params.value(u"hash"_s);
    if (value.trimmed().compare(u"all", Qt::CaseInsensitive) == 0)
        return {u"all"_s};

    QStringList hashes;
    for (const QString &hash : value.split(u'|', Qt::SkipEmptyParts))
    {
        const QString clean = hash.trimmed().toLower();
        if (!clean.isEmpty())
            hashes.append(clean);
    }
    return hashes;
}

QString Access::describe(const QString &endpoint, const QHash<QString, QString> &params
        , const QHash<QString, QString> &names, const QStringList &fileNames)
{
    const auto p = [&params](const QString &key) { return params.value(key); };

    if (endpoint == u"torrents/add")
    {
        QStringList sources;
        for (const QString &url : p(u"urls"_s).split(u'\n', Qt::SkipEmptyParts))
        {
            if (!url.trimmed().isEmpty())
                sources.append(torrentSource(url));
        }
        for (const QString &fileName : fileNames)
            sources.append(quoted(fileName));
        QString text = u"Added torrent: "_s + joinNames(sources);
        if (!p(u"rename"_s).isEmpty())
            text += u" as "_s + quoted(p(u"rename"_s));
        if (!p(u"category"_s).isEmpty())
            text += u" (category %1)"_s.arg(quoted(p(u"category"_s)));
        return text;
    }
    if (endpoint == u"torrents/delete")
    {
        if (!isTrue(p(u"deleteFiles"_s)))
            return u"Deleted %1 (kept files)"_s.arg(targets(params, names));
        return u"Deleted %1 and their files"_s.arg(targets(params, names));
    }
    if (endpoint == u"torrents/rename")
        return u"Renamed %1 to %2"_s.arg(targets(params, names), quoted(p(u"name"_s)));
    if (endpoint == u"torrents/setCategory")
    {
        const QString category = p(u"category"_s);
        return u"Set category of %1 to %2"_s.arg(targets(params, names), category.isEmpty() ? u"none"_s : quoted(category));
    }
    if ((endpoint == u"torrents/addTags") || (endpoint == u"torrents/removeTags") || (endpoint == u"torrents/setTags"))
    {
        const QString verb = (endpoint == u"torrents/addTags") ? u"Added tags"_s
            : (endpoint == u"torrents/removeTags") ? u"Removed tags"_s : u"Set tags"_s;
        const QString preposition = (endpoint == u"torrents/removeTags") ? u"from"_s : u"on"_s;
        return u"%1 %2 %3 %4"_s.arg(verb, quoted(p(u"tags"_s)), preposition, targets(params, names));
    }
    if (endpoint == u"torrents/setLocation")
        return u"Moved %1 to %2"_s.arg(targets(params, names), quoted(p(u"location"_s)));
    if ((endpoint == u"torrents/setUploadLimit") || (endpoint == u"torrents/setDownloadLimit"))
    {
        const QString kind = endpoint.contains(u"Upload") ? u"upload"_s : u"download"_s;
        return u"Set %1 limit of %2 to %3"_s.arg(kind, targets(params, names), speed(p(u"limit"_s)));
    }
    if (TOGGLES.contains(endpoint))
    {
        const QString value = !p(u"value"_s).isEmpty() ? p(u"value"_s) : p(u"enable"_s);
        const QString state = (value.compare(u"true", Qt::CaseInsensitive) == 0) ? u"on"_s : u"off"_s;
        return u"Turned %1 %2 for %3"_s.arg(TOGGLES.value(endpoint), state, targets(params, names));
    }
    if (TORRENT_VERBS.contains(endpoint))
        return u"%1 %2"_s.arg(TORRENT_VERBS.value(endpoint), targets(params, names));
    if (endpoint == u"torrents/createCategory")
        return u"Created category "_s + quoted(p(u"category"_s));
    if (endpoint == u"torrents/editCategory")
    {
        const QString savePath = p(u"savePath"_s);
        return u"Edited category %1 (save path %2)"_s.arg(quoted(p(u"category"_s)), quoted(savePath.isEmpty() ? u"default"_s : savePath));
    }
    if (endpoint == u"torrents/removeCategories")
        return u"Removed categories: "_s + joinNames(split(p(u"categories"_s), u'\n'));
    if (endpoint == u"torrents/createTags")
        return u"Created tags: "_s + joinNames(split(p(u"tags"_s), u','));
    if (endpoint == u"torrents/deleteTags")
        return u"Deleted tags: "_s + joinNames(split(p(u"tags"_s), u','));

    if (endpoint == u"torrents/downloadFile")
        return u"Downloaded file %1 of %2"_s.arg(quoted(p(u"file"_s)), targets(params, names));
    if (endpoint == u"transfer/setDownloadLimit")
        return u"Set global download limit to "_s + speed(p(u"limit"_s));
    if (endpoint == u"transfer/setUploadLimit")
        return u"Set global upload limit to "_s + speed(p(u"limit"_s));
    if (endpoint == u"transfer/setSpeedLimits")
        return u"Changed global speed limits"_s;
    if (endpoint == u"transfer/pauseSession")
        return u"Paused the BitTorrent session"_s;
    if (endpoint == u"transfer/resumeSession")
        return u"Resumed the BitTorrent session"_s;
    if (endpoint == u"transfer/toggleSpeedLimitsMode")
        return u"Toggled alternative speed limits"_s;
    if (endpoint == u"transfer/setSpeedLimitsMode")
        return u"Turned alternative speed limits "_s + ((p(u"mode"_s) == u"1") ? u"on"_s : u"off"_s);
    if (endpoint == u"transfer/banPeers")
        return u"Banned peers: "_s + joinNames(split(p(u"peers"_s), u'|'));

    if (endpoint == u"app/setPreferences")
        return u"Changed settings: "_s + preferenceChanges(p(u"json"_s));
    if (endpoint == u"app/shutdown")
        return u"Shut down qBittorrent"_s;
    if (endpoint == u"app/sendTestEmail")
        return u"Sent a test email"_s;
    if (endpoint == u"app/setCookies")
        return u"Changed stored cookies"_s;
    if (endpoint == u"app/rotateAPIKey")
        return u"Generated a new WebUI API key"_s;
    if (endpoint == u"app/deleteAPIKey")
        return u"Deleted the WebUI API key"_s;

    if (endpoint == u"rss/addFeed")
        return u"Added RSS feed "_s + redactUrls(p(u"url"_s)) + (p(u"path"_s).isEmpty() ? QString() : (u" as "_s + quoted(p(u"path"_s))));
    if (endpoint == u"rss/addFolder")
        return u"Added RSS folder "_s + quoted(p(u"path"_s));
    if (endpoint == u"rss/removeItem")
        return u"Removed RSS item "_s + quoted(p(u"path"_s));
    if (endpoint == u"rss/moveItem")
        return u"Moved RSS item %1 to %2"_s.arg(quoted(p(u"itemPath"_s)), quoted(p(u"destPath"_s)));
    if (endpoint == u"rss/setFeedURL")
        return u"Changed URL of RSS feed %1 to %2"_s.arg(quoted(p(u"path"_s)), redactUrls(p(u"url"_s)));
    if (endpoint == u"rss/setFeedRefreshInterval")
        return u"Changed refresh interval of RSS feed "_s + quoted(p(u"path"_s));
    if (endpoint == u"rss/setRule")
        return u"Saved RSS auto-download rule "_s + quoted(p(u"ruleName"_s));
    if (endpoint == u"rss/renameRule")
        return u"Renamed RSS rule %1 to %2"_s.arg(quoted(p(u"ruleName"_s)), quoted(p(u"newRuleName"_s)));
    if (endpoint == u"rss/cloneRule")
        return u"Copied RSS rule %1 to %2"_s.arg(quoted(p(u"sourceName"_s)), quoted(p(u"cloneName"_s)));
    if (endpoint == u"rss/importRules")
        return u"Imported RSS auto-download rules"_s;
    if (endpoint == u"rss/removeRule")
        return u"Removed RSS rule "_s + quoted(p(u"ruleName"_s));

    if (endpoint == u"search/start")
    {
        const QString category = p(u"category"_s);
        return u"Searched for "_s + quoted(p(u"pattern"_s))
            + ((!category.isEmpty() && (category != u"all")) ? (u" in "_s + category) : QString());
    }
    if (endpoint == u"search/downloadTorrent")
        return u"Added torrent from search: "_s + torrentSource(p(u"url"_s));
    if (endpoint == u"search/installPlugin")
        return u"Installed search plugins: "_s + joinNames(split(p(u"sources"_s), u'|'));
    if (endpoint == u"search/uninstallPlugin")
        return u"Removed search plugins: "_s + joinNames(split(p(u"names"_s), u'|'));
    if (endpoint == u"search/enablePlugin")
    {
        const QString state = (p(u"enable"_s).compare(u"true", Qt::CaseInsensitive) == 0) ? u"Enabled"_s : u"Disabled"_s;
        return u"%1 search plugins: %2"_s.arg(state, joinNames(split(p(u"names"_s), u'|')));
    }
    if (endpoint == u"search/updatePlugins")
        return u"Updated search plugins"_s;

    if (endpoint == u"torrentcreator/addTask")
        return u"Started creating a torrent from "_s + quoted(p(u"sourcePath"_s));
    if (endpoint == u"torrentcreator/deleteTask")
        return u"Deleted a torrent creator task"_s;

    return u"Used API action "_s + endpoint;
}

QJsonObject Access::safeDetails(const QString &endpoint, const QHash<QString, QString> &params)
{
    QJsonObject clean;
    for (auto iter = params.cbegin(); iter != params.cend(); ++iter)
    {
        const QString &key = iter.key();
        const QString &value = iter.value();
        if (key.contains(u"password", Qt::CaseInsensitive) || (key == u"cookie") || (key == u"cookies")
            || (key == u"ssl_private_key"))
            clean.insert(key, u"(hidden)"_s);
        else if ((endpoint == u"app/setPreferences") && (key == u"json"))
            clean.insert(key, preferenceChanges(value));
        else
        {
            const QString text = redactUrls(value);
            clean.insert(key, (text.size() <= 500) ? text : (text.left(497) + u"..."_s));
        }
    }
    return {{u"endpoint"_s, endpoint}, {u"params"_s, clean}};
}
