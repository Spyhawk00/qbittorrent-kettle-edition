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

#include "permissions.h"

#include <QJsonDocument>
#include <QJsonObject>

#include "base/path.h"

namespace
{
    using Access::EndpointRule;

    const QString CATEGORY_TORRENT = u"torrent"_s;

    // A new file or folder path that points outside the torrent's own folder ("../x", "C:x").
    bool leavesTorrentFolder(const QString &value)
    {
        const Path path {value};
        const QString text = path.data();
        return path.isAbsolute() || (text == u"..") || text.startsWith(u"../") || text.contains(u':');
    }

    // An RSS auto-download rule that sets its own save or download folder.
    bool ruleSetsLocation(const QString &ruleDef)
    {
        const QJsonObject rule = QJsonDocument::fromJson(ruleDef.toUtf8()).object();
        const QJsonObject torrentParams = rule.value(u"torrentParams"_s).toObject();
        return !rule.value(u"savePath"_s).toString().isEmpty()
            || !torrentParams.value(u"save_path"_s).toString().isEmpty()
            || !torrentParams.value(u"download_path"_s).toString().isEmpty();
    }

    void addRules(QHash<QString, EndpointRule> &map, const QString &permission, const QStringList &endpoints
            , const bool logged = false, const QString &category = CATEGORY_TORRENT)
    {
        for (const QString &endpoint : endpoints)
            map.insert(endpoint, {.permission = permission, .logged = logged, .category = category});
    }

    QHash<QString, EndpointRule> buildEndpointMap()
    {
        QHash<QString, EndpointRule> map;

        addRules(map, u"view"_s, {
            u"app/version"_s, u"app/webapiVersion"_s, u"app/buildInfo"_s, u"app/processInfo"_s,
            u"app/preferences"_s, u"app/defaultSavePath"_s,
            u"clientdata/load"_s, u"clientdata/store"_s,
            u"sync/maindata"_s, u"sync/torrentPeers"_s,
            u"transfer/info"_s, u"transfer/getSpeedLimits"_s, u"transfer/speedLimitsMode"_s, u"transfer/uploadLimit"_s, u"transfer/downloadLimit"_s,
            u"torrents/count"_s, u"torrents/info"_s, u"torrents/properties"_s, u"torrents/trackers"_s,
            u"torrents/webseeds"_s, u"torrents/files"_s, u"torrents/pieceHashes"_s, u"torrents/pieceStates"_s,
            u"torrents/pieceAvailability"_s, u"torrents/categories"_s, u"torrents/tags"_s,
            u"torrents/uploadLimit"_s, u"torrents/downloadLimit"_s, u"torrents/export"_s});
        addRules(map, u"add"_s, {u"torrents/fetchMetadata"_s, u"torrents/parseMetadata"_s, u"torrents/saveMetadata"_s});
        addRules(map, u"add"_s, {u"torrents/add"_s}, true);
        addRules(map, u"control"_s, {
            u"torrents/start"_s, u"torrents/stop"_s, u"torrents/pause"_s, u"torrents/resume"_s,
            u"torrents/recheck"_s, u"torrents/reannounce"_s, u"torrents/setForceStart"_s,
            u"torrents/increasePrio"_s, u"torrents/decreasePrio"_s, u"torrents/topPrio"_s, u"torrents/bottomPrio"_s,
            u"transfer/pauseSession"_s, u"transfer/resumeSession"_s}, true);
        addRules(map, u"edit"_s, {u"torrents/SSLParameters"_s});
        addRules(map, u"edit"_s, {
            u"torrents/rename"_s, u"torrents/setComment"_s, u"torrents/setCategory"_s,
            u"torrents/createCategory"_s, u"torrents/removeCategories"_s,
            u"torrents/addTags"_s, u"torrents/setTags"_s, u"torrents/removeTags"_s, u"torrents/createTags"_s,
            u"torrents/deleteTags"_s, u"torrents/addTrackers"_s, u"torrents/editTracker"_s, u"torrents/removeTrackers"_s,
            u"torrents/addPeers"_s, u"torrents/addWebSeeds"_s, u"torrents/editWebSeed"_s, u"torrents/removeWebSeeds"_s,
            u"torrents/filePrio"_s, u"torrents/setUploadLimit"_s, u"torrents/setDownloadLimit"_s,
            u"torrents/setShareLimits"_s, u"torrents/setAutoManagement"_s, u"torrents/setSuperSeeding"_s,
            u"torrents/toggleSequentialDownload"_s, u"torrents/toggleFirstLastPiecePrio"_s,
            u"torrents/renameFile"_s, u"torrents/renameFolder"_s, u"torrents/setSSLParameters"_s}, true);
        addRules(map, u"save_location"_s, {u"app/getDirectoryContent"_s, u"app/getFreeSpaceAtPath"_s});
        addRules(map, u"save_location"_s, {
            u"torrents/setLocation"_s, u"torrents/setSavePath"_s, u"torrents/setDownloadPath"_s,
            u"torrents/editCategory"_s}, true);
        addRules(map, u"delete"_s, {u"torrents/delete"_s}, true);
        addRules(map, u"download_files"_s, {u"torrents/downloadFile"_s}, true);
        addRules(map, u"speed_limits"_s, {
            u"transfer/setSpeedLimitsMode"_s, u"transfer/toggleSpeedLimitsMode"_s,
            u"transfer/setUploadLimit"_s, u"transfer/setDownloadLimit"_s, u"transfer/setSpeedLimits"_s}, true, u"speed"_s);
        addRules(map, u"settings"_s, {
            u"app/networkInterfaceList"_s, u"app/networkInterfaceAddressList"_s, u"app/cookies"_s});
        addRules(map, u"settings"_s, {
            u"app/setPreferences"_s, u"app/shutdown"_s, u"app/sendTestEmail"_s, u"app/setCookies"_s,
            u"app/rotateAPIKey"_s, u"app/deleteAPIKey"_s, u"transfer/banPeers"_s}, true, u"settings"_s);
        addRules(map, u"qbt_log"_s, {u"log/main"_s, u"log/peers"_s});
        addRules(map, u"torrent_creator"_s, {u"torrentcreator/status"_s, u"torrentcreator/torrentFile"_s});
        addRules(map, u"torrent_creator"_s, {u"torrentcreator/addTask"_s, u"torrentcreator/deleteTask"_s}
            , true, u"creator"_s);
        addRules(map, u"rss_read"_s, {
            u"rss/items"_s, u"rss/rules"_s, u"rss/exportRules"_s, u"rss/matchingArticles"_s, u"rss/markAsRead"_s, u"rss/refreshItem"_s});
        addRules(map, u"rss_manage"_s, {
            u"rss/addFolder"_s, u"rss/addFeed"_s, u"rss/setFeedURL"_s, u"rss/setFeedRefreshInterval"_s,
            u"rss/removeItem"_s, u"rss/moveItem"_s, u"rss/setRule"_s, u"rss/renameRule"_s, u"rss/cloneRule"_s, u"rss/removeRule"_s,
            u"rss/importRules"_s}
            , true, u"rss"_s);
        addRules(map, u"search"_s, {
            u"search/stop"_s, u"search/status"_s, u"search/results"_s, u"search/delete"_s, u"search/plugins"_s});
        addRules(map, u"search"_s, {u"search/start"_s, u"search/downloadTorrent"_s}, true, u"search"_s);
        addRules(map, u"search_plugins"_s, {
            u"search/installPlugin"_s, u"search/uninstallPlugin"_s, u"search/enablePlugin"_s,
            u"search/updatePlugins"_s}, true, u"search"_s);

        return map;
    }

    const QHash<QString, EndpointRule> &endpointMap()
    {
        static const QHash<QString, EndpointRule> map = buildEndpointMap();
        return map;
    }
}

const QList<Access::Permission> &Access::permissions()
{
    static const QList<Permission> list
    {
        {u"view"_s, u"View torrents"_s, u"See the torrent list, speeds, statistics and torrent details."_s, u"Torrents"_s},
        {u"add"_s, u"Add torrents"_s, u"Add torrents from files, magnet links or URLs."_s, u"Torrents"_s},
        {u"control"_s, u"Start / stop torrents"_s, u"Start, stop, force start, recheck, reannounce and change queue order."_s, u"Torrents"_s},
        {u"edit"_s, u"Edit torrents"_s, u"Rename, change category and tags, trackers, file priorities and per-torrent limits."_s, u"Torrents"_s},
        {u"save_location"_s, u"Choose save locations"_s, u"Pick where torrents are saved, move files, edit category folders and browse the PC's folders."_s, u"Torrents"_s, true},
        {u"delete"_s, u"Delete torrents"_s, u"Remove torrents from the list (downloaded files are kept)."_s, u"Torrents"_s},
        {u"delete_files"_s, u"Delete files from disk"_s, u"Remove torrents together with their downloaded files."_s, u"Torrents"_s, true},
        {u"download_files"_s, u"Download files"_s, u"Download the files of a torrent from the PC through the WebUI."_s, u"Torrents"_s, true},
        {u"speed_limits"_s, u"Global speed limits"_s, u"Change global speed limits and switch alternative speed mode."_s, u"qBittorrent"_s},
        {u"settings"_s, u"Change qBittorrent settings"_s, u"Change qBittorrent options, ban peers and shut qBittorrent down. Sign-in, security, WebUI address and run-program options, and the API key, also need \"Manage users and roles\"."_s, u"qBittorrent"_s, true},
        {u"qbt_log"_s, u"View qBittorrent log"_s, u"Read qBittorrent's own execution and peer logs."_s, u"qBittorrent"_s},
        {u"torrent_creator"_s, u"Torrent creator"_s, u"Create new torrents from files and folders on the PC."_s, u"qBittorrent"_s, true},
        {u"rss_read"_s, u"Read RSS"_s, u"Read RSS feeds and articles."_s, u"RSS"_s},
        {u"rss_manage"_s, u"Manage RSS"_s, u"Add, remove and edit RSS feeds and auto-download rules."_s, u"RSS"_s},
        {u"search"_s, u"Use search"_s, u"Search for torrents with the installed search plugins."_s, u"Search"_s},
        {u"search_plugins"_s, u"Manage search plugins"_s, u"Install, remove, enable and update search plugins. Plugins are Python code that runs on the PC."_s, u"Search"_s, true},
        {u"activity_log"_s, u"View activity log"_s, u"Read the activity log of who did what."_s, u"Users"_s},
        {u"manage_users"_s, u"Manage users and roles"_s, u"Create and edit users and roles, and sign people out."_s, u"Users"_s, true},
        {u"unlisted_api"_s, u"Unrecognised API features"_s, u"Use WebAPI actions that this access list doesn't know about (for example, ones added by a future update)."_s, u"Users"_s, true},
    };
    return list;
}

bool Access::isValidPermission(const QString &id)
{
    for (const Permission &permission : permissions())
    {
        if (permission.id == id)
            return true;
    }
    return false;
}

QString Access::permissionLabel(const QString &id)
{
    for (const Permission &permission : permissions())
    {
        if (permission.id == id)
            return permission.label;
    }
    return id;
}

QList<Access::DefaultRole> Access::defaultRoles()
{
    QStringList all;
    for (const Permission &permission : permissions())
        all.append(permission.id);
    all.sort();

    return {
        {u"Admin"_s, u"Full control, including settings and user management."_s, all},
        {u"User"_s, u"Add and manage torrents. Can't delete, change settings or choose save locations."_s
            , {u"add"_s, u"control"_s, u"edit"_s, u"rss_read"_s, u"search"_s, u"view"_s}},
        {u"Viewer"_s, u"Read-only access."_s, {u"rss_read"_s, u"view"_s}},
    };
}

Access::EndpointRule Access::ruleFor(const QString &endpoint)
{
    const auto iter = endpointMap().constFind(endpoint);
    if (iter != endpointMap().cend())
        return iter.value();
    return {.permission = u"unlisted_api"_s, .logged = true, .category = u"other"_s};
}

QSet<QString> Access::requiredPermissions(const QString &endpoint, const QHash<QString, QString> &params)
{
    QSet<QString> required {ruleFor(endpoint).permission};
    if (endpoint == u"search/downloadTorrent")
        required.insert(u"add"_s);
    // The API key opens everything, so only an admin may replace or remove it.
    if ((endpoint == u"app/rotateAPIKey") || (endpoint == u"app/deleteAPIKey"))
        required.insert(ADMIN_PERMISSION);
    if ((endpoint == u"torrents/delete") && isTrue(params.value(u"deleteFiles"_s)))
        required.insert(u"delete_files"_s);
    // Renaming can move files out of the torrent's folder, and RSS rules can pick their own folders.
    if (((endpoint == u"torrents/renameFile") || (endpoint == u"torrents/renameFolder"))
        && leavesTorrentFolder(params.value(u"newPath"_s)))
    {
        required.insert(u"save_location"_s);
    }
    if ((endpoint == u"rss/setRule") && ruleSetsLocation(params.value(u"ruleDef"_s)))
        required.insert(u"save_location"_s);
    // An imported rules file can contain rules with their own folders.
    if (endpoint == u"rss/importRules")
        required.insert(u"save_location"_s);
    return required;
}

QStringList Access::saveLocationParams(const QString &endpoint)
{
    if (endpoint == u"torrents/add")
        return {u"savepath"_s, u"downloadPath"_s, u"useDownloadPath"_s};
    if (endpoint == u"torrents/createCategory")
        return {u"savePath"_s, u"downloadPath"_s, u"downloadPathEnabled"_s};
    return {};
}

const QSet<QString> &Access::redactedPreferences()
{
    static const QSet<QString> keys
    {
        u"autorun_on_torrent_added_program"_s, u"autorun_program"_s,
        u"banned_IPs"_s, u"bypass_auth_subnet_whitelist"_s,
        u"dyndns_domain"_s, u"dyndns_password"_s, u"dyndns_username"_s,
        u"mail_notification_email"_s, u"mail_notification_password"_s, u"mail_notification_sender"_s,
        u"mail_notification_smtp"_s, u"mail_notification_username"_s,
        u"proxy_ip"_s, u"proxy_password"_s, u"proxy_username"_s,
        u"web_ui_api_key"_s, u"web_ui_custom_http_headers"_s, u"web_ui_domain_list"_s,
        u"web_ui_https_cert_path"_s, u"web_ui_https_key_path"_s, u"web_ui_reverse_proxies_list"_s,
        u"web_ui_username"_s
    };
    return keys;
}

const QSet<QString> &Access::adminOnlyPreferences()
{
    static const QSet<QString> keys
    {
        u"autorun_enabled"_s, u"autorun_program"_s,
        u"autorun_on_torrent_added_enabled"_s, u"autorun_on_torrent_added_program"_s,
        u"alternative_webui_enabled"_s, u"alternative_webui_path"_s,
        u"bypass_auth_subnet_whitelist"_s, u"bypass_auth_subnet_whitelist_enabled"_s, u"bypass_local_auth"_s,
        u"use_https"_s, u"web_ui_activity_log_days"_s, u"web_ui_address"_s, u"web_ui_ban_duration"_s,
        u"web_ui_clickjacking_protection_enabled"_s, u"web_ui_csrf_protection_enabled"_s,
        u"web_ui_custom_http_headers"_s, u"web_ui_domain_list"_s, u"web_ui_host_header_validation_enabled"_s,
        u"web_ui_https_cert_path"_s, u"web_ui_https_key_path"_s, u"web_ui_max_auth_fail_count"_s,
        u"web_ui_password"_s, u"web_ui_port"_s, u"web_ui_reverse_proxies_list"_s,
        u"web_ui_reverse_proxy_enabled"_s, u"web_ui_secure_cookie_enabled"_s,
        u"web_ui_session_timeout"_s, u"web_ui_sessions_count_limit"_s, u"web_ui_shared_login_allowed"_s,
        u"web_ui_upnp"_s, u"web_ui_use_custom_http_headers_enabled"_s, u"web_ui_user_lock_duration"_s,
        u"web_ui_user_max_auth_fail_count"_s, u"web_ui_user_sign_in_days"_s, u"web_ui_username"_s
    };
    return keys;
}

bool Access::isTrue(const QString &value)
{
    const QString trimmed = value.trimmed().toLower();
    return !trimmed.isEmpty() && (trimmed != u"false") && (trimmed != u"0");
}
