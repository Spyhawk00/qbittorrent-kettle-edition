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

#include <QSet>
#include <QString>

#include "base/global.h"
#include "permissions.h"

namespace Access
{
    enum class IdentityKind
    {
        None,       // no access at all (safe default)
        User,       // a personal account from the user database
        Localhost,  // "Bypass authentication" for localhost or a whitelisted subnet
        ApiKey,     // the WebUI API key
        Legacy,     // the single username/password from qBittorrent's own settings
        Desktop     // changes made in qBittorrent's own window
    };

    struct Identity
    {
        IdentityKind kind = IdentityKind::None;
        qint64 userId = -1;
        QString username;
        QString roleName;
        QSet<QString> permissions;
        double sessionCreatedAt = 0;

        bool can(const QString &permission) const
        {
            return permissions.contains(permission);
        }

        // Name stored in the activity log.
        QString logName() const
        {
            switch (kind)
            {
            case IdentityKind::User:
                return username;
            case IdentityKind::Localhost:
                return u"(localhost)"_s;
            case IdentityKind::ApiKey:
                return u"(API key)"_s;
            case IdentityKind::Legacy:
                return username + u" (shared login)"_s;
            case IdentityKind::Desktop:
                return u"(desktop)"_s;
            case IdentityKind::None:
                break;
            }
            return {};
        }

        QString logRole() const
        {
            return (kind == IdentityKind::User) ? roleName : QString();
        }
    };

    inline QSet<QString> allPermissions()
    {
        QSet<QString> all;
        for (const Permission &permission : permissions())
            all.insert(permission.id);
        return all;
    }

    inline Identity fullAccessIdentity(const IdentityKind kind, const QString &username = {})
    {
        return {.kind = kind, .userId = -1, .username = username, .roleName = {}, .permissions = allPermissions()
            , .sessionCreatedAt = 0};
    }
}
