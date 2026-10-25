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

#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QList>
#include <QPoint>

#include "base/settingvalue.h"

class QComboBox;
class QGridLayout;
class QGroupBox;
class QLabel;
class QTabWidget;
class QVBoxLayout;

namespace BitTorrent
{
    struct UsageHistory;
}

namespace Ui
{
    class StatsDialog;
}

class StatsDialog final : public QDialog
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(StatsDialog)

public:
    explicit StatsDialog(QWidget *parent);
    ~StatsDialog() override;

private slots:
    void update();

private:
    QVBoxLayout *addPage(const QString &title);
    QGroupBox *addSection(QVBoxLayout *page, const QString &title);
    QLabel *addRow(QGroupBox *section, const QString &text);
    void buildOverviewPage();
    void buildHistoryPage();
    void buildTechnicalPage();

    void updateOverview();
    void updateHistory();
    void updateTechnical();
    void updatePeriod(const BitTorrent::UsageHistory &history);
    void updateMonths(const BitTorrent::UsageHistory &history);
    void updateAddedTorrents();
    void copyCurrentTab() const;

    Ui::StatsDialog *m_ui = nullptr;
    QTabWidget *m_tabs = nullptr;
    SettingValue<QSize> m_storeDialogSize;
    SettingValue<QPoint> m_storeDialogPosition;
    SettingValue<int> m_storePeriod;

    // Overview
    QLabel *m_sessionDownloaded = nullptr;
    QLabel *m_sessionUploaded = nullptr;
    QLabel *m_sessionRatio = nullptr;
    QLabel *m_allTimeDownloaded = nullptr;
    QLabel *m_allTimeUploaded = nullptr;
    QLabel *m_allTimeRatio = nullptr;
    QLabel *m_currentSpeed = nullptr;
    QLabel *m_torrentsDownloading = nullptr;
    QLabel *m_torrentsSeeding = nullptr;
    QLabel *m_torrentsStopped = nullptr;
    QLabel *m_torrentsErrored = nullptr;
    QLabel *m_peers = nullptr;
    QLabel *m_runningFor = nullptr;

    // History
    QComboBox *m_period = nullptr;
    QLabel *m_periodDownloaded = nullptr;
    QLabel *m_periodUploaded = nullptr;
    QLabel *m_periodRatio = nullptr;
    QGroupBox *m_monthsSection = nullptr;
    QGroupBox *m_addedSection = nullptr;
    QLabel *m_trackingSince = nullptr;
    QList<QLabel *> m_monthCells;
    QList<QLabel *> m_addedCells;
    bool m_addedDirty = true;

    // Technical
    QLabel *m_writeStarve = nullptr;
    QLabel *m_readStarve = nullptr;
    QLabel *m_queuedJobs = nullptr;
    QLabel *m_jobsTime = nullptr;
    QLabel *m_queuedBytes = nullptr;
    QLabel *m_requestLatency = nullptr;
    QLabel *m_totalBuffers = nullptr;
    QLabel *m_cacheHits = nullptr;
    QLabel *m_dhtNodes = nullptr;
    QLabel *m_queuedAnnounces = nullptr;
    QLabel *m_discarded = nullptr;
};
