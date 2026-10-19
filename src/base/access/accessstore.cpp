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

#include "accessstore.h"

#include <algorithm>
#include <array>

#include <openssl/evp.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTimer>
#include <QUuid>

#include "base/global.h"
#include "base/logger.h"
#include "base/preferences.h"
#include "base/profile.h"
#include "base/utils/password.h"
#include "permissions.h"

using namespace std::chrono_literals;

namespace
{
    const QString LEGACY_HASH_PREFIX = u"pbkdf2_sha256$"_s;
    const int MIN_PASSWORD_LENGTH = 8;
    const int MAX_PASSWORD_LENGTH = 1024;

    const QStringList SCHEMA
    {
        u"CREATE TABLE IF NOT EXISTS roles (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE, "
            "description TEXT NOT NULL DEFAULT '', permissions TEXT NOT NULL DEFAULT '[]', created_at REAL NOT NULL)"_s,
        u"CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE COLLATE NOCASE, "
            "password_hash TEXT NOT NULL, role_id INTEGER NOT NULL REFERENCES roles(id), "
            "disabled INTEGER NOT NULL DEFAULT 0, created_at REAL NOT NULL, last_login_at REAL)"_s,
        u"CREATE TABLE IF NOT EXISTS sessions (id INTEGER PRIMARY KEY, token_hash TEXT NOT NULL UNIQUE, "
            "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE, created_at REAL NOT NULL, "
            "last_seen_at REAL NOT NULL, ip TEXT NOT NULL DEFAULT '', user_agent TEXT NOT NULL DEFAULT '', "
            "backend_cookie TEXT NOT NULL DEFAULT '')"_s,
        u"CREATE TABLE IF NOT EXISTS activity (id INTEGER PRIMARY KEY, ts REAL NOT NULL, "
            "username TEXT NOT NULL DEFAULT '', role TEXT NOT NULL DEFAULT '', ip TEXT NOT NULL DEFAULT '', "
            "category TEXT NOT NULL, endpoint TEXT NOT NULL DEFAULT '', summary TEXT NOT NULL, outcome TEXT NOT NULL, "
            "details TEXT NOT NULL DEFAULT '{}')"_s,
        u"CREATE INDEX IF NOT EXISTS activity_ts ON activity(ts)"_s,
        u"CREATE INDEX IF NOT EXISTS activity_user ON activity(username, ts)"_s,
        u"CREATE TABLE IF NOT EXISTS login_failures (id INTEGER PRIMARY KEY, ts REAL NOT NULL, ip TEXT NOT NULL, "
            "username TEXT NOT NULL COLLATE NOCASE)"_s,
        u"CREATE INDEX IF NOT EXISTS login_failures_ts ON login_failures(ts)"_s,
        u"CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)"_s,
        u"CREATE TABLE IF NOT EXISTS client_data (user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE, "
            "data TEXT NOT NULL)"_s,
    };

    QString toJsonText(const QStringList &list)
    {
        return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(list)).toJson(QJsonDocument::Compact));
    }

    QStringList fromJsonList(const QString &text)
    {
        QStringList list;
        for (const QJsonValue &value : QJsonDocument::fromJson(text.toUtf8()).array())
            list.append(value.toString());
        return list;
    }

    QStringList cleanPermissions(const QStringList &permissions)
    {
        QStringList clean;
        for (const QString &permission : permissions)
        {
            const QString trimmed = permission.trimmed();
            if (Access::isValidPermission(trimmed) && !clean.contains(trimmed))
                clean.append(trimmed);
        }
        clean.sort();
        return clean;
    }

    QString checkPasswordStrength(const QString &password)
    {
        if (password.size() < MIN_PASSWORD_LENGTH)
            return u"Password must be at least %1 characters long."_s.arg(MIN_PASSWORD_LENGTH);
        if (password.size() > MAX_PASSWORD_LENGTH)
            return u"Password is too long."_s;
        return {};
    }

    QString hashPassword(const QString &password)
    {
        return QString::fromLatin1(Utils::Password::PBKDF2::generate(password));
    }

    // Hashes created by the stand-alone Python gateway: pbkdf2_sha256$<iterations>$<salt>$<digest>
    bool verifyLegacyHash(const QString &password, const QString &encoded)
    {
        const QStringList parts = encoded.split(u'$');
        if (parts.size() != 4)
            return false;

        bool ok = false;
        const int iterations = parts[1].toInt(&ok);
        if (!ok || (iterations <= 0))
            return false;

        const QByteArray salt = QByteArray::fromBase64(parts[2].toLatin1());
        const QByteArray expected = QByteArray::fromBase64(parts[3].toLatin1());
        const QByteArray passwordData = password.toUtf8();
        std::array<unsigned char, 64> output {};
        if ((expected.size() <= 0) || (expected.size() > static_cast<qsizetype>(output.size())))
            return false;

        const int result = PKCS5_PBKDF2_HMAC(passwordData.constData(), static_cast<int>(passwordData.size())
            , reinterpret_cast<const unsigned char *>(salt.constData()), static_cast<int>(salt.size())
            , iterations, EVP_sha256(), static_cast<int>(expected.size()), output.data());
        if (result != 1)
            return false;

        return Utils::Password::slowEquals(expected
            , QByteArrayView(reinterpret_cast<const char *>(output.data()), expected.size()));
    }

    bool verifyPassword(const QString &password, const QString &encoded)
    {
        if (encoded.startsWith(LEGACY_HASH_PREFIX))
            return verifyLegacyHash(password, encoded);
        return Utils::Password::PBKDF2::verify(encoded.toLatin1(), password);
    }

    QString escapeLike(QString text)
    {
        text.replace(u"\\"_s, u"\\\\"_s);
        text.replace(u"%"_s, u"\\%"_s);
        text.replace(u"_"_s, u"\\_"_s);
        return text;
    }
}

double Access::now()
{
    return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

using namespace Access;

AccessStore *AccessStore::m_instance = nullptr;

void AccessStore::initInstance()
{
    if (!m_instance)
        m_instance = new AccessStore(specialFolderLocation(SpecialFolder::Data) / Path(u"webui_access.db"_s));
}

void AccessStore::freeInstance()
{
    delete m_instance;
    m_instance = nullptr;
}

AccessStore *AccessStore::instance()
{
    return m_instance;
}

double AccessStore::sessionExpiry(const double createdAt, const double lastSeenAt)
{
    const auto *pref = Preferences::instance();
    if (const int days = pref->getWebUIUserSignInDays(); days > 0)
        return createdAt + (days * 86400.0);
    if (const int timeout = pref->getWebUISessionTimeout(); timeout > 0)
        return lastSeenAt + timeout;
    return lastSeenAt + (365 * 86400.0);
}

AccessStore::AccessStore(const Path &dbPath, QObject *parent)
    : QObject(parent)
    , m_connectionName {u"WebUIAccess-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces)}
    , m_databasePath {dbPath}
{
    auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, m_connectionName);
    db.setDatabaseName(dbPath.data());
    db.setConnectOptions(u"QSQLITE_BUSY_TIMEOUT=5000"_s);
    if (!db.open())
    {
        LogMsg(tr("WebUI access: couldn't open the user database \"%1\". Error: %2")
            .arg(dbPath.toString(), db.lastError().text()), Log::CRITICAL);
        return;
    }

    QSqlQuery query {db};
    query.exec(u"PRAGMA journal_mode=WAL"_s);
    query.exec(u"PRAGMA foreign_keys=ON"_s);
    for (const QString &statement : SCHEMA)
    {
        if (!query.exec(statement))
        {
            LogMsg(tr("WebUI access: couldn't prepare the user database. Error: %1").arg(query.lastError().text())
                , Log::CRITICAL);
            return;
        }
    }

    query.exec(u"SELECT COUNT(*) FROM roles"_s);
    if (query.next() && (query.value(0).toInt() == 0))
    {
        for (const DefaultRole &role : defaultRoles())
        {
            QSqlQuery insert {db};
            insert.prepare(u"INSERT INTO roles (name, description, permissions, created_at) VALUES (?, ?, ?, ?)"_s);
            insert.addBindValue(role.name);
            insert.addBindValue(role.description);
            insert.addBindValue(toJsonText(cleanPermissions(role.permissions)));
            insert.addBindValue(now());
            insert.exec();
        }
    }

    m_isOpen = true;

    m_purgeTimer = new QTimer(this);
    connect(m_purgeTimer, &QTimer::timeout, this, &AccessStore::purge);
    m_purgeTimer->start(1h);
    purge();
}

AccessStore::~AccessStore()
{
    {
        auto db = QSqlDatabase::database(m_connectionName);
        db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool AccessStore::isOpen() const
{
    return m_isOpen;
}

Path AccessStore::databasePath() const
{
    return m_databasePath;
}

// ---- helpers ---------------------------------------------------------------------------------------

namespace
{
    // Qt binds a null QString as SQL NULL, but the text columns are NOT NULL.
    QVariant sqlValue(const QVariant &value)
    {
        if ((value.typeId() == QMetaType::QString) && value.toString().isNull())
            return QVariant(u""_s);
        return value;
    }

    QSqlQuery run(const QString &connectionName, const QString &sql, const QVariantList &binds = {})
    {
        QSqlQuery query {QSqlDatabase::database(connectionName)};
        query.prepare(sql);
        for (const QVariant &value : binds)
            query.addBindValue(sqlValue(value));
        if (!query.exec())
            throw RuntimeError(u"Database error: %1"_s.arg(query.lastError().text()));
        return query;
    }

    QJsonObject roleObject(const QSqlQuery &query)
    {
        return {
            {u"id"_s, query.value(u"id"_s).toLongLong()},
            {u"name"_s, query.value(u"name"_s).toString()},
            {u"description"_s, query.value(u"description"_s).toString()},
            {u"permissions"_s, QJsonArray::fromStringList(fromJsonList(query.value(u"permissions"_s).toString()))},
            {u"user_count"_s, query.value(u"user_count"_s).toInt()},
        };
    }

    User userFromQuery(const QSqlQuery &query)
    {
        return {
            .id = query.value(u"id"_s).toLongLong(),
            .username = query.value(u"username"_s).toString(),
            .roleId = query.value(u"role_id"_s).toLongLong(),
            .roleName = query.value(u"role_name"_s).toString(),
            .disabled = query.value(u"disabled"_s).toBool(),
            .createdAt = query.value(u"created_at"_s).toDouble(),
            .lastLoginAt = query.value(u"last_login_at"_s).toDouble(),
        };
    }

    QJsonObject userObject(const User &user)
    {
        return {
            {u"id"_s, user.id},
            {u"username"_s, user.username},
            {u"role_id"_s, user.roleId},
            {u"role_name"_s, user.roleName},
            {u"disabled"_s, user.disabled},
            {u"created_at"_s, user.createdAt},
            {u"last_login_at"_s, (user.lastLoginAt > 0) ? QJsonValue(user.lastLoginAt) : QJsonValue()},
        };
    }

    const QString USER_SELECT = u"SELECT u.id, u.username, u.role_id, u.disabled, u.created_at, u.last_login_at, "
        "r.name AS role_name FROM users u JOIN roles r ON r.id = u.role_id"_s;
    const QString ROLE_SELECT = u"SELECT r.*, (SELECT COUNT(*) FROM users u WHERE u.role_id = r.id) AS user_count "
        "FROM roles r"_s;
    const QRegularExpression USERNAME_PATTERN {u"^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$"_s};
    const QRegularExpression ROLE_NAME_PATTERN {u"^[^\\x00-\\x1f\\x7f]{1,40}$"_s};
}

void AccessStore::transaction(const std::function<void ()> &change)
{
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction())
        throw RuntimeError(u"Database error: %1"_s.arg(db.lastError().text()));
    try
    {
        // with no accounts yet, roles can be edited freely
        const bool hadUsers = hasUsers();
        change();
        if (hadUsers && (adminCount() < 1))
            throw AccessError(u"This would leave no active user who can manage users and roles."_s);
    }
    catch (...)
    {
        db.rollback();
        throw;
    }
    db.commit();

    emit accountsChanged();
    emit sessionsChanged();  // deleting or disabling a user also ends their sessions
}

int AccessStore::adminCount() const
{
    int count = 0;
    QSqlQuery query = run(m_connectionName
        , u"SELECT r.permissions FROM users u JOIN roles r ON r.id = u.role_id WHERE u.disabled = 0"_s);
    while (query.next())
    {
        if (fromJsonList(query.value(0).toString()).contains(ADMIN_PERMISSION))
            ++count;
    }
    return count;
}

QString AccessStore::hashSessionId(const QString &sessionId)
{
    return QString::fromLatin1(QCryptographicHash::hash(sessionId.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool AccessStore::hasUsers() const
{
    if (!m_isOpen)
        return false;
    QSqlQuery query = run(m_connectionName, u"SELECT COUNT(*) FROM users"_s);
    return query.next() && (query.value(0).toInt() > 0);
}

// ---- roles -----------------------------------------------------------------------------------------

QJsonArray AccessStore::listRoles() const
{
    QJsonArray roles;
    QSqlQuery query = run(m_connectionName, ROLE_SELECT + u" ORDER BY r.id"_s);
    while (query.next())
        roles.append(roleObject(query));
    return roles;
}

QJsonObject AccessStore::role(const qint64 roleId) const
{
    QSqlQuery query = run(m_connectionName, ROLE_SELECT + u" WHERE r.id = ?"_s, {roleId});
    return query.next() ? roleObject(query) : QJsonObject();
}

QJsonObject AccessStore::createRole(const QString &name, const QString &description, const QStringList &permissions)
{
    const QString cleanName = name.trimmed();
    if (!ROLE_NAME_PATTERN.match(cleanName).hasMatch())
        throw AccessError(u"Role name must be 1-40 characters."_s);

    qint64 roleId = -1;
    transaction([&]
    {
        QSqlQuery query {QSqlDatabase::database(m_connectionName)};
        query.prepare(u"INSERT INTO roles (name, description, permissions, created_at) VALUES (?, ?, ?, ?)"_s);
        query.addBindValue(cleanName);
        query.addBindValue(sqlValue(description.trimmed().left(200)));
        query.addBindValue(toJsonText(cleanPermissions(permissions)));
        query.addBindValue(now());
        if (!query.exec())
            throw AccessError(u"A role named '%1' already exists."_s.arg(cleanName));
        roleId = query.lastInsertId().toLongLong();
    });
    return role(roleId);
}

QJsonObject AccessStore::updateRole(const qint64 roleId, const std::optional<QString> &name
        , const std::optional<QString> &description, const std::optional<QStringList> &permissions)
{
    transaction([&]
    {
        if (role(roleId).isEmpty())
            throw AccessError(u"Role not found."_s);
        if (name)
        {
            const QString cleanName = name->trimmed();
            if (!ROLE_NAME_PATTERN.match(cleanName).hasMatch())
                throw AccessError(u"Role name must be 1-40 characters."_s);
            QSqlQuery query {QSqlDatabase::database(m_connectionName)};
            query.prepare(u"UPDATE roles SET name = ? WHERE id = ?"_s);
            query.addBindValue(cleanName);
            query.addBindValue(roleId);
            if (!query.exec())
                throw AccessError(u"A role named '%1' already exists."_s.arg(cleanName));
        }
        if (description)
            run(m_connectionName, u"UPDATE roles SET description = ? WHERE id = ?"_s, {description->trimmed().left(200), roleId});
        if (permissions)
            run(m_connectionName, u"UPDATE roles SET permissions = ? WHERE id = ?"_s, {toJsonText(cleanPermissions(*permissions)), roleId});
    });
    return role(roleId);
}

void AccessStore::deleteRole(const qint64 roleId)
{
    transaction([&]
    {
        QSqlQuery count = run(m_connectionName, u"SELECT COUNT(*) FROM users WHERE role_id = ?"_s, {roleId});
        if (count.next() && (count.value(0).toInt() > 0))
            throw AccessError(u"Move this role's users to another role before deleting it."_s);
        QSqlQuery query = run(m_connectionName, u"DELETE FROM roles WHERE id = ?"_s, {roleId});
        if (query.numRowsAffected() == 0)
            throw AccessError(u"Role not found."_s);
    });
}

// ---- users -----------------------------------------------------------------------------------------

QJsonArray AccessStore::listUsers() const
{
    QJsonArray users;
    QSqlQuery query = run(m_connectionName, USER_SELECT + u" ORDER BY u.username COLLATE NOCASE"_s);
    while (query.next())
        users.append(userObject(userFromQuery(query)));
    return users;
}

std::optional<User> AccessStore::user(const qint64 userId) const
{
    QSqlQuery query = run(m_connectionName, USER_SELECT + u" WHERE u.id = ?"_s, {userId});
    if (!query.next())
        return std::nullopt;
    return userFromQuery(query);
}

std::optional<User> AccessStore::findUser(const QString &username) const
{
    QSqlQuery query = run(m_connectionName, USER_SELECT + u" WHERE u.username = ?"_s, {username});
    if (!query.next())
        return std::nullopt;
    return userFromQuery(query);
}

User AccessStore::createUser(const QString &username, const QString &password, const qint64 roleId)
{
    const QString cleanName = username.trimmed();
    if (!USERNAME_PATTERN.match(cleanName).hasMatch())
    {
        throw AccessError(u"Username must be 1-32 characters: letters, numbers, dot, dash or underscore, "
            "starting with a letter or number."_s);
    }
    if (const QString error = checkPasswordStrength(password); !error.isEmpty())
        throw AccessError(error);
    if (!hasUsers() && !role(roleId).value(u"permissions"_s).toArray().contains(ADMIN_PERMISSION))
        throw AccessError(u"The first user must have a role that can manage users and roles."_s);

    const QString passwordHash = hashPassword(password);
    qint64 userId = -1;
    transaction([&]
    {
        if (role(roleId).isEmpty())
            throw AccessError(u"Role not found."_s);
        QSqlQuery query {QSqlDatabase::database(m_connectionName)};
        query.prepare(u"INSERT INTO users (username, password_hash, role_id, created_at) VALUES (?, ?, ?, ?)"_s);
        query.addBindValue(cleanName);
        query.addBindValue(passwordHash);
        query.addBindValue(roleId);
        query.addBindValue(now());
        if (!query.exec())
            throw AccessError(u"A user named '%1' already exists."_s.arg(cleanName));
        userId = query.lastInsertId().toLongLong();
    });
    return user(userId).value();
}

User AccessStore::updateUser(const qint64 userId, const std::optional<qint64> roleId
        , const std::optional<bool> disabled, const std::optional<QString> &password)
{
    QString passwordHash;
    if (password)
    {
        if (const QString error = checkPasswordStrength(*password); !error.isEmpty())
            throw AccessError(error);
        passwordHash = hashPassword(*password);
    }

    transaction([&]
    {
        if (!user(userId))
            throw AccessError(u"User not found."_s);
        if (roleId)
        {
            if (role(*roleId).isEmpty())
                throw AccessError(u"Role not found."_s);
            run(m_connectionName, u"UPDATE users SET role_id = ? WHERE id = ?"_s, {*roleId, userId});
        }
        if (disabled)
        {
            run(m_connectionName, u"UPDATE users SET disabled = ? WHERE id = ?"_s, {*disabled ? 1 : 0, userId});
            if (*disabled)
                run(m_connectionName, u"DELETE FROM sessions WHERE user_id = ?"_s, {userId});
        }
        if (!passwordHash.isEmpty())
            run(m_connectionName, u"UPDATE users SET password_hash = ? WHERE id = ?"_s, {passwordHash, userId});
    });
    return user(userId).value();
}

void AccessStore::deleteUser(const qint64 userId)
{
    transaction([&]
    {
        run(m_connectionName, u"DELETE FROM sessions WHERE user_id = ?"_s, {userId});
        run(m_connectionName, u"DELETE FROM client_data WHERE user_id = ?"_s, {userId});
        QSqlQuery query = run(m_connectionName, u"DELETE FROM users WHERE id = ?"_s, {userId});
        if (query.numRowsAffected() == 0)
            throw AccessError(u"User not found."_s);
    });
}

std::optional<User> AccessStore::checkCredentials(const QString &username, const QString &password)
{
    if (!m_isOpen)
        return std::nullopt;

    QSqlQuery query = run(m_connectionName, u"SELECT id, password_hash, disabled FROM users WHERE username = ?"_s
        , {username.trimmed()});
    if (!query.next())
    {
        // Spend the same time as a real check so response times don't reveal which usernames exist.
        static const QString dummyHash = hashPassword(u"dummy-password"_s);
        verifyPassword(password, dummyHash);
        return std::nullopt;
    }

    const qint64 userId = query.value(0).toLongLong();
    const QString storedHash = query.value(1).toString();
    if (!verifyPassword(password, storedHash) || query.value(2).toBool())
        return std::nullopt;

    // Upgrade hashes made by the stand-alone gateway to qBittorrent's own format.
    if (storedHash.startsWith(LEGACY_HASH_PREFIX))
        run(m_connectionName, u"UPDATE users SET password_hash = ? WHERE id = ?"_s, {hashPassword(password), userId});

    return user(userId);
}

bool AccessStore::verifyUserPassword(const qint64 userId, const QString &password) const
{
    QSqlQuery query = run(m_connectionName, u"SELECT password_hash FROM users WHERE id = ?"_s, {userId});
    return query.next() && verifyPassword(password, query.value(0).toString());
}

QSet<QString> AccessStore::userPermissions(const qint64 userId) const
{
    QSqlQuery query = run(m_connectionName
        , u"SELECT r.permissions FROM users u JOIN roles r ON r.id = u.role_id WHERE u.id = ? AND u.disabled = 0"_s
        , {userId});
    if (!query.next())
        return {};
    const QStringList list = fromJsonList(query.value(0).toString());
    return {list.cbegin(), list.cend()};
}

// ---- sessions --------------------------------------------------------------------------------------

void AccessStore::createSession(const QString &sessionId, const qint64 userId, const QString &ip, const QString &userAgent)
{
    const double timestamp = now();
    run(m_connectionName, u"INSERT INTO sessions (token_hash, user_id, created_at, last_seen_at, ip, user_agent) "
        "VALUES (?, ?, ?, ?, ?, ?)"_s, {hashSessionId(sessionId), userId, timestamp, timestamp, ip, userAgent.left(300)});
    run(m_connectionName, u"UPDATE users SET last_login_at = ? WHERE id = ?"_s, {timestamp, userId});
    emit sessionsChanged();
}

std::optional<StoredSession> AccessStore::session(const QString &sessionId)
{
    if (!m_isOpen || sessionId.isEmpty())
        return std::nullopt;

    QSqlQuery query = run(m_connectionName
        , u"SELECT s.id, s.user_id, s.created_at, s.last_seen_at, u.username, u.disabled, r.name AS role_name, "
          "r.permissions FROM sessions s JOIN users u ON u.id = s.user_id JOIN roles r ON r.id = u.role_id "
          "WHERE s.token_hash = ?"_s, {hashSessionId(sessionId)});
    if (!query.next())
        return std::nullopt;

    const qint64 id = query.value(u"id"_s).toLongLong();
    const double createdAt = query.value(u"created_at"_s).toDouble();
    const double timestamp = now();
    if (query.value(u"disabled"_s).toBool()
        || (timestamp > sessionExpiry(createdAt, query.value(u"last_seen_at"_s).toDouble())))
    {
        run(m_connectionName, u"DELETE FROM sessions WHERE id = ?"_s, {id});
        emit sessionsChanged();
        return std::nullopt;
    }
    if ((timestamp - query.value(u"last_seen_at"_s).toDouble()) > 60)
        run(m_connectionName, u"UPDATE sessions SET last_seen_at = ? WHERE id = ?"_s, {timestamp, id});

    const QStringList permissions = fromJsonList(query.value(u"permissions"_s).toString());
    return StoredSession {
        .id = id,
        .userId = query.value(u"user_id"_s).toLongLong(),
        .username = query.value(u"username"_s).toString(),
        .roleName = query.value(u"role_name"_s).toString(),
        .permissions = {permissions.cbegin(), permissions.cend()},
        .createdAt = createdAt,
    };
}

void AccessStore::deleteSession(const QString &sessionId)
{
    run(m_connectionName, u"DELETE FROM sessions WHERE token_hash = ?"_s, {hashSessionId(sessionId)});
    emit sessionsChanged();
}

void AccessStore::deleteSessionById(const qint64 id)
{
    run(m_connectionName, u"DELETE FROM sessions WHERE id = ?"_s, {id});
    emit sessionsChanged();
}

void AccessStore::deleteUserSessions(const qint64 userId, const QString &exceptSessionId)
{
    run(m_connectionName, u"DELETE FROM sessions WHERE user_id = ? AND token_hash != ?"_s
        , {userId, exceptSessionId.isEmpty() ? QString() : hashSessionId(exceptSessionId)});
    emit sessionsChanged();
}

QJsonArray AccessStore::listSessions(const QString &currentSessionId) const
{
    const QString currentHash = hashSessionId(currentSessionId);
    QJsonArray sessions;
    QSqlQuery query = run(m_connectionName
        , u"SELECT s.id, s.token_hash, s.created_at, s.last_seen_at, s.ip, s.user_agent, u.username "
          "FROM sessions s JOIN users u ON u.id = s.user_id ORDER BY s.last_seen_at DESC"_s);
    while (query.next())
    {
        sessions.append(QJsonObject {
            {u"id"_s, query.value(u"id"_s).toLongLong()},
            {u"username"_s, query.value(u"username"_s).toString()},
            {u"ip"_s, query.value(u"ip"_s).toString()},
            {u"user_agent"_s, query.value(u"user_agent"_s).toString()},
            {u"created_at"_s, query.value(u"created_at"_s).toDouble()},
            {u"last_seen_at"_s, query.value(u"last_seen_at"_s).toDouble()},
            {u"expires_at"_s, sessionExpiry(query.value(u"created_at"_s).toDouble(), query.value(u"last_seen_at"_s).toDouble())},
            {u"current"_s, query.value(u"token_hash"_s).toString() == currentHash},
        });
    }
    return sessions;
}

// ---- failed logins ---------------------------------------------------------------------------------

void AccessStore::recordLoginFailure(const QString &ip, const QString &username)
{
    if (!m_isOpen)
        return;
    run(m_connectionName, u"INSERT INTO login_failures (ts, ip, username) VALUES (?, ?, ?)"_s
        , {now(), ip, username.left(64)});
}

void AccessStore::clearLoginFailures(const QString &username)
{
    if (!m_isOpen)
        return;
    run(m_connectionName, u"DELETE FROM login_failures WHERE username = ?"_s, {username});
}

int AccessStore::lockoutRemaining(const QString &username) const
{
    const auto *pref = Preferences::instance();
    const int limit = pref->getWebUIUserMaxAuthFailCount();
    if (!m_isOpen || (limit <= 0) || username.isEmpty())
        return 0;

    const double window = static_cast<double>(pref->getWebUIUserLockDuration().count());
    QSqlQuery query = run(m_connectionName
        , u"SELECT ts FROM login_failures WHERE username = ? AND ts > ? ORDER BY ts DESC LIMIT ?"_s
        , {username, now() - window, limit});
    int count = 0;
    double oldest = 0;
    while (query.next())
    {
        ++count;
        oldest = query.value(0).toDouble();
    }
    if (count < limit)
        return 0;

    const double remaining = oldest + window - now();
    return (remaining > 0) ? (static_cast<int>(remaining) + 1) : 0;
}

// ---- client data -----------------------------------------------------------------------------------

std::optional<QJsonObject> AccessStore::clientData(const qint64 userId) const
{
    if (!m_isOpen)
        return std::nullopt;
    QSqlQuery query = run(m_connectionName, u"SELECT data FROM client_data WHERE user_id = ?"_s, {userId});
    if (!query.next())
        return std::nullopt;
    return QJsonDocument::fromJson(query.value(0).toString().toUtf8()).object();
}

void AccessStore::storeClientData(const qint64 userId, const QJsonObject &data)
{
    run(m_connectionName, u"INSERT INTO client_data (user_id, data) VALUES (?, ?) "
        "ON CONFLICT(user_id) DO UPDATE SET data = excluded.data"_s
        , {userId, QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact))});
}

// ---- activity log ----------------------------------------------------------------------------------

void AccessStore::logActivity(const QString &username, const QString &role, const QString &ip, const QString &category
        , const QString &endpoint, const QString &summary, const QString &outcome, const QJsonObject &details)
{
    if (!m_isOpen)
        return;
    run(m_connectionName, u"INSERT INTO activity (ts, username, role, ip, category, endpoint, summary, outcome, details) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"_s
        , {now(), username, role, ip, category, endpoint, summary.left(2000), outcome
        , QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Compact)).left(20000)});
    emit activityAdded();
}

bool AccessStore::recentActivityExists(const QString &username, const QString &endpoint, const QString &outcome
        , const int seconds) const
{
    QSqlQuery query = run(m_connectionName
        , u"SELECT 1 FROM activity WHERE username = ? AND endpoint = ? AND outcome = ? AND ts > ? LIMIT 1"_s
        , {username, endpoint, outcome, now() - seconds});
    return query.next();
}

namespace
{
    std::pair<QString, QVariantList> activityWhere(const ActivityFilter &filter)
    {
        QStringList clauses;
        QVariantList binds;
        if (!filter.user.isEmpty())
        {
            clauses.append(u"username = ? COLLATE NOCASE"_s);
            binds.append(filter.user);
        }
        if (!filter.category.isEmpty())
        {
            clauses.append(u"category = ?"_s);
            binds.append(filter.category);
        }
        if (!filter.outcome.isEmpty())
        {
            clauses.append(u"outcome = ?"_s);
            binds.append(filter.outcome);
        }
        if (filter.since)
        {
            clauses.append(u"ts >= ?"_s);
            binds.append(*filter.since);
        }
        if (filter.until)
        {
            clauses.append(u"ts < ?"_s);
            binds.append(*filter.until);
        }
        if (!filter.text.isEmpty())
        {
            clauses.append(u"(summary LIKE ? ESCAPE '\\' OR ip LIKE ? ESCAPE '\\')"_s);
            const QString pattern = u"%"_s + escapeLike(filter.text) + u"%"_s;
            binds.append(pattern);
            binds.append(pattern);
        }
        return {clauses.isEmpty() ? QString() : (u" WHERE "_s + clauses.join(u" AND "_s)), binds};
    }

    QJsonObject activityObject(const QSqlQuery &query)
    {
        return {
            {u"id"_s, query.value(u"id"_s).toLongLong()},
            {u"ts"_s, query.value(u"ts"_s).toDouble()},
            {u"username"_s, query.value(u"username"_s).toString()},
            {u"role"_s, query.value(u"role"_s).toString()},
            {u"ip"_s, query.value(u"ip"_s).toString()},
            {u"category"_s, query.value(u"category"_s).toString()},
            {u"endpoint"_s, query.value(u"endpoint"_s).toString()},
            {u"summary"_s, query.value(u"summary"_s).toString()},
            {u"outcome"_s, query.value(u"outcome"_s).toString()},
            {u"details"_s, QJsonDocument::fromJson(query.value(u"details"_s).toString().toUtf8()).object()},
        };
    }
}

QJsonObject AccessStore::queryActivity(const ActivityFilter &filter, const int limit, const int offset) const
{
    const auto [where, binds] = activityWhere(filter);
    QSqlQuery count = run(m_connectionName, u"SELECT COUNT(*) FROM activity"_s + where, binds);
    const int total = count.next() ? count.value(0).toInt() : 0;

    QJsonArray items;
    QSqlQuery query = run(m_connectionName, u"SELECT * FROM activity"_s + where + u" ORDER BY ts DESC, id DESC LIMIT ? OFFSET ?"_s
        , binds + QVariantList {limit, offset});
    while (query.next())
        items.append(activityObject(query));

    QJsonArray users;
    QSqlQuery names = run(m_connectionName
        , u"SELECT DISTINCT username FROM activity WHERE username != '' ORDER BY username COLLATE NOCASE"_s);
    while (names.next())
        users.append(names.value(0).toString());

    return {{u"total"_s, total}, {u"items"_s, items}, {u"users"_s, users}};
}

void AccessStore::forEachActivity(const ActivityFilter &filter, const std::function<void (const QJsonObject &)> &callback) const
{
    const auto [where, binds] = activityWhere(filter);
    QSqlQuery query = run(m_connectionName, u"SELECT * FROM activity"_s + where + u" ORDER BY ts DESC, id DESC"_s, binds);
    while (query.next())
        callback(activityObject(query));
}

void AccessStore::purge()
{
    if (!m_isOpen)
        return;
    try
    {
        const auto *pref = Preferences::instance();
        const double timestamp = now();

        QList<qint64> expired;
        QSqlQuery sessions = run(m_connectionName, u"SELECT id, created_at, last_seen_at FROM sessions"_s);
        while (sessions.next())
        {
            if (timestamp > sessionExpiry(sessions.value(1).toDouble(), sessions.value(2).toDouble()))
                expired.append(sessions.value(0).toLongLong());
        }
        for (const qint64 id : asConst(expired))
            run(m_connectionName, u"DELETE FROM sessions WHERE id = ?"_s, {id});
        if (!expired.isEmpty())
            emit sessionsChanged();

        run(m_connectionName, u"DELETE FROM activity WHERE ts < ?"_s
            , {timestamp - (pref->getWebUIActivityLogDays() * 86400.0)});
        run(m_connectionName, u"DELETE FROM login_failures WHERE ts < ?"_s
            , {timestamp - std::max(86400.0, static_cast<double>(pref->getWebUIUserLockDuration().count()))});
    }
    catch (const RuntimeError &error)
    {
        LogMsg(tr("WebUI access: housekeeping failed. %1").arg(error.message()), Log::WARNING);
    }
}
