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

#include "accesscontroller.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

#include "base/access/accessstore.h"
#include "base/access/activitytext.h"
#include "base/access/permissions.h"
#include "base/global.h"
#include "apierror.h"

using namespace Access;

namespace
{
    QJsonObject namesObject(const QList<ActivityName> &names)
    {
        QJsonObject object;
        for (const ActivityName &name : names)
            object.insert(name.id, name.label);
        return object;
    }

    qint64 requireId(const StringMap &params, const QString &name)
    {
        bool ok = false;
        const qint64 id = params.value(name).toLongLong(&ok);
        if (!ok)
            throw APIError(APIErrorType::BadParams, u"%1 must be a number."_s.arg(name));
        return id;
    }

    std::optional<double> optionalNumber(const StringMap &params, const QString &name)
    {
        const QString value = params.value(name);
        if (value.isEmpty())
            return std::nullopt;
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok)
            throw APIError(APIErrorType::BadParams, u"%1 must be a number."_s.arg(name));
        return number;
    }

    QStringList splitList(const QString &value)
    {
        return value.split(u',', Qt::SkipEmptyParts);
    }

    ActivityFilter activityFilter(const StringMap &params)
    {
        return {
            .user = params.value(u"user"_s),
            .category = params.value(u"category"_s),
            .outcome = params.value(u"outcome"_s),
            .text = params.value(u"q"_s).trimmed().left(200),
            .since = optionalNumber(params, u"since"_s),
            .until = optionalNumber(params, u"until"_s),
        };
    }

    QString labels(const QStringList &permissions)
    {
        QStringList result;
        for (const QString &permission : permissions)
            result.append(permissionLabel(permission));
        return result.join(u", "_s);
    }
}

AccessController::AccessController(IAccessContext *context, IApplication *app, QObject *parent)
    : APIController(app, parent)
    , m_context {context}
{
}

AccessStore *AccessController::store() const
{
    AccessStore *accessStore = m_context->accessStore();
    if (!accessStore || !accessStore->isOpen())
        throw APIError(APIErrorType::Conflict, u"The user database is not available. Check qBittorrent's log."_s);
    return accessStore;
}

Identity AccessController::require(const QString &permission) const
{
    const Identity identity = m_context->currentIdentity();
    if (!identity.can(permission))
        throw APIError(APIErrorType::AccessDenied, u"Your role doesn't allow this: %1."_s.arg(permissionLabel(permission)));
    return identity;
}

void AccessController::audit(const QString &summary, const QJsonObject &details)
{
    m_context->logAccessEvent(u"admin"_s, u"access/admin"_s, summary, u"ok"_s, details);
}

template <typename Func>
void AccessController::guarded(Func &&func)
{
    try
    {
        func();
    }
    catch (const AccessError &error)
    {
        throw APIError(APIErrorType::BadParams, error.message());
    }
    catch (const RuntimeError &error)
    {
        throw APIError(APIErrorType::Conflict, error.message());
    }
}

// ---- the current user --------------------------------------------------------------------------

void AccessController::meAction()
{
    const Identity identity = m_context->currentIdentity();
    QStringList permissions {identity.permissions.cbegin(), identity.permissions.cend()};
    permissions.sort();

    const char *kinds[] = {"none", "user", "localhost", "api_key", "shared", "desktop"};
    QJsonObject result
    {
        {u"username"_s, (identity.kind == IdentityKind::User) ? identity.username : identity.logName()},
        {u"role"_s, (identity.kind == IdentityKind::User) ? identity.roleName : u"Full access"_s},
        {u"kind"_s, QString::fromLatin1(kinds[static_cast<int>(identity.kind)])},
        {u"permissions"_s, QJsonArray::fromStringList(permissions)},
    };
    if (identity.kind == IdentityKind::User)
    {
        result.insert(u"session_started"_s, identity.sessionCreatedAt);
        result.insert(u"session_expires"_s, AccessStore::sessionExpiry(identity.sessionCreatedAt, now()));
    }
    setResult(result);
}

void AccessController::changePasswordAction()
{
    requireParams({u"current_password"_s, u"new_password"_s});
    const Identity identity = m_context->currentIdentity();
    if (identity.kind != IdentityKind::User)
        throw APIError(APIErrorType::Conflict, u"Only personal accounts can change their password here."_s);

    guarded([&]
    {
        // wrong current passwords count towards the account lock, like failed logins
        if (const int remaining = store()->lockoutRemaining(identity.username); remaining > 0)
        {
            throw AccessError(u"This account is locked after too many wrong passwords. Try again in %1 minute(s)."_s
                .arg(std::max(1, (remaining + 59) / 60)));
        }
        if (!store()->verifyUserPassword(identity.userId, params()[u"current_password"_s]))
        {
            store()->recordLoginFailure(m_context->currentClientAddress(), identity.username);
            m_context->logAccessEvent(u"security"_s, u"access/changePassword"_s
                , u"Wrong current password when changing password"_s, u"failed"_s);
            throw AccessError(u"Your current password is wrong."_s);
        }
        store()->clearLoginFailures(identity.username);
        store()->updateUser(identity.userId, std::nullopt, std::nullopt, params()[u"new_password"_s]);
        store()->deleteUserSessions(identity.userId, m_context->currentSessionId());
    });
    audit(u"Changed own password (other devices signed out)"_s);
    setResult(QJsonObject {{u"ok"_s, true}});
}

void AccessController::catalogAction()
{
    QJsonArray permissionList;
    for (const Permission &permission : permissions())
    {
        permissionList.append(QJsonObject {
            {u"id"_s, permission.id},
            {u"label"_s, permission.label},
            {u"description"_s, permission.description},
            {u"group"_s, permission.group},
            {u"dangerous"_s, permission.dangerous},
        });
    }

    setResult(QJsonObject {
        {u"permissions"_s, permissionList},
        {u"categories"_s, namesObject(activityCategories())},
        {u"outcomes"_s, namesObject(activityOutcomes())},
        {u"password_min_length"_s, MIN_PASSWORD_LENGTH},
    });
}

// ---- activity log ------------------------------------------------------------------------------

void AccessController::activityAction()
{
    require(u"activity_log"_s);
    const int limit = std::clamp(params().value(u"limit"_s, u"100"_s).toInt(), 1, 500);
    const int offset = std::max(params().value(u"offset"_s).toInt(), 0);
    guarded([&] { setResult(store()->queryActivity(activityFilter(params()), limit, offset)); });
}

void AccessController::exportActivityAction()
{
    require(u"activity_log"_s);
    // UTF-8 byte order mark, so spreadsheet programs read the file as UTF-8
    QString csv = QChar(0xFEFF) + activityCsvHeader() + u"\r\n"_s;
    guarded([&]
    {
        store()->forEachActivity(activityFilter(params()), [&csv](const QJsonObject &row)
        {
            csv += activityCsvLine(row) + u"\r\n"_s;
        }, MAX_EXPORT_ROWS);
    });
    const QString filename = QDateTime::currentDateTime().toString(u"'qbittorrent-activity-'yyyyMMdd-HHmmss'.csv'"_s);
    setResult(csv.toUtf8(), u"text/csv; charset=utf-8"_s, filename);
}

// GET param: since (seconds since 1970; the start of this month if missing)
void AccessController::addedTorrentsAction()
{
    require(u"activity_log"_s);
    const QDate today = QDate::currentDate();
    const double since = params().contains(u"since"_s)
        ? params()[u"since"_s].toDouble()
        : QDateTime(QDate(today.year(), today.month(), 1), QTime(0, 0)).toSecsSinceEpoch();
    guarded([&] { setResult(store()->countAddedTorrents(since)); });
}

// ---- users -------------------------------------------------------------------------------------

void AccessController::usersAction()
{
    require(ADMIN_PERMISSION);
    guarded([&] { setResult(QJsonObject {{u"users"_s, store()->listUsers()}, {u"roles"_s, store()->listRoles()}}); });
}

void AccessController::createUserAction()
{
    require(ADMIN_PERMISSION);
    requireParams({u"username"_s, u"password"_s, u"role_id"_s});
    guarded([&]
    {
        const User user = store()->createUser(params()[u"username"_s], params()[u"password"_s]
            , requireId(params(), u"role_id"_s));
        audit(u"Created user '%1' with role '%2'"_s.arg(user.username, user.roleName));
        setResult(QJsonObject {{u"id"_s, user.id}, {u"username"_s, user.username}, {u"role_name"_s, user.roleName}});
    });
}

void AccessController::updateUserAction()
{
    const Identity identity = require(ADMIN_PERMISSION);
    requireParams({u"id"_s});
    const qint64 userId = requireId(params(), u"id"_s);

    guarded([&]
    {
        const std::optional<User> before = store()->user(userId);
        if (!before)
            throw AccessError(u"User not found."_s);

        std::optional<qint64> roleId;
        std::optional<bool> disabled;
        std::optional<QString> password;
        if (params().contains(u"role_id"_s))
            roleId = requireId(params(), u"role_id"_s);
        if (params().contains(u"disabled"_s))
            disabled = isTrue(params()[u"disabled"_s]);
        if (params().contains(u"password"_s))
            password = params()[u"password"_s];
        if ((identity.kind == IdentityKind::User) && (userId == identity.userId) && disabled.value_or(false))
            throw AccessError(u"You can't disable your own account."_s);

        const User after = store()->updateUser(userId, roleId, disabled, password);
        if (password)
            store()->deleteUserSessions(userId, m_context->currentSessionId());

        QStringList changes;
        if (roleId && (before->roleId != *roleId))
            changes.append(u"role %1 → %2"_s.arg(before->roleName, after.roleName));
        if (disabled && (before->disabled != *disabled))
            changes.append(*disabled ? u"disabled"_s : u"enabled"_s);
        if (password)
            changes.append(u"password reset"_s);
        if (!changes.isEmpty())
            audit(u"Changed user '%1': %2"_s.arg(after.username, changes.join(u", "_s)));
        setResult(QJsonObject {{u"ok"_s, true}});
    });
}

void AccessController::deleteUserAction()
{
    const Identity identity = require(ADMIN_PERMISSION);
    requireParams({u"id"_s});
    const qint64 userId = requireId(params(), u"id"_s);
    if ((identity.kind == IdentityKind::User) && (userId == identity.userId))
        throw APIError(APIErrorType::BadParams, u"You can't delete your own account."_s);

    guarded([&]
    {
        const std::optional<User> user = store()->user(userId);
        if (!user)
            throw AccessError(u"User not found."_s);
        store()->deleteUser(userId);
        audit(u"Deleted user '%1'"_s.arg(user->username));
        setResult(QJsonObject {{u"ok"_s, true}});
    });
}

void AccessController::signOutUserAction()
{
    require(ADMIN_PERMISSION);
    requireParams({u"id"_s});
    const qint64 userId = requireId(params(), u"id"_s);
    guarded([&]
    {
        const std::optional<User> user = store()->user(userId);
        if (!user)
            throw AccessError(u"User not found."_s);
        store()->deleteUserSessions(userId, m_context->currentSessionId());
        audit(u"Signed out '%1' everywhere"_s.arg(user->username));
        setResult(QJsonObject {{u"ok"_s, true}});
    });
}

// ---- roles -------------------------------------------------------------------------------------

void AccessController::rolesAction()
{
    require(ADMIN_PERMISSION);
    guarded([&] { setResult(QJsonObject {{u"roles"_s, store()->listRoles()}}); });
}

void AccessController::createRoleAction()
{
    require(ADMIN_PERMISSION);
    requireParams({u"name"_s});
    guarded([&]
    {
        const QJsonObject role = store()->createRole(params()[u"name"_s], params().value(u"description"_s)
            , splitList(params().value(u"permissions"_s)));
        audit(u"Created role '%1'"_s.arg(role[u"name"_s].toString()), {{u"permissions"_s, role[u"permissions"_s]}});
        setResult(role);
    });
}

void AccessController::updateRoleAction()
{
    require(ADMIN_PERMISSION);
    requireParams({u"id"_s});
    const qint64 roleId = requireId(params(), u"id"_s);

    guarded([&]
    {
        const QJsonObject before = store()->role(roleId);
        if (before.isEmpty())
            throw AccessError(u"Role not found."_s);

        std::optional<QString> name;
        std::optional<QString> description;
        std::optional<QStringList> permissionList;
        if (params().contains(u"name"_s))
            name = params()[u"name"_s];
        if (params().contains(u"description"_s))
            description = params()[u"description"_s];
        if (params().contains(u"permissions"_s))
            permissionList = splitList(params()[u"permissions"_s]);

        const QJsonObject after = store()->updateRole(roleId, name, description, permissionList);

        const QVariantList beforeList = before[u"permissions"_s].toArray().toVariantList();
        const QVariantList afterList = after[u"permissions"_s].toArray().toVariantList();
        QStringList added;
        QStringList removed;
        for (const QVariant &permission : afterList)
        {
            if (!beforeList.contains(permission))
                added.append(permission.toString());
        }
        for (const QVariant &permission : beforeList)
        {
            if (!afterList.contains(permission))
                removed.append(permission.toString());
        }

        QStringList changes;
        if (after[u"name"_s] != before[u"name"_s])
            changes.append(u"renamed from '%1'"_s.arg(before[u"name"_s].toString()));
        if (!added.isEmpty())
            changes.append(u"allowed "_s + labels(added));
        if (!removed.isEmpty())
            changes.append(u"removed "_s + labels(removed));
        if (!changes.isEmpty())
            audit(u"Changed role '%1': %2"_s.arg(after[u"name"_s].toString(), changes.join(u"; "_s)));
        setResult(after);
    });
}

void AccessController::deleteRoleAction()
{
    require(ADMIN_PERMISSION);
    requireParams({u"id"_s});
    const qint64 roleId = requireId(params(), u"id"_s);
    guarded([&]
    {
        const QJsonObject role = store()->role(roleId);
        if (role.isEmpty())
            throw AccessError(u"Role not found."_s);
        store()->deleteRole(roleId);
        audit(u"Deleted role '%1'"_s.arg(role[u"name"_s].toString()));
        setResult(QJsonObject {{u"ok"_s, true}});
    });
}

// ---- sessions ----------------------------------------------------------------------------------

void AccessController::sessionsAction()
{
    require(ADMIN_PERMISSION);
    guarded([&] { setResult(QJsonObject {{u"sessions"_s, store()->listSessions(m_context->currentSessionId())}}); });
}

void AccessController::revokeSessionAction()
{
    require(ADMIN_PERMISSION);
    requireParams({u"id"_s});
    const qint64 sessionId = requireId(params(), u"id"_s);
    guarded([&]
    {
        const QJsonArray sessions = store()->listSessions(m_context->currentSessionId());
        for (const QJsonValue &value : sessions)
        {
            const QJsonObject session = value.toObject();
            if (session[u"id"_s].toInteger() != sessionId)
                continue;
            if (session[u"current"_s].toBool())
                throw AccessError(u"Use Log out to end your own session."_s);
            store()->deleteSessionById(sessionId);
            audit(u"Signed out a session of '%1' (%2)"_s.arg(session[u"username"_s].toString(), session[u"ip"_s].toString()));
            setResult(QJsonObject {{u"ok"_s, true}});
            return;
        }
        throw AccessError(u"Session not found."_s);
    });
}
