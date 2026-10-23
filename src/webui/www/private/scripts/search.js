/*
 * MIT License
 * Copyright (C) 2024 Thomas Piccirello
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
window.qBittorrent.Search ??= (() => {
    const exports = () => {
        return {
            startStopSearch: startStopSearch,
            manageSearchPlugins: manageSearchPlugins,
            searchPlugins: searchPlugins,
            searchText: searchText,
            searchSeedsFilter: searchSeedsFilter,
            searchSizeFilter: searchSizeFilter,
            init: init,
            getPlugin: getPlugin,
            searchInTorrentName: searchInTorrentName,
            onSearchPatternChanged: onSearchPatternChanged,
            categorySelected: categorySelected,
            pluginSelected: pluginSelected,
            searchSeedsFilterChanged: searchSeedsFilterChanged,
            searchSizeFilterChanged: searchSizeFilterChanged,
            searchSizeFilterPrefixChanged: searchSizeFilterPrefixChanged,
            closeSearchTab: closeSearchTab,
            closeSelectedSearch: closeSelectedSearch,
            downloadSelected: downloadSearchTorrent,
            openSelectedDescriptionPages: openSearchTorrentDescriptionUrl,
        };
    };

    const localPreferences = new window.qBittorrent.LocalPreferences.LocalPreferences();

    const searchTabIdPrefix = "Search-";
    let loadSearchPluginsTimer = -1;
    const searchPlugins = [];
    let prevSearchPluginsResponse;
    // the storage can be unavailable (private windows, blocked site data)
    const loadPref = (key, defaultValue) => {
        try {
            return localPreferences.get(key, defaultValue);
        }
        catch (err) {
            return defaultValue;
        }
    };

    let selectedCategory = loadPref("search_category", "QBT_TR(All categories)QBT_TR[CONTEXT=SearchEngineWidget]");
    let selectedPlugin = loadPref("search_plugin", "enabled");
    let prevSelectedPlugin;
    let pluginsAvailable = false;
    let patternEdited = false;
    let noticeKind = "";

    let searchResultsTable;
    /** @type Map<number, {
     * searchPattern: string,
     * category: string,
     * plugins: string,
     * filterPattern: string,
     * seedsFilter: {min: number, max: number},
     * sizeFilter: {min: number, minUnit: number, max: number, maxUnit: number},
     * searchIn: string,
     * rows: [],
     * rowId: number,
     * selectedRowIds: number[],
     * running: boolean,
     * status: string,
     * errorMessage: string,
     * loadResultsTimer: Timer,
     * sort: {column: string, reverse: string},
     * }> **/
    const searchState = new Map();
    const searchText = {
        pattern: "",
        filterPattern: ""
    };
    const searchSeedsFilter = {
        min: 0,
        max: 0
    };
    const searchSizeFilter = {
        min: 0,
        minUnit: 2, // B = 0, KiB = 1, MiB = 2, GiB = 3, TiB = 4, PiB = 5, EiB = 6
        max: 0,
        maxUnit: 3
    };

    const searchResultsTabsContextMenu = new window.qBittorrent.ContextMenu.ContextMenu({
        targets: ".searchTab",
        menu: "searchResultsTabsMenu",
        actions: {
            stopSearch: (tab) => {
                const searchId = getSearchIdFromTab(tab);
                stopSearch(searchId);
            },
            refreshTab: (tab) => { refreshSearch(tab); },
            useSearchText: (tab) => {
                const searchId = getSearchIdFromTab(tab);
                const state = searchState.get(searchId);
                if (state) {
                    document.getElementById("searchPattern").value = state.searchPattern;
                    updateSearchButtonState();
                }
            },
            closeTab: (tab) => { closeSearchTab(tab); },
            closeAllTabs: () => {
                for (const tab of document.querySelectorAll("#searchTabs .searchTab"))
                    closeSearchTab(tab);
            }
        },
        offsets: {
            x: 2,
            y: -60
        },
        onShow: function() {
            const tab = this.options.element;
            setActiveTab(tab);

            const searchId = getSearchIdFromTab(tab);
            const state = searchState.get(searchId);
            if (state && state.running)
                this.showItem("stopSearch");
            else
                this.hideItem("stopSearch");
        }
    });

    const init = () => {
        document.getElementById("searchInTorrentName").value = (window.qBittorrent.ClientData.get("search_in_filter") === "everywhere") ? "everywhere" : "names";
        const searchResultsTableContextMenu = new window.qBittorrent.ContextMenu.ContextMenu({
            targets: "#searchResultsTableDiv tbody tr",
            menu: "searchResultsTableMenu",
            actions: {
                OpenDownloadWindow: openDownloadWindow,
                OpenSharedDownloadWindow: openSharedDownloadWindow,
                Download: downloadSearchTorrent,
                OpenDescriptionUrl: openSearchTorrentDescriptionUrl
            },
            offsets: {
                x: 0,
                y: -60
            },
            onShow: function() {
                const multiSelect = searchResultsTable.selectedRowsIds().length > 1;

                const openDownloadAnchor = this.menu.querySelector("a[href=\"#OpenDownloadWindow\"]");
                const label = multiSelect
                    ? "QBT_TR(Open separate download windows)QBT_TR[CONTEXT=SearchJobWidget]"
                    : "QBT_TR(Open download window)QBT_TR[CONTEXT=SearchJobWidget]";
                openDownloadAnchor.querySelector("img").alt = label;
                openDownloadAnchor.lastChild.nodeValue = ` ${label}`;

                if (multiSelect)
                    this.showItem("OpenSharedDownloadWindow");
                else
                    this.hideItem("OpenSharedDownloadWindow");
            }
        });
        searchResultsTable = new window.qBittorrent.DynamicTable.SearchResultsTable();
        searchResultsTable.setup("searchResultsTableDiv", "searchResultsTableFixedHeaderDiv", searchResultsTableContextMenu);

        // without a saved sort, the most seeded results come first, as on the desktop
        if (loadPref(`sorted_column_${searchResultsTable.dynamicTableDivId}`) === null) {
            searchResultsTable.sortedColumn = "nbSeeders";
            searchResultsTable.reverseSort = "1";
            searchResultsTable.setSortedColumnIcon("nbSeeders", null, true);
        }

        getPlugins();
        setupListHandle();

        searchResultsTable.dynamicTableDiv.addEventListener("dblclick", (e) => { openDownloadWindow(); });
        // the table doesn't tell when the selection changes, so look again after the events that change it
        for (const type of ["click", "keydown", "contextmenu", "touchstart"])
            searchResultsTable.dynamicTableDiv.addEventListener(type, () => { setTimeout(updateActionButtons, 0); });

        // listen for changes to searchInNameFilter
        let searchInNameFilterTimer = -1;
        document.getElementById("searchInNameFilter").addEventListener("input", (event) => {
            clearTimeout(searchInNameFilterTimer);
            searchInNameFilterTimer = setTimeout(() => {
                searchInNameFilterTimer = -1;

                const value = document.getElementById("searchInNameFilter").value;
                searchText.filterPattern = value;
                searchFilterChanged();
            }, window.qBittorrent.Misc.FILTER_INPUT_DELAY);
        });

        document.getElementById("SearchPanel").addEventListener("keydown", (event) => {
            switch (event.key) {
                case "Enter":
                    event.preventDefault();
                    event.stopPropagation();

                    switch (event.target.id) {
                        case "manageSearchPlugins":
                            manageSearchPlugins();
                            break;
                        case "searchPattern":
                            startSearchFromBox();
                            break;
                        case "startSearchButton":
                        case "searchDownloadButton":
                        case "searchOpenPageButton":
                        case "closeSearchButton":
                            event.target.click();
                            break;
                    }

                    break;
            }
        });

        restoreSearchJobs();
    };

    const restoreSearchJobs = () => {
        fetch("api/v2/search/status", {
                method: "GET",
                cache: "no-store"
            })
            .then(async (response) => {
                if (!response.ok)
                    return;

                const serverJobs = await response.json();

                for (const job of serverJobs) {
                    if (!document.getElementById(`${searchTabIdPrefix}${job.id}`))
                        createSearchTab(job.id, job.pattern, job.category, job.plugins);
                }
            });
    };

    const numSearchTabs = () => {
        return document.querySelectorAll("#searchTabs li").length;
    };

    const getSearchIdFromTab = (tab) => {
        return Number(tab.id.substring(searchTabIdPrefix.length));
    };

    const setupListHandle = () => {
        const pane = document.getElementById("searchListPane");
        const handle = document.getElementById("searchListHandle");
        const minWidth = 150;
        const clampWidth = (width) => {
            const maxWidth = Math.max(minWidth, document.getElementById("searchBody").clientWidth - 300);
            return Math.min(Math.max(width, minWidth), maxWidth);
        };

        // the page is hidden until the plugins are known, so only the lower limit can be applied here
        const savedWidth = Number(loadPref("search_list_width", document.getElementById("Filters")?.offsetWidth || 270));
        if (Number.isFinite(savedWidth))
            pane.style.width = `${Math.max(savedWidth, minWidth)}px`;

        let startX = 0;
        let startWidth = 0;
        let dragging = false;
        handle.addEventListener("pointerdown", (e) => {
            dragging = true;
            startX = e.clientX;
            startWidth = pane.offsetWidth;
            handle.setPointerCapture(e.pointerId);
            e.preventDefault();
        });
        handle.addEventListener("pointermove", (e) => {
            if (dragging)
                pane.style.width = `${clampWidth(startWidth + e.clientX - startX)}px`;
        });
        handle.addEventListener("pointerup", () => {
            if (!dragging)
                return;
            dragging = false;
            localPreferences.set("search_list_width", pane.offsetWidth);
        });
    };

    const showNotice = (text, kind = "") => {
        const notice = document.getElementById("searchNotice");
        notice.textContent = text;
        notice.classList.toggle("invisible", text === "");
        noticeKind = (text === "") ? "" : kind;
    };

    // a notice goes away when what caused it works again
    const clearNotice = (kind) => {
        if ((kind === undefined) || (kind === noticeKind))
            showNotice("");
    };

    const statusInfo = (status) => {
        switch (status) {
            case "finished":
                return { icon: "images/task-complete.svg", text: "QBT_TR(Search has finished)QBT_TR[CONTEXT=SearchJobWidget]" };
            case "noResults":
                return { icon: "images/dialog-warning.svg", text: "QBT_TR(Search returned no results)QBT_TR[CONTEXT=SearchJobWidget]" };
            case "aborted":
                return { icon: "images/task-reject.svg", text: "QBT_TR(Search aborted)QBT_TR[CONTEXT=SearchJobWidget]" };
            case "error":
                return { icon: "images/error.svg", text: "QBT_TR(An error occurred during search...)QBT_TR[CONTEXT=SearchJobWidget]" };
            default:
                return { icon: "images/queued.svg", text: "QBT_TR(Searching...)QBT_TR[CONTEXT=SearchJobWidget]" };
        }
    };

    // number of rows that pass the filters saved for a search
    const countVisibleRows = (state) => {
        const toBytes = (value, unit) => ((value > 0) ? (value * Math.pow(1024, unit)) : 0);
        let minSize = toBytes(state.sizeFilter.min, state.sizeFilter.minUnit);
        let maxSize = toBytes(state.sizeFilter.max, state.sizeFilter.maxUnit);
        if ((minSize > maxSize) && (maxSize > 0))
            [minSize, maxSize] = [maxSize, minSize];

        let minSeeds = (state.seedsFilter.min > 0) ? Number(state.seedsFilter.min) : 0;
        let maxSeeds = (state.seedsFilter.max > 0) ? Number(state.seedsFilter.max) : 0;
        if ((minSeeds > maxSeeds) && (maxSeeds > 0))
            [minSeeds, maxSeeds] = [maxSeeds, minSeeds];

        const searchTerms = state.searchPattern.toLowerCase().split(" ");
        const filterTerms = state.filterPattern.toLowerCase().split(" ");
        let count = 0;
        for (const row of state.rows) {
            if ((state.searchIn === "names") && !window.qBittorrent.Misc.containsAllTerms(row.fileName, searchTerms))
                continue;
            if (!window.qBittorrent.Misc.containsAllTerms(row.fileName, filterTerms))
                continue;
            if (((minSize > 0) && (row.fileSize < minSize)) || ((maxSize > 0) && (row.fileSize > maxSize)))
                continue;
            if (((minSeeds > 0) && (row.nbSeeders < minSeeds)) || ((maxSeeds > 0) && (row.nbSeeders > maxSeeds)))
                continue;
            ++count;
        }
        return count;
    };

    const updateTabTitle = (searchId, shownCount) => {
        const state = searchState.get(searchId);
        if (!state)
            return;

        const tab = document.getElementById(`${searchTabIdPrefix}${searchId}`);
        if (!tab)
            return;

        const shown = shownCount ?? ((searchId === getSelectedSearchId())
            ? searchResultsTable.getFilteredAndSortedRows().length
            : countVisibleRows(state));
        tab.querySelector(".tabTitle").textContent = `${state.searchPattern} (${shown})`;
        tab.title = `${state.searchPattern}\n${statusText(state, shown)}`;
    };

    const statusText = (state, shown = state.rows.length) => {
        const total = state.rows.length;
        const hidden = total - shown;
        const shownText = (hidden > 0)
            ? "QBT_TR(%1 of %2 shown (%3 hidden))QBT_TR[CONTEXT=SearchJobWidget]".replace("%1", shown).replace("%2", total).replace("%3", hidden)
            : "QBT_TR(%1 of %2 shown)QBT_TR[CONTEXT=SearchJobWidget]".replace("%1", shown).replace("%2", total);

        switch (state.status) {
            case "ongoing":
                return "QBT_TR(Searching: %1)QBT_TR[CONTEXT=SearchJobWidget]".replace("%1", shownText);
            case "finished":
                return "QBT_TR(Finished: %1)QBT_TR[CONTEXT=SearchJobWidget]".replace("%1", shownText);
            case "aborted":
                return (total > 0)
                    ? "QBT_TR(Search aborted: %1)QBT_TR[CONTEXT=SearchJobWidget]".replace("%1", shownText)
                    : statusInfo("aborted").text;
            case "error":
                return state.errorMessage || statusInfo("error").text;
            default:
                return statusInfo(state.status).text;
        }
    };

    // the status line under the results, and the same text in the empty table
    const updateStatusLine = (searchId, shownCount) => {
        const state = searchState.get(searchId);
        const statusLine = document.getElementById("searchStatusText");
        const emptyText = document.getElementById("searchResultsEmptyText");
        if (!state) {
            statusLine.textContent = "";
            emptyText.classList.add("invisible");
            return;
        }

        const text = statusText(state, shownCount ?? searchResultsTable.getFilteredAndSortedRows().length);
        statusLine.textContent = text;
        emptyText.textContent = text;
        emptyText.classList.toggle("invisible", (state.rows.length > 0) || !["noResults", "aborted", "error"].includes(state.status));
    };

    const updateActionButtons = () => {
        const hasSelection = (searchResultsTable.selectedRowsIds().length > 0);
        document.getElementById("searchDownloadButton").disabled = !hasSelection;
        document.getElementById("searchOpenPageButton").disabled = !hasSelection;
    };

    // after the table, the filters or the selected search changed
    const updateResultsInfo = () => {
        const searchId = getSelectedSearchId();
        const shown = (searchId === null) ? 0 : searchResultsTable.getFilteredAndSortedRows().length;
        updateStatusLine(searchId, shown);
        updateTabTitle(searchId, shown);
        updateActionButtons();
    };

    const setSearchStatus = (searchId, status, errorMessage = "") => {
        const state = searchState.get(searchId);
        if (!state)
            return;

        state.status = status;
        state.errorMessage = errorMessage;

        const tab = document.getElementById(`${searchTabIdPrefix}${searchId}`);
        if (tab) {
            const info = statusInfo(status);
            const statusIcon = tab.querySelector(".statusIcon");
            statusIcon.alt = info.text;
            statusIcon.src = info.icon;
        }

        if (searchId === getSelectedSearchId())
            updateResultsInfo();
        else
            updateTabTitle(searchId);
    };

    const createSearchTab = (searchId, pattern, category, plugins) => {
        const newTabId = `${searchTabIdPrefix}${searchId}`;

        const tabElem = document.createElement("span");
        tabElem.className = "link";

        const statusIcon = getStatusIconElement("QBT_TR(Searching...)QBT_TR[CONTEXT=SearchJobWidget]", "images/queued.svg");
        tabElem.appendChild(statusIcon);

        const tabTitleElem = document.createElement("span");
        tabTitleElem.className = "tabTitle";
        tabTitleElem.textContent = pattern;
        tabElem.appendChild(tabTitleElem);

        const listItem = document.createElement("li");
        listItem.id = newTabId;
        listItem.classList.add("searchTab");
        listItem.addEventListener("click", (e) => {
            setActiveTab(listItem);
        });
        listItem.addEventListener("auxclick", (e) => {
            // middle click closes it
            if (e.button === 1) {
                e.preventDefault();
                closeSearchTab(listItem);
            }
        });
        listItem.addEventListener("dblclick", (e) => {
            // the pattern goes back into the search box
            document.getElementById("searchPattern").value = pattern;
            patternEdited = true;
            updateSearchButtonState();
        });
        listItem.appendChild(tabElem);
        document.getElementById("searchTabs").appendChild(listItem);
        searchResultsTabsContextMenu.addTarget(listItem);

        // unhide the results elements
        if (numSearchTabs() >= 1) {
            document.getElementById("searchResultsNoSearches").classList.add("invisible");
            document.getElementById("searchResultsFilters").classList.remove("invisible");
            document.getElementById("searchResultsTableContainer").classList.remove("invisible");
            document.getElementById("searchResultsFooter").classList.remove("invisible");
        }
        document.getElementById("closeSearchButton").disabled = false;

        searchState.set(searchId, {
            searchPattern: pattern,
            category: category,
            plugins: Array.isArray(plugins) ? plugins.join("|") : plugins,
            filterPattern: "",
            seedsFilter: { min: 0, max: 0 },
            sizeFilter: { min: 0, minUnit: 2, max: 0, maxUnit: 3 },
            searchIn: getSearchInTorrentName(),
            rows: [],
            rowId: 0,
            selectedRowIds: [],
            running: true,
            status: "ongoing",
            errorMessage: "",
            loadResultsTimer: -1,
            sort: { column: searchResultsTable.sortedColumn, reverse: searchResultsTable.reverseSort },
        });

        // select new tab: this empties the table and sets the filters back to their defaults
        setActiveTab(listItem);
        updateSearchResultsData(searchId);
    };

    const refreshSearchTab = (oldSearchId, searchId, pattern) => {
        fetch("api/v2/search/delete", {
            method: "POST",
            body: new URLSearchParams({
                id: oldSearchId
            })
        });

        // update existing tab w/ new search id
        const tab = document.getElementById(`${searchTabIdPrefix}${oldSearchId}`);
        tab.id = `${searchTabIdPrefix}${searchId}`;

        // copy over relevant state
        const state = searchState.get(oldSearchId);
        state.rows = [];
        state.rowId = 0;
        state.selectedRowIds = [];
        state.running = true;
        state.loadResultsTimer = -1;
        searchState.set(searchId, state);
        searchState.delete(oldSearchId);

        searchResultsTable.clear();
        setSearchStatus(searchId, "ongoing");
        updateSearchButtonState();
        updateSearchResultsData(searchId);
    };

    const closeSearchTab = (el) => {
        const tab = el.closest("li.searchTab");
        if (!tab)
            return;

        const isTabSelected = tab.classList.contains("selected");
        const newTabToSelect = isTabSelected ? (tab.nextSibling || tab.previousSibling) : null;

        const searchId = getSearchIdFromTab(tab);
        resetSearchState(searchId);

        const state = searchState.get(searchId);
        // don't bother sending a stop request if already stopped
        if (state && state.running)
            stopSearch(searchId);

        tab.remove();

        fetch("api/v2/search/delete", {
            method: "POST",
            body: new URLSearchParams({
                id: searchId
            })
        });

        searchState.delete(searchId);

        if (numSearchTabs() === 0) {
            resetFilters();
            searchResultsTable.clear();

            updateStatusLine(null);
            updateActionButtons();
            document.getElementById("closeSearchButton").disabled = true;
            document.getElementById("searchResultsNoSearches").classList.remove("invisible");
            document.getElementById("searchResultsFilters").classList.add("invisible");
            document.getElementById("searchResultsTableContainer").classList.add("invisible");
            document.getElementById("searchResultsFooter").classList.add("invisible");
        }
        else if (isTabSelected && newTabToSelect) {
            setActiveTab(newTabToSelect);
        }
    };

    const closeSelectedSearch = () => {
        const tab = document.querySelector("#searchTabs li.selected");
        if (tab)
            closeSearchTab(tab);
    };

    const saveCurrentTabState = () => {
        const currentSearchId = getSelectedSearchId();
        if (!currentSearchId)
            return;

        const state = searchState.get(currentSearchId);
        if (!state)
            return;

        state.filterPattern = searchText.filterPattern;
        state.seedsFilter = {
            min: searchSeedsFilter.min,
            max: searchSeedsFilter.max,
        };
        state.sizeFilter = {
            min: searchSizeFilter.min,
            minUnit: searchSizeFilter.minUnit,
            max: searchSizeFilter.max,
            maxUnit: searchSizeFilter.maxUnit,
        };
        state.searchIn = getSearchInTorrentName();

        state.sort = {
            column: searchResultsTable.sortedColumn,
            reverse: searchResultsTable.reverseSort,
        };

        // we must copy the array to avoid taking a reference to it
        state.selectedRowIds = [...searchResultsTable.selectedRows];
    };

    const setActiveTab = (tab) => {
        const searchId = getSearchIdFromTab(tab);
        if (searchId === getSelectedSearchId())
            return;

        saveCurrentTabState();

        for (const item of document.querySelectorAll("#searchTabs li")) {
            item.classList.toggle("selected", item === tab);
            item.classList.toggle("selectedFilter", item === tab);
        }
        patternEdited = false;

        const state = searchState.get(searchId);
        let rowsToSelect = [];

        // restore table rows
        searchResultsTable.clear();
        if (state) {
            for (const row of state.rows)
                searchResultsTable.updateRowData(row);

            rowsToSelect = state.selectedRowIds;

            // restore filters
            searchText.pattern = state.searchPattern;
            searchText.filterPattern = state.filterPattern;
            document.getElementById("searchInNameFilter").value = state.filterPattern;

            searchSeedsFilter.min = state.seedsFilter.min;
            searchSeedsFilter.max = state.seedsFilter.max;
            document.getElementById("searchMinSeedsFilter").value = state.seedsFilter.min;
            document.getElementById("searchMaxSeedsFilter").value = limitText(state.seedsFilter.max);

            searchSizeFilter.min = state.sizeFilter.min;
            searchSizeFilter.minUnit = state.sizeFilter.minUnit;
            searchSizeFilter.max = state.sizeFilter.max;
            searchSizeFilter.maxUnit = state.sizeFilter.maxUnit;
            document.getElementById("searchMinSizeFilter").value = state.sizeFilter.min;
            document.getElementById("searchMinSizePrefix").value = state.sizeFilter.minUnit;
            document.getElementById("searchMaxSizeFilter").value = limitText(state.sizeFilter.max);
            document.getElementById("searchMaxSizePrefix").value = state.sizeFilter.maxUnit;

            searchResultsTable.setSortedColumn(state.sort.column, state.sort.reverse);

            document.getElementById("searchInTorrentName").value = state.searchIn;
        }

        // must restore all filters before calling updateTable
        searchResultsTable.updateTable();

        // must reselect rows after calling updateTable
        if (rowsToSelect.length > 0)
            searchResultsTable.reselectRows(rowsToSelect);

        updateResultsInfo();
        updateSearchButtonState();
    };

    const getStatusIconElement = (text, image) => {
        const statusIcon = document.createElement("img");
        statusIcon.alt = text;
        statusIcon.src = image;
        statusIcon.className = "statusIcon";
        statusIcon.width = "16";
        statusIcon.height = "16";
        return statusIcon;
    };

    // a 0 limit means "no limit", which the fields show as an empty box with the infinity sign
    const limitText = (value) => ((Number(value) > 0) ? value : "");

    const startSearch = (pattern, category, plugins) => {
        fetch("api/v2/search/start", {
                method: "POST",
                body: new URLSearchParams({
                    pattern: pattern,
                    category: category,
                    plugins: plugins
                })
            })
            .then(async (response) => {
                if (!response.ok) {
                    // the server says why, e.g. Python is missing or too many searches are running
                    const message = (response.status === 409) ? (await response.text()) : "";
                    showNotice(message || "QBT_TR(Unable to start the search.)QBT_TR[CONTEXT=SearchEngineWidget]", "start");
                    return;
                }

                showNotice("");
                const responseJSON = await response.json();
                const searchId = responseJSON.id;
                createSearchTab(searchId, pattern, category, plugins);
                updateSearchButtonState();
            })
            .catch((error) => {
                console.error(error);
                showNotice("QBT_TR(qBittorrent client is not reachable)QBT_TR[CONTEXT=HttpServer]", "start");
            });
    };

    const refreshSearch = (el) => {
        const tab = el.closest("li.searchTab");
        if (!tab)
            return;

        const oldSearchId = getSearchIdFromTab(tab);
        const state = searchState.get(oldSearchId);
        const pattern = state.searchPattern;

        fetch("api/v2/search/start", {
                method: "POST",
                body: new URLSearchParams({
                    pattern: state.searchPattern,
                    category: state.category,
                    plugins: state.plugins
                })
            })
            .then(async (response) => {
                if (!response.ok) {
                    const message = (response.status === 409) ? (await response.text()) : "";
                    showNotice(message || "QBT_TR(Unable to start the search.)QBT_TR[CONTEXT=SearchEngineWidget]", "start");
                    return;
                }

                showNotice("");
                const responseJSON = await response.json();
                const searchId = responseJSON.id;
                refreshSearchTab(oldSearchId, searchId, pattern);
            })
            .catch((error) => {
                console.error(error);
                showNotice("QBT_TR(qBittorrent client is not reachable)QBT_TR[CONTEXT=HttpServer]", "start");
            });
    };

    const stopSearch = (searchId) => {
        fetch("api/v2/search/stop", {
                method: "POST",
                body: new URLSearchParams({
                    id: searchId
                })
            })
            .then((response) => {
                if (!response.ok)
                    return;

                resetSearchState(searchId);
                // not strictly necessary to do this when the tab is being closed, but there's no harm in it
                setSearchStatus(searchId, "aborted");
            });
    };

    const getSelectedSearchId = () => {
        const selectedTab = document.getElementById("searchTabs").querySelector("li.selected");
        return selectedTab ? getSearchIdFromTab(selectedTab) : null;
    };

    const updateSearchButtonState = () => {
        const state = searchState.get(getSelectedSearchId());
        // the selected search is running, and the box wasn't changed to start another one
        const canStop = (state !== undefined) && state.running && !patternEdited;
        const hasPattern = (document.getElementById("searchPattern").value.trim() !== "");

        const button = document.getElementById("startSearchButton");
        button.querySelector("span").textContent = canStop
            ? "QBT_TR(Stop)QBT_TR[CONTEXT=SearchEngineWidget]"
            : "QBT_TR(Search)QBT_TR[CONTEXT=SearchEngineWidget]";
        button.disabled = !pluginsAvailable || (!canStop && !hasPattern);
    };

    const startSearchFromBox = () => {
        const pattern = document.getElementById("searchPattern").value.trim();
        const category = document.getElementById("categorySelect").value;
        const plugins = document.getElementById("pluginsSelect").value;

        if (!pattern) {
            showNotice("QBT_TR(Empty search pattern)QBT_TR[CONTEXT=SearchEngineWidget]", "start");
            return;
        }
        if (!category || !plugins)
            return;

        searchText.pattern = pattern;
        startSearch(pattern, category, plugins);
    };

    const startStopSearch = () => {
        const currentSearchId = getSelectedSearchId();
        const state = searchState.get(currentSearchId);

        if (state && state.running && !patternEdited)
            stopSearch(currentSearchId);
        else
            startSearchFromBox();
    };

    const openSearchTorrentDescriptionUrl = () => {
        const blockedUrls = [];
        for (const rowID of searchResultsTable.selectedRowsIds()) {
            const { descrLink, fileName } = searchResultsTable.getRow(rowID).full_data;
            if (window.qBittorrent.Misc.isHttpUrl(descrLink)) {
                window.open(descrLink, "_blank");
            }
            else {
                console.error(`Blocked opening search result description page URL. Only http:// and https:// links can be opened. Name: "${fileName}". URL: "${descrLink}".`);
                // collapse whitespace and cap the length so that a crafted URL can't pad the dialog with its own text
                blockedUrls.push(descrLink.replace(/\s+/g, " ").slice(0, 256));
            }
        }

        if (blockedUrls.length > 0)
            alert(`QBT_TR(Blocked opening search result description page URL. Only http:// and https:// links can be opened.)QBT_TR[CONTEXT=SearchJobWidget]\n\n${blockedUrls.join("\n")}`);
    };

    const copySearchTorrentName = () => {
        const names = [];
        for (const rowId of searchResultsTable.selectedRowsIds())
            names.push(searchResultsTable.getRow(rowId).full_data.fileName);
        return names.join("\n");
    };

    const copySearchTorrentDownloadLink = () => {
        const urls = [];
        for (const rowId of searchResultsTable.selectedRowsIds())
            urls.push(searchResultsTable.getRow(rowId).full_data.fileUrl);
        return urls.join("\n");
    };

    const copySearchTorrentDescriptionUrl = () => {
        const urls = [];
        for (const rowId of searchResultsTable.selectedRowsIds())
            urls.push(searchResultsTable.getRow(rowId).full_data.descrLink);
        return urls.join("\n");
    };

    const openDownloadWindow = () => {
        for (const rowID of searchResultsTable.selectedRowsIds()) {
            const { engineName, fileName, fileUrl } = searchResultsTable.getRow(rowID).full_data;
            qBittorrent.Client.createAddTorrentWindow(fileName, fileUrl, undefined, { engines: [engineName] });
        }
    };

    const openSharedDownloadWindow = () => {
        const rowIds = searchResultsTable.selectedRowsIds();
        if (rowIds.length === 0)
            return;
        const urls = [];
        const engines = [];
        for (const rowID of rowIds) {
            const row = searchResultsTable.getRow(rowID).full_data;
            urls.push(row.fileUrl);
            engines.push(row.engineName);
        }
        const title = "QBT_TR(Add %1 torrents)QBT_TR[CONTEXT=SearchJobWidget]".replace("%1", urls.length);
        qBittorrent.Client.createAddTorrentWindow(title, urls.join("\n"), undefined, { shared: true, engines: engines });
    };

    const downloadSearchTorrent = () => {
        const urls = [];
        for (const rowID of searchResultsTable.selectedRowsIds()) {
            const { fileUrl } = searchResultsTable.getRow(rowID).full_data;
            urls.push(fileUrl);
        }

        if (urls.length === 0)
            return;

        fetch("api/v2/torrents/add", {
                method: "POST",
                body: new URLSearchParams({
                    urls: urls.join("\n")
                })
            })
            .then((response) => {
                if (!response.ok)
                    showNotice("QBT_TR(Unable to add torrents.)QBT_TR[CONTEXT=HttpServer]", "download");
                else
                    clearNotice("download");
            })
            .catch((error) => {
                console.error("Failed to add torrents:", error);
                showNotice("QBT_TR(Unable to add torrents.)QBT_TR[CONTEXT=HttpServer]", "download");
            });
    };

    const manageSearchPlugins = () => {
        const id = "searchPlugins";
        if (!document.getElementById(id)) {
            new MochaUI.Window({
                id: id,
                title: "QBT_TR(Search plugins)QBT_TR[CONTEXT=PluginSelectDlg]",
                icon: "images/qbittorrent-tray.svg",
                loadMethod: "xhr",
                contentURL: "views/searchplugins.html?v=${CACHEID}",
                scrollbars: false,
                maximizable: false,
                paddingVertical: 0,
                paddingHorizontal: 0,
                width: loadWindowWidth(id, 600),
                height: loadWindowHeight(id, 400),
                onResize: window.qBittorrent.Misc.createDebounceHandler(500, (e) => {
                    saveWindowSize(id);
                }),
                onBeforeBuild: () => {
                    loadSearchPlugins();
                },
                onClose: () => {
                    clearTimeout(loadSearchPluginsTimer);
                    loadSearchPluginsTimer = -1;
                }
            });
        }
    };

    const loadSearchPlugins = () => {
        getPlugins();
        loadSearchPluginsTimer = loadSearchPlugins.delay(2000);
    };

    const onSearchPatternChanged = () => {
        patternEdited = true;
        if (noticeKind === "start")
            showNotice("");
        updateSearchButtonState();
    };

    const categorySelected = () => {
        selectedCategory = document.getElementById("categorySelect").value;
        if (selectedCategory !== "")
            localPreferences.set("search_category", selectedCategory);
    };

    const pluginSelected = () => {
        selectedPlugin = document.getElementById("pluginsSelect").value;
        if (selectedPlugin !== "")
            localPreferences.set("search_plugin", selectedPlugin);

        if (selectedPlugin !== prevSelectedPlugin) {
            prevSelectedPlugin = selectedPlugin;
            getSearchCategories();
        }
    };

    const reselectCategory = () => {
        for (let i = 0; i < document.getElementById("categorySelect").options.length; ++i) {
            if (document.getElementById("categorySelect").options[i].get("value") === selectedCategory)
                document.getElementById("categorySelect").options[i].selected = true;
        }

        categorySelected();
    };

    const reselectPlugin = () => {
        for (let i = 0; i < document.getElementById("pluginsSelect").options.length; ++i) {
            if (document.getElementById("pluginsSelect").options[i].get("value") === selectedPlugin)
                document.getElementById("pluginsSelect").options[i].selected = true;
        }

        pluginSelected();
    };

    const resetSearchState = (searchId) => {
        const state = searchState.get(searchId);
        if (state) {
            state.running = false;
            clearTimeout(state.loadResultsTimer);
            state.loadResultsTimer = -1;
        }
        updateSearchButtonState();
    };

    const getSearchCategories = () => {
        const populateCategorySelect = (categories) => {
            const categoryOptions = [];

            for (const category of categories) {
                const option = document.createElement("option");
                option.value = category.id;
                option.textContent = category.name;
                categoryOptions.push(option);
            }

            // first category is "All Categories"
            if (categoryOptions.length > 1) {
                // add separator
                const option = document.createElement("option");
                option.disabled = true;
                option.textContent = "──────────";
                categoryOptions.splice(1, 0, option);
            }

            document.getElementById("categorySelect").replaceChildren(...categoryOptions);
        };

        const selectedPlugin = document.getElementById("pluginsSelect").value;

        if ((selectedPlugin === "all") || (selectedPlugin === "enabled")) {
            const uniqueCategories = {};
            for (const plugin of searchPlugins) {
                if ((selectedPlugin === "enabled") && !plugin.enabled)
                    continue;
                for (const category of plugin.supportedCategories) {
                    if (uniqueCategories[category.id] === undefined)
                        uniqueCategories[category.id] = category;
                }
            }
            // we must sort the ids to maintain consistent order.
            const categories = Object.keys(uniqueCategories).sort().map(id => uniqueCategories[id]);
            populateCategorySelect(categories);
        }
        else {
            const plugin = getPlugin(selectedPlugin);
            const plugins = (plugin === null) ? [] : plugin.supportedCategories;
            populateCategorySelect(plugins);
        }

        reselectCategory();
    };

    const getPlugins = () => {
        fetch("api/v2/search/plugins", {
                method: "GET",
                cache: "no-store"
            })
            .then(async (response) => {
                if (!response.ok)
                    return;

                const responseJSON = await response.json();

                const createOption = (text, value, disabled = false) => {
                    const option = document.createElement("option");
                    if (value !== undefined)
                        option.value = value;
                    option.textContent = text;
                    option.disabled = disabled;
                    return option;
                };

                if (prevSearchPluginsResponse !== responseJSON) {
                    prevSearchPluginsResponse = responseJSON;
                    searchPlugins.length = 0;
                    for (const plugin of responseJSON)
                        searchPlugins.push(plugin);

                    const pluginOptions = [createOption("QBT_TR(Only enabled)QBT_TR[CONTEXT=SearchEngineWidget]", "enabled"), createOption("QBT_TR(All plugins)QBT_TR[CONTEXT=SearchEngineWidget]", "all")];

                    const searchPluginsEmpty = (searchPlugins.length === 0);
                    pluginsAvailable = !searchPluginsEmpty;
                    document.getElementById("searchListPane").classList.toggle("invisible", searchPluginsEmpty);
                    document.getElementById("searchListHandle").classList.toggle("invisible", searchPluginsEmpty);
                    if (!searchPluginsEmpty) {
                        document.getElementById("searchResultsNoPlugins").classList.add("invisible");
                        if (numSearchTabs() === 0)
                            document.getElementById("searchResultsNoSearches").classList.remove("invisible");

                        // sort plugins alphabetically
                        searchPlugins.sort((left, right) => {
                            const leftName = left.fullName;
                            const rightName = right.fullName;
                            return window.qBittorrent.Misc.naturalSortCollator.compare(leftName, rightName);
                        });

                        for (const plugin of searchPlugins) {
                            if (plugin.enabled === true)
                                pluginOptions.push(createOption(plugin.fullName, plugin.name));
                        }

                        if (pluginOptions.length > 2)
                            pluginOptions.splice(2, 0, createOption("──────────", undefined, true));
                    }

                    document.getElementById("pluginsSelect").replaceChildren(...pluginOptions);

                    document.getElementById("searchPattern").disabled = searchPluginsEmpty;
                    document.getElementById("categorySelect").disabled = searchPluginsEmpty;
                    document.getElementById("pluginsSelect").disabled = searchPluginsEmpty;
                    updateSearchButtonState();

                    if (window.qBittorrent.SearchPlugins !== undefined)
                        window.qBittorrent.SearchPlugins.updateTable();

                    reselectPlugin();
                }
            });
    };

    const getPlugin = (name) => {
        for (const searchPlugin of searchPlugins) {
            if (searchPlugin.name === name)
                return searchPlugin;
        }

        return null;
    };

    const resetFilters = () => {
        searchText.filterPattern = "";
        document.getElementById("searchInNameFilter").value = "";

        searchSeedsFilter.min = 0;
        searchSeedsFilter.max = 0;
        document.getElementById("searchMinSeedsFilter").value = searchSeedsFilter.min;
        document.getElementById("searchMaxSeedsFilter").value = limitText(searchSeedsFilter.max);

        searchSizeFilter.min = 0;
        searchSizeFilter.minUnit = 2; // B = 0, KiB = 1, MiB = 2, GiB = 3, TiB = 4, PiB = 5, EiB = 6
        searchSizeFilter.max = 0;
        searchSizeFilter.maxUnit = 3;
        document.getElementById("searchMinSizeFilter").value = searchSizeFilter.min;
        document.getElementById("searchMinSizePrefix").value = searchSizeFilter.minUnit;
        document.getElementById("searchMaxSizeFilter").value = limitText(searchSizeFilter.max);
        document.getElementById("searchMaxSizePrefix").value = searchSizeFilter.maxUnit;
    };

    const getSearchInTorrentName = () => {
        return (document.getElementById("searchInTorrentName").value === "names") ? "names" : "everywhere";
    };

    const searchInTorrentName = () => {
        // don't await this
        window.qBittorrent.ClientData.set({ search_in_filter: getSearchInTorrentName() }).catch(console.error);
        searchFilterChanged();
    };

    const searchSeedsFilterChanged = () => {
        searchSeedsFilter.min = document.getElementById("searchMinSeedsFilter").value;
        searchSeedsFilter.max = document.getElementById("searchMaxSeedsFilter").value;

        searchFilterChanged();
    };

    const searchSizeFilterChanged = () => {
        searchSizeFilter.min = document.getElementById("searchMinSizeFilter").value;
        searchSizeFilter.minUnit = document.getElementById("searchMinSizePrefix").value;
        searchSizeFilter.max = document.getElementById("searchMaxSizeFilter").value;
        searchSizeFilter.maxUnit = document.getElementById("searchMaxSizePrefix").value;

        searchFilterChanged();
    };

    const searchSizeFilterPrefixChanged = () => {
        if ((Number(document.getElementById("searchMinSizeFilter").value) !== 0) || (Number(document.getElementById("searchMaxSizeFilter").value) !== 0))
            searchSizeFilterChanged();
    };

    const searchFilterChanged = () => {
        searchResultsTable.updateTable();
        updateResultsInfo();
    };

    const loadSearchResultsData = (searchId) => {
        const state = searchState.get(searchId);
        // search tab might have been closed
        if (!state)
            return;

        const maxResults = 500;
        const url = new URL("api/v2/search/results", window.location);
        url.search = new URLSearchParams({
            id: searchId,
            limit: maxResults,
            offset: state.rowId
        });
        fetch(url, {
                method: "GET",
                cache: "no-store"
            })
            .then(async (response) => {
                const state = searchState.get(searchId);

                if (!response.ok) {
                    if ((response.status === 400) || (response.status === 404)) {
                        // bad params. search id is invalid
                        resetSearchState(searchId);
                        setSearchStatus(searchId, "error");
                    }
                    else if (response.status === 409) {
                        // offset out of range, the server no longer has the results
                        updateStatusIconElement(searchId, "QBT_TR(Search results are no longer available)QBT_TR[CONTEXT=SearchJobWidget]", "images/error.svg");
                        resetSearchState(searchId);
                    }
                    else if (state) {
                        showNotice("QBT_TR(Unable to get the search results.)QBT_TR[CONTEXT=SearchEngineWidget]", "poll");
                        clearTimeout(state.loadResultsTimer);
                        state.loadResultsTimer = loadSearchResultsData.delay(3000, this, searchId);
                    }
                    return;
                }

                clearNotice("poll");

                // check if user stopped the search prior to receiving the response
                if (!state || !state.running) {
                    if (state)
                        clearTimeout(state.loadResultsTimer);
                    setSearchStatus(searchId, "aborted");
                    return;
                }

                const responseJSON = await response.json();
                if (responseJSON) {
                    const newRows = [];

                    if (responseJSON.results) {
                        const results = responseJSON.results;
                        for (const result of results) {
                            const row = {
                                rowId: state.rowId,
                                descrLink: result.descrLink,
                                fileName: result.fileName,
                                fileSize: result.fileSize,
                                fileUrl: result.fileUrl,
                                nbLeechers: result.nbLeechers,
                                nbSeeders: result.nbSeeders,
                                engineName: result.engineName,
                                siteUrl: result.siteUrl,
                                pubDate: result.pubDate,
                            };

                            newRows.push(row);
                            state.rows.push(row);
                            state.rowId += 1;
                        }
                    }

                    // only update table if this search is currently being displayed
                    if (searchId === getSelectedSearchId()) {
                        for (const row of newRows)
                            searchResultsTable.updateRowData(row);

                        searchResultsTable.updateTable();
                        updateResultsInfo();
                    }
                    else if (newRows.length > 0) {
                        // update the count of a search in the background
                        updateTabTitle(searchId);
                    }

                    if ((responseJSON.status === "Stopped") && (state.rowId >= responseJSON.total)) {
                        resetSearchState(searchId);
                        setSearchStatus(searchId, (state.rows.length > 0) ? "finished" : "noResults");
                        return;
                    }

                    // if we got max results, more are likely waiting - fetch immediately
                    if (newRows.length >= maxResults) {
                        clearTimeout(state.loadResultsTimer);
                        state.loadResultsTimer = loadSearchResultsData.delay(0, this, searchId);
                        return;
                    }
                }

                clearTimeout(state.loadResultsTimer);
                state.loadResultsTimer = loadSearchResultsData.delay(2000, this, searchId);
            }, (error) => {
                console.error(error);

                showNotice("QBT_TR(qBittorrent client is not reachable)QBT_TR[CONTEXT=HttpServer]", "poll");
                clearTimeout(state.loadResultsTimer);
                state.loadResultsTimer = loadSearchResultsData.delay(3000, this, searchId);
            });
    };

    const updateSearchResultsData = (searchId) => {
        const state = searchState.get(searchId);
        clearTimeout(state.loadResultsTimer);
        state.loadResultsTimer = loadSearchResultsData.delay(500, this, searchId);
    };

    for (const element of document.getElementsByClassName("copySearchDataToClipboard")) {
        const setupClickEvent = (textFunc) => element.addEventListener("click", async (event) => await clipboardCopy(textFunc()));
        switch (element.id) {
            case "copySearchTorrentName":
                setupClickEvent(copySearchTorrentName);
                break;
            case "copySearchTorrentDownloadLink":
                setupClickEvent(copySearchTorrentDownloadLink);
                break;
            case "copySearchTorrentDescriptionUrl":
                setupClickEvent(copySearchTorrentDescriptionUrl);
                break;
        }
    }

    return exports();
})();
Object.freeze(window.qBittorrent.Search);
