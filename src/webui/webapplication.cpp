/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2014-2026  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2024  Radu Carpa <radu.carpa@cern.ch>
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

#include "webapplication.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QMimeType>
#include <QNetworkCookie>
#include <QRegularExpression>
#include <QThread>
#include <QUrl>

#include "base/access/accessstore.h"
#include "base/access/activitytext.h"
#include "base/access/permissions.h"
#include "base/algorithm.h"
#include "base/bittorrent/infohash.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/torrent.h"
#include "base/bittorrent/torrentcreationmanager.h"
#include "base/http/httperror.h"
#include "base/http/response.h"
#include "base/http/responsewriter.h"
#include "base/logger.h"
#include "base/preferences.h"
#include "base/profile.h"
#include "base/types.h"
#include "base/utils/apikey.h"
#include "base/utils/fs.h"
#include "base/utils/io.h"
#include "base/utils/misc.h"
#include "base/utils/password.h"
#include "base/utils/random.h"
#include "base/utils/string.h"
#include "api/accesscontroller.h"
#include "api/apierror.h"
#include "api/appcontroller.h"
#include "api/authcontroller.h"
#include "api/clientdatacontroller.h"
#include "api/logcontroller.h"
#include "api/rsscontroller.h"
#include "api/searchcontroller.h"
#include "api/synccontroller.h"
#include "api/torrentcreatorcontroller.h"
#include "api/torrentscontroller.h"
#include "api/transfercontroller.h"
#include "clientdatastorage.h"
#include "searchjobmanager.h"
#include "websession.h"

const int MAX_ALLOWED_FILESIZE = 10 * 1024 * 1024;
const QString SESSION_COOKIE_NAME_PREFIX = u"QBT_SID_"_s;

const QString WWW_FOLDER = u":/www"_s;
const QString PUBLIC_FOLDER = u"/public"_s;
const QString PRIVATE_FOLDER = u"/private"_s;
const QString INDEX_HTML = u"/index.html"_s;

const QString BASIC_AUTH = u"Basic"_s;
const QString BEARER_AUTH = u"Bearer"_s;

const QString API_PATH = u"/api/v2/"_s;

namespace
{
    QStringMap parseCookie(const QStringView cookieStr)
    {
        // [rfc6265] 4.2.1. Syntax
        QStringMap ret;
        const QList<QStringView> cookies = cookieStr.split(u';', Qt::SkipEmptyParts);

        for (const QStringView cookie : cookies)
        {
            const qsizetype idx = cookie.indexOf(u'=');
            if (idx < 0)
                continue;

            const QString name = cookie.first(idx).trimmed().toString();
            const QString value = Utils::String::unquote(cookie.sliced(idx + 1).trimmed()).toString();
            ret.insert(name, value);
        }
        return ret;
    }

    std::tuple<QString, QString> parseAPIEndpoint(const QString &endpoint)
    {
        const QRegularExpression apiEndpointPattern {u"^(?<scope>[A-Za-z_][A-Za-z_0-9]*)/(?<action>[A-Za-z_][A-Za-z_0-9]*)$"_s};
        if (const QRegularExpressionMatch match = apiEndpointPattern.match(endpoint); match.hasMatch())
            return std::make_tuple(match.captured(u"scope"), match.captured(u"action"));

        return {};
    }

    std::tuple<QString, QString> parseAuthorizationHeader(const QString &authHeader)
    {
        const QRegularExpression authHeaderPattern {u"^(?<scheme>\\S+)\\s+(?<value>.+)$"_s, QRegularExpression::DotMatchesEverythingOption};
        if (const QRegularExpressionMatch match = authHeaderPattern.match(authHeader); match.hasMatch())
            return std::make_tuple(match.captured(u"scheme"), match.captured(u"value"));

        return {};
    }

    QUrl urlFromHostHeader(const QString &hostHeader)
    {
        if (!hostHeader.contains(u"://"))
            return {u"http://"_s + hostHeader};
        return hostHeader;
    }

    QString getCachingInterval(QString contentType)
    {
        contentType = contentType.toLower();

        if (contentType.startsWith(u"image/"))
            return u"private, max-age=604800"_s;  // 1 week

        if ((contentType == Http::CONTENT_TYPE_CSS) || (contentType == Http::CONTENT_TYPE_JS))
        {
            // short interval in case of program update
            return u"private, max-age=43200"_s;  // 12 hrs
        }

        return u"no-store"_s;
    }

    bool isAddedByTag(const QString &tag)
    {
        return tag.trimmed().startsWith(u"added-by:"_s, Qt::CaseInsensitive);
    }

    // Whether any torrent an API call is aimed at says who added it.
    bool anyTorrentHasAddedByTag(const QHash<QString, QString> &params)
    {
        const auto hasAddedByTag = [](const BitTorrent::Torrent *torrent)
        {
            return std::ranges::any_of(torrent->tags(), [](const Tag &tag) { return isAddedByTag(tag.toString()); });
        };

        const auto *btSession = BitTorrent::Session::instance();
        const QStringList hashes = Access::torrentHashes(params);
        if (hashes == QStringList {u"all"_s})
            return std::ranges::any_of(btSession->torrents(), hasAddedByTag);

        for (const QString &hash : hashes)
        {
            const BitTorrent::Torrent *torrent = btSession->getTorrent(BitTorrent::TorrentID::fromString(hash));
            if (torrent && hasAddedByTag(torrent))
                return true;
        }
        return false;
    }

    QString createLanguagesOptionsHtml()
    {
        // List language files
        const QStringList langFiles = QDir(u":/www/translations"_s)
            .entryList({u"webui_*.qm"_s}, QDir::Files, QDir::Name);

        QStringList languages;
        languages.reserve(langFiles.size());

        for (const QString &langFile : langFiles)
        {
            const auto langCode = QStringView(langFile).sliced(6).chopped(3); // remove "webui_" and ".qm"
            const QString entry = u"<option value=\"%1\">%2</option>"_s
                .arg(langCode, Utils::Misc::languageToLocalizedString(langCode));
            languages.append(entry);
        }

        return languages.join(u'\n');
    }
}

WebApplication::WebApplication(IApplication *app, QObject *parent)
    : ApplicationComponent(app, parent)
    , m_cacheID {QString::number(Utils::Random::rand(), 36)}
    , m_trRegex {u"QBT_TR\\((([^\\)]|\\)(?!QBT_TR))+)\\)QBT_TR\\[CONTEXT=([a-zA-Z_][a-zA-Z0-9_]*)\\]"_s}
    , m_authController {new AuthController(this, app, this)}
    , m_torrentCreationManager {new BitTorrent::TorrentCreationManager(app, this)}
    , m_searchJobManager {new SearchJobManager(this)}
    , m_clientDataStorage {new ClientDataStorage(this)}
    , m_accessStore {Access::AccessStore::instance()}
{
    Q_ASSERT(m_accessStore);

    declarePublicAPI(u"auth/login"_s);

    configure();
    connect(Preferences::instance(), &Preferences::changed, this, &WebApplication::configure);
}

WebApplication::~WebApplication()
{
    // cleanup sessions data
    qDeleteAll(m_sessions);
}

void WebApplication::sendWebUIFile(const Http::HeaderMap &commonHeaders, Http::ResponseWriter &responseWriter)
{
    if (request().path.contains(u'\\'))
        throw BadRequestHTTPError();

    if (const QList<QStringView> pathItems = QStringView(request().path).split(u'/', Qt::SkipEmptyParts)
            ; pathItems.contains(u".") || pathItems.contains(u".."))
    {
        throw BadRequestHTTPError();
    }

    const QString path = (request().path != u"/")
        ? request().path
        : INDEX_HTML;

    Path localPath = m_rootFolder
            / Path(session() ? PRIVATE_FOLDER : PUBLIC_FOLDER)
            / Path(path);
    if (!localPath.exists() && session())
    {
        // try to send public file if there is no private one
        localPath = m_rootFolder / Path(PUBLIC_FOLDER) / Path(path);
    }

    if (m_isAltUIUsed)
    {
        if (!Utils::Fs::isRegularFile(localPath))
        {
#ifdef DISABLE_GUI
            if (path == INDEX_HTML)
            {
                auto *preferences = Preferences::instance();
                preferences->setAltWebUIEnabled(false);
                preferences->apply();
            }
#endif
            throw InternalServerErrorHTTPError(tr("Unacceptable file type, only regular file is allowed."));
        }

        const QString rootFolder = m_rootFolder.data();

        QFileInfo fileInfo {localPath.parentPath().data()};
        while (fileInfo.path() != rootFolder)
        {
            if (fileInfo.isSymLink())
            {
#ifdef DISABLE_GUI
                if (path == INDEX_HTML)
                {
                    auto *preferences = Preferences::instance();
                    preferences->setAltWebUIEnabled(false);
                    preferences->apply();
                }
#endif
                throw InternalServerErrorHTTPError(tr("Symlinks inside alternative UI folder are forbidden."));
            }

            fileInfo.setFile(fileInfo.path());
        }
    }

    sendFile(localPath, commonHeaders, responseWriter);
}

void WebApplication::translateDocument(QString &data) const
{
    data.replace(u"${LANG}"_s, m_currentLocale.left(2));
    data.replace(u"${CACHEID}"_s, m_cacheID);

    qsizetype i = 0;
    bool found = true;
    while ((i < data.size()) && found)
    {
        QRegularExpressionMatch regexMatch;
        i = data.indexOf(m_trRegex, i, &regexMatch);
        if (i >= 0)
        {
            const QStringView sourceText = regexMatch.capturedView(1);
            const QStringView context = regexMatch.capturedView(3);

            const QString loadedText = m_translationFileLoaded
                ? m_translator.translate(context.toUtf8().constData(), sourceText.toUtf8().constData())
                : QString();
            // `loadedText` is empty when translation is not provided
            // it should fallback to `sourceText`
            QString translation = loadedText.isEmpty() ? sourceText.toString() : loadedText;

            // Escape quotes to workaround issues with HTML attributes
            // FIXME: this is a dirty workaround to deal with broken translation strings:
            // 1. Translation strings is the culprit of the issue, they should be fixed instead
            // 2. The escaped quote/string is wrong for JS. JS use backslash to escape the quote: "\""
            translation.replace(u'"', u"&#34;"_s);

            data.replace(i, regexMatch.capturedLength(), translation);
            i += translation.length();
        }
        else
        {
            found = false; // no more translatable strings
        }
    }
}

ISession *WebApplication::session()
{
    return m_currentSession;
}

const Http::Request &WebApplication::request() const
{
    return m_request;
}

const Http::Environment &WebApplication::env() const
{
    return m_env;
}

void WebApplication::setUsername(const QString &username)
{
    m_username = username;
}

void WebApplication::setPasswordHash(const QByteArray &passwordHash)
{
    m_passwordHash = passwordHash;
}

void WebApplication::processAPIRequest(const QString &endpoint, const Http::HeaderMap &commonHeaders, Http::ResponseWriter &responseWriter)
{
    const auto [scope, action] = parseAPIEndpoint(endpoint);
    if (scope.isEmpty())
        throw NotFoundHTTPError();

    // Check public/private scope
    if (!session() && !isPublicAPI(scope, action))
        throw ForbiddenHTTPError();

    // Find matching API
    APIController *controller = nullptr;
    if (m_currentSession)
        controller = m_currentSession->getAPIController(scope);

    if (!controller)
    {
        if (scope == u"auth")
            controller = m_authController;
        else
            throw NotFoundHTTPError();
    }

    // Filter HTTP methods
    const QSet<QString> defaultAllowedMethods {
            Http::HEADER_REQUEST_METHOD_GET,
            Http::HEADER_REQUEST_METHOD_POST,
            Http::HEADER_REQUEST_METHOD_HEAD};
    const auto allowedMethodIter = m_allowedMethod.constFind({scope, action});
    if (allowedMethodIter == m_allowedMethod.cend())
    {
        // by default allow GET, POST and HEAD methods
        if (!defaultAllowedMethods.contains(m_request.method))
            throw MethodNotAllowedHTTPError();
    }
    else
    {
        if (*allowedMethodIter != m_request.method)
            throw MethodNotAllowedHTTPError();
    }

    QHash<QString, QString> params;
    if ((m_request.method == Http::HEADER_REQUEST_METHOD_GET)
        || (m_request.method == Http::HEADER_REQUEST_METHOD_HEAD))
    {
        params.reserve(m_request.query.size());
        for (auto iter = m_request.query.cbegin(); iter != m_request.query.cend(); ++iter)
            params.insert(iter.key(), QString::fromUtf8(iter.value()));
    }
    else
    {
        params = m_request.posts;
    }

    DataMap data;
    QStringList fileNames;
    for (const Http::UploadedFile &torrent : request().files)
    {
        data[torrent.filename] = torrent.data;
        fileNames.append(torrent.filename);
    }

    // Multi-user access control. "auth" (login/logout) and "access" (user management, which checks
    // its own permissions) are handled separately.
    const bool isAccessControlled = session() && (scope != u"auth") && (scope != u"access");
    if (isAccessControlled)
        checkAccess(endpoint, params, fileNames);
    const bool isLogged = isAccessControlled && Access::ruleFor(endpoint).logged;
    const QHash<QString, QString> namesBefore = isLogged ? torrentNames(params) : QHash<QString, QString>();

    try
    {
        const APIResult apiResult = controller->run(action, params, data);
        if (std::holds_alternative<StreamFileAPIResult>(apiResult))
        {
            const auto result = std::get<StreamFileAPIResult>(apiResult);
            if (isLogged)
                logRequest(endpoint, params, fileNames, namesBefore, u"ok"_s, {});
            responseWriter.streamFile(result.filePath, commonHeaders);
            return;
        }

        Http::Response response {.headers = commonHeaders};

        if (m_cookieBasedSessionStateChange == SessionStateChange::Start)
        {
            setSessionCookie(response.headers);
        }
        else if (m_cookieBasedSessionStateChange == SessionStateChange::End)
        {
            QNetworkCookie cookie {m_sessionCookieName.toLatin1()};
            cookie.setPath(u"/"_s);
            cookie.setExpirationDate(QDateTime::currentDateTime().addDays(-1));
            response.headers.insert(Http::HEADER_SET_COOKIE, QString::fromLatin1(cookie.toRawForm()));
        }

        auto result = std::get<RegularAPIResult>(apiResult);

        // Without "settings" the private values are blanked; without "manage_users" the API key is, because
        // it opens everything.
        if (isAccessControlled && (endpoint == u"app/preferences") && (result.data.userType() == QMetaType::QJsonDocument))
        {
            const Access::Identity &viewer = m_currentSession->identity();
            const bool canSeeSettings = viewer.can(u"settings"_s);
            if (!canSeeSettings || !viewer.can(Access::ADMIN_PERMISSION))
            {
                QJsonObject preferences = result.data.toJsonDocument().object();
                const QSet<QString> blankedKeys = canSeeSettings ? QSet<QString> {u"web_ui_api_key"_s}
                    : Access::redactedPreferences();
                for (const QString &key : blankedKeys)
                {
                    if (preferences.contains(key))
                        preferences[key] = QString();
                }
                result.data = QJsonDocument(preferences);
            }
        }

        if (isLogged)
            logRequest(endpoint, params, fileNames, namesBefore, u"ok"_s, {});
        if (result.data.isNull())
        {
            response.status = {.code = 204};
        }
        else
        {
            switch (result.status)
            {
            case APIStatus::Async:
                response.status = {.code = 202};
                break;
            case APIStatus::Ok:
                response.status = {.code = 200};
                break;
            }

            switch (result.data.userType())
            {
            case QMetaType::QJsonDocument:
                response.headers.insert(Http::HEADER_CONTENT_TYPE, Http::CONTENT_TYPE_JSON);
                response.content = result.data.toJsonDocument().toJson(QJsonDocument::Compact);
                break;
            case QMetaType::QByteArray:
                response.headers.insert(Http::HEADER_CONTENT_TYPE, (!result.mimeType.isEmpty() ? result.mimeType : Http::CONTENT_TYPE_TXT));
                if (!result.filename.isEmpty())
                    response.headers.insert(Http::HEADER_CONTENT_DISPOSITION, u"attachment; filename=\"%1\""_s.arg(result.filename));
                response.content = result.data.toByteArray();
                break;
            case QMetaType::QString:
            default:
                response.headers.insert(Http::HEADER_CONTENT_TYPE, Http::CONTENT_TYPE_TXT);
                response.content = result.data.toString().toUtf8();
                break;
            }
        }

        responseWriter.setResponse(response);
    }
    catch (const APIError &error)
    {
        if (isLogged)
            logRequest(endpoint, params, fileNames, namesBefore, u"failed"_s, error.message());

        // re-throw as HTTPError
        switch (error.type())
        {
        case APIErrorType::AccessDenied:
            throw ForbiddenHTTPError(error.message());
        case APIErrorType::BadData:
            throw UnsupportedMediaTypeHTTPError(error.message());
        case APIErrorType::BadParams:
            throw BadRequestHTTPError(error.message());
        case APIErrorType::Conflict:
            throw ConflictHTTPError(error.message());
        case APIErrorType::NotFound:
            throw NotFoundHTTPError(error.message());
        case APIErrorType::Unauthorized:
            throw UnauthorizedHTTPError(error.message());
        default:
            Q_UNREACHABLE();
            break;
        }
    }
}

void WebApplication::configure()
{
    const auto *pref = Preferences::instance();

    const bool isAltUIUsed = pref->isAltWebUIEnabled();
    const Path rootFolder = (!isAltUIUsed ? Path(WWW_FOLDER) : pref->getWebUIRootFolder());
    if ((isAltUIUsed != m_isAltUIUsed) || (rootFolder != m_rootFolder))
    {
        m_isAltUIUsed = isAltUIUsed;
        m_rootFolder = rootFolder;
        m_translatedFiles.clear();
        if (!m_isAltUIUsed)
            LogMsg(tr("Using built-in WebUI."));
        else
            LogMsg(tr("Using custom WebUI. Location: \"%1\".").arg(m_rootFolder.toString()));
    }

    const QString newLocale = pref->getLocale();
    if (m_currentLocale != newLocale)
    {
        m_currentLocale = newLocale;
        m_translatedFiles.clear();

        m_translationFileLoaded = m_translator.load((m_rootFolder / Path(u"translations/webui_"_s) + newLocale).data());
        if (m_translationFileLoaded)
        {
            LogMsg(tr("WebUI translation for selected locale (%1) has been successfully loaded.")
                   .arg(newLocale));
        }
        else
        {
            LogMsg(tr("Couldn't load WebUI translation for selected locale (%1).").arg(newLocale), Log::WARNING);
        }
    }

    m_isLocalAuthEnabled = pref->isWebUILocalAuthEnabled();
    m_isAuthSubnetWhitelistEnabled = pref->isWebUIAuthSubnetWhitelistEnabled();
    m_authSubnetWhitelist = pref->getWebUIAuthSubnetWhitelist();
    m_sessionTimeout = std::chrono::seconds(pref->getWebUISessionTimeout());
    m_sessionsCountLimit = std::max(0, pref->getWebUISessionsCountLimit());
    m_sessionCookieName = SESSION_COOKIE_NAME_PREFIX + QString::number(pref->getWebUIPort());

    // all sessions need to update the cookie expiration date
    for (WebSession *session : asConst(m_sessions))
    {
        if (session->type() == WebSessionType::CookieBased)
            static_cast<CookieBasedWebSession *>(session)->setCookieRefreshTime(0s);
    }

    m_serverDomains.clear();
    const QStringList domains = pref->getServerDomains().split(u';', Qt::SkipEmptyParts);
    m_serverDomains.reserve(domains.size());
    for (const QString &domain : domains)
    {
        // regex must be anchored
        const QString expr = QRegularExpression::wildcardToRegularExpression(domain.trimmed(), QRegularExpression::NonPathWildcardConversion);
        m_serverDomains.emplace_back(expr, QRegularExpression::CaseInsensitiveOption);
    }

    m_isCSRFProtectionEnabled = pref->isWebUICSRFProtectionEnabled();
    m_isSecureCookieEnabled = pref->isWebUISecureCookieEnabled();
    m_isHostHeaderValidationEnabled = pref->isWebUIHostHeaderValidationEnabled();
    m_isHttpsEnabled = pref->isWebUIHttpsEnabled();

    m_prebuiltHeaders.clear();
    m_prebuiltHeaders.insert(Http::HEADER_X_XSS_PROTECTION, u"1; mode=block"_s);
    m_prebuiltHeaders.insert(Http::HEADER_X_CONTENT_TYPE_OPTIONS, u"nosniff"_s);

    if (!m_isAltUIUsed)
    {
        m_prebuiltHeaders.insert(Http::HEADER_CROSS_ORIGIN_OPENER_POLICY, u"same-origin"_s);
        m_prebuiltHeaders.insert(Http::HEADER_REFERRER_POLICY, u"same-origin"_s);
    }

    const bool isClickjackingProtectionEnabled = pref->isWebUIClickjackingProtectionEnabled();
    if (isClickjackingProtectionEnabled)
        m_prebuiltHeaders.insert(Http::HEADER_X_FRAME_OPTIONS, u"SAMEORIGIN"_s);

    const QString contentSecurityPolicy =
        (m_isAltUIUsed
            ? QString()
            : u"default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; script-src 'self' 'unsafe-inline'; object-src 'none'; form-action 'self'; frame-src 'self' blob:;"_s)
        + (isClickjackingProtectionEnabled ? u" frame-ancestors 'self';"_s : QString())
        + (m_isHttpsEnabled ? u" upgrade-insecure-requests;"_s : QString());
    if (!contentSecurityPolicy.isEmpty())
        m_prebuiltHeaders.insert(Http::HEADER_CONTENT_SECURITY_POLICY, contentSecurityPolicy);

    if (pref->isWebUICustomHTTPHeadersEnabled())
    {
        const QString customHeaders = pref->getWebUICustomHTTPHeaders();
        const QList<QStringView> customHeaderLines = QStringView(customHeaders).trimmed().split(u'\n', Qt::SkipEmptyParts);

        for (const QStringView line : customHeaderLines)
        {
            const qsizetype idx = line.indexOf(u':');
            if (idx < 0)
            {
                // require separator `:` to be present even if `value` field can be empty
                LogMsg(tr("Missing ':' separator in WebUI custom HTTP header: \"%1\"").arg(line.toString()), Log::WARNING);
                continue;
            }

            const QString header = line.first(idx).trimmed().toString();
            const QString value = line.sliced(idx + 1).trimmed().toString();
            m_prebuiltHeaders.insert(header, value);
        }
    }

    m_isReverseProxySupportEnabled = pref->isWebUIReverseProxySupportEnabled();
    if (m_isReverseProxySupportEnabled)
    {
        const QStringList proxyList = pref->getWebUITrustedReverseProxiesList().split(u';', Qt::SkipEmptyParts);

        m_trustedReverseProxyList.clear();
        m_trustedReverseProxyList.reserve(proxyList.size());

        for (QString proxy : proxyList)
        {
            if (!proxy.contains(u'/'))
            {
                const QAbstractSocket::NetworkLayerProtocol protocol = QHostAddress(proxy).protocol();
                if (protocol == QAbstractSocket::IPv4Protocol)
                {
                    proxy.append(u"/32");
                }
                else if (protocol == QAbstractSocket::IPv6Protocol)
                {
                    proxy.append(u"/128");
                }
            }

            const std::optional<Utils::Net::Subnet> subnet = Utils::Net::parseSubnet(proxy);
            if (subnet)
                m_trustedReverseProxyList.push_back(subnet.value());
        }

        if (m_trustedReverseProxyList.isEmpty())
            m_isReverseProxySupportEnabled = false;
    }

    if (const QString apiKey = pref->getWebUIApiKey(); apiKey.isEmpty() || Utils::APIKey::isValid(apiKey))
        m_apiKey = apiKey;
}

void WebApplication::declarePublicAPI(const QString &apiPath)
{
    m_publicAPIs << apiPath;
}

void WebApplication::sendFile(const Path &path, const Http::HeaderMap &commonHeaders, Http::ResponseWriter &responseWriter)
{
    const QDateTime lastModified = Utils::Fs::lastModified(path);

    Http::Response response {.status = {.code = 200}, .headers = commonHeaders};

    // find translated file in cache
    if (const auto it = m_translatedFiles.constFind(path);
        (it != m_translatedFiles.constEnd()) && (lastModified <= it->lastModified))
    {
        response.headers.insert(Http::HEADER_CONTENT_TYPE, it->mimeType);
        response.headers.insert(Http::HEADER_CACHE_CONTROL, getCachingInterval(it->mimeType));
        response.content = it->data;
        responseWriter.setResponse(response);
        return;
    }

    const auto readResult = Utils::IO::readFile(path, MAX_ALLOWED_FILESIZE);
    if (!readResult)
    {
        const QString message = tr("Web server error. %1").arg(readResult.error().message);

        switch (readResult.error().status)
        {
        case Utils::IO::ReadError::NotExist:
            qDebug("%s", qUtf8Printable(message));
            // don't write log messages here to avoid exhausting the disk space
            throw NotFoundHTTPError();

        case Utils::IO::ReadError::ExceedSize:
            qWarning("%s", qUtf8Printable(message));
            LogMsg(message, Log::WARNING);
            throw InternalServerErrorHTTPError(readResult.error().message);

        case Utils::IO::ReadError::Failed:
        case Utils::IO::ReadError::SizeMismatch:
            LogMsg(message, Log::WARNING);
            throw InternalServerErrorHTTPError(readResult.error().message);
        }

        throw InternalServerErrorHTTPError(tr("Web server error. Unknown error."));
    }

    QByteArray data = readResult.value();
    const QMimeType mimeType = QMimeDatabase().mimeTypeForFileNameAndData(path.data(), data);
    const bool isTranslatable = mimeType.inherits(u"text/plain"_s);

    if (isTranslatable)
    {
        auto dataStr = QString::fromUtf8(data);
        // Translate the file
        translateDocument(dataStr);

        // Add the language options
        if (path == (m_rootFolder / Path(PRIVATE_FOLDER) / Path(u"views/preferences.html"_s)))
            dataStr.replace(u"${LANGUAGE_OPTIONS}"_s, createLanguagesOptionsHtml());

        data = dataStr.toUtf8();
        m_translatedFiles[path] = {data, mimeType.name(), lastModified}; // caching translated file
    }

    response.headers.insert(Http::HEADER_CONTENT_TYPE, mimeType.name());
    response.headers.insert(Http::HEADER_CACHE_CONTROL, getCachingInterval(mimeType.name()));
    response.content = data;
    responseWriter.setResponse(response);
}

void WebApplication::processRequest(const Http::Request &request, const Http::Environment &env, Http::ResponseWriter &responseWriter)
{
    m_currentSession = nullptr;
    m_isRequestOnlySession = false;
    m_accessDeniedPermissions.clear();
    m_request = request;
    m_env = env;
    m_cookieBasedSessionStateChange = SessionStateChange::None;

    const QString authHeader = m_request.headers.value(Http::HEADER_AUTHORIZATION);
    const auto [authScheme, authData] = parseAuthorizationHeader(authHeader);
    const bool isUsingApiKey = (authScheme.compare(BEARER_AUTH, Qt::CaseInsensitive) == 0);

    Http::HeaderMap commonHeaders = m_prebuiltHeaders;

    try
    {
        // block suspicious requests
        // CSRF check is skipped for the webroot so that cross-origin hyperlinks to
        // the WebUI succeed; the root document is static and has no side effects.
        const bool isWebRoot = (request.path == u"/") || (request.path == INDEX_HTML);
        if ((!isUsingApiKey && m_isCSRFProtectionEnabled && !isWebRoot && isCrossSiteRequest(m_request))
            || (m_isHostHeaderValidationEnabled && !validateHostHeader()))
        {
            throw UnauthorizedHTTPError();
        }

        // reverse proxy resolve client address
        m_clientAddress = resolveClientAddress();

        if (isUsingApiKey)
        {
            apiKeySessionInitialize(authData);
        }
        else
        {
            cookieSessionInitialize(authScheme, authData);
            if (m_currentSession && !m_isRequestOnlySession)
                setSessionCookie(commonHeaders);
        }

        if (request.path.startsWith(API_PATH))
        {
            const QString endpoint = request.path.sliced(API_PATH.size());

            if (isUsingApiKey && (endpoint.startsWith(u"auth/")))
                throw ForbiddenHTTPError();

            processAPIRequest(endpoint, commonHeaders, responseWriter);
        }
        else
        {
            if (isUsingApiKey)
                throw NotFoundHTTPError();

            sendWebUIFile(commonHeaders, responseWriter);
        }
    }
    catch (const HTTPError &error)
    {
        const Http::ResponseStatus &errorStatus = error.status();
        Http::Response response {.status = errorStatus, .headers = commonHeaders};
        if (!m_accessDeniedPermissions.isEmpty())
            response.headers.insert(u"x-access-denied"_s, m_accessDeniedPermissions);
        response.headers.insert(Http::HEADER_CONTENT_TYPE, Http::CONTENT_TYPE_TXT);
        response.content = (!error.message().isEmpty() ? error.message() : errorStatus.text).toUtf8();
        responseWriter.setResponse(response);
    }
    catch (const RuntimeError &error)
    {
        // e.g. a user database error: answer with an error instead of bringing qBittorrent down
        LogMsg(tr("WebUI: request failed. %1").arg(error.message()), Log::WARNING);
        Http::Response response {.status = {.code = 500, .text = u"Internal Server Error"_s}, .headers = commonHeaders};
        response.headers.insert(Http::HEADER_CONTENT_TYPE, Http::CONTENT_TYPE_TXT);
        // the details are in qBittorrent's log; they can name files and database errors
        response.content = tr("Something went wrong on the server. Details are in qBittorrent's log.").toUtf8();
        responseWriter.setResponse(response);
    }

    if (m_isRequestOnlySession && m_currentSession)
    {
        delete m_sessions.take(m_currentSession->id());
        m_currentSession = nullptr;
    }
}

QString WebApplication::clientId() const
{
    return m_clientAddress.toString();
}

void WebApplication::setSessionCookie(Http::HeaderMap &headers)
{
    Q_ASSERT(m_currentSession && (m_currentSession->type() == WebSessionType::CookieBased));

    auto *currentSession = static_cast<CookieBasedWebSession *>(m_currentSession);
    if (currentSession->shouldRefreshCookie())
    {
        // 'Permanent Cookie' still require an expiration date so set it to a date in the distant future
        std::chrono::seconds cookieExpireDuration = (m_sessionTimeout > 0s) ? m_sessionTimeout : std::chrono::years(1);

        // Personal accounts can stay signed in for a fixed time after signing in ("Keep users signed in for").
        if (const Access::Identity &identity = currentSession->identity()
            ; (identity.kind == Access::IdentityKind::User) && (Preferences::instance()->getWebUIUserSignInDays() > 0))
        {
            const double expiresAt = Access::AccessStore::sessionExpiry(identity.sessionCreatedAt, Access::now());
            cookieExpireDuration = std::chrono::seconds(std::max<qint64>(60, static_cast<qint64>(expiresAt - Access::now())));
        }

        QNetworkCookie cookie {m_sessionCookieName.toLatin1(), currentSession->id().toLatin1()};
        cookie.setExpirationDate(QDateTime::currentDateTime().addDuration(cookieExpireDuration));
        cookie.setHttpOnly(true);
        cookie.setSecure(m_isSecureCookieEnabled && isOriginTrustworthy());  // [rfc6265] 4.1.2.5. The Secure Attribute
        cookie.setPath(u"/"_s);
        if (m_isCSRFProtectionEnabled)
            cookie.setSameSitePolicy(QNetworkCookie::SameSite::Lax);
        else if (cookie.isSecure())
            cookie.setSameSitePolicy(QNetworkCookie::SameSite::None);
        headers.insert(Http::HEADER_SET_COOKIE, QString::fromLatin1(cookie.toRawForm()));
        currentSession->setCookieRefreshTime(cookieExpireDuration);
    }
}

void WebApplication::cookieSessionInitialize(const QString &authScheme, const QString &authData)
{
    Q_ASSERT(!m_currentSession);

    if (const QString sessionId = parseCookie(m_request.headers.value(Http::HEADER_COOKIE)).value(m_sessionCookieName);
        !sessionId.isEmpty())
    {
        if (WebSession *session = m_sessions.value(sessionId))
        {
            Q_ASSERT(session->type() == WebSessionType::CookieBased);
            if (session->type() != WebSessionType::CookieBased) [[unlikely]]
                return;

            if (!session->hasExpired(m_sessionTimeout))
            {
                m_currentSession = session;
                m_currentSession->updateTimestamp();
            }
            else
            {
                // session is outdated - removing it
                delete m_sessions.take(sessionId);
            }
        }

        // Personal sessions live in the user database: check them on every request, so revoked sessions and
        // disabled accounts lose access at once, and bring them back after a restart.
        if (!m_currentSession || (m_currentSession->identity().kind == Access::IdentityKind::User))
            restoreStoredSession(sessionId);
    }

    if (!m_currentSession)
    {
        if (!isAuthNeeded())
        {
            m_pendingIdentity = Access::fullAccessIdentity(Access::IdentityKind::Localhost);
            sessionStart();
        }
        else if (authScheme.compare(BASIC_AUTH, Qt::CaseInsensitive) == 0)
        {
            if (!validateBasicAuth(authData))
                throw UnauthorizedHTTPError();

            // The password comes with every request, so a personal account's session lasts for this request only
            // instead of piling up stored sessions for clients that ignore the cookie.
            if (m_pendingIdentity && (m_pendingIdentity->kind == Access::IdentityKind::User))
            {
                m_pendingIdentity->sessionCreatedAt = Access::now();
                sessionStartImpl(generateSid(), WebSessionType::CookieBased, false);
                m_isRequestOnlySession = true;
                return;
            }
            sessionStart();
        }
    }
}

void WebApplication::apiKeySessionInitialize(const QString &apiKey)
{
    Q_ASSERT(!m_currentSession);

    if (m_apiKey.isEmpty())
        return;

    if (Utils::Password::slowEquals(apiKey.toLatin1(), m_apiKey.toLatin1()))
    {
        if (WebSession *session = m_sessions.value(apiKey))
        {
            Q_ASSERT(session->type() == WebSessionType::APIKeyBased);
            if (session->type() != WebSessionType::APIKeyBased) [[unlikely]]
                return;

            m_currentSession = session;
            m_currentSession->updateTimestamp();
        }
        else
        {
            m_pendingIdentity = Access::fullAccessIdentity(Access::IdentityKind::ApiKey);
            sessionStartImpl(apiKey, WebSessionType::APIKeyBased);
        }
    }
}

QString WebApplication::generateSid() const
{
    QString sid;

    do
    {
        const quint32 tmp[] = {Utils::Random::rand(), Utils::Random::rand(), Utils::Random::rand()
                , Utils::Random::rand(), Utils::Random::rand(), Utils::Random::rand()};
        sid = QString::fromLatin1(QByteArray::fromRawData(reinterpret_cast<const char *>(tmp), sizeof(tmp)).toBase64());
    }
    while (m_sessions.contains(sid));

    return sid;
}

bool WebApplication::isAuthNeeded()
{
    // A request a proxy passed on isn't from this PC, even when the proxy runs on it.
    const bool isForwarded = m_request.headers.contains(Http::HEADER_X_FORWARDED_FOR)
        || m_request.headers.contains(u"cf-connecting-ip"_s) || m_request.headers.contains(u"forwarded"_s);
    if (!m_isLocalAuthEnabled && m_clientAddress.isLoopback() && !isForwarded)
        return false;
    if (m_isAuthSubnetWhitelistEnabled && Utils::Net::isIPInSubnets(m_clientAddress, m_authSubnetWhitelist))
        return false;
    return true;
}

bool WebApplication::isPublicAPI(const QString &scope, const QString &action) const
{
    return m_publicAPIs.contains(u"%1/%2"_s.arg(scope, action));
}

void WebApplication::sessionStart()
{
    sessionStartImpl(generateSid(), WebSessionType::CookieBased);
}

void WebApplication::sessionStartImpl(const QString &sessionId, const WebSessionType sessionType, const bool setCookie)
{
    Q_ASSERT(!m_currentSession);

    // remove outdated sessions
    Algorithm::removeIf(m_sessions, [this](const QString &, const WebSession *session)
    {
        if (session->hasExpired(m_sessionTimeout))
        {
            delete session;
            return true;
        }

        return false;
    });

    if (m_sessionsCountLimit > 0)
    {
        while (m_sessions.size() >= m_sessionsCountLimit)
        {
            const auto coldSessionIter = std::ranges::min_element(m_sessions
                    , [](const WebSession *lhs, const WebSession *rhs)
            {
                return lhs->timestamp() < rhs->timestamp();
            });
            delete *coldSessionIter;
            m_sessions.erase(coldSessionIter);
        }
    }

    Access::Identity identity = m_pendingIdentity.value_or(Access::Identity {});
    m_pendingIdentity.reset();
    if ((identity.kind == Access::IdentityKind::User) && setCookie)
    {
        // a new sign-in
        identity.sessionCreatedAt = Access::now();
        m_accessStore->createSession(sessionId, identity.userId, clientId()
            , m_request.headers.value(u"user-agent"_s));
    }

    m_currentSession = WebSession::create(sessionType, sessionId);
    m_currentSession->setIdentity(identity);
    m_sessions[m_currentSession->id()] = m_currentSession;
    if ((sessionType == WebSessionType::CookieBased) && setCookie)
        m_cookieBasedSessionStateChange = SessionStateChange::Start;

    m_currentSession->registerAPIController(u"access"_s, [this, app = app(), parent = m_currentSession] { return new AccessController(this, app, parent); });
    m_currentSession->registerAPIController(u"app"_s, [app = app(), parent = m_currentSession] { return new AppController(app, parent); });
    m_currentSession->registerAPIController(u"log"_s, [app = app(), parent = m_currentSession] { return new LogController(app, parent); });
    m_currentSession->registerAPIController(u"rss"_s, [app = app(), parent = m_currentSession] { return new RSSController(app, parent); });
    m_currentSession->registerAPIController(u"torrents"_s, [app = app(), parent = m_currentSession] { return new TorrentsController(app, parent); });
    m_currentSession->registerAPIController(u"transfer"_s, [app = app(), parent = m_currentSession] { return new TransferController(app, parent); });
    m_currentSession->registerAPIController(u"clientdata"_s
            , [this, app = app(), parent = m_currentSession, identity]
    {
        return new ClientDataController(clientDataStorage(identity, parent), app, parent);
    });
    m_currentSession->registerAPIController(u"torrentcreator"_s
            , [app = app(), parent = m_currentSession, torrentCreationManager = m_torrentCreationManager]
    {
        return new TorrentCreatorController(torrentCreationManager, app, parent);
    });
    m_currentSession->registerAPIController(u"search"_s
            , [app = app(), parent = m_currentSession, searchJobManager = m_searchJobManager]
    {
        return new SearchController(searchJobManager, app, parent);
    });
    m_currentSession->registerAPIController(u"sync"_s
            , [app = app(), parent = m_currentSession, btSession = BitTorrent::Session::instance()]
    {
        auto *syncController = new SyncController(app, parent);
        syncController->updateFreeDiskSpace(btSession->freeDiskSpace());
        connect(btSession, &BitTorrent::Session::freeDiskSpaceChecked, syncController, &SyncController::updateFreeDiskSpace);
        return syncController;
    });
}

void WebApplication::sessionEnd()
{
    Q_ASSERT(m_currentSession);

    if (m_currentSession->type() == WebSessionType::CookieBased)
        m_cookieBasedSessionStateChange = SessionStateChange::End;

    if (const Access::Identity identity = m_currentSession->identity()
        ; (identity.kind == Access::IdentityKind::User) || (identity.kind == Access::IdentityKind::Legacy))
    {
        if (identity.kind == Access::IdentityKind::User)
            m_accessStore->deleteSession(m_currentSession->id());
        logEvent(identity, u"login"_s, u"auth/logout"_s, u"Logged out"_s, u"ok"_s);
    }

    delete m_sessions.take(m_currentSession->id());
    m_currentSession = nullptr;
}

bool WebApplication::isOriginTrustworthy() const
{
    // https://w3c.github.io/webappsec-secure-contexts/#is-origin-trustworthy

    if (m_isReverseProxySupportEnabled)
    {
        const QString forwardedProto = request().headers.value(Http::HEADER_X_FORWARDED_PROTO);
        if (forwardedProto.compare(u"https", Qt::CaseInsensitive) == 0)
            return true;
    }

    if (m_isHttpsEnabled)
        return true;

    return false;
}

bool WebApplication::isCrossSiteRequest(const Http::Request &request) const
{
    // https://www.owasp.org/index.php/Cross-Site_Request_Forgery_(CSRF)_Prevention_Cheat_Sheet#Verifying_Same_Origin_with_Standard_Headers

    const auto isSameOrigin = [](const QUrl &left, const QUrl &right) -> bool
    {
        // [rfc6454] 5. Comparing Origins
        return ((left.port() == right.port())
                // && (left.scheme() == right.scheme())  // not present in this context
                && (left.host() == right.host()));
    };

    const QString targetOrigin = m_isReverseProxySupportEnabled
        ? request.headers.value(Http::HEADER_X_FORWARDED_HOST, request.headers.value(Http::HEADER_HOST))
        : request.headers.value(Http::HEADER_HOST);

    // Prefer 'Sec-Fetch-Site' when present: it is set by the browser itself, cannot be suppressed or tampered with
    // by the requesting page, and doesn't rely on the 'Host' header (e.g. behind a reverse proxy).
    // Browsers only send it to potentially trustworthy origins (HTTPS or localhost).
    if (const QString secFetchSiteValue = request.headers.value(Http::HEADER_SEC_FETCH_SITE); !secFetchSiteValue.isEmpty())
    {
        // "none" means the request wasn't initiated by a page (typed URL, bookmark, etc.)
        const bool isValid = (secFetchSiteValue.compare(u"same-origin", Qt::CaseInsensitive) == 0)
                || (secFetchSiteValue.compare(u"none", Qt::CaseInsensitive) == 0);
        if (!isValid)
        {
            LogMsg(tr("WebUI: Cross-site request blocked. Source IP: '%1'. Sec-Fetch-Site header: '%2'. Target origin: '%3'")
                   .arg(m_env.clientAddress.toString(), secFetchSiteValue, targetOrigin)
                   , Log::WARNING);
        }
        return !isValid;
    }

    const QString originValue = request.headers.value(Http::HEADER_ORIGIN);
    const QString refererValue = request.headers.value(Http::HEADER_REFERER);

    // sent with CORS requests, as well as with POST requests
    if (!originValue.isEmpty())
    {
        const bool isInvalid = !isSameOrigin(urlFromHostHeader(targetOrigin), originValue);
        if (isInvalid)
        {
            LogMsg(tr("WebUI: Origin header & Target origin mismatch! Source IP: '%1'. Origin header: '%2'. Target origin: '%3'")
                   .arg(m_env.clientAddress.toString(), originValue, targetOrigin)
                   , Log::WARNING);
        }
        return isInvalid;
    }

    if (!refererValue.isEmpty())
    {
        const bool isInvalid = !isSameOrigin(urlFromHostHeader(targetOrigin), refererValue);
        if (isInvalid)
        {
            LogMsg(tr("WebUI: Referer header & Target origin mismatch! Source IP: '%1'. Referer header: '%2'. Target origin: '%3'")
                   .arg(m_env.clientAddress.toString(), refererValue, targetOrigin)
                   , Log::WARNING);
        }
        return isInvalid;
    }

    // owasp.org recommends to block this request, but doing so will inevitably lead Web API users to spoof headers
    // so let's be permissive here (Web API clients don't send any of these headers)
    return false;
}

bool WebApplication::validateHostHeader() const
{
    const QUrl hostHeader = urlFromHostHeader(m_request.headers[Http::HEADER_HOST]);
    const QString requestHost = hostHeader.host();

    // (if present) try matching host header's port with local port
    const int requestPort = hostHeader.port();
    if ((requestPort != -1) && (m_env.localPort != requestPort))
    {
        LogMsg(tr("WebUI: Invalid Host header, port mismatch. Request source IP: '%1'. Server port: '%2'. Received Host header: '%3'")
               .arg(m_env.clientAddress.toString()).arg(m_env.localPort)
               .arg(m_request.headers[Http::HEADER_HOST])
                , Log::WARNING);
        return false;
    }

    // try matching host header with local address
    const bool sameAddr = m_env.localAddress.isEqual(QHostAddress(requestHost));
    if (sameAddr)
        return true;

    // try matching host header with domain list
    const bool hasMatch = std::ranges::any_of(asConst(m_serverDomains), [&requestHost](const QRegularExpression &expr)
    {
        return requestHost.contains(expr);
    });
    if (hasMatch)
        return true;

    LogMsg(tr("WebUI: Invalid Host header. Request source IP: '%1'. Received Host header: '%2'")
           .arg(m_env.clientAddress.toString(), m_request.headers[Http::HEADER_HOST])
            , Log::WARNING);
    return false;
}

QHostAddress WebApplication::resolveClientAddress() const
{
    if (!m_isReverseProxySupportEnabled)
        return m_env.clientAddress;

    // Only reverse proxy can overwrite client address
    if (!Utils::Net::isIPInSubnets(m_env.clientAddress, m_trustedReverseProxyList))
        return m_env.clientAddress;

    // Cloudflare puts the real visitor address in CF-Connecting-IP; visitors can't set it themselves.
    QHostAddress clientAddress;
    if (const QString connectingIP = m_request.headers.value(u"cf-connecting-ip"_s).trimmed(); !connectingIP.isEmpty()
        && clientAddress.setAddress(connectingIP))
    {
        return clientAddress.isLoopback() ? m_env.clientAddress : clientAddress;
    }

    const QString forwardedFor = m_request.headers.value(Http::HEADER_X_FORWARDED_FOR);
    clientAddress = Utils::Net::resolveForwardedClientAddress(m_env.clientAddress, forwardedFor, m_trustedReverseProxyList);

    // A forwarded address never counts as this PC itself, so it can't get "Bypass authentication for clients
    // on localhost".
    return clientAddress.isLoopback() ? m_env.clientAddress : clientAddress;
}

bool WebApplication::validateCredentials(const QStringView username, const QStringView password) const
{
    const QString clientAddr = clientId();
    const QString name = username.toString().trimmed().left(64);
    const auto logLogin = [this, &clientAddr](const QString &user, const QString &role, const QString &summary
            , const QString &outcome)
    {
        try
        {
            m_accessStore->logActivity(user, role, clientAddr, u"login"_s, u"auth/login"_s, summary, outcome);
        }
        catch (const RuntimeError &error)
        {
            // never fail a login because the activity log couldn't be written
            LogMsg(tr("WebUI access: couldn't write the activity log. %1").arg(error.message()), Log::WARNING);
        }
    };

    if (const int remaining = m_accessStore->lockoutRemaining(name, clientAddr); remaining > 0)
    {
        logLogin(name, {}, u"Login refused for '%1': account locked after too many failed attempts from this address"_s.arg(name), u"denied"_s);
        throw ForbiddenHTTPError(tr("This account is locked after too many failed login attempts. Try again in %1 minute(s).")
            .arg(std::max(1, (remaining + 59) / 60)));
    }

    if (isBanned())
    {
        LogMsg(tr("WebAPI login failure. Reason: IP has been banned, IP: %1, username: %2")
                .arg(clientAddr, username)
            , Log::WARNING);
        throw ForbiddenHTTPError(tr("Your IP address has been banned after too many failed authentication attempts."));
    }

    // personal accounts
    if (const std::optional<Access::User> account = m_accessStore->checkCredentials(name, password.toString()))
    {
        m_accessStore->clearLoginFailures(name);
        m_clientFailedLogins.remove(clientAddr);
        m_pendingIdentity = Access::Identity {.kind = Access::IdentityKind::User, .userId = account->id
            , .username = account->username, .roleName = account->roleName
            , .permissions = m_accessStore->userPermissions(account->id), .sessionCreatedAt = 0};
        // HTTP Basic authentication signs in on every request: note it once every 10 minutes
        const auto [authScheme, authData] = parseAuthorizationHeader(m_request.headers.value(Http::HEADER_AUTHORIZATION));
        const bool isBasicAuth = (authScheme.compare(BASIC_AUTH, Qt::CaseInsensitive) == 0) && !authData.isEmpty();
        if (!isBasicAuth)
        {
            logLogin(account->username, account->roleName, u"Logged in"_s, u"ok"_s);
            LogMsg(tr("WebAPI login success. IP: %1, user: %2").arg(clientAddr, account->username));
        }
        else if (!m_accessStore->recentActivityExists(account->username, u"auth/login"_s, u"ok"_s, 600))
        {
            logLogin(account->username, account->roleName, u"Logged in with HTTP Basic authentication"_s, u"ok"_s);
            LogMsg(tr("WebAPI login success. IP: %1, user: %2").arg(clientAddr, account->username));
        }
        return true;
    }

    // the shared login from qBittorrent's own settings: always allowed until the first account exists
    const bool isSharedLoginAllowed = !m_accessStore->isOpen() || !m_accessStore->hasUsers()
        || Preferences::instance()->isWebUISharedLoginAllowed();
    const bool usernameEqual = Utils::Password::slowEquals(username.toUtf8(), m_username.toUtf8());
    const bool passwordEqual = Utils::Password::PBKDF2::verify(m_passwordHash, password);

    if (isSharedLoginAllowed && usernameEqual && passwordEqual)
    {
        if (!m_accessStore->isOpen())
        {
            LogMsg(tr("WebUI access: the user database isn't open, so the shared WebUI login was accepted without roles or limits. IP: %1")
                .arg(clientAddr), Log::WARNING);
        }
        m_clientFailedLogins.remove(clientAddr);
        m_pendingIdentity = Access::fullAccessIdentity(Access::IdentityKind::Legacy, m_username);
        logLogin(m_pendingIdentity->logName(), {}, u"Logged in with the shared WebUI login"_s, u"ok"_s);
        LogMsg(tr("WebAPI login success. IP: %1").arg(clientAddr));
        return true;
    }

    // count the address ban first, so a user database error can't skip it
    const auto *pref = Preferences::instance();
    if (pref->getWebUIMaxAuthFailCount() > 0)
        increaseFailedAttempts();

    m_accessStore->recordLoginFailure(clientAddr, name);
    logLogin(name, {}, u"Failed login as '%1'"_s.arg(name), u"failed"_s);

    LogMsg(tr("WebAPI login failure. Reason: invalid credentials, attempt count: %1, IP: %2, username: %3")
            .arg(QString::number(failedAttemptsCount()), clientAddr, username)
        , Log::WARNING);
    return false;
}

bool WebApplication::validateBasicAuth(const QStringView credentials) const
{
    const QString decodedCredentials = QString::fromUtf8(QByteArray::fromBase64(credentials.toLatin1()));
    const auto usernamePassword = QStringView(decodedCredentials);
    if (const qsizetype idx = usernamePassword.indexOf(u':'); idx > 0)
    {
        const QStringView username = usernamePassword.first(idx);
        const QStringView password = usernamePassword.sliced(idx + 1);
        return validateCredentials(username, password);
    }

    return false;
}

bool WebApplication::isBanned() const
{
    const auto failedLoginIter = m_clientFailedLogins.constFind(clientId());
    if (failedLoginIter == m_clientFailedLogins.cend())
        return false;

    bool isBanned = (failedLoginIter->banTimer.remainingTime() >= 0);
    if (isBanned && failedLoginIter->banTimer.hasExpired())
    {
        m_clientFailedLogins.erase(failedLoginIter);
        isBanned = false;
    }

    return isBanned;
}

int WebApplication::failedAttemptsCount() const
{
    return m_clientFailedLogins.value(clientId()).failedAttemptsCount;
}

void WebApplication::increaseFailedAttempts() const
{
    Q_ASSERT(Preferences::instance()->getWebUIMaxAuthFailCount() > 0);

    FailedLogin &failedLogin = m_clientFailedLogins[clientId()];
    ++failedLogin.failedAttemptsCount;

    if (failedLogin.failedAttemptsCount >= Preferences::instance()->getWebUIMaxAuthFailCount())
    {
        // Max number of failed attempts reached
        // Start ban period
        failedLogin.banTimer.setRemainingTime(Preferences::instance()->getWebUIBanDuration());
    }
}

// Multi-user access control

Access::AccessStore *WebApplication::accessStore() const
{
    return m_accessStore;
}

Access::Identity WebApplication::currentIdentity() const
{
    return m_currentSession ? m_currentSession->identity() : Access::Identity {};
}

QString WebApplication::currentSessionId() const
{
    return m_currentSession ? m_currentSession->id() : QString();
}

QString WebApplication::currentClientAddress() const
{
    return clientId();
}

ClientDataStorage *WebApplication::clientDataStorage(const Access::Identity &identity, QObject *parent)
{
    // localhost, the API key and the shared login use qBittorrent's own shared copy
    if (identity.kind != Access::IdentityKind::User)
        return m_clientDataStorage;

    // until they save their own, people start from the shared copy
    return new ClientDataStorage(m_accessStore, identity.userId, m_clientDataStorage->loadData(), parent);
}

void WebApplication::logAccessEvent(const QString &category, const QString &endpoint, const QString &summary
        , const QString &outcome, const QJsonObject &details)
{
    logEvent(currentIdentity(), category, endpoint, summary, outcome, details);
}

void WebApplication::restoreStoredSession(const QString &sessionId)
{
    const std::optional<Access::StoredSession> stored = m_accessStore->session(sessionId);
    if (!stored)
    {
        // signed out elsewhere, expired, or the account was disabled or deleted
        if (m_currentSession)
        {
            delete m_sessions.take(sessionId);
            m_currentSession = nullptr;
        }
        return;
    }

    const Access::Identity identity {.kind = Access::IdentityKind::User, .userId = stored->userId
        , .username = stored->username, .roleName = stored->roleName, .permissions = stored->permissions
        , .sessionCreatedAt = stored->createdAt};
    if (m_currentSession)
    {
        // permissions are re-read every time, so role changes apply immediately
        m_currentSession->setIdentity(identity);
        return;
    }

    m_pendingIdentity = identity;
    sessionStartImpl(sessionId, WebSessionType::CookieBased, false);
}

void WebApplication::checkAccess(const QString &endpoint, QHash<QString, QString> &params, const QStringList &fileNames)
{
    const Access::Identity &identity = m_currentSession->identity();

    QStringList missing;
    for (const QString &permission : Access::requiredPermissions(endpoint, params))
    {
        if (!identity.can(permission))
            missing.append(permission);
    }

    if (!missing.isEmpty())
    {
        missing.sort();
        const Access::EndpointRule rule = Access::ruleFor(endpoint);
        QJsonObject details = Access::safeDetails(endpoint, params);
        details[u"missing"_s] = QJsonArray::fromStringList(missing);
        // read-only actions are polled often, so only note a blocked one every 10 minutes
        if (rule.logged || !m_accessStore->recentActivityExists(identity.logName(), endpoint, u"denied"_s, 600))
        {
            logEvent(identity, rule.category, endpoint, Access::describe(endpoint, params, torrentNames(params), fileNames)
                , u"denied"_s, details);
        }

        QStringList labels;
        for (const QString &permission : missing)
            labels.append(Access::permissionLabel(permission));
        m_accessDeniedPermissions = missing.join(u',');
        throw ForbiddenHTTPError(tr("Your role (%1) doesn't allow this: %2.").arg(identity.roleName, labels.join(u", "_s)));
    }

    // Without "Choose save locations", torrents and new categories use qBittorrent's default folders.
    if (!identity.can(u"save_location"_s))
    {
        for (const QString &key : Access::saveLocationParams(endpoint))
            params.remove(key);
    }

    // "added-by:" tags say who added a torrent, so only an admin may write one by hand.
    static const QStringList tagEndpoints {u"torrents/add"_s, u"search/downloadTorrent"_s, u"torrents/addTags"_s
        , u"torrents/setTags"_s, u"torrents/createTags"_s};
    if (tagEndpoints.contains(endpoint) && params.contains(u"tags"_s) && !identity.can(Access::ADMIN_PERMISSION))
    {
        QStringList tags;
        for (const QString &tag : params.value(u"tags"_s).split(u',', Qt::SkipEmptyParts))
        {
            if (!tag.trimmed().startsWith(u"added-by:"_s, Qt::CaseInsensitive))
                tags.append(tag);
        }
        params[u"tags"_s] = tags.join(u',');
    }

    // ... and only an admin may take one off. Tags that would clear or replace the "added-by:" tag of a torrent
    // are refused, because those calls can't leave a single tag alone.
    if (!identity.can(Access::ADMIN_PERMISSION))
    {
        if ((endpoint == u"torrents/removeTags") || (endpoint == u"torrents/deleteTags"))
        {
            const QStringList requested = params.value(u"tags"_s).split(u',', Qt::SkipEmptyParts);
            QStringList tags;
            for (const QString &tag : requested)
            {
                if (!isAddedByTag(tag))
                    tags.append(tag);
            }
            if (tags.isEmpty() && !requested.isEmpty())
                throw ForbiddenHTTPError(tr("Only an admin can remove the tag that says who added a torrent."));
            params[u"tags"_s] = tags.join(u',');
        }

        // removeTags without tags clears every tag; setTags replaces them all
        if ((endpoint == u"torrents/setTags")
            || ((endpoint == u"torrents/removeTags") && params.value(u"tags"_s).trimmed().isEmpty()))
        {
            if (anyTorrentHasAddedByTag(params))
                throw ForbiddenHTTPError(tr("Only an admin can clear or replace all tags of a torrent that has a tag saying who added it."));
        }
    }

    // Sign-in, security and run-program options belong to admins: "settings" alone leaves them out.
    if ((endpoint == u"app/setPreferences") && !identity.can(Access::ADMIN_PERMISSION))
    {
        QJsonObject preferences = QJsonDocument::fromJson(params.value(u"json"_s).toUtf8()).object();
        bool isChanged = false;
        for (const QString &key : Access::adminOnlyPreferences())
        {
            if (preferences.contains(key))
            {
                preferences.remove(key);
                isChanged = true;
            }
        }
        if (isChanged)
            params[u"json"_s] = QString::fromUtf8(QJsonDocument(preferences).toJson(QJsonDocument::Compact));
    }

    // Tag new torrents with the person who added them.
    if (((endpoint == u"torrents/add") || (endpoint == u"search/downloadTorrent"))
        && (identity.kind == Access::IdentityKind::User))
    {
        QStringList tags;
        for (const QString &tag : params.value(u"tags"_s).split(u',', Qt::SkipEmptyParts))
        {
            if (!tag.trimmed().isEmpty())
                tags.append(tag.trimmed());
        }
        const QString ownTag = u"added-by:"_s + identity.username;
        if (!tags.contains(ownTag))
            tags.append(ownTag);
        params[u"tags"_s] = tags.join(u',');
    }
}

void WebApplication::logRequest(const QString &endpoint, const QHash<QString, QString> &params
        , const QStringList &fileNames, const QHash<QString, QString> &names, const QString &outcome
        , const QString &error) const
{
    QJsonObject details = Access::safeDetails(endpoint, params);
    if (!error.isEmpty())
        details[u"response"_s] = error.left(300);
    logEvent(m_currentSession->identity(), Access::ruleFor(endpoint).category, endpoint
        , Access::describe(endpoint, params, names, fileNames), outcome, details);
}

void WebApplication::logEvent(const Access::Identity &identity, const QString &category, const QString &endpoint
        , const QString &summary, const QString &outcome, const QJsonObject &details) const
{
    try
    {
        m_accessStore->logActivity(identity.logName(), identity.logRole(), clientId(), category, endpoint, summary
            , outcome, details);
    }
    catch (const RuntimeError &error)
    {
        // never fail a request because the activity log couldn't be written
        LogMsg(tr("WebUI access: couldn't write the activity log. %1").arg(error.message()), Log::WARNING);
    }
}

QHash<QString, QString> WebApplication::torrentNames(const QHash<QString, QString> &params) const
{
    QHash<QString, QString> names;
    const QStringList hashes = Access::torrentHashes(params);
    if (hashes == QStringList {u"all"_s})
        return names;

    const auto *btSession = BitTorrent::Session::instance();
    for (const QString &hash : hashes.first(std::min<qsizetype>(hashes.size(), 200)))
    {
        if (const BitTorrent::Torrent *torrent = btSession->getTorrent(BitTorrent::TorrentID::fromString(hash)))
            names.insert(hash, torrent->name());
    }
    return names;
}
