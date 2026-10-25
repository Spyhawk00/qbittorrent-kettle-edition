/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2013  Nick Tiskov <daymansmail@gmail.com>
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

#include "statsdialog.h"

#include <algorithm>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include "base/access/accessstore.h"
#include "base/bittorrent/cachestatus.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/sessionstatus.h"
#include "base/bittorrent/torrent.h"
#include "base/global.h"
#include "base/utils/misc.h"
#include "base/utils/string.h"
#include "groupboxheadingstyle.h"
#include "hidabletabwidget.h"
#include "ui_statsdialog.h"
#include "utils.h"

#define SETTINGS_KEY(name) u"StatisticsDialog/" name

namespace
{
    enum Period
    {
        Today,
        Last7Days,
        ThisMonth,
        LastMonth,
        Last12Months,
        AllTime
    };

    const int HISTORY_TAB = 1;

    // upload / download, "-" when nothing was downloaded
    QString ratioText(const qint64 downloaded, const qint64 uploaded)
    {
        return (downloaded > 0) ? Utils::String::fromDouble(static_cast<qreal>(uploaded) / downloaded, 2) : u"-"_s;
    }

    // peers waiting on the disk, as a share of all peers
    QString percentText(const qint64 count, const qint64 peers)
    {
        return u"%1%"_s.arg(((count > 0) && (peers > 0)) ? Utils::String::fromDouble((100. * count / peers), 2) : u"0"_s);
    }

    QLabel *makeValueLabel(QWidget *parent)
    {
        auto *label = new QLabel(parent);
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    }

    QLabel *makeTextLabel(const QString &text, QWidget *parent)
    {
        auto *label = new QLabel(text, parent);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    }

    QDate firstOfMonth(const QDate &date)
    {
        return QDate(date.year(), date.month(), 1);
    }

    BitTorrent::UsageEntry sumMonths(const BitTorrent::UsageHistory &history, const QDate &from, const QDate &to)
    {
        BitTorrent::UsageEntry sum;
        for (auto it = history.months.lowerBound(from); (it != history.months.end()) && (it.key() <= to); ++it)
        {
            sum.downloaded += it->downloaded;
            sum.uploaded += it->uploaded;
        }
        return sum;
    }

    // the text of the rows of the sections of a page, for copying
    QString pageText(const QWidget *page)
    {
        QStringList lines;
        for (const auto *section : page->findChildren<QGroupBox *>(QString(), Qt::FindDirectChildrenOnly))
        {
            if (section->isHidden())
                continue;
            if (!lines.isEmpty())
                lines.append(QString());
            lines.append(section->title());
            const auto *grid = qobject_cast<const QGridLayout *>(section->layout());
            for (int row = 0; row < grid->rowCount(); ++row)
            {
                QStringList cells;
                for (int column = 0; column < grid->columnCount(); ++column)
                {
                    const QLayoutItem *item = grid->itemAtPosition(row, column);
                    if (!item || !item->widget())
                        continue;
                    if (const auto *label = qobject_cast<const QLabel *>(item->widget()))
                        cells.append(label->text());
                    else if (const auto *combo = qobject_cast<const QComboBox *>(item->widget()))
                        cells.append(combo->currentText());
                }
                if (!cells.isEmpty())
                    lines.append(cells.join(u'\t'));
            }
        }
        return lines.join(u'\n');
    }
}

StatsDialog::StatsDialog(QWidget *parent)
    : QDialog(parent)
    , m_ui(new Ui::StatsDialog)
    , m_storeDialogSize(SETTINGS_KEY(u"Size"_s))
    , m_storeDialogPosition(SETTINGS_KEY(u"Position"_s))
    , m_storePeriod(SETTINGS_KEY(u"Period"_s))
{
    m_ui->setupUi(this);

    m_tabs = new HidableTabWidget(this);
    m_tabs->setProperty("flatPage", true);
    m_ui->verticalLayout->insertWidget(0, m_tabs);

    buildOverviewPage();
    buildHistoryPage();
    buildTechnicalPage();
    // sections are headings with a line under them, as in Options
    GroupBoxHeadingStyle::apply(this);

    QPushButton *copyButton = m_ui->buttonBox->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    connect(copyButton, &QPushButton::clicked, this, &StatsDialog::copyCurrentTab);
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &StatsDialog::close);

    connect(BitTorrent::Session::instance(), &BitTorrent::Session::statsUpdated
            , this, &StatsDialog::update);
    connect(m_tabs, &QTabWidget::currentChanged, this, &StatsDialog::update);
    if (const Access::AccessStore *store = Access::AccessStore::instance())
        connect(store, &Access::AccessStore::activityAdded, this, [this] { m_addedDirty = true; });
    update();

    // a size saved by the old, narrower window would cut the tabs off, so the window is never smaller than it starts out
    if (const QSize dialogSize = m_storeDialogSize; dialogSize.isValid())
        resize(dialogSize.expandedTo(size()));

    // back where it was last closed, if that place is still on a screen
    const QPoint position = m_storeDialogPosition.get(QPoint(-32768, -32768));
    for (const QScreen *screen : asConst(QGuiApplication::screens()))
    {
        if (screen->availableGeometry().contains(position))
        {
            move(position);
            break;
        }
    }
}

StatsDialog::~StatsDialog()
{
    m_storeDialogSize = size();
    m_storeDialogPosition = pos();
    m_storePeriod = m_period->currentIndex();
    delete m_ui;
}

QVBoxLayout *StatsDialog::addPage(const QString &title)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(fontMetrics().height());

    auto *scrollArea = new QScrollArea(m_tabs);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setWidget(page);
    // the numbers on the right keep a gap to the scroll bar when it shows
    const int scrollBarGap = fontMetrics().averageCharWidth() * 2;
    connect(scrollArea->verticalScrollBar(), &QScrollBar::rangeChanged, page, [layout, scrollBarGap](const int min, const int max)
    {
        layout->setContentsMargins(0, 0, ((max > min) ? scrollBarGap : 0), 0);
    });
    // the page of the tabs is painted by the tab widget, so no band of another color shows between its line and the rows
    scrollArea->viewport()->setAutoFillBackground(false);
    page->setAutoFillBackground(false);
    m_tabs->addTab(scrollArea, title);
    return layout;
}

QGroupBox *StatsDialog::addSection(QVBoxLayout *page, const QString &title)
{
    auto *section = new QGroupBox(title);
    auto *grid = new QGridLayout(section);
    grid->setColumnStretch(0, 1);
    page->addWidget(section);
    return section;
}

// A row of a section: the text, then its value on the right
QLabel *StatsDialog::addRow(QGroupBox *section, const QString &text)
{
    auto *grid = static_cast<QGridLayout *>(section->layout());
    const int row = grid->rowCount();
    QLabel *value = makeValueLabel(section);
    grid->addWidget(makeTextLabel(text, section), row, 0);
    grid->addWidget(value, row, 1);
    return value;
}

void StatsDialog::buildOverviewPage()
{
    QVBoxLayout *page = addPage(tr("Overview"));

    QGroupBox *transfer = addSection(page, tr("Transfer"));
    auto *grid = static_cast<QGridLayout *>(transfer->layout());
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);
    QFont headerFont = font();
    headerFont.setBold(true);
    const QStringList titles {tr("This session"), tr("All time")};
    for (int i = 0; i < titles.size(); ++i)
    {
        QLabel *header = makeValueLabel(transfer);
        header->setFont(headerFont);
        header->setText(titles[i]);
        grid->addWidget(header, 0, (i + 1));
    }
    const auto addPairRow = [transfer, grid](const QString &text, QLabel *&session, QLabel *&allTime)
    {
        const int row = grid->rowCount();
        session = makeValueLabel(transfer);
        allTime = makeValueLabel(transfer);
        grid->addWidget(makeTextLabel(text, transfer), row, 0);
        grid->addWidget(session, row, 1);
        grid->addWidget(allTime, row, 2);
    };
    addPairRow(tr("Downloaded:"), m_sessionDownloaded, m_allTimeDownloaded);
    addPairRow(tr("Uploaded:"), m_sessionUploaded, m_allTimeUploaded);
    addPairRow(tr("Share ratio:"), m_sessionRatio, m_allTimeRatio);
    const int speedRow = grid->rowCount();
    m_currentSpeed = makeValueLabel(transfer);
    grid->addWidget(makeTextLabel(tr("Current speed:"), transfer), speedRow, 0);
    grid->addWidget(m_currentSpeed, speedRow, 1, 1, 2);

    QGroupBox *activity = addSection(page, tr("Activity"));
    m_torrentsDownloading = addRow(activity, tr("Torrents downloading:"));
    m_torrentsSeeding = addRow(activity, tr("Torrents seeding:"));
    m_torrentsStopped = addRow(activity, tr("Torrents stopped:"));
    m_torrentsErrored = addRow(activity, tr("Torrents with errors:"));
    m_peers = addRow(activity, tr("Connected peers:"));
    m_runningFor = addRow(activity, tr("Running for:"));

    page->addStretch();
}

void StatsDialog::buildHistoryPage()
{
    QVBoxLayout *page = addPage(tr("History"));

    QGroupBox *period = addSection(page, tr("Period"));
    auto *grid = static_cast<QGridLayout *>(period->layout());
    m_period = new QComboBox(period);
    m_period->addItems({tr("Today"), tr("Last 7 days"), tr("This month"), tr("Last month"), tr("Last 12 months"), tr("All time")});
    m_period->setCurrentIndex(std::clamp(m_storePeriod.get(ThisMonth), 0, (m_period->count() - 1)));
    grid->addWidget(m_period, 0, 0, 1, 2);
    connect(m_period, &QComboBox::currentIndexChanged, this, &StatsDialog::update);
    m_periodDownloaded = addRow(period, tr("Downloaded:"));
    m_periodUploaded = addRow(period, tr("Uploaded:"));
    m_periodRatio = addRow(period, tr("Share ratio:"));
    m_trackingSince = addRow(period, tr("Tracking since:"));

    m_monthsSection = addSection(page, tr("By month"));
    auto *monthGrid = static_cast<QGridLayout *>(m_monthsSection->layout());
    QFont headerFont = font();
    headerFont.setBold(true);
    const QStringList titles {tr("Month"), tr("Downloaded"), tr("Uploaded"), tr("Share ratio")};
    for (int i = 0; i < titles.size(); ++i)
    {
        QLabel *header = (i == 0) ? new QLabel(m_monthsSection) : makeValueLabel(m_monthsSection);
        header->setText(titles[i]);
        header->setFont(headerFont);
        monthGrid->addWidget(header, 0, i);
    }
    for (int column = 1; column < 4; ++column)
        monthGrid->setColumnStretch(column, 1);

    m_addedSection = addSection(page, tr("WebUI downloads added, this month"));
    auto *note = new QLabel(tr("Counts the requests that added a download. Apps without a login, such as Sonarr and Radarr, are shown as (localhost). Downloads added in the desktop window are not counted."), m_addedSection);
    note->setWordWrap(true);
    static_cast<QGridLayout *>(m_addedSection->layout())->addWidget(note, 0, 0, 1, 2);

    page->addStretch();
}

void StatsDialog::buildTechnicalPage()
{
    QVBoxLayout *page = addPage(tr("Technical"));

    QGroupBox *disk = addSection(page, tr("Disk"));
    m_writeStarve = addRow(disk, tr("Peers waiting on disk writes:"));
    m_readStarve = addRow(disk, tr("Peers waiting on disk reads:"));
    m_queuedJobs = addRow(disk, tr("Queued I/O jobs:"));
    m_jobsTime = addRow(disk, tr("Average time in queue:"));
    m_queuedBytes = addRow(disk, tr("Total queued size:"));
    m_requestLatency = addRow(disk, tr("Request latency:"));
    m_totalBuffers = addRow(disk, tr("Total buffer size:"));
#ifndef QBT_USES_LIBTORRENT2
    m_cacheHits = addRow(disk, tr("Read cache hits:"));
#endif

    QGroupBox *network = addSection(page, tr("Network"));
    m_dhtNodes = addRow(network, tr("DHT nodes:"));
    m_queuedAnnounces = addRow(network, tr("Queued tracker announces:"));
    m_discarded = addRow(network, tr("Data discarded this session:"));

    page->addStretch();
}

void StatsDialog::update()
{
    updateOverview();
    updateHistory();
    updateTechnical();
}

void StatsDialog::updateOverview()
{
    const auto *session = BitTorrent::Session::instance();
    const BitTorrent::SessionStatus &ss = session->status();

    m_sessionDownloaded->setText(Utils::Misc::friendlyUnit(ss.totalDownload));
    m_sessionUploaded->setText(Utils::Misc::friendlyUnit(ss.totalUpload));
    m_sessionRatio->setText(ratioText(ss.totalDownload, ss.totalUpload));
    m_allTimeDownloaded->setText(Utils::Misc::friendlyUnit(ss.allTimeDownload));
    m_allTimeUploaded->setText(Utils::Misc::friendlyUnit(ss.allTimeUpload));
    m_allTimeRatio->setText(ratioText(ss.allTimeDownload, ss.allTimeUpload));
    m_currentSpeed->setText(u"↓ %1   ↑ %2"_s.arg(Utils::Misc::friendlyUnit(ss.payloadDownloadRate, true)
            , Utils::Misc::friendlyUnit(ss.payloadUploadRate, true)));

    const BitTorrent::TorrentCounts counts = BitTorrent::countTorrents(session->torrents());
    m_torrentsDownloading->setText(QString::number(counts.downloading));
    m_torrentsSeeding->setText(QString::number(counts.seeding));
    m_torrentsStopped->setText(QString::number(counts.stopped));
    m_torrentsErrored->setText(QString::number(counts.errored));
    // errors are shown in amber, readable on both the light and the dark window
    QPalette errorPalette;
    if (counts.errored > 0)
    {
        const bool isDark = palette().color(QPalette::Window).lightness() < 128;
        errorPalette.setColor(QPalette::WindowText, isDark ? QColor(0xE3, 0xB3, 0x41) : QColor(0x9A, 0x67, 0x00));
    }
    m_torrentsErrored->setPalette(errorPalette);
    m_peers->setText(QString::number(ss.peersCount));
    m_runningFor->setText(Utils::Misc::userFriendlyDuration(session->uptimeSeconds()));
}

void StatsDialog::updateHistory()
{
    const BitTorrent::UsageHistory history = BitTorrent::Session::instance()->usageHistory();
    updatePeriod(history);
    updateMonths(history);
    updateAddedTorrents();
}

void StatsDialog::updatePeriod(const BitTorrent::UsageHistory &history)
{
    const BitTorrent::SessionStatus &ss = BitTorrent::Session::instance()->status();
    const QDate today = QDate::currentDate();
    const QDate thisMonth = firstOfMonth(today);

    BitTorrent::UsageEntry total;
    switch (m_period->currentIndex())
    {
    case Today:
        total = history.days.value(today);
        break;
    case Last7Days:
        for (auto it = history.days.lowerBound(today.addDays(-6)); it != history.days.end(); ++it)
        {
            total.downloaded += it->downloaded;
            total.uploaded += it->uploaded;
        }
        break;
    case ThisMonth:
        total = history.months.value(thisMonth);
        break;
    case LastMonth:
        total = history.months.value(thisMonth.addMonths(-1));
        break;
    case Last12Months:
        total = sumMonths(history, thisMonth.addMonths(-11), thisMonth);
        break;
    case AllTime:
        total = {.downloaded = ss.allTimeDownload, .uploaded = ss.allTimeUpload};
        break;
    }

    m_periodDownloaded->setText(Utils::Misc::friendlyUnit(total.downloaded));
    m_periodUploaded->setText(Utils::Misc::friendlyUnit(total.uploaded));
    m_periodRatio->setText(ratioText(total.downloaded, total.uploaded));
    m_trackingSince->setText(QLocale().toString(history.since, QLocale::ShortFormat));
}

void StatsDialog::updateMonths(const BitTorrent::UsageHistory &history)
{
    const BitTorrent::SessionStatus &ss = BitTorrent::Session::instance()->status();
    auto *grid = static_cast<QGridLayout *>(m_monthsSection->layout());

    // what was moved before tracking began is what the months don't account for
    const BitTorrent::UsageEntry tracked = sumMonths(history, QDate(), QDate(9999, 12, 1));
    const BitTorrent::UsageEntry before {.downloaded = std::max<qint64>((ss.allTimeDownload - tracked.downloaded), 0)
        , .uploaded = std::max<qint64>((ss.allTimeUpload - tracked.uploaded), 0)};

    QList<QStringList> rows;
    // the newest month first
    for (auto it = history.months.cend(); it != history.months.cbegin();)
    {
        --it;
        rows.append({QLocale().toString(it.key(), u"MMMM yyyy"_s), Utils::Misc::friendlyUnit(it->downloaded)
            , Utils::Misc::friendlyUnit(it->uploaded), ratioText(it->downloaded, it->uploaded)});
    }
    if ((before.downloaded > 0) || (before.uploaded > 0))
    {
        rows.append({tr("Before tracking began"), Utils::Misc::friendlyUnit(before.downloaded)
            , Utils::Misc::friendlyUnit(before.uploaded), ratioText(before.downloaded, before.uploaded)});
    }

    // the cells are made again only when the number of rows changes
    if (m_monthCells.size() != (rows.size() * 4))
    {
        qDeleteAll(m_monthCells);
        m_monthCells.clear();
        for (int row = 0; row < rows.size(); ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                QLabel *cell = (column == 0) ? makeTextLabel({}, m_monthsSection) : makeValueLabel(m_monthsSection);
                grid->addWidget(cell, (row + 1), column);
                m_monthCells.append(cell);
            }
        }
    }
    for (int row = 0; row < rows.size(); ++row)
    {
        for (int column = 0; column < 4; ++column)
            m_monthCells[(row * 4) + column]->setText(rows[row][column]);
    }
}

// who added downloads this month, from the activity log of the WebUI
void StatsDialog::updateAddedTorrents()
{
    const Access::AccessStore *store = Access::AccessStore::instance();
    m_addedSection->setVisible(store && store->isOpen());
    if (m_addedSection->isHidden() || !m_addedDirty || (m_tabs->currentIndex() != HISTORY_TAB))
        return;
    m_addedDirty = false;

    const double since = QDateTime(firstOfMonth(QDate::currentDate()), QTime(0, 0)).toSecsSinceEpoch();
    const QJsonArray counts = store->countAddedTorrents(since);

    auto *grid = static_cast<QGridLayout *>(m_addedSection->layout());
    qDeleteAll(m_addedCells);
    m_addedCells.clear();
    const auto addCells = [this, grid](const QString &name, const QString &count)
    {
        const int row = grid->rowCount();
        QLabel *nameLabel = makeTextLabel(name, m_addedSection);
        QLabel *countLabel = makeValueLabel(m_addedSection);
        countLabel->setText(count);
        grid->addWidget(nameLabel, row, 0);
        grid->addWidget(countLabel, row, 1);
        m_addedCells.append({nameLabel, countLabel});
    };
    if (counts.isEmpty())
        addCells(tr("Nothing added yet"), {});
    for (const QJsonValue &value : counts)
        addCells(value[u"username"_s].toString(), QString::number(value[u"added"_s].toInteger()));
}

void StatsDialog::updateTechnical()
{
    const BitTorrent::Session *session = BitTorrent::Session::instance();
    const BitTorrent::SessionStatus &ss = session->status();
    const BitTorrent::CacheStatus &cs = session->cacheStatus();

    // From lt manual: disk_write_queue and disk_read_queue are the number of peers currently waiting on a disk write or disk read
    // to complete before it receives or sends any more data on the socket. It's a metric of how disk bound you are.
    m_writeStarve->setText(percentText(ss.diskWriteQueue, ss.peersCount));
    m_readStarve->setText(percentText(ss.diskReadQueue, ss.peersCount));

    const QString msSuffix = tr("%1 ms");
    m_queuedJobs->setText(QString::number(cs.jobQueueLength));
    m_jobsTime->setText(msSuffix.arg(cs.averageJobTime));
    m_queuedBytes->setText(Utils::Misc::friendlyUnit(cs.queuedBytes));
    m_requestLatency->setText(msSuffix.arg(cs.requestLatency));
    m_totalBuffers->setText(Utils::Misc::friendlyUnit(cs.totalUsedBuffers * 16 * 1024));
#ifndef QBT_USES_LIBTORRENT2
    const qreal readRatio = cs.readRatio;
    m_cacheHits->setText(u"%1%"_s.arg((readRatio > 0)
        ? Utils::String::fromDouble((100 * readRatio), 2)
        : u"0"_s));
#endif

    m_dhtNodes->setText(session->isDHTEnabled() ? QString::number(ss.dhtNodes) : tr("Off"));
    m_queuedAnnounces->setText(QString::number(ss.queuedTrackerAnnounces));
    m_discarded->setText(Utils::Misc::friendlyUnit(ss.totalWasted));
}

void StatsDialog::copyCurrentTab() const
{
    const auto *page = static_cast<const QScrollArea *>(m_tabs->currentWidget())->widget();
    QApplication::clipboard()->setText(m_tabs->tabText(m_tabs->currentIndex()) + u"\n\n"_s + pageText(page));
}
