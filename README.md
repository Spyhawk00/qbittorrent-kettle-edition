Kettle Edition
--------------

Kettle Edition is an unofficial build of [qBittorrent](https://www.qbittorrent.org) 5.3.0 for Windows, made for
sharing one qBittorrent between several people. It is not affiliated with or endorsed by the qBittorrent project.
A kettle is a group of hawks soaring together.

### What is different from qBittorrent 5.3.0

* **User accounts and roles in the WebUI.** Each person signs in with their own account. Roles decide what they may do
  (view, add, start/stop, edit, choose save locations, delete, delete files, change settings, and so on).
  Accounts, roles and sessions are managed from the WebUI and from the desktop program.
* **Activity log.** Who did what, from which address, and what was refused. Available in the WebUI and on the desktop.
* **Sign-in protection.** An address is banned after too many wrong passwords, and an account is locked for the
  address that keeps failing, so a stranger can't lock the owner out.
* **Redesigned desktop and WebUI**, with a WebUI that works as an installable app on a phone.
* **Silent signed updates.** The program checks this repository's releases, downloads what changed, verifies the
  signature and file hashes, installs at 4:00 AM or at the next start, and goes back to the previous version by itself
  if the new one doesn't run well.

The qBittorrent version stays `v5.3.0`, and the WebUI API is the same one, so tools such as Sonarr and Radarr keep
working. Kettle Edition has its own version number (see About).

### Installing

Download `qbittorrent-kettle-edition_<version>_x64_setup.exe` from the [releases](../../releases) page and run it.
The installer is not code-signed, so Windows SmartScreen may ask you to confirm. Kettle Edition replaces the official
qBittorrent: it takes over the Start Menu folder, the `.torrent` and magnet associations and the uninstall entry, and
removes them again when you uninstall it. Settings and torrents are kept. For automatic updates, the user who runs the
program must be allowed to change the program folder (a ticked option on the installer's components page); that also
means anything running as that user can replace the program.

Python is needed only for the search plugins.

### Going back to official qBittorrent

1. Close qBittorrent.
2. Uninstall "qBittorrent Kettle Edition" in Windows Settings, Apps (Programs and Features).
3. Install the current official qBittorrent from [qbittorrent.org](https://www.qbittorrent.org).

Your torrents, categories and settings are kept, and the official program reads them. What changes:

* **WebUI accounts stop working.** The extra accounts and roles are in `webui_access.db` in the data folder, which
  the official program ignores (it is not deleted). The WebUI goes back to the single administrator login from
  Options, Web UI. If no password was ever set there, the official program shows a temporary one in its log
  at each start.
* **Go back to 5.3.0 or newer, not to 5.2.x.** Kettle Edition is based on 5.3.0, which stores the WebUI settings under
  new names in `qBittorrent.ini`. An older official version can't read them and starts the WebUI with default settings.
* Settings that only Kettle Edition has are ignored by the official program.

### Building

See [INSTALL](INSTALL). The Kettle Edition code is in `src/base/access`, `src/webui/api/accesscontroller.*`,
`src/gui/useraccount*`, `src/gui/userrole*`, `src/gui/log/webuiactivitywidget.*` and `src/updatehelper`.

### Licence

GNU General Public License version 2 or later, with the additional permission to link with OpenSSL (see
[COPYING](COPYING)). Kettle Edition is based on qBittorrent, copyright the qBittorrent project. The free
[IP to Country Lite database](https://db-ip.com/db/download/ip-to-country-lite) by [DB-IP](https://db-ip.com/) is
licensed under the Creative Commons Attribution 4.0 International License.
