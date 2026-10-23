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

#include "useraccountswidget.h"

#include <QDateTime>
#include <QHeaderView>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>

#include "base/access/accessstore.h"
#include "base/access/identity.h"
#include "base/access/permissions.h"
#include "base/global.h"
#include "ui_useraccountswidget.h"
#include "uithememanager.h"
#include "useraccountdialog.h"
#include "userroledialog.h"

// the window this page used to be: keeps the saved column widths
#define SETTINGS_KEY(name) u"UserAccountsDialog/" name

namespace
{
    enum UserColumn
    {
        USER_NAME,
        USER_ROLE,
        USER_STATUS,
        USER_LAST_LOGIN,
        USER_CREATED
    };

    enum RoleColumn
    {
        ROLE_NAME,
        ROLE_DESCRIPTION,
        ROLE_USERS,
        ROLE_PERMISSIONS
    };

    enum SessionColumn
    {
        SESSION_USER,
        SESSION_IP,
        SESSION_BROWSER,
        SESSION_LOGGED_IN,
        SESSION_LAST_ACTIVE,
        SESSION_EXPIRES
    };

    QVariant timeValue(const QJsonValue &seconds)
    {
        if (seconds.toDouble() <= 0)
            return {};
        return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(seconds.toDouble() * 1000));
    }
}

UserAccountsWidget::UserAccountsWidget(QWidget *parent)
    : QWidget(parent)
    , m_ui(new Ui::UserAccountsWidget)
    , m_store {Access::AccessStore::instance()}
    , m_storeUsersHeaderState(SETTINGS_KEY(u"UsersHeaderState"_s))
    , m_storeRolesHeaderState(SETTINGS_KEY(u"RolesHeaderState"_s))
    , m_storeSessionsHeaderState(SETTINGS_KEY(u"SessionsHeaderState"_s))
{
    m_ui->setupUi(this);

    m_ui->treeUsers->setHeaderLabels({tr("Username"), tr("Role"), tr("Status"), tr("Last login"), tr("Created")});
    m_ui->treeRoles->setHeaderLabels({tr("Name"), tr("Description"), tr("Users"), tr("Permissions")});
    m_ui->treeSessions->setHeaderLabels({tr("User"), tr("IP"), tr("Browser"), tr("Logged in"), tr("Last active"), tr("Expires")});
    m_ui->treeUsers->sortByColumn(USER_NAME, Qt::AscendingOrder);
    m_ui->treeRoles->sortByColumn(ROLE_NAME, Qt::AscendingOrder);
    m_ui->treeSessions->sortByColumn(SESSION_LAST_ACTIVE, Qt::DescendingOrder);
    m_ui->treeUsers->setColumnWidth(USER_LAST_LOGIN, 150);
    m_ui->treeRoles->setColumnWidth(ROLE_DESCRIPTION, 320);
    m_ui->treeSessions->setColumnWidth(SESSION_BROWSER, 180);

    m_ui->tabWidget->setTabIcon(0, UIThemeManager::instance()->getIcon(u"peers"_s));
    m_ui->tabWidget->setTabIcon(1, UIThemeManager::instance()->getIcon(u"object-locked"_s));
    m_ui->tabWidget->setTabIcon(2, UIThemeManager::instance()->getIcon(u"system-log-out"_s));
    m_ui->btnAddUser->setIcon(UIThemeManager::instance()->getIcon(u"list-add"_s));
    m_ui->btnRemoveUser->setIcon(UIThemeManager::instance()->getIcon(u"list-remove"_s));
    m_ui->btnAddRole->setIcon(UIThemeManager::instance()->getIcon(u"list-add"_s));
    m_ui->btnRemoveRole->setIcon(UIThemeManager::instance()->getIcon(u"list-remove"_s));

    m_ui->treeUsers->header()->restoreState(m_storeUsersHeaderState);
    m_ui->treeRoles->header()->restoreState(m_storeRolesHeaderState);
    m_ui->treeSessions->header()->restoreState(m_storeSessionsHeaderState);

    if (!m_store || !m_store->isOpen())
    {
        m_ui->labelError->setText(tr("The user database could not be opened. See the Execution Log for details."));
        m_ui->tabWidget->setEnabled(false);
        return;
    }
    m_ui->labelError->hide();

    connect(m_ui->btnAddUser, &QPushButton::clicked, this, &UserAccountsWidget::addUser);
    connect(m_ui->btnEditUser, &QPushButton::clicked, this, &UserAccountsWidget::editUser);
    connect(m_ui->btnRemoveUser, &QPushButton::clicked, this, &UserAccountsWidget::removeUser);
    connect(m_ui->btnAddRole, &QPushButton::clicked, this, &UserAccountsWidget::addRole);
    connect(m_ui->btnEditRole, &QPushButton::clicked, this, &UserAccountsWidget::editRole);
    connect(m_ui->btnRemoveRole, &QPushButton::clicked, this, &UserAccountsWidget::removeRole);
    connect(m_ui->btnLogOutSession, &QPushButton::clicked, this, &UserAccountsWidget::logOutSessions);
    connect(m_ui->treeUsers, &QTreeWidget::itemDoubleClicked, this, &UserAccountsWidget::editUser);
    connect(m_ui->treeRoles, &QTreeWidget::itemDoubleClicked, this, &UserAccountsWidget::editRole);
    connect(m_ui->treeUsers, &QWidget::customContextMenuRequested, this, &UserAccountsWidget::showUsersMenu);
    connect(m_ui->treeRoles, &QWidget::customContextMenuRequested, this, &UserAccountsWidget::showRolesMenu);
    connect(m_ui->treeSessions, &QWidget::customContextMenuRequested, this, &UserAccountsWidget::showSessionsMenu);

    // changes made in the WebUI show up here too
    connect(m_store, &Access::AccessStore::accountsChanged, this, [this]
    {
        loadRoles();
        loadUsers();
    });
    connect(m_store, &Access::AccessStore::sessionsChanged, this, &UserAccountsWidget::loadSessions);

    loadRoles();
    loadUsers();
    loadSessions();
}

UserAccountsWidget::~UserAccountsWidget()
{
    m_storeUsersHeaderState = m_ui->treeUsers->header()->saveState();
    m_storeRolesHeaderState = m_ui->treeRoles->header()->saveState();
    m_storeSessionsHeaderState = m_ui->treeSessions->header()->saveState();
    delete m_ui;
}

void UserAccountsWidget::loadUsers()
{
    const qint64 selected = selectedId(m_ui->treeUsers);
    QJsonArray users;
    if (!read([&] { users = m_store->listUsers(); }))
        return;

    m_ui->treeUsers->clear();
    for (const QJsonValue &value : asConst(users))
    {
        const QJsonObject user = value.toObject();
        const bool disabled = user[u"disabled"_s].toBool();
        auto *item = new QTreeWidgetItem(m_ui->treeUsers);
        item->setData(USER_NAME, Qt::UserRole, user[u"id"_s].toInteger());
        item->setText(USER_NAME, user[u"username"_s].toString());
        item->setText(USER_ROLE, user[u"role_name"_s].toString());
        item->setText(USER_STATUS, (disabled ? tr("Disabled") : tr("Enabled")));
        if (const QVariant lastLogin = timeValue(user[u"last_login_at"_s]); lastLogin.isValid())
            item->setData(USER_LAST_LOGIN, Qt::DisplayRole, lastLogin);
        else
            item->setText(USER_LAST_LOGIN, tr("Never"));
        item->setData(USER_CREATED, Qt::DisplayRole, timeValue(user[u"created_at"_s]));
        if (disabled)
            item->setForeground(USER_STATUS, Qt::red);
        if (user[u"id"_s].toInteger() == selected)
            m_ui->treeUsers->setCurrentItem(item);
    }

    m_ui->labelUsers->setText(users.isEmpty()
        ? tr("No accounts yet, so everyone still uses the shared WebUI login. Add an Admin account first.")
        : tr("People who can log in to the WebUI with their own username and password."));
}

void UserAccountsWidget::loadRoles()
{
    const qint64 selected = selectedId(m_ui->treeRoles);
    if (!read([&] { m_roles = m_store->listRoles(); }))
        return;

    m_ui->treeRoles->clear();
    for (const QJsonValue &value : asConst(m_roles))
    {
        const QJsonObject role = value.toObject();
        QStringList labels;
        for (const QJsonValue &permission : role[u"permissions"_s].toArray())
            labels.append(Access::permissionLabel(permission.toString()));

        auto *item = new QTreeWidgetItem(m_ui->treeRoles);
        item->setData(ROLE_NAME, Qt::UserRole, role[u"id"_s].toInteger());
        item->setText(ROLE_NAME, role[u"name"_s].toString());
        item->setText(ROLE_DESCRIPTION, role[u"description"_s].toString());
        item->setToolTip(ROLE_DESCRIPTION, role[u"description"_s].toString());
        item->setData(ROLE_USERS, Qt::DisplayRole, role[u"user_count"_s].toInt());
        item->setData(ROLE_PERMISSIONS, Qt::DisplayRole, labels.size());
        item->setToolTip(ROLE_PERMISSIONS, labels.join(u'\n'));
        if (role[u"id"_s].toInteger() == selected)
            m_ui->treeRoles->setCurrentItem(item);
    }
}

void UserAccountsWidget::loadSessions()
{
    QJsonArray sessions;
    if (!read([&] { sessions = m_store->listSessions({}); }))
        return;

    m_ui->treeSessions->clear();
    for (const QJsonValue &value : asConst(sessions))
    {
        const QJsonObject session = value.toObject();
        auto *item = new QTreeWidgetItem(m_ui->treeSessions);
        item->setData(SESSION_USER, Qt::UserRole, session[u"id"_s].toInteger());
        item->setText(SESSION_USER, session[u"username"_s].toString());
        item->setText(SESSION_IP, session[u"ip"_s].toString());
        item->setText(SESSION_BROWSER, session[u"user_agent"_s].toString());
        item->setToolTip(SESSION_BROWSER, session[u"user_agent"_s].toString());
        item->setData(SESSION_LOGGED_IN, Qt::DisplayRole, timeValue(session[u"created_at"_s]));
        item->setData(SESSION_LAST_ACTIVE, Qt::DisplayRole, timeValue(session[u"last_seen_at"_s]));
        item->setData(SESSION_EXPIRES, Qt::DisplayRole, timeValue(session[u"expires_at"_s]));
    }
}

qint64 UserAccountsWidget::selectedId(const QTreeWidget *tree) const
{
    const QTreeWidgetItem *item = tree->currentItem();
    return (item && item->isSelected()) ? item->data(0, Qt::UserRole).toLongLong() : -1;
}

std::optional<Access::User> UserAccountsWidget::selectedUser()
{
    std::optional<Access::User> user;
    change([&] { user = m_store->user(selectedId(m_ui->treeUsers)); });
    return user;
}

QJsonObject UserAccountsWidget::selectedRole()
{
    QJsonObject role;
    change([&] { role = m_store->role(selectedId(m_ui->treeRoles)); });
    return role;
}

bool UserAccountsWidget::read(const std::function<void ()> &func)
{
    try
    {
        func();
        return true;
    }
    catch (const RuntimeError &error)
    {
        m_ui->labelError->setText(tr("Couldn't read the user database. %1").arg(error.message()));
        m_ui->labelError->show();
        return false;
    }
}

bool UserAccountsWidget::change(const std::function<void ()> &func)
{
    try
    {
        func();
        return true;
    }
    catch (const RuntimeError &error)
    {
        QMessageBox::warning(this, windowTitle(), error.message());
        return false;
    }
}

void UserAccountsWidget::audit(const QString &summary)
{
    const Access::Identity desktop = Access::fullAccessIdentity(Access::IdentityKind::Desktop);
    m_store->logActivity(desktop.logName(), desktop.logRole(), {}, u"admin"_s, u"desktop"_s, summary, u"ok"_s);
}

void UserAccountsWidget::addUser()
{
    auto *dialog = new UserAccountDialog(m_roles, std::nullopt, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog]
    {
        change([&]
        {
            const Access::User user = m_store->createUser(dialog->username(), dialog->password(), dialog->roleId());
            audit(u"Created user '%1' with role '%2'"_s.arg(user.username, user.roleName));
        });
    });
    dialog->open();
}

void UserAccountsWidget::editUser()
{
    const std::optional<Access::User> before = selectedUser();
    if (!before)
        return;

    auto *dialog = new UserAccountDialog(m_roles, before, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog, before]
    {
        change([&]
        {
            const QString password = dialog->password();
            const Access::User after = m_store->updateUser(before->id, dialog->roleId(), dialog->isDisabled()
                , (password.isEmpty() ? std::nullopt : std::optional<QString>(password)));
            if (!password.isEmpty())
                m_store->deleteUserSessions(after.id);

            QStringList changes;
            if (before->roleId != after.roleId)
                changes.append(u"role %1 → %2"_s.arg(before->roleName, after.roleName));
            if (before->disabled != after.disabled)
                changes.append(after.disabled ? u"disabled"_s : u"enabled"_s);
            if (!password.isEmpty())
                changes.append(u"password reset"_s);
            if (!changes.isEmpty())
                audit(u"Changed user '%1': %2"_s.arg(after.username, changes.join(u", "_s)));
        });
    });
    dialog->open();
}

void UserAccountsWidget::setUserDisabled(const bool disabled)
{
    const std::optional<Access::User> user = selectedUser();
    if (!user || (user->disabled == disabled))
        return;

    change([&]
    {
        m_store->updateUser(user->id, std::nullopt, disabled, std::nullopt);
        audit(u"Changed user '%1': %2"_s.arg(user->username, (disabled ? u"disabled"_s : u"enabled"_s)));
    });
}

void UserAccountsWidget::logOutUser()
{
    const std::optional<Access::User> user = selectedUser();
    if (!user)
        return;

    change([&]
    {
        m_store->deleteUserSessions(user->id);
        audit(u"Signed out '%1' everywhere"_s.arg(user->username));
    });
}

void UserAccountsWidget::removeUser()
{
    const std::optional<Access::User> user = selectedUser();
    if (!user)
        return;

    const QMessageBox::StandardButton answer = QMessageBox::question(this, tr("Remove user")
        , tr("Are you sure you want to remove the user '%1'?").arg(user->username));
    if (answer != QMessageBox::Yes)
        return;

    change([&]
    {
        m_store->deleteUser(user->id);
        audit(u"Deleted user '%1'"_s.arg(user->username));
    });
}

void UserAccountsWidget::addRole()
{
    auto *dialog = new UserRoleDialog({}, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog]
    {
        change([&]
        {
            const QJsonObject role = m_store->createRole(dialog->name(), dialog->description(), dialog->permissions());
            audit(u"Created role '%1'"_s.arg(role[u"name"_s].toString()));
        });
    });
    dialog->open();
}

void UserAccountsWidget::editRole()
{
    const QJsonObject before = selectedRole();
    if (before.isEmpty())
        return;

    auto *dialog = new UserRoleDialog(before, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog, before]
    {
        change([&]
        {
            const QJsonObject after = m_store->updateRole(before[u"id"_s].toInteger(), dialog->name()
                , dialog->description(), dialog->permissions());
            audit(u"Changed role '%1'"_s.arg(after[u"name"_s].toString()));
        });
    });
    dialog->open();
}

void UserAccountsWidget::removeRole()
{
    const QJsonObject role = selectedRole();
    if (role.isEmpty())
        return;

    const QMessageBox::StandardButton answer = QMessageBox::question(this, tr("Remove role")
        , tr("Are you sure you want to remove the role '%1'?").arg(role[u"name"_s].toString()));
    if (answer != QMessageBox::Yes)
        return;

    change([&]
    {
        m_store->deleteRole(role[u"id"_s].toInteger());
        audit(u"Deleted role '%1'"_s.arg(role[u"name"_s].toString()));
    });
}

void UserAccountsWidget::logOutSessions()
{
    const QList<QTreeWidgetItem *> items = m_ui->treeSessions->selectedItems();
    QList<std::pair<qint64, QString>> sessions;
    for (const QTreeWidgetItem *item : items)
        sessions.append({item->data(SESSION_USER, Qt::UserRole).toLongLong(), item->text(SESSION_USER)});

    change([&]
    {
        for (const auto &[id, username] : asConst(sessions))
        {
            m_store->deleteSessionById(id);
            audit(u"Signed out a session of '%1'"_s.arg(username));
        }
    });
}

void UserAccountsWidget::showUsersMenu()
{
    const std::optional<Access::User> user = selectedUser();
    if (!user)
        return;

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(UIThemeManager::instance()->getIcon(u"edit-rename"_s), tr("Edit..."), this, &UserAccountsWidget::editUser);
    if (user->disabled)
        menu->addAction(UIThemeManager::instance()->getIcon(u"checked-completed"_s), tr("Enable"), this, [this] { setUserDisabled(false); });
    else
        menu->addAction(UIThemeManager::instance()->getIcon(u"object-locked"_s), tr("Disable"), this, [this] { setUserDisabled(true); });
    menu->addAction(UIThemeManager::instance()->getIcon(u"system-log-out"_s), tr("Log Out Everywhere"), this, &UserAccountsWidget::logOutUser);
    menu->addSeparator();
    menu->addAction(UIThemeManager::instance()->getIcon(u"list-remove"_s), tr("Remove"), this, &UserAccountsWidget::removeUser);
    menu->popup(QCursor::pos());
}

void UserAccountsWidget::showRolesMenu()
{
    if (selectedId(m_ui->treeRoles) < 0)
        return;

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(UIThemeManager::instance()->getIcon(u"edit-rename"_s), tr("Edit..."), this, &UserAccountsWidget::editRole);
    menu->addSeparator();
    menu->addAction(UIThemeManager::instance()->getIcon(u"list-remove"_s), tr("Remove"), this, &UserAccountsWidget::removeRole);
    menu->popup(QCursor::pos());
}

void UserAccountsWidget::showSessionsMenu()
{
    if (m_ui->treeSessions->selectedItems().isEmpty())
        return;

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(UIThemeManager::instance()->getIcon(u"system-log-out"_s), tr("Log Out"), this, &UserAccountsWidget::logOutSessions);
    menu->popup(QCursor::pos());
}
