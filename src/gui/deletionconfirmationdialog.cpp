/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2024-2026  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2006  Christophe Dumez <chris@qbittorrent.org>
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

#include "deletionconfirmationdialog.h"

#include <QPalette>
#include <QPushButton>
#include <QStyle>

#include "base/bittorrent/session.h"
#include "base/preferences.h"
#include "uithememanager.h"
#include "utils.h"

using namespace Qt::Literals::StringLiterals;

DeletionConfirmationDialog::DeletionConfirmationDialog(QWidget *parent, const int torrentsCount, const QString &name, const bool defaultDeleteFiles)
    : QDialog(parent)
    , m_ui {new Ui::DeletionConfirmationDialog}
{
    m_ui->setupUi(this);

    if (torrentsCount == 1)
        m_ui->label->setText(tr("Are you sure you want to remove '%1' from the transfer list?", "Are you sure you want to remove 'ubuntu-linux-iso' from the transfer list?").arg(name.toHtmlEscaped()));
    else
        m_ui->label->setText(tr("Are you sure you want to remove these %1 torrents from the transfer list?", "Are you sure you want to remove these 5 torrents from the transfer list?").arg(QString::number(torrentsCount)));

    // Icons
    const QSize iconSize = Utils::Gui::largeIconSize();
    m_ui->labelWarning->setPixmap(UIThemeManager::instance()->getIcon(u"dialog-warning"_s).pixmap(iconSize));
    m_ui->labelWarning->setFixedWidth(iconSize.width());
    // line the checkbox up with the message text, which starts after the warning icon
    const int spacing = m_ui->horizontalLayout->spacing();
    m_ui->horizontalLayout_2->setContentsMargins((iconSize.width() + ((spacing >= 0) ? spacing : style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing))), 0, 0, 0);

    // a forced "remove files" removal does not change the remembered choice
    m_ui->labelRemember->setVisible(!defaultDeleteFiles);
    // the note sits under the checkbox text, in a softer but still readable color
    m_ui->labelRemember->setEnabled(true);
    m_ui->labelRemember->setForegroundRole(QPalette::PlaceholderText);
    m_ui->labelRemember->setIndent(style()->pixelMetric(QStyle::PM_IndicatorWidth) + style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing) + 2);
    // more room around the content, with a clear gap above the buttons
    const int margin = style()->pixelMetric(QStyle::PM_LayoutLeftMargin) * 3 / 2;
    m_ui->verticalLayout->setContentsMargins(margin, margin, margin, margin);
    m_ui->verticalLayout->setSpacing(margin);
    m_ui->verticalLayoutChoice->setSpacing(2);
    m_ui->checkRemoveContent->setChecked(defaultDeleteFiles || Preferences::instance()->removeTorrentContent());
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Remove"));
    m_ui->buttonBox->button(QDialogButtonBox::Cancel)->setFocus();

    // The last choice becomes the default for the next removal
    connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, [this, defaultDeleteFiles]
    {
        if (!defaultDeleteFiles)
            Preferences::instance()->setRemoveTorrentContent(m_ui->checkRemoveContent->isChecked());
    });
    connect(m_ui->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

DeletionConfirmationDialog::~DeletionConfirmationDialog()
{
    delete m_ui;
}

bool DeletionConfirmationDialog::isRemoveContentSelected() const
{
    return m_ui->checkRemoveContent->isChecked();
}
