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

"use strict";

// Files and magnet links the installed WebUI app (see manifest.json) was opened with
window.qBittorrent ??= {};
window.qBittorrent.AppLaunch ??= (() => {
    const exports = () => {
        return {
            keepForSignIn: keepForSignIn,
            receive: receive
        };
    };

    const LINK_PARAM = "magnet";
    // the login page reloads after signing in, so files opened before that wait in IndexedDB
    const DB_NAME = "qbt-app-launch";
    const STORE_NAME = "files";
    const MAX_KEPT_AGE = 10 * 60 * 1000; // 10 minutes
    // the same launch may be handed over twice: by the address and the launch queue, or again after a reload
    const HANDLED_KEY = "qbt-app-launch-handled";
    const REPEAT_WINDOW = 5 * 1000;
    const RELOAD_WINDOW = 3 * 1000;

    const linkFrom = (url) => {
        const link = new URL(url, location.href).searchParams.get(LINK_PARAM)?.trim();
        return (link?.toLowerCase().startsWith("magnet:")) ? link : null;
    };

    const openDb = () => {
        return new Promise((resolve, reject) => {
            const request = indexedDB.open(DB_NAME, 1);
            request.onupgradeneeded = () => request.result.createObjectStore(STORE_NAME, { autoIncrement: true });
            request.onsuccess = () => resolve(request.result);
            request.onerror = () => reject(request.error);
        });
    };

    const keepFiles = async (files) => {
        const db = await openDb();
        try {
            await new Promise((resolve, reject) => {
                const transaction = db.transaction(STORE_NAME, "readwrite");
                const store = transaction.objectStore(STORE_NAME);
                for (const file of files)
                    store.add({ file: file, time: Date.now() });
                transaction.oncomplete = () => resolve();
                transaction.onerror = () => reject(transaction.error);
                transaction.onabort = () => reject(transaction.error);
            });
        }
        finally {
            db.close();
        }
    };

    const takeKeptFiles = async () => {
        // don't create the database on every page load
        if ((typeof window.indexedDB?.databases !== "function")
            || !(await indexedDB.databases()).some((info) => (info.name === DB_NAME)))
            return [];

        const db = await openDb();
        try {
            return await new Promise((resolve, reject) => {
                const transaction = db.transaction(STORE_NAME, "readwrite");
                const store = transaction.objectStore(STORE_NAME);
                const request = store.getAll();
                store.clear();
                transaction.oncomplete = () => {
                    const now = Date.now();
                    resolve(request.result.filter((entry) => ((now - entry.time) <= MAX_KEPT_AGE)).map((entry) => entry.file));
                };
                transaction.onerror = () => reject(transaction.error);
                transaction.onabort = () => reject(transaction.error);
            });
        }
        finally {
            db.close();
        }
    };

    const getFiles = async (launchParams) => {
        const handles = (launchParams.files ?? []).filter((handle) => (handle.kind === "file"));
        return await Promise.all(handles.map((handle) => handle.getFile()));
    };

    // Login page: keep what the app was opened with until the user has signed in
    const keepForSignIn = () => {
        window.launchQueue?.setConsumer(async (launchParams) => {
            try {
                // the sign-in reload keeps the address, also when an open window got the link without navigating
                if (launchParams.targetURL && linkFrom(launchParams.targetURL))
                    history.replaceState(history.state, "", launchParams.targetURL);

                const files = await getFiles(launchParams);
                if (files.length > 0)
                    await keepFiles(files);
            }
            catch (error) {
                console.error(`Failed to keep opened files. Reason: "${error}".`);
            }
        });
    };

    const loadHandled = () => {
        try {
            return JSON.parse(sessionStorage.getItem(HANDLED_KEY)) ?? {};
        }
        catch {
            return {};
        }
    };

    const saveHandled = (handled) => {
        try {
            const recent = Object.entries(handled).sort((a, b) => (b[1] - a[1])).slice(0, 50);
            sessionStorage.setItem(HANDLED_KEY, JSON.stringify(Object.fromEntries(recent)));
        }
        catch {
            // without session storage only the repeats in this page are skipped
        }
    };

    // Main page: hand files and links to the given callbacks, skipping repeats of the same launch
    const receive = ({ onFiles, onLink }) => {
        const handled = loadHandled();
        const isReload = (performance.getEntriesByType("navigation")[0]?.type === "reload");
        const isRepeat = (key) => {
            const time = handled[key];
            if (time === undefined)
                return false;
            if ((Date.now() - time) <= REPEAT_WINDOW)
                return true;
            // a reload may hand over the last launch again
            return isReload && (performance.now() <= RELOAD_WINDOW);
        };
        const takeNew = (key) => {
            const isNew = !isRepeat(key);
            handled[key] = Date.now();
            saveHandled(handled);
            return isNew;
        };

        const handleFiles = (files) => {
            const newFiles = files.filter((file) => takeNew(`file:${file.name}|${file.size}|${file.lastModified}`));
            if (newFiles.length > 0)
                onFiles(newFiles);
        };
        const handleLink = (link) => {
            if (link && takeNew(`link:${link}`))
                onLink(link);
        };

        // a link in the address is removed once read, as with "#download="
        const addressLink = linkFrom(location.href);
        if (new URLSearchParams(location.search).has(LINK_PARAM)) {
            const url = new URL(location.href);
            url.searchParams.delete(LINK_PARAM);
            history.replaceState(history.state, "", url);
        }
        handleLink(addressLink);

        // files kept by the login page go first, so a repeated launch of them is recognized
        const kept = takeKeptFiles()
            .then((files) => {
                if (files.length > 0)
                    handleFiles(files);
            })
            .catch((error) => {
                console.error(`Failed to read opened files. Reason: "${error}".`);
            });

        window.launchQueue?.setConsumer(async (launchParams) => {
            await kept;
            try {
                if (launchParams.targetURL)
                    handleLink(linkFrom(launchParams.targetURL));

                const files = await getFiles(launchParams);
                if (files.length > 0)
                    handleFiles(files);
            }
            catch (error) {
                console.error(`Failed to read opened files. Reason: "${error}".`);
            }
        });
    };

    return exports();
})();
Object.freeze(window.qBittorrent.AppLaunch);
