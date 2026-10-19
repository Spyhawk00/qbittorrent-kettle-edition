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

#include <QWidget>

#include "base/access/accessstore.h"

class QComboBox;
class QLabel;
class QTimer;
class QTreeWidget;
class LineEdit;

// Who did what in the WebUI (the "WebUI Activity" tab of the Execution Log).
class WebUIActivityWidget final : public QWidget
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(WebUIActivityWidget)

public:
    explicit WebUIActivityWidget(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    Access::ActivityFilter filter() const;
    void scheduleRefresh();
    void refresh();
    void updateUserList(const QJsonArray &users);
    void showContextMenu();
    void copySelection() const;
    void exportToFile();

    Access::AccessStore *m_store = nullptr;
    QComboBox *m_comboUser = nullptr;
    QComboBox *m_comboCategory = nullptr;
    QComboBox *m_comboOutcome = nullptr;
    LineEdit *m_textFilter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_labelSummary = nullptr;
    QTimer *m_refreshTimer = nullptr;
    bool m_needsRefresh = true;
};
