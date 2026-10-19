/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  Kettle Edition
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

/*
 * The phone layout (main window narrower than 640 px, see css/Phone.css):
 *  - popups fill the screen
 *  - the filter sidebar slides in from the left
 *  - the "More" menu holds the toolbar buttons that don't fit
 *  - the properties panel is a full-height sheet that opens when a torrent is tapped
 *  - the Back button closes the popup, sheet or sidebar on top, instead of leaving the WebUI
 */

"use strict";

window.qBittorrent ??= {};
window.qBittorrent.Phone ??= (() => {
    const exports = () => {
        return {
            setup: setup
        };
    };

    const isPhone = () => {
        return window.qBittorrent.Misc.isPhoneLayout();
    };

    // The Back button closes what is on top. Every popup, the sheet and the sidebar add a history entry when they open;
    // when one is closed some other way, the entry is taken away again.
    const layers = [];
    let ignoredPops = 0;

    const openLayer = (close) => {
        const layer = { close: close, dead: false };
        layers.push(layer);
        // the entry says how many layer entries there are under it, for a reload
        history.pushState({ qbtLayer: layers.length }, "");
        return layer;
    };

    // A reload closes every layer but keeps their history entries. This page knows from its own entry how many
    // there are, and goes back past them, so a Back press leaves the WebUI instead of doing nothing.
    const staleLayers = history.state?.qbtLayer;
    if (Number.isInteger(staleLayers) && (staleLayers > 0)) {
        ++ignoredPops;
        history.go(-staleLayers);
    }

    // A layer closed under another one (a popup that opens the next one before it closes) keeps its entry until
    // the ones above it are gone; then the entries are taken away together, so no Back press lands on one and closes nothing.
    const dropClosedLayers = () => {
        let count = 0;
        while ((layers.length > 0) && layers.at(-1).dead) {
            layers.pop();
            ++count;
        }
        if (count > 0) {
            ++ignoredPops;
            history.go(-count);
        }
    };

    const layerClosed = (layer) => {
        if (!layers.includes(layer)) // closed by the Back button
            return;
        layer.dead = true;
        dropClosedLayers();
    };

    window.addEventListener("popstate", (e) => {
        if (ignoredPops > 0) {
            --ignoredPops;
            return;
        }
        const layer = layers.pop();
        if (layer === undefined)
            return;
        layer.close();
        dropClosedLayers();
    });

    // Popups (MochaUI windows) fill the screen. Dragging and resizing are off, because there is nowhere to go.
    const PHONE_HEADER_HEIGHT = 40;

    const patchWindows = () => {
        // what a window has between its title bar and its content
        const toolbarsHeight = (options) => {
            const defaults = MochaUI.Window.prototype.options;
            return (options.toolbar ? (options.toolbarHeight ?? defaults.toolbarHeight) : 0)
                + (options.toolbar2 ? (options.toolbar2Height ?? defaults.toolbar2Height) : 0);
        };

        const initialize = MochaUI.Window.prototype.initialize;
        MochaUI.Window.prototype.initialize = function(options) {
            if (isPhone() && (options !== undefined)) {
                options = Object.assign({}, options, {
                    draggable: false,
                    headerHeight: PHONE_HEADER_HEIGHT,
                    height: window.innerHeight - PHONE_HEADER_HEIGHT - toolbarsHeight(options), // the size of the content: the title bar comes on top
                    resizable: false,
                    width: window.innerWidth,
                    x: 0,
                    y: 0
                });
            }
            const result = initialize.call(this, options);
            // a window that was already open is shown again, not built again
            if (isPhone() && this.windowEl) {
                const layer = openLayer(() => { MochaUI.closeWindow(this.windowEl); });
                this.addEvent("onClose", () => { layerClosed(layer); });
            }
            return result;
        };

        // the size a window asks for is the size of its content; on a phone it is always the whole screen
        const resizeWindow = MochaUI.resizeWindow;
        MochaUI.resizeWindow = (windowEl, options) => {
            if (isPhone()) {
                const instance = windowEl.retrieve("instance");
                options = {
                    centered: false,
                    height: window.innerHeight - instance.options.headerHeight - instance.options.footerHeight - toolbarsHeight(instance.options),
                    left: 0,
                    top: 0,
                    width: window.innerWidth
                };
            }
            return resizeWindow(windowEl, options);
        };

        window.addEventListener("resize", window.qBittorrent.Misc.createDebounceHandler(200, () => {
            if (!isPhone())
                return;
            for (const instance of Object.values(MochaUI.Windows.instances)) {
                const windowEl = instance.windowEl;
                if (windowEl && (windowEl.style.display !== "none") && !windowEl.classList.contains("windowClosed"))
                    MochaUI.resizeWindow(windowEl, {});
            }
        }));
    };

    // the filter sidebar, opened by the button at the left of the toolbar
    const setupSidebar = () => {
        const button = document.getElementById("sidebarToggleButton");

        let layer = null;
        const setOpen = (open) => {
            if (open === document.body.classList.contains("sidebarOpen"))
                return;
            document.body.classList.toggle("sidebarOpen", open);
            button.setAttribute("aria-expanded", open.toString());
            if (open) {
                layer = openLayer(() => { setOpen(false); });
            }
            else {
                layerClosed(layer);
                layer = null;
            }
        };

        button.addEventListener("click", (e) => {
            setOpen(!document.body.classList.contains("sidebarOpen"));
        });
        document.getElementById("sidebarScrim").addEventListener("click", (e) => { setOpen(false); });

        // choosing a filter shows the list again
        document.getElementById("Filters").addEventListener("click", (e) => {
            if (e.target.closest("ul.filterList li"))
                setOpen(false);
        });

        window.matchMedia("(max-width: 639px)").addEventListener("change", (e) => {
            if (!e.matches)
                setOpen(false);
        });

        return setOpen;
    };

    // the toolbar buttons that don't fit in one row, in the menu the other lists use
    const setupMoreMenu = () => {
        const menu = document.getElementById("moreMenu");

        // an item does what the menu item or button it stands for does
        const actions = {};
        for (const anchor of menu.querySelectorAll("a[data-source]")) {
            actions[anchor.getAttribute("href").slice(1)] = () => {
                document.getElementById(anchor.dataset.source).click();
            };
        }

        const moreMenu = new window.qBittorrent.ContextMenu.MoreContextMenu({
            actions: actions,
            menu: "moreMenu",
            targets: ""
        });

        const button = document.getElementById("moreButton");
        button.addEventListener("click", (e) => {
            const rect = button.getBoundingClientRect();
            // under the button, ending where the button ends
            moreMenu.triggerMenu({
                pageX: rect.right,
                pageY: rect.bottom,
                preventDefault: () => { e.preventDefault(); },
                stopPropagation: () => { e.stopPropagation(); }
            }, button);
        });
    };

    // the properties panel leaves the column when the window is a phone's, and fills the screen when a torrent is tapped
    const setupPropertiesSheet = () => {
        const localPreferences = new window.qBittorrent.LocalPreferences.LocalPreferences();
        const wrapper = document.getElementById("propertiesPanel_wrapper");
        const column = wrapper.parentElement;

        const bar = document.createElement("div");
        bar.id = "propertiesSheetBar";
        const title = document.createElement("span");
        title.id = "propertiesSheetTitle";
        const close = document.createElement("button");
        close.id = "propertiesSheetClose";
        close.type = "button";
        close.title = "QBT_TR(Close)QBT_TR[CONTEXT=HttpServer]";
        close.setAttribute("aria-label", close.title);
        bar.append(title, close);

        let layer = null;
        const setOpen = (open) => {
            if (open === document.body.classList.contains("propertiesOpen"))
                return;
            document.body.classList.toggle("propertiesOpen", open);
            if (open) {
                layer = openLayer(() => { setOpen(false); });
            }
            else {
                layerClosed(layer);
                layer = null;
            }
        };

        const place = () => {
            if (isPhone()) {
                wrapper.prepend(bar);
                document.body.append(wrapper);
            }
            else if (wrapper.parentElement !== column) {
                setOpen(false);
                bar.remove();
                column.append(wrapper);
                // back to the height it had on a wide window
                const relativeHeight = localPreferences.get("properties_height_rel");
                const propertiesHeight = ((relativeHeight === null) ? 0.5 : Number(relativeHeight)) * window.innerHeight;
                document.getElementById("propertiesPanel").style.height = `${propertiesHeight}px`;
                // the list gets the rest of the column
                const headerHeight = document.getElementById("propertiesPanel_header").offsetHeight;
                const handleHeight = document.getElementById("transferList_handle").offsetHeight;
                const columnHeight = document.getElementById("pageWrapper").offsetHeight;
                document.getElementById("transferList").style.height = `${Math.max(0, columnHeight - propertiesHeight - headerHeight - handleHeight)}px`;
            }
            MochaUI.Desktop.setDesktopSize();
        };
        place();
        window.matchMedia("(max-width: 639px)").addEventListener("change", place);

        close.addEventListener("click", (e) => { setOpen(false); });

        // a tap on a torrent opens it; a long press opens its menu instead
        const transferList = document.getElementById("transferList");
        const LONG_PRESS = 600; // as the menus' touchTimer
        let touchStartAt = 0;
        let wasLongPress = false;
        transferList.addEventListener("touchstart", (e) => {
            touchStartAt = performance.now();
            wasLongPress = false;
        }, { passive: true });
        transferList.addEventListener("touchend", (e) => {
            wasLongPress = ((performance.now() - touchStartAt) >= LONG_PRESS);
        }, { passive: true });

        transferList.addEventListener("click", (e) => {
            // some browsers click after a long press too
            if (wasLongPress) {
                wasLongPress = false;
                return;
            }
            if (!isPhone() || e.ctrlKey || e.metaKey || e.shiftKey)
                return;
            const tr = e.target.closest("#torrentsTableDiv tr");
            if (!tr)
                return;
            const row = torrentsTable.getRow(tr.rowId);
            title.textContent = row?.full_data.name ?? "";
            setOpen(true);
        });

        return setOpen;
    };

    const setup = () => {
        const closeSidebar = setupSidebar();
        setupMoreMenu();
        const closeSheet = setupPropertiesSheet();

        // Escape closes what is on top: a popup (handled elsewhere), the sheet, then the sidebar
        window.addEventListener("keydown", (e) => {
            if ((e.key !== "Escape") || !isPhone() || document.querySelector(".mocha"))
                return;
            if (document.body.classList.contains("propertiesOpen")) {
                e.stopImmediatePropagation();
                closeSheet(false);
            }
            else if (document.body.classList.contains("sidebarOpen")) {
                e.stopImmediatePropagation();
                closeSidebar(false);
            }
        }, true);
    };

    patchWindows();

    return exports();
})();
Object.freeze(window.qBittorrent.Phone);
