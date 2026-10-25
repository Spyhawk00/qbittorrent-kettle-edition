# Kettle Edition 1.0.0

What is different from official qBittorrent 5.3.0, in three lists: new, changed and removed.
The qBittorrent version stays `v5.3.0`, and your settings, torrents and the WebUI API keep working,
so Sonarr, Radarr and other tools are not affected.

## New

### Several people, one qBittorrent

Official qBittorrent has one WebUI login that everyone shares. Kettle Edition has real accounts.

- **Accounts and roles.** Each person signs in with their own name and password. A role is a set of permissions:
  view, add, start/stop, edit, choose save locations, delete, delete files, download finished files, speed limits,
  settings, RSS (read and manage), search, search plugins, torrent creator, log, activity log, and managing users.
  Six roles come ready-made: Admin (everything), Manager (runs the downloads day to day; can delete torrents,
  change speed limits, read the log and manage RSS), User (add and manage torrents), Requester (search and add only),
  Downloader (read-only, plus downloading finished files from the PC) and Viewer (read-only). You can make your own.
  Existing installs keep the roles they already have; new roles are created in Options > User Accounts.
- **Safe by default.** Anything the access list doesn't know about needs its own permission. People without
  "Choose save locations" can't send files outside the default folder, not even by renaming or with an RSS rule.
  Only the admin sees settings that hold passwords and keys. The "settings" permission leaves out the sign-in,
  security, WebUI address and run-program options and the API key; only an admin can change those.
  A request that a proxy passed on (it carries a forwarded address) never counts as this PC, so it always signs in.
- **Activity log.** A record of who signed in, from which address, what they added, started, stopped, changed or
  deleted, and what was refused. It is in the WebUI and in the desktop program (Log window), can be filtered and
  exported, and clears itself after the number of days you choose. Addresses are logged by site only, so keys in
  tracker and feed links (passkeys, tokens) never reach the log or an export.
- **Sign-in protection.** Wrong passwords ban the address and lock the account for the address that kept failing,
  so a stranger can't lock the owner out. New passwords need at least 12 characters. Sessions can be listed and
  ended, and changing your password signs your other devices out. Torrents added by a person are tagged
  `added-by:<name>`, and only an admin can write or remove such a tag by hand.
- **Per-person WebUI layout.** Columns, filters and display choices are saved for each account.
- **User Accounts page in Options** (desktop and WebUI): Users, Roles and Logged In tabs, how long people stay
  signed in, the lock rules, how long the activity log is kept, and whether the old shared login still works.
- **Still works the old way.** Localhost access without a login (for Sonarr and Radarr), the API key and the old
  shared login keep working until you turn them off. The shared login is switched off once the first account exists.

### Self-updates

- Kettle Edition checks this repository's releases, downloads only the files that changed, and checks each file's
  hash and the release's digital signature before it keeps anything. Without the matching signature nothing is installed.
- It installs at 4:00 AM or at the next start (only while "Automatically install Kettle Edition updates" is on), and
  puts the old version back by itself if the new one doesn't start or its WebUI doesn't answer.
  A small helper program (`kettle-update-helper.exe`) does the swap.
- **Check for updates** in the Help menu tells you the edition version you have, or that it couldn't check.

### Statistics window, rebuilt

- Tabs: Overview, History and Technical, on the desktop and in the WebUI.
- **Usage history.** Downloaded and uploaded per day (kept 400 days) and per month (kept for good), with a period
  picker. It starts empty the first time the new version runs and fills as you transfer. "Before tracking began"
  shows the rest of the all-time total.
- Torrent counts (downloading, seeding, stopped, errored) that match the sidebar, data this session, and uptime.
- A Copy button copies the open tab as text.
- New WebUI API: `transfer/usageHistory`, extra keys in the sync data, and `access/addedTorrents`. They only add;
  nothing existing changed.

### Phone and app use of the WebUI

- A phone layout (narrower than 640 px): filter sidebar behind a button, filter row under the tabs, 40 px touch targets,
  one tap on a column header is one sort.
- The WebUI installs as an app on a phone or PC, and can open `.torrent` files and `magnet:` links.

### Smaller additions

- **Options > Advanced is organised.** The 80-plus rows are grouped in 4 tabs (General, Network, Storage, Peers and
  trackers) and 10 titled sections, on the desktop and in the WebUI. Options search covers them like any other page.
- **Search tab:** shows how many results are shown and hidden, "Reset auto width", "Refresh search", "Close search",
  "Close all searches".
- **Hide unused statuses** in the Transfers sidebar is on by default (turn it off in Options).
- **WebUI Options page** has the Windows-only "show console window for external program" and "process memory
  priority" settings, which the official WebUI lacked.
- **Windows themes:** Options lists Windows 11, Windows Vista and Windows Classic by name, with a hint that Windows 11
  works best with dark mode.
- **Warning when the WebUI address is set to this PC only**, because remote access would stop.
- About shows the edition version, SQLite version, and credits for this edition.

## Changed

- **Look.** Every dialog, table, list, scroll bar, tab and status bar on the desktop, and every page of the WebUI,
  follows the current Windows look in light and dark, with one system font. One outline colour, one 8 px grid,
  22 px rows and 24 px headers everywhere; number columns are right-aligned; the sidebar lines up with the lists.
  The progress bar is as before.
- **Add torrent window.** A magnet link opens it about 300 ms sooner (a fixed wait in `main.cpp` is gone), and it
  always comes to the front, with a log line if Windows tried to keep it behind.
- **Reworked screens:** Search tab, Add torrent, delete confirmation, RSS, filter panels, Speed limit, Torrent creator,
  Execution log, tracker list, speed graph and status bar. The desktop and the WebUI look alike.
- **Options, desktop:** sidebar of fixed width, the mouse wheel scrolls the page and no longer changes fields, thin
  boxes around groups, "Status bar and log", "Files" and "Checking" are their own groups.
- **Update check.** "Check for program updates" now means Kettle Edition releases. See Removed.
- **Splash screen** is shown at start (official 5.3.0 hides it by default), with a Kettle Edition image.
- **Names.** The program file is `qbittorrent-ke.exe`, so it can't be mixed up with the official one. Window titles read
  "qBittorrent v5.3.0 - Kettle Edition". The app and API version stays a plain `v5.3.0`.
- **Windows installer** for Kettle Edition, built from this repository.
- **Application icon and tray icons** are new.

## Removed

- **The official update check.** Official qBittorrent releases are no longer looked for, and the "A new version is
  available / Do you want to download" pop-up is gone, so the official page can't replace this edition.
- **The "Preferences" toolbar page and a toolbar divider image in the WebUI**, replaced by the new Options layout.
- **The shared WebUI login as the only way in.** It still works until the first account exists, then it is off unless
  you allow it again.
- **Upstream's project files that don't apply here** (GitHub issue templates, CI workflows, funding file).
- Nothing was removed from the WebUI API or from the qBittorrent settings and data files.

## Same as official 5.3.0

Everything else: the BitTorrent engine (libtorrent), the Lua plugins engine, RSS, search plugins, the WebUI API for
other programs, and the settings and torrent data on disk. A profile from official 5.3.0 opens as it is.

Kettle Edition is unofficial and not affiliated with the qBittorrent project. Licence: GPL, same as qBittorrent.
