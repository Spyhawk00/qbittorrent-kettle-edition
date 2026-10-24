/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2023-2025  Vladimir Golovnev <glassez@yandex.ru>
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

#include "transferlistfilterswidget.h"

#include <QHeaderView>
#include <QIcon>
#include <QListWidgetItem>
#include <QMenu>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

#include "base/bittorrent/session.h"
#include "base/global.h"
#include "base/preferences.h"
#include "base/utils/compare.h"
#include "listviewlayout.h"
#include "transferlistfilters/categoryfilterwidget.h"
#include "transferlistfilters/statusfilterwidget.h"
#include "transferlistfilters/tagfilterwidget.h"
#include "transferlistfilters/trackersfilterwidget.h"
#include "transferlistfilters/trackerstatusfilterwidget.h"
#include "transferlistfilterswidgetitem.h"
#include "transferlistwidget.h"
#include "uithememanager.h"
#include "utils.h"

TransferListFiltersWidget::TransferListFiltersWidget(QWidget *parent, TransferListWidget *transferList, const bool downloadFavicon)
    : QWidget(parent)
    , m_transferList {transferList}
{
    setBackgroundRole(QPalette::Base);

    Preferences *const pref = Preferences::instance();

    // The section titles are as tall as the column headers of the transfer list and start at the same height
    const int titleHeight = ListViewLayout::headerHeight(this);
    connect(UIThemeManager::instance(), &UIThemeManager::styleChanged, this, [this]
    {
        for (auto *item : asConst(findChildren<TransferListFiltersWidgetItem *>()))
            item->setCaptionHeight(ListViewLayout::headerHeight(this));
    });

    // Construct lists
    auto *mainWidget = new QWidget;
    auto *mainWidgetLayout = new QVBoxLayout(mainWidget);
    mainWidgetLayout->setContentsMargins(0, 0, 0, 0);
    mainWidgetLayout->setSpacing(8);
    mainWidgetLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    {
        auto *statusFilterWidget = new StatusFilterWidget(this, transferList);
        auto *item = new TransferListFiltersWidgetItem(tr("Status"), statusFilterWidget, this);
        item->setCaptionHeight(titleHeight);
        item->setChecked(pref->getStatusFilterState());
        connect(item, &TransferListFiltersWidgetItem::toggled, statusFilterWidget, &StatusFilterWidget::toggleFilter);
        connect(item, &TransferListFiltersWidgetItem::toggled, pref, &Preferences::setStatusFilterState);
        statusFilterWidget->toggleFilter(item->isChecked());
        mainWidgetLayout->addWidget(item);
    }

    {
        auto *categoryFilterWidget = new CategoryFilterWidget(this);
        connect(categoryFilterWidget, &CategoryFilterWidget::actionDeleteTorrentsTriggered
                , transferList, &TransferListWidget::deleteVisibleTorrents);
        connect(categoryFilterWidget, &CategoryFilterWidget::actionStopTorrentsTriggered
                , transferList, &TransferListWidget::stopVisibleTorrents);
        connect(categoryFilterWidget, &CategoryFilterWidget::actionStartTorrentsTriggered
                , transferList, &TransferListWidget::startVisibleTorrents);
        connect(categoryFilterWidget, &CategoryFilterWidget::actionForceStartTorrentsTriggered
                , transferList, &TransferListWidget::forceStartVisibleTorrents);
        connect(categoryFilterWidget, &CategoryFilterWidget::categoryChanged
                , transferList, &TransferListWidget::applyCategoryFilter);

        auto *item = new TransferListFiltersWidgetItem(tr("Categories"), categoryFilterWidget, this);
        item->setCaptionHeight(titleHeight);
        item->setChecked(pref->getCategoryFilterState());
        connect(item, &TransferListFiltersWidgetItem::toggled, this, [this, categoryFilterWidget](const bool enabled)
        {
            m_transferList->applyCategoryFilter(enabled ? categoryFilterWidget->currentCategory() : QString());
        });
        connect(item, &TransferListFiltersWidgetItem::toggled, pref, &Preferences::setCategoryFilterState);
        mainWidgetLayout->addWidget(item);
    }

    {
        auto *tagFilterWidget = new TagFilterWidget(this);
        connect(tagFilterWidget, &TagFilterWidget::actionDeleteTorrentsTriggered
                , transferList, &TransferListWidget::deleteVisibleTorrents);
        connect(tagFilterWidget, &TagFilterWidget::actionStopTorrentsTriggered
                , transferList, &TransferListWidget::stopVisibleTorrents);
        connect(tagFilterWidget, &TagFilterWidget::actionStartTorrentsTriggered
                , transferList, &TransferListWidget::startVisibleTorrents);
        connect(tagFilterWidget, &TagFilterWidget::actionForceStartTorrentsTriggered
                , transferList, &TransferListWidget::forceStartVisibleTorrents);
        connect(tagFilterWidget, &TagFilterWidget::tagChanged
                , transferList, &TransferListWidget::applyTagFilter);

        auto *item = new TransferListFiltersWidgetItem(tr("Tags"), tagFilterWidget, this);
        item->setCaptionHeight(titleHeight);
        item->setChecked(pref->getTagFilterState());
        connect(item, &TransferListFiltersWidgetItem::toggled, this, [this, tagFilterWidget](const bool enabled)
        {
            m_transferList->applyTagFilter(enabled ? tagFilterWidget->currentTag() : std::nullopt);
        });
        connect(item, &TransferListFiltersWidgetItem::toggled, pref, &Preferences::setTagFilterState);
        mainWidgetLayout->addWidget(item);
    }

    const int trackerStatusItemPos = mainWidgetLayout->count();

    {
        m_trackersFilterWidget = new TrackersFilterWidget(this, transferList, downloadFavicon);

        auto *item = new TransferListFiltersWidgetItem(tr("Trackers"), m_trackersFilterWidget, this);
        item->setCaptionHeight(titleHeight);
        item->setChecked(pref->getTrackerFilterState());
        connect(item, &TransferListFiltersWidgetItem::toggled, m_trackersFilterWidget, &TrackersFilterWidget::toggleFilter);
        connect(item, &TransferListFiltersWidgetItem::toggled, pref, &Preferences::setTrackerFilterState);
        m_trackersFilterWidget->toggleFilter(item->isChecked());
        mainWidgetLayout->addWidget(item);
    }

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // the outline of the lists next to it
    scroll->setProperty("outlined", true);
    scroll->setWidget(mainWidget);
    // white like the list of the Search tab
    mainWidget->setBackgroundRole(QPalette::Base);
    mainWidget->setAutoFillBackground(true);
    // the Windows 11 style leaves the lists on the window color
    mainWidget->setStyleSheet(u"QAbstractScrollArea { background: palette(base); }"_s);
    m_mainWidget = mainWidget;

    auto *vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->addWidget(scroll);

    const auto createTrackerStatusItem = [this, mainWidgetLayout, trackerStatusItemPos, pref, titleHeight]
    {
        auto *trackerStatusFilterWidget = new TrackerStatusFilterWidget(this, m_transferList);
        auto *item = new TransferListFiltersWidgetItem(tr("Tracker status"), trackerStatusFilterWidget, this);
        item->setCaptionHeight(titleHeight);
        item->setChecked(pref->getTrackerStatusFilterState());
        connect(item, &TransferListFiltersWidgetItem::toggled, trackerStatusFilterWidget, &TrackerStatusFilterWidget::toggleFilter);
        connect(item, &TransferListFiltersWidgetItem::toggled, pref, &Preferences::setTrackerStatusFilterState);
        trackerStatusFilterWidget->toggleFilter(item->isChecked());
        mainWidgetLayout->insertWidget(trackerStatusItemPos, item);
    };

    const auto removeTrackerStatusItem = [mainWidgetLayout, trackerStatusItemPos]
    {
        QLayoutItem *layoutItem = mainWidgetLayout->takeAt(trackerStatusItemPos);
        delete layoutItem->widget();
        delete layoutItem;
    };

    m_useSeparateTrackerStatusFilter = pref->useSeparateTrackerStatusFilter();
    if (m_useSeparateTrackerStatusFilter)
        createTrackerStatusItem();

    connect(pref, &Preferences::changed, this, [this, pref, createTrackerStatusItem, removeTrackerStatusItem]
    {
        if (m_useSeparateTrackerStatusFilter == pref->useSeparateTrackerStatusFilter())
            return;

        m_useSeparateTrackerStatusFilter = !m_useSeparateTrackerStatusFilter;
        if (m_useSeparateTrackerStatusFilter)
            createTrackerStatusItem();
        else
            removeTrackerStatusItem();
    });
}

void TransferListFiltersWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // the style sheet keeps the colors of the palette it was last applied with, as the selection of the lists
    if ((event->type() == QEvent::PaletteChange) && m_mainWidget)
        m_mainWidget->setStyleSheet(m_mainWidget->styleSheet());
}

void TransferListFiltersWidget::setDownloadTrackerFavicon(bool value)
{
    m_trackersFilterWidget->setDownloadTrackerFavicon(value);
}
