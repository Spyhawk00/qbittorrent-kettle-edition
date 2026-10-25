/*
 * MIT License
 * Copyright (C) 2025 Thomas Piccirello <thomas@piccirello.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

"use strict";

window.qBittorrent ??= {};
window.qBittorrent.Statistics ??= (() => {
    const exports = () => {
        return {
            init: init,
            save: save,
            render: render,
        };
    };

    const HISTORY_REFRESH_MS = 5000;

    const statistics = {
        alltimeDL: 0,
        alltimeUL: 0,
        sessionDL: 0,
        sessionUL: 0,
        dlSpeed: 0,
        upSpeed: 0,
        totalWastedSession: 0,
        globalRatio: 0,
        totalPeerConnections: 0,
        torrentsDownloading: 0,
        torrentsSeeding: 0,
        torrentsStopped: 0,
        torrentsErrored: 0,
        uptime: 0,
        readCacheHits: 0,
        totalBuffersSize: 0,
        writeCacheOverload: 0,
        readCacheOverload: 0,
        queuedIOJobs: 0,
        averageTimeInQueue: 0,
        totalQueuedSize: 0,
        requestLatency: 0,
        dhtEnabled: true,
        dhtNodes: 0,
        // Tracker statistics
        queuedTrackerAnnounces: 0
    };

    // what the server last said about the days and months, and when it was asked
    let usage = null;
    let historyRequestedAt = 0;
    let historyKey = "";
    let addedKey = "";

    const save = (serverState) => {
        statistics.alltimeDL = serverState.alltime_dl;
        statistics.alltimeUL = serverState.alltime_ul;
        statistics.sessionDL = serverState.session_dl;
        statistics.sessionUL = serverState.session_ul;
        statistics.dlSpeed = serverState.dl_info_speed;
        statistics.upSpeed = serverState.up_info_speed;
        statistics.totalWastedSession = serverState.total_wasted_session;
        statistics.globalRatio = serverState.global_ratio;
        statistics.totalPeerConnections = serverState.total_peer_connections;
        statistics.torrentsDownloading = serverState.torrents_downloading;
        statistics.torrentsSeeding = serverState.torrents_seeding;
        statistics.torrentsStopped = serverState.torrents_stopped;
        statistics.torrentsErrored = serverState.torrents_errored;
        statistics.uptime = serverState.uptime;
        statistics.readCacheHits = serverState.read_cache_hits;
        statistics.totalBuffersSize = serverState.total_buffers_size;
        statistics.writeCacheOverload = serverState.write_cache_overload;
        statistics.readCacheOverload = serverState.read_cache_overload;
        statistics.queuedIOJobs = serverState.queued_io_jobs;
        statistics.averageTimeInQueue = serverState.average_time_queue;
        statistics.totalQueuedSize = serverState.total_queued_size;
        statistics.requestLatency = serverState.request_latency;
        statistics.dhtEnabled = serverState.dht_enabled;
        statistics.dhtNodes = serverState.dht_nodes;
        // Tracker statistics
        statistics.queuedTrackerAnnounces = serverState.queued_tracker_announces;
    };

    // upload / download, "-" when nothing was downloaded
    const ratioText = (downloaded, uploaded) => {
        return (downloaded > 0) ? window.qBittorrent.Misc.toFixedPointString((uploaded / downloaded), 2) : "-";
    };

    const unitText = (bytes) => {
        return window.qBittorrent.Misc.friendlyUnit(bytes, false);
    };

    const setText = (id, text) => {
        document.getElementById(id).textContent = text;
    };

    const addDays = (isoDate, days) => {
        const [year, month, day] = isoDate.split("-").map(Number);
        return new Date(Date.UTC(year, (month - 1), (day + days))).toISOString().slice(0, 10);
    };

    const addMonths = (isoDate, months) => {
        const [year, month] = isoDate.split("-").map(Number);
        return new Date(Date.UTC(year, (month - 1 + months), 1)).toISOString().slice(0, 10);
    };

    const monthName = (isoDate) => {
        const [year, month] = isoDate.split("-").map(Number);
        return new Date(year, (month - 1), 1).toLocaleString(undefined, { month: "long", year: "numeric" });
    };

    const sumEntries = (entries) => {
        return entries.reduce((sum, entry) => {
            sum.downloaded += entry.downloaded;
            sum.uploaded += entry.uploaded;
            return sum;
        }, { downloaded: 0, uploaded: 0 });
    };

    const refreshHistory = async () => {
        historyRequestedAt = Date.now();
        try {
            const response = await fetch(new URL("api/v2/transfer/usageHistory", window.location), {
                method: "GET",
                cache: "no-store"
            });
            if (response.ok)
                usage = await response.json();
        }
        catch (error) {
            // the numbers stay as they were
        }
        renderHistory();
    };

    const refreshAdded = async () => {
        const section = document.getElementById("statisticsAddedSection");
        try {
            const response = await fetch(new URL("api/v2/access/addedTorrents", window.location), {
                method: "GET",
                cache: "no-store"
            });
            // without the right to see the activity log, the section stays hidden
            if (!response.ok) {
                section?.classList.add("invisible");
                return;
            }
            const counts = await response.json();
            if (!section)
                return;
            section.classList.remove("invisible");
            const key = JSON.stringify(counts);
            if (key === addedKey)
                return;
            addedKey = key;

            const rows = counts.map((item) => [item.username, item.added]);
            if (rows.length === 0)
                rows.push(["QBT_TR(Nothing added yet)QBT_TR[CONTEXT=StatsDialog]", ""]);
            document.getElementById("statisticsAdded").replaceChildren(...rows.map((cells) => {
                const row = document.createElement("tr");
                cells.forEach((text, index) => {
                    const cell = document.createElement("td");
                    cell.textContent = text;
                    cell.classList.toggle("statisticsValue", (index > 0));
                    row.append(cell);
                });
                return row;
            }));
        }
        catch (error) {
            section?.classList.add("invisible");
        }
    };

    const renderHistory = () => {
        if (!usage || !document.getElementById("statisticsPeriod"))
            return;

        const today = usage.today;
        const thisMonth = `${today.slice(0, 8)}01`;
        let total = { downloaded: 0, uploaded: 0 };
        switch (Number(document.getElementById("statisticsPeriod").value)) {
            case 0:
                total = sumEntries(usage.days.filter((entry) => entry.date === today));
                break;
            case 1:
                total = sumEntries(usage.days.filter((entry) => entry.date >= addDays(today, -6)));
                break;
            case 2:
                total = sumEntries(usage.months.filter((entry) => entry.date === thisMonth));
                break;
            case 3:
                total = sumEntries(usage.months.filter((entry) => entry.date === addMonths(thisMonth, -1)));
                break;
            case 4:
                total = sumEntries(usage.months.filter((entry) => (entry.date >= addMonths(thisMonth, -11)) && (entry.date <= thisMonth)));
                break;
            case 5:
                total = { downloaded: statistics.alltimeDL, uploaded: statistics.alltimeUL };
                break;
        }
        setText("PeriodDL", unitText(total.downloaded));
        setText("PeriodUL", unitText(total.uploaded));
        setText("PeriodRatio", ratioText(total.downloaded, total.uploaded));
        setText("TrackingSince", new Date(`${usage.since}T12:00:00`).toLocaleDateString());

        // what was moved before tracking began is what the months don't account for
        const tracked = sumEntries(usage.months);
        const before = {
            downloaded: Math.max((statistics.alltimeDL - tracked.downloaded), 0),
            uploaded: Math.max((statistics.alltimeUL - tracked.uploaded), 0)
        };
        const rows = usage.months.toReversed().map((entry) => [monthName(entry.date), entry.downloaded, entry.uploaded]);
        if ((before.downloaded > 0) || (before.uploaded > 0))
            rows.push(["QBT_TR(Before tracking began)QBT_TR[CONTEXT=StatsDialog]", before.downloaded, before.uploaded]);

        const key = JSON.stringify(rows);
        if (key === historyKey)
            return;
        historyKey = key;
        const body = document.getElementById("statisticsMonths");
        body.replaceChildren(body.rows[0], ...rows.map(([name, downloaded, uploaded]) => {
            const row = document.createElement("tr");
            [name, unitText(downloaded), unitText(uploaded), ratioText(downloaded, uploaded)].forEach((text, index) => {
                const cell = document.createElement("td");
                cell.textContent = text;
                cell.classList.toggle("statisticsValue", (index > 0));
                row.append(cell);
            });
            return row;
        }));
    };

    const isHistoryOpen = () => {
        return !document.getElementById("statisticsHistoryTab").classList.contains("invisible");
    };

    // the rows of the open tab as plain text, like the Copy button of the desktop window
    const copyOpenTab = async () => {
        const link = document.querySelector("#statisticsTabs li.selected");
        const pane = document.getElementById(link.id.replace(/Link$/, "Tab"));
        const lines = [link.textContent, ""];
        for (const section of pane.querySelectorAll(".statisticsSection:not(.invisible)")) {
            if (lines.length > 2)
                lines.push("");
            lines.push(section.querySelector(".statisticsTitle").textContent);
            const select = section.querySelector("select");
            if (select)
                lines.push(select.selectedOptions[0].textContent);
            for (const row of section.querySelectorAll("tr"))
                lines.push([...row.cells].map((cell) => cell.textContent).join("\t"));
        }
        try {
            await navigator.clipboard.writeText(lines.join("\n"));
        }
        catch (error) {
            // copying needs a secure page; selecting the text by hand still works
        }
    };

    const init = () => {
        const menu = document.getElementById("statisticsTabs");
        menu.addEventListener("click", (event) => {
            const link = event.target.closest("li");
            if (link === null)
                return;
            for (const item of menu.querySelectorAll("li")) {
                item.classList.toggle("selected", (item === link));
                document.getElementById(item.id.replace(/Link$/, "Tab")).classList.toggle("invisible", (item !== link));
            }
            if (isHistoryOpen()) {
                refreshHistory();
                refreshAdded();
            }
        });
        document.getElementById("statisticsPeriod").addEventListener("change", renderHistory);
        document.getElementById("statisticsCopy").addEventListener("click", copyOpenTab);
        usage = null;
        historyKey = "";
        addedKey = "";
        historyRequestedAt = 0;
    };

    const render = () => {
        if (!document.getElementById("statisticsContent"))
            return;

        setText("SessionDL", unitText(statistics.sessionDL));
        setText("SessionUL", unitText(statistics.sessionUL));
        setText("SessionRatio", ratioText(statistics.sessionDL, statistics.sessionUL));
        setText("AlltimeDL", unitText(statistics.alltimeDL));
        setText("AlltimeUL", unitText(statistics.alltimeUL));
        setText("GlobalRatio", ratioText(statistics.alltimeDL, statistics.alltimeUL));
        setText("CurrentSpeed", `↓ ${window.qBittorrent.Misc.friendlyUnit(statistics.dlSpeed, true)}   ↑ ${window.qBittorrent.Misc.friendlyUnit(statistics.upSpeed, true)}`);

        setText("TorrentsDownloading", statistics.torrentsDownloading);
        setText("TorrentsSeeding", statistics.torrentsSeeding);
        setText("TorrentsStopped", statistics.torrentsStopped);
        setText("TorrentsErrored", statistics.torrentsErrored);
        document.getElementById("TorrentsErrored").classList.toggle("statisticsWarning", (statistics.torrentsErrored > 0));
        setText("TotalPeerConnections", statistics.totalPeerConnections);
        setText("RunningFor", window.qBittorrent.Misc.friendlyDuration(statistics.uptime));

        setText("WriteCacheOverload", `${statistics.writeCacheOverload}%`);
        setText("ReadCacheOverload", `${statistics.readCacheOverload}%`);
        setText("QueuedIOJobs", statistics.queuedIOJobs);
        setText("AverageTimeInQueue", `${statistics.averageTimeInQueue} ms`);
        setText("TotalQueuedSize", unitText(statistics.totalQueuedSize));
        setText("RequestLatency", `${statistics.requestLatency} ms`);
        setText("TotalBuffersSize", unitText(statistics.totalBuffersSize));
        setText("DHTNodesCount", statistics.dhtEnabled ? statistics.dhtNodes : "QBT_TR(Off)QBT_TR[CONTEXT=StatsDialog]");
        // Tracker statistics
        setText("queuedTrackerAnnounces", statistics.queuedTrackerAnnounces);
        setText("TotalWastedSession", unitText(statistics.totalWastedSession));

        if (isHistoryOpen()) {
            if ((Date.now() - historyRequestedAt) >= HISTORY_REFRESH_MS) {
                refreshHistory();
                refreshAdded();
            }
            else {
                renderHistory();
            }
        }
    };

    return exports();
})();
Object.freeze(window.qBittorrent.Statistics);
