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

#include "webuiactivitywidget.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "base/access/activitytext.h"
#include "base/global.h"
#include "gui/lineedit.h"
#include "gui/uithememanager.h"

using namespace std::chrono_literals;

namespace
{
    // the newest entries; narrow the filter to see older ones
    const int ROW_LIMIT = 500;

    enum Column
    {
        COL_TIME,
        COL_USER,
        COL_ROLE,
        COL_IP,
        COL_CATEGORY,
        COL_ACTION,
        COL_RESULT,

        NB_COLUMNS
    };

    void addNames(QComboBox *comboBox, const QList<Access::ActivityName> &names)
    {
        for (const Access::ActivityName &name : names)
            comboBox->addItem(name.label, name.id);
    }
}

WebUIActivityWidget::WebUIActivityWidget(QWidget *parent)
    : QWidget(parent)
    , m_store {Access::AccessStore::instance()}
    , m_comboUser {new QComboBox(this)}
    , m_comboCategory {new QComboBox(this)}
    , m_comboOutcome {new QComboBox(this)}
    , m_textFilter {new LineEdit(this)}
    , m_tree {new QTreeWidget(this)}
    , m_labelSummary {new QLabel(this)}
    , m_refreshTimer {new QTimer(this)}
{
    m_comboUser->addItem(tr("All"), QString());
    m_comboCategory->addItem(tr("All"), QString());
    addNames(m_comboCategory, Access::activityCategories());
    m_comboOutcome->addItem(tr("All"), QString());
    addNames(m_comboOutcome, Access::activityOutcomes());
    m_comboUser->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_textFilter->setPlaceholderText(tr("Filter activity"));

    auto *exportButton = new QPushButton(tr("Export..."), this);
    connect(exportButton, &QPushButton::clicked, this, &WebUIActivityWidget::exportToFile);

    auto *filterLayout = new QHBoxLayout;
    filterLayout->addWidget(new QLabel(tr("User:"), this));
    filterLayout->addWidget(m_comboUser);
    filterLayout->addWidget(new QLabel(tr("Category:"), this));
    filterLayout->addWidget(m_comboCategory);
    filterLayout->addWidget(new QLabel(tr("Result:"), this));
    filterLayout->addWidget(m_comboOutcome);
    filterLayout->addWidget(m_textFilter, 1);
    filterLayout->addWidget(exportButton);

    m_tree->setHeaderLabels({tr("Time"), tr("User"), tr("Role"), tr("IP"), tr("Category"), tr("Action"), tr("Result")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setItemsExpandable(false);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(COL_ACTION, QHeaderView::Stretch);
    m_tree->setColumnWidth(COL_TIME, 150);
    m_tree->setColumnWidth(COL_IP, 120);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &WebUIActivityWidget::showContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(filterLayout);
    layout->addWidget(m_tree);
    layout->addWidget(m_labelSummary);

    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(500ms);
    connect(m_refreshTimer, &QTimer::timeout, this, &WebUIActivityWidget::refresh);

    connect(m_comboUser, &QComboBox::currentIndexChanged, this, &WebUIActivityWidget::scheduleRefresh);
    connect(m_comboCategory, &QComboBox::currentIndexChanged, this, &WebUIActivityWidget::scheduleRefresh);
    connect(m_comboOutcome, &QComboBox::currentIndexChanged, this, &WebUIActivityWidget::scheduleRefresh);
    connect(m_textFilter, &LineEdit::textUpdated, this, &WebUIActivityWidget::scheduleRefresh);

    if (!m_store || !m_store->isOpen())
    {
        m_labelSummary->setText(tr("The user database could not be opened. See the General tab for details."));
        setEnabled(false);
        return;
    }

    connect(m_store, &Access::AccessStore::activityAdded, this, &WebUIActivityWidget::scheduleRefresh);
}

void WebUIActivityWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_needsRefresh)
        refresh();
}

Access::ActivityFilter WebUIActivityWidget::filter() const
{
    return {
        .user = m_comboUser->currentData().toString(),
        .category = m_comboCategory->currentData().toString(),
        .outcome = m_comboOutcome->currentData().toString(),
        .text = m_textFilter->text().trimmed().left(200),
        .since = std::nullopt,
        .until = std::nullopt,
    };
}

void WebUIActivityWidget::scheduleRefresh()
{
    // the log isn't read while it's hidden
    m_needsRefresh = true;
    if (isVisible())
        m_refreshTimer->start();
}

void WebUIActivityWidget::refresh()
{
    if (!m_store || !m_store->isOpen())
        return;

    m_needsRefresh = false;
    QJsonObject result;
    try
    {
        result = m_store->queryActivity(filter(), ROW_LIMIT, 0);
    }
    catch (const RuntimeError &error)
    {
        m_labelSummary->setText(error.message());
        return;
    }

    updateUserList(result[u"users"_s].toArray());

    const auto *themeManager = UIThemeManager::instance();
    const QColor failedColor = themeManager->getColor(u"Log.Warning"_s);
    const QColor deniedColor = themeManager->getColor(u"Log.Critical"_s);

    const QJsonArray items = result[u"items"_s].toArray();
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    for (const QJsonValue &value : items)
    {
        const QJsonObject entry = value.toObject();
        const QString outcome = entry[u"outcome"_s].toString();

        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(COL_TIME, QLocale().toString(QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(entry[u"ts"_s].toDouble() * 1000)), QLocale::ShortFormat));
        item->setText(COL_USER, entry[u"username"_s].toString());
        item->setText(COL_ROLE, entry[u"role"_s].toString());
        item->setText(COL_IP, entry[u"ip"_s].toString());
        item->setText(COL_CATEGORY, Access::activityCategoryLabel(entry[u"category"_s].toString()));
        item->setText(COL_ACTION, entry[u"summary"_s].toString());
        item->setToolTip(COL_ACTION, entry[u"summary"_s].toString());
        item->setText(COL_RESULT, Access::activityOutcomeLabel(outcome));

        if ((outcome == u"failed") || (outcome == u"denied"))
        {
            const QColor color = (outcome == u"failed") ? failedColor : deniedColor;
            for (int column = 0; column < NB_COLUMNS; ++column)
                item->setForeground(column, color);
        }
    }
    m_tree->setUpdatesEnabled(true);

    m_labelSummary->setText(tr("Results (showing %1 out of %2)")
        .arg(QString::number(items.size()), QString::number(result[u"total"_s].toInt())));
}

void WebUIActivityWidget::updateUserList(const QJsonArray &users)
{
    for (const QJsonValue &value : users)
    {
        const QString user = value.toString();
        if (m_comboUser->findData(user) < 0)
            m_comboUser->addItem(user, user);
    }
}

void WebUIActivityWidget::showContextMenu()
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (!m_tree->selectedItems().isEmpty())
        menu->addAction(UIThemeManager::instance()->getIcon(u"edit-copy"_s), tr("Copy"), this, &WebUIActivityWidget::copySelection);
    menu->addAction(UIThemeManager::instance()->getIcon(u"document-save"_s, u"document-save"_s), tr("Export..."), this, &WebUIActivityWidget::exportToFile);
    menu->popup(QCursor::pos());
}

void WebUIActivityWidget::copySelection() const
{
    QStringList lines;
    for (const QTreeWidgetItem *item : asConst(m_tree->selectedItems()))
    {
        QStringList cells;
        for (int column = 0; column < NB_COLUMNS; ++column)
            cells.append(item->text(column));
        lines.append(cells.join(u'\t'));
    }
    QApplication::clipboard()->setText(lines.join(u'\n'));
}

void WebUIActivityWidget::exportToFile()
{
    const QString defaultName = QDateTime::currentDateTime().toString(u"'qbittorrent-activity-'yyyyMMdd-HHmmss'.csv'"_s);
    const QString path = QFileDialog::getSaveFileName(this, tr("Export WebUI activity"), defaultName
        , (tr("CSV files") + u" (*.csv)"));
    if (path.isEmpty())
        return;

    // UTF-8 byte order mark, so spreadsheet programs read the file as UTF-8
    QString csv = QChar(0xFEFF) + Access::activityCsvHeader() + u"\r\n"_s;
    try
    {
        m_store->forEachActivity(filter(), [&csv](const QJsonObject &entry)
        {
            csv += Access::activityCsvLine(entry) + u"\r\n"_s;
        }, Access::MAX_EXPORT_ROWS);
    }
    catch (const RuntimeError &error)
    {
        QMessageBox::warning(this, tr("Export WebUI activity"), error.message());
        return;
    }

    QFile file {path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || (file.write(csv.toUtf8()) < 0))
    {
        QMessageBox::warning(this, tr("Export WebUI activity")
            , tr("Couldn't save the file. Reason: %1").arg(file.errorString()));
    }
}
