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

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "base/global.h"

// Multi-user WebUI access control: the permission catalogue and the map from every
// WebAPI action ("scope/action") to the permission it needs.
// Anything not in the map needs "unlisted_api", so new API actions are never open by default.
namespace Access
{
    struct Permission
    {
        QString id;
        QString label;
        QString description;
        QString group;
        bool dangerous = false;
    };

    struct EndpointRule
    {
        QString permission;
        bool logged = false;
        QString category;
    };

    struct DefaultRole
    {
        QString name;
        QString description;
        QStringList permissions;
    };

    inline const QString ADMIN_PERMISSION = u"manage_users"_s;

    const QList<Permission> &permissions();
    bool isValidPermission(const QString &id);
    QString permissionLabel(const QString &id);
    QList<DefaultRole> defaultRoles();

    EndpointRule ruleFor(const QString &endpoint);
    QSet<QString> requiredPermissions(const QString &endpoint, const QHash<QString, QString> &params);

    // Parameters dropped when the user may not choose save locations.
    QStringList saveLocationParams(const QString &endpoint);

    // app/preferences values blanked for anyone without the "settings" permission.
    const QSet<QString> &redactedPreferences();

    // app/setPreferences keys that only an admin may change: they decide who can sign in, open the WebUI to
    // others or run programs, so "settings" alone must not reach them.
    const QSet<QString> &adminOnlyPreferences();

    // Anything that isn't clearly false counts as true, to stay on the safe side.
    bool isTrue(const QString &value);
}
