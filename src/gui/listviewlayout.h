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

#include <QObject>
#include <QSet>

class QEvent;
class QTimer;
class QTreeView;

// Keeps the columns of a list as wide as their contents and gives its rows a little more room.
// The columns are fitted when the list first gets rows. After that they grow when needed, and shrink only when
// far too wide, so nothing is cut off and the columns do not jump around. A column that was resized by hand is left alone, and a very long
// text is cut off at half the width of the list.
class ListViewLayout final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(ListViewLayout)

public:
    // call it after the view got its model; the stretch column, if any, takes the width the other columns leave
    explicit ListViewLayout(QTreeView *view, int stretchColumn = -1);

    // back to the automatic widths: forgets the widths that were set by hand and fits all columns again
    void reset();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void scheduleFit(int delay);
    void fitColumns();

    QTreeView *m_view = nullptr;
    QTimer *m_timer = nullptr;
    int m_stretchColumn = -1;
    bool m_fitted = false;
    bool m_fitting = false;
    QSet<int> m_userSizedColumns;
};
