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

#include "userroledialog.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QStyle>
#include <QVBoxLayout>

#include "base/access/permissions.h"
#include "base/global.h"
#include "groupboxheadingstyle.h"
#include "ui_userroledialog.h"
#include "uithememanager.h"

UserRoleDialog::UserRoleDialog(const QJsonObject &role, QWidget *parent)
    : QDialog(parent)
    , m_ui(new Ui::UserRoleDialog)
{
    m_ui->setupUi(this);

    connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &UserRoleDialog::accept);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    const QJsonArray rolePermissions = role[u"permissions"_s].toArray();

    // one group box per group, in catalogue order
    QHash<QString, QVBoxLayout *> groups;
    QWidget *previous = m_ui->textDescription;
    for (const Access::Permission &permission : Access::permissions())
    {
        if (!groups.contains(permission.group))
        {
            auto *groupBox = new QGroupBox(permission.group, m_ui->permissionsWidget);
            groups.insert(permission.group, new QVBoxLayout(groupBox));
            m_ui->permissionsLayout->addWidget(groupBox);
        }

        auto *checkBox = new QCheckBox(permission.label, m_ui->permissionsWidget);
        checkBox->setChecked(rolePermissions.contains(permission.id));
        QString toolTip = permission.description;
        if (permission.dangerous)
        {
            checkBox->setIcon(UIThemeManager::instance()->getIcon(u"dialog-warning"_s));
            toolTip += u"\n\n"_s + tr("Gives control over the PC or qBittorrent itself.");
        }
        checkBox->setToolTip(toolTip);

        // the description goes under the check box, lined up with its text, as in the WebUI
        auto *descriptionLabel = new QLabel(permission.description, m_ui->permissionsWidget);
        descriptionLabel->setWordWrap(true);
        descriptionLabel->setForegroundRole(QPalette::PlaceholderText);
        const int indent = checkBox->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, checkBox)
            + checkBox->style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, checkBox);
        descriptionLabel->setContentsMargins(indent, 0, 0, 0);

        auto *permissionLayout = new QVBoxLayout;
        permissionLayout->setSpacing(0);
        permissionLayout->addWidget(checkBox);
        permissionLayout->addWidget(descriptionLabel);
        groups[permission.group]->addLayout(permissionLayout);
        m_checkBoxes.insert(permission.id, checkBox);

        // the check boxes are made after the buttons, so Tab would reach them last
        setTabOrder(previous, checkBox);
        previous = checkBox;
    }
    setTabOrder(previous, m_ui->buttonBox);
    m_ui->permissionsLayout->addStretch();
    GroupBoxHeadingStyle::apply(this);

    if (role.isEmpty())
    {
        setWindowTitle(tr("Add Role"));
    }
    else
    {
        setWindowTitle(tr("Edit Role"));
        m_ui->textName->setText(role[u"name"_s].toString());
        m_ui->textDescription->setText(role[u"description"_s].toString());
    }
    m_ui->textName->setFocus();
}

UserRoleDialog::~UserRoleDialog()
{
    delete m_ui;
}

QString UserRoleDialog::name() const
{
    return m_ui->textName->text().trimmed();
}

QString UserRoleDialog::description() const
{
    return m_ui->textDescription->text().trimmed();
}

QStringList UserRoleDialog::permissions() const
{
    QStringList list;
    for (auto iter = m_checkBoxes.cbegin(); iter != m_checkBoxes.cend(); ++iter)
    {
        if (iter.value()->isChecked())
            list.append(iter.key());
    }
    list.sort();
    return list;
}

void UserRoleDialog::accept()
{
    if (name().isEmpty())
    {
        QMessageBox::warning(this, windowTitle(), tr("Enter a role name."));
        return;
    }

    QDialog::accept();
}
