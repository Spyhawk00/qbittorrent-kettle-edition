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

#include <functional>
#include <optional>

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>

#include "base/exceptions.h"
#include "base/path.h"

class QTimer;

namespace Access
{
    // Passwords are typed on a WebUI that can face the internet, so short ones are refused
    inline constexpr int MIN_PASSWORD_LENGTH = 12;
    // The newest rows an exported activity log holds; the file is built in memory
    inline constexpr int MAX_EXPORT_ROWS = 100000;

    // A change was refused. The message is safe to show to the user.
    class AccessError : public RuntimeError
    {
    public:
        using RuntimeError::RuntimeError;
    };

    struct User
    {
        qint64 id = -1;
        QString username;
        qint64 roleId = -1;
        QString roleName;
        bool disabled = false;
        double createdAt = 0;
        double lastLoginAt = 0;
    };

    struct StoredSession
    {
        qint64 id = -1;
        qint64 userId = -1;
        QString username;
        QString roleName;
        QSet<QString> permissions;
        double createdAt = 0;
    };

    struct ActivityFilter
    {
        QString user;
        QString category;
        QString outcome;
        QString text;
        std::optional<double> since;
        std::optional<double> until;
    };

    double now();

    // WebUI user accounts, roles, their sessions and the activity log (webui_access.db in the profile's data
    // folder). Settings live in Preferences, like the rest of the WebUI's.
    class AccessStore final : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(AccessStore)

    public:
        static void initInstance();
        static void freeInstance();
        static AccessStore *instance();

        bool isOpen() const;
        Path databasePath() const;
        bool hasUsers() const;

        // When a personal session stops being valid: a fixed time after sign-in, or after the WebUI session
        // timeout without use when "keep signed in" is off.
        static double sessionExpiry(double createdAt, double lastSeenAt);

        // roles
        QJsonArray listRoles() const;
        QJsonObject role(qint64 roleId) const;
        QJsonObject createRole(const QString &name, const QString &description, const QStringList &permissions);
        QJsonObject updateRole(qint64 roleId, const std::optional<QString> &name
                , const std::optional<QString> &description, const std::optional<QStringList> &permissions);
        void deleteRole(qint64 roleId);

        // users
        QJsonArray listUsers() const;
        std::optional<User> user(qint64 userId) const;
        std::optional<User> findUser(const QString &username) const;
        User createUser(const QString &username, const QString &password, qint64 roleId);
        User updateUser(qint64 userId, std::optional<qint64> roleId, std::optional<bool> disabled
                , const std::optional<QString> &password);
        void deleteUser(qint64 userId);
        std::optional<User> checkCredentials(const QString &username, const QString &password);
        bool verifyUserPassword(qint64 userId, const QString &password) const;
        QSet<QString> userPermissions(qint64 userId) const;

        // sessions (the session ID itself is never stored, only its SHA-256)
        void createSession(const QString &sessionId, qint64 userId, const QString &ip, const QString &userAgent);
        std::optional<StoredSession> session(const QString &sessionId);
        void deleteSession(const QString &sessionId);
        void deleteSessionById(qint64 id);
        void deleteUserSessions(qint64 userId, const QString &exceptSessionId = {});
        QJsonArray listSessions(const QString &currentSessionId) const;

        // failed logins (per account and address, so a stranger can't lock the owner out; the WebUI also bans addresses)
        void recordLoginFailure(const QString &ip, const QString &username);
        void clearLoginFailures(const QString &username);
        // with an address, only failures from that address count; without one, all failures of the account
        int lockoutRemaining(const QString &username, const QString &ip = {}) const;

        // WebUI display preferences of one user (see ClientDataStorage); none until they save some
        std::optional<QJsonObject> clientData(qint64 userId) const;
        void storeClientData(qint64 userId, const QJsonObject &data);

        // activity log
        void logActivity(const QString &username, const QString &role, const QString &ip, const QString &category
                , const QString &endpoint, const QString &summary, const QString &outcome
                , const QJsonObject &details = {});
        bool recentActivityExists(const QString &username, const QString &endpoint, const QString &outcome
                , int seconds) const;
        QJsonObject queryActivity(const ActivityFilter &filter, int limit, int offset) const;
        void forEachActivity(const ActivityFilter &filter, const std::function<void (const QJsonObject &)> &callback
                , int limit = -1) const;

        void purge();

    signals:
        void accountsChanged();
        void sessionsChanged();
        void activityAdded();

    private:
        explicit AccessStore(const Path &dbPath, QObject *parent = nullptr);
        ~AccessStore() override;

        void transaction(const std::function<void ()> &change);
        int adminCount() const;
        static QString hashSessionId(const QString &sessionId);

        static AccessStore *m_instance;

        QString m_connectionName;
        Path m_databasePath;
        bool m_isOpen = false;
        QTimer *m_purgeTimer = nullptr;
    };
}
