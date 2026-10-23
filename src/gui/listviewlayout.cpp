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

#include "listviewlayout.h"

#include <algorithm>

#include <QAbstractItemModel>
#include <QApplication>
#include <QEvent>
#include <QHeaderView>
#include <QProxyStyle>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeView>

namespace
{
    // delay before fitting after rows were added or the list was scrolled, and after only their data changed
    const int FIT_DELAY = 300;
    const int REFIT_DELAY = 2000;

    // The style of the application, with every item of a list a bit taller
    class RoomyRowStyle final : public QProxyStyle
    {
    public:
        explicit RoomyRowStyle(QObject *parent)
            : QProxyStyle(QApplication::style()->name())
        {
            setParent(parent);
        }

        QSize sizeFromContents(const ContentsType type, const QStyleOption *option, const QSize &size, const QWidget *widget) const override
        {
            QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
            if ((type == CT_ItemViewItem) && widget)
                result.rheight() += qRound(widget->fontMetrics().height() * 0.2);
            return result;
        }
    };
}

ListViewLayout::ListViewLayout(QTreeView *view, const int stretchColumn)
    : QObject(view)
    , m_view {view}
    , m_timer {new QTimer(this)}
    , m_stretchColumn {stretchColumn}
{
    Q_ASSERT(m_view);
    Q_ASSERT(m_view->model());

    m_view->setStyle(new RoomyRowStyle(m_view));

    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &ListViewLayout::fitColumns);

    const QAbstractItemModel *model = m_view->model();
    connect(model, &QAbstractItemModel::rowsInserted, this, [this]() { scheduleFit(FIT_DELAY); });
    connect(model, &QAbstractItemModel::dataChanged, this, [this]() { scheduleFit(REFIT_DELAY); });
    connect(model, &QAbstractItemModel::layoutChanged, this, [this]() { scheduleFit(REFIT_DELAY); });
    // once the list is empty, the next rows are fitted from scratch
    const auto onCleared = [this, model]()
    {
        if (model->rowCount() == 0)
            m_fitted = false;
    };
    connect(model, &QAbstractItemModel::rowsRemoved, this, onCleared);
    connect(model, &QAbstractItemModel::modelReset, this, onCleared);
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { scheduleFit(FIT_DELAY); });

    // a column dragged by the user, not resized by the fit, keeps the width it was given
    connect(m_view->header(), &QHeaderView::sectionResized, this, [this](const int logicalIndex)
    {
        if (!m_fitting && (QApplication::mouseButtons() & Qt::LeftButton))
            m_userSizedColumns.insert(logicalIndex);
    });

    // a list on a tab that was not shown cannot be measured yet, and the stretch column follows the size of the list
    m_view->installEventFilter(this);
    m_view->viewport()->installEventFilter(this);
}

bool ListViewLayout::eventFilter(QObject *watched, QEvent *event)
{
    if (((watched == m_view) && (event->type() == QEvent::Show))
        || ((watched == m_view->viewport()) && (event->type() == QEvent::Resize)))
    {
        scheduleFit(FIT_DELAY);
    }
    return QObject::eventFilter(watched, event);
}

void ListViewLayout::reset()
{
    m_userSizedColumns.clear();
    m_fitted = false;
    fitColumns();
}

void ListViewLayout::scheduleFit(const int delay)
{
    // a fit that is already waiting and comes sooner is kept
    if (const int remaining = m_timer->remainingTime(); (remaining >= 0) && (remaining <= delay))
        return;
    m_timer->start(delay);
}

void ListViewLayout::fitColumns()
{
    if (!m_view->isVisible())
        return;

    // the first fit with rows sets every width, later ones grow a column, or shrink one that is far too wide (after a long
    // text is gone). Without rows only the headers are fitted
    const bool hasRows = m_view->model()->rowCount() > 0;
    const bool shrink = hasRows && !m_fitted;
    if (hasRows)
        m_fitted = true;

    QHeaderView *header = m_view->header();
    // a last column that stretches by itself would take the room meant for the stretch column
    if (m_stretchColumn >= 0)
        header->setStretchLastSection(false);

    const int maxWidth = std::max((m_view->viewport()->width() / 2), (30 * m_view->fontMetrics().averageCharWidth()));
    int stretchWidth = 0;
    m_fitting = true;
    for (int i = 0, count = header->count(); i < count; ++i)
    {
        if (m_view->isColumnHidden(i) || m_userSizedColumns.contains(i))
            continue;

        // measure the column without anyone noticing, then set the width it should get
        const int oldWidth = header->sectionSize(i);
        int fittedWidth = 0;
        {
            const QSignalBlocker blocker {header};
            m_view->resizeColumnToContents(i);
            fittedWidth = header->sectionSize(i);
            header->resizeSection(i, oldWidth);
        }

        // the arrow of the sorted column takes room of its own
        if (header->isSortIndicatorShown() && (header->sortIndicatorSection() == i))
            fittedWidth += header->fontMetrics().height();

        const int width = std::min(fittedWidth, maxWidth);
        if (i == m_stretchColumn)
        {
            stretchWidth = width;
            continue;
        }

        if (shrink ? (width != oldWidth) : ((width > oldWidth) || ((width * 10) < (oldWidth * 7))))
            header->resizeSection(i, width);
    }

    // the stretch column takes what the other columns leave, but is never narrower than its text (up to the limit)
    if ((m_stretchColumn >= 0) && !m_view->isColumnHidden(m_stretchColumn) && !m_userSizedColumns.contains(m_stretchColumn))
    {
        int othersWidth = 0;
        for (int i = 0, count = header->count(); i < count; ++i)
        {
            if ((i != m_stretchColumn) && !m_view->isColumnHidden(i))
                othersWidth += header->sectionSize(i);
        }

        const int width = std::max(stretchWidth, (m_view->viewport()->width() - othersWidth));
        if (width != header->sectionSize(m_stretchColumn))
            header->resizeSection(m_stretchColumn, width);
    }
    m_fitting = false;
}
