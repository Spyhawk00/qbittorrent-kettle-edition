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

#include <QJsonObject>

#include "base/access/identity.h"
#include "apicontroller.h"

namespace Access
{
    class AccessStore;
}

// What the access controller needs from the WebUI about the current request.
class IAccessContext
{
public:
    virtual ~IAccessContext() = default;

    virtual Access::AccessStore *accessStore() const = 0;
    virtual Access::Identity currentIdentity() const = 0;
    virtual QString currentSessionId() const = 0;
    virtual QString currentClientAddress() const = 0;
    virtual void logAccessEvent(const QString &category, const QString &endpoint, const QString &summary
            , const QString &outcome, const QJsonObject &details = {}) = 0;
};

// Users, roles, sessions and the activity log (/api/v2/access/...). Their settings are ordinary
// preferences (app/preferences).
class AccessController final : public APIController
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(AccessController)

public:
    AccessController(IAccessContext *context, IApplication *app, QObject *parent = nullptr);

private slots:
    void meAction();
    void changePasswordAction();
    void catalogAction();

    void activityAction();
    void exportActivityAction();

    void usersAction();
    void createUserAction();
    void updateUserAction();
    void deleteUserAction();
    void signOutUserAction();

    void rolesAction();
    void createRoleAction();
    void updateRoleAction();
    void deleteRoleAction();

    void sessionsAction();
    void revokeSessionAction();

private:
    Access::AccessStore *store() const;
    Access::Identity require(const QString &permission) const;
    void audit(const QString &summary, const QJsonObject &details = {});
    template <typename Func> void guarded(Func &&func);

    IAccessContext *m_context = nullptr;
};
