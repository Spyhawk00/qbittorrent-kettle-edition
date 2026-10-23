# Kettle Edition 1.0.0

From official qBittorrent 5.2.4 to Kettle Edition 1.0.0: what is different, in one page.
The qBittorrent version stays `v5.2.4`, and your settings, torrents and the WebUI API are unchanged, so Sonarr, Radarr and other tools keep working.

## Several people, one qBittorrent

Official qBittorrent has one WebUI login that everyone shares. Kettle Edition has real accounts.

- **Accounts and roles.** Each person signs in with their own name and password. A role is a set of permissions:
  view, add, start/stop, edit, choose save locations, delete, delete files, speed limits, settings, RSS, search,
  search plugins, torrent creator, log, activity log, and managing users. Three roles come ready-made:
  Admin (everything), User (add and manage torrents, but not delete, change settings or pick folders) and
  Viewer (read-only). You can make your own.
- **Safe by default.** Anything the access list doesn't know about needs its own permission. People without
  "Choose save locations" can't send files outside the default folder, not even by renaming or with an RSS rule.
  Only the admin sees settings that hold passwords and keys.
- **Activity log.** A record of who signed in, from which address, what they added, started, stopped, changed or
  deleted, and what was refused. It is in the WebUI and in the desktop program, can be filtered and exported,
  and clears itself after the number of days you choose. Addresses are logged by site only, so keys in tracker
  and feed links (passkeys, tokens) never reach the log or an export.
- **Sign-in protection.** Wrong passwords ban the address and lock the account for the address that kept failing,
  so a stranger can't lock the owner out. New passwords need at least 12 characters. Sessions can be listed and ended, and changing your password signs your
  other devices out. Torrents added by a person are tagged `added-by:<name>`, and only an admin can write such a tag by hand.
- **Per-person WebUI layout.** Columns, filters and display choices are saved for each account.
- **Still works the old way.** Localhost access without a login (for Sonarr and Radarr), the API key and the old
  shared login keep working until you turn them off. The shared login is switched off once the first account exists.

## A new look, on the desktop and in the browser

- Dialogs on the desktop and every page of the WebUI follow the current Windows look, in light and dark, with one
  system font and no decoration for its own sake.
- The WebUI has a phone layout and installs as an app on a phone, with handlers for `.torrent` files and magnet links.
- The Search tab, Add torrent, delete confirmation, RSS and filter panels were reworked on both sides, so the
  desktop and the WebUI look alike.
- A magnet link now opens its Add torrent window about 300 ms sooner and always comes to the front.

## Updates that install themselves

Official qBittorrent sends you to a download page. Kettle Edition updates itself.

- It checks this repository's releases, downloads only the files that changed, and checks each file's hash and the
  release's digital signature before it keeps anything. Without the matching signature nothing is installed.
- It installs at 4:00 AM or at the next start (only while "Automatically install Kettle Edition updates" is on), and puts the old
  version back by itself if the new one doesn't start, or its WebUI doesn't answer.
- Official qBittorrent releases are no longer checked for, so the official update pop-up can't replace this edition.

## Windows installer and naming

- A Windows installer for Kettle Edition. The program file is `qbittorrent-ke.exe`, so it can't be mixed up with the official one.
- Window titles read "qBittorrent v5.2.4 - Kettle Edition". About shows the edition version and credits.
- Kettle Edition is unofficial and not affiliated with the qBittorrent project. Licence: GPL, same as qBittorrent.
