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

#include <QWidget>
#include <QJsonArray>

#include "base/settingvalue.h"

class QTreeWidget;

namespace Access
{
    class AccessStore;
    struct User;
}

namespace Ui
{
    class UserAccountsWidget;
}

// WebUI user accounts, roles and the browsers they're logged in on (a page of the Options dialog).
// Changes apply at once, not with the dialog's OK/Apply.
class UserAccountsWidget final : public QWidget
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(UserAccountsWidget)

public:
    explicit UserAccountsWidget(QWidget *parent = nullptr);
    ~UserAccountsWidget() override;

private:
    void loadUsers();
    void loadRoles();
    void loadSessions();

    void addUser();
    void editUser();
    void setUserDisabled(bool disabled);
    void logOutUser();
    void removeUser();
    void addRole();
    void editRole();
    void removeRole();
    void logOutSessions();

    void showUsersMenu();
    void showRolesMenu();
    void showSessionsMenu();

    qint64 selectedId(const QTreeWidget *tree) const;
    std::optional<Access::User> selectedUser();
    QJsonObject selectedRole();
    // Runs a change, telling the person when it was refused. Returns whether it worked.
    bool change(const std::function<void ()> &func);
    // Reads from the user database, showing the error in the window if that fails. Returns whether it worked.
    bool read(const std::function<void ()> &func);
    void audit(const QString &summary);

    Ui::UserAccountsWidget *m_ui = nullptr;
    Access::AccessStore *m_store = nullptr;
    QJsonArray m_roles;
    SettingValue<QByteArray> m_storeUsersHeaderState;
    SettingValue<QByteArray> m_storeRolesHeaderState;
    SettingValue<QByteArray> m_storeSessionsHeaderState;
};
