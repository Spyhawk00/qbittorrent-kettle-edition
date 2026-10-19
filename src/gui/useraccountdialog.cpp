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

#include "useraccountdialog.h"

#include <QJsonObject>
#include <QMessageBox>

#include "base/global.h"
#include "ui_useraccountdialog.h"

namespace
{
    const int MIN_PASSWORD_LENGTH = 8;
}

UserAccountDialog::UserAccountDialog(const QJsonArray &roles, const std::optional<Access::User> &user, QWidget *parent)
    : QDialog(parent)
    , m_ui(new Ui::UserAccountDialog)
    , m_isNew {!user.has_value()}
{
    m_ui->setupUi(this);

    connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &UserAccountDialog::accept);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (const QJsonValue &value : roles)
    {
        const QJsonObject role = value.toObject();
        m_ui->comboRole->addItem(role[u"name"_s].toString(), role[u"id"_s].toInteger());
        m_ui->comboRole->setItemData((m_ui->comboRole->count() - 1), role[u"description"_s].toString(), Qt::ToolTipRole);
    }

    if (user)
    {
        setWindowTitle(tr("Edit User"));
        m_ui->textUsername->setText(user->username);
        m_ui->textUsername->setEnabled(false);
        m_ui->comboRole->setCurrentIndex(m_ui->comboRole->findData(user->roleId));
        m_ui->checkDisabled->setChecked(user->disabled);
        m_ui->textPassword->setPlaceholderText(tr("Leave empty to keep the current password"));
        m_ui->comboRole->setFocus();
    }
    else
    {
        setWindowTitle(tr("Add User"));
        // new people get the least access by default
        if (const int viewer = m_ui->comboRole->findText(u"Viewer"_s); viewer >= 0)
            m_ui->comboRole->setCurrentIndex(viewer);
        m_ui->checkDisabled->hide();
        m_ui->textUsername->setFocus();
    }
}

UserAccountDialog::~UserAccountDialog()
{
    delete m_ui;
}

QString UserAccountDialog::username() const
{
    return m_ui->textUsername->text().trimmed();
}

qint64 UserAccountDialog::roleId() const
{
    return m_ui->comboRole->currentData().toLongLong();
}

QString UserAccountDialog::password() const
{
    return m_ui->textPassword->text();
}

bool UserAccountDialog::isDisabled() const
{
    return m_ui->checkDisabled->isChecked();
}

void UserAccountDialog::accept()
{
    if (username().isEmpty())
    {
        QMessageBox::warning(this, windowTitle(), tr("Enter a username."));
        return;
    }
    if ((m_isNew || !password().isEmpty()) && (password().size() < MIN_PASSWORD_LENGTH))
    {
        QMessageBox::warning(this, windowTitle(), tr("The password must be at least %1 characters long.").arg(MIN_PASSWORD_LENGTH));
        return;
    }
    if (password() != m_ui->textPasswordConfirm->text())
    {
        QMessageBox::warning(this, windowTitle(), tr("The passwords don't match."));
        return;
    }

    QDialog::accept();
}
