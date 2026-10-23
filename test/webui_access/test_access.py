"""End-to-end tests for multi-user WebUI access control, against a real (headless) qBittorrent.

Each test class starts its own qbittorrent-nox with a throw-away profile, so the user's real
qBittorrent is never touched.

Run with:
    python -m unittest discover -s test/webui_access -v
Environment:
    QBT_NOX       path to qbittorrent-nox.exe (default: build-nox/qbittorrent-nox.exe in the repository folder)
    QBT_GATEWAY   folder of the stand-alone gateway, used to prepare user databases
                  (default: qbt-gateway in your home folder)
"""

import base64
import contextlib
import hashlib
import http.client
import json
import os
import re
import secrets
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlsplit

NOX = Path(os.environ.get("QBT_NOX", Path(__file__).resolve().parents[2] / "build-nox" / "qbittorrent-nox.exe"))
GATEWAY = Path(os.environ.get("QBT_GATEWAY", Path.home() / "qbt-gateway"))
sys.path.insert(0, str(GATEWAY))

from qbtgw import passwords  # noqa: E402
from qbtgw.store import Store  # noqa: E402

passwords.ITERATIONS = 1000  # cheap gateway-format hashes; qBittorrent upgrades them on first login
SHARED_USER, SHARED_PASSWORD = "shared", "shared-password"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def qbt_hash(password):
    salt = secrets.token_bytes(16)
    key = hashlib.pbkdf2_hmac("sha512", password.encode(), salt, 100_000, 64)
    return f"{base64.b64encode(salt).decode()}:{base64.b64encode(key).decode()}"


def bencode(value):
    if isinstance(value, int):
        return b"i%de" % value
    if isinstance(value, bytes):
        return b"%d:%s" % (len(value), value)
    if isinstance(value, str):
        return bencode(value.encode())
    if isinstance(value, list):
        return b"l" + b"".join(bencode(v) for v in value) + b"e"
    if isinstance(value, dict):
        return b"d" + b"".join(bencode(k) + bencode(value[k]) for k in sorted(value)) + b"e"
    raise TypeError(value)


def make_torrent(name):
    """A tiny single-file torrent and its v1 info-hash."""
    data = name.encode() * 10
    info = {"name": f"{name}.txt", "length": len(data), "piece length": 16384, "pieces": hashlib.sha1(data).digest()}
    return bencode({"info": info}), hashlib.sha1(bencode(info)).hexdigest()


class QbtInstance:
    def __init__(self, *, users=True, localhost_auth=True, max_auth_fail_count=0, extra_ini="", broken_db=False):
        self.root = Path(tempfile.mkdtemp(prefix="qbt-access-test-"))
        self.port = free_port()
        self.process = None
        base = self.root / "qBittorrent"
        (base / "config").mkdir(parents=True)
        (base / "data").mkdir(parents=True)
        self.downloads = self.root / "downloads"
        self.downloads.mkdir()
        self.db_path = base / "data" / "webui_access.db"
        (base / "data" / "web_clientdata.json").write_text(json.dumps({"qbt_date_format": "shared-format"}), encoding="utf-8")
        (base / "config" / "qBittorrent.ini").write_text(f"""[LegalNotice]
Accepted=true

[BitTorrent]
Session\\Port={free_port()}
Session\\DHTEnabled=false
Session\\LSDEnabled=false
Session\\PeXEnabled=false
Session\\DefaultSavePath={self.downloads.as_posix()}

[Network]
PortForwardingEnabled=false

[Preferences]
WebUI\\Enabled=true
WebUI\\Address=127.0.0.1
WebUI\\Port={self.port}
WebUI\\Username={SHARED_USER}
WebUI\\Password_PBKDF2="@ByteArray({qbt_hash(SHARED_PASSWORD)})"
WebUI\\LocalHostAuth={'true' if localhost_auth else 'false'}
WebUI\\CSRFProtection=true
WebUI\\HostHeaderValidation=true
WebUI\\ReverseProxySupportEnabled=true
WebUI\\TrustedReverseProxiesList=127.0.0.1
WebUI\\MaxAuthenticationFailCount={max_auth_fail_count}
General\\Locale=en
{extra_ini}
""", encoding="utf-8")

        if broken_db:
            self.db_path.mkdir()  # a folder where the database file should be: qBittorrent can't open it

        self.ids = {}
        if users:
            store = Store(str(self.db_path))
            roles = {r["name"]: r["id"] for r in store.list_roles()}
            self.roles = roles
            for name, role in (("morgan", "Admin"), ("alice", "User"), ("victor", "Viewer"), ("hashuser", "Viewer")):
                self.ids[name] = store.create_user(name, f"{name}-password", roles[role])["id"]
            store.close()

    def start(self):
        self.process = subprocess.Popen(
            [str(NOX), f"--profile={self.root}", "--confirm-legal-notice"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.wait_ready()

    def wait_ready(self):
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                status, _, _ = Client(self).request("GET", "/")
                if status in (200, 403):
                    return
            except OSError:
                time.sleep(0.3)
        raise RuntimeError("qbittorrent-nox did not start")

    def stop(self):
        if self.process and self.process.poll() is None:
            try:
                admin = Client(self)
                if admin.login("morgan", "morgan-password")[0] // 100 != 2:
                    admin.login(SHARED_USER, SHARED_PASSWORD)
                admin.request("POST", "/api/v2/app/shutdown", form={})
                self.process.wait(30)
            except (OSError, subprocess.TimeoutExpired, AssertionError):
                self.process.kill()
                self.process.wait(10)

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)

    @contextlib.contextmanager
    def db(self):
        # closed afterwards: an open database file can't be deleted on Windows, so cleanup() would leave it behind
        con = sqlite3.connect(self.db_path)
        con.row_factory = sqlite3.Row
        try:
            with con:
                yield con
        finally:
            con.close()

    def activity(self, **where):
        clauses = " AND ".join(f"{k} = ?" for k in where) or "1"
        with self.db() as con:
            return [dict(r) for r in con.execute(f"SELECT * FROM activity WHERE {clauses} ORDER BY id DESC",
                                                 list(where.values()))]


class Client:
    def __init__(self, qbt, ip=None):
        self.qbt = qbt
        self.cookie = None
        self.ip = ip

    def request(self, method, path, body=None, headers=None, form=None):
        headers = dict(headers or {})
        headers.setdefault("Host", f"127.0.0.1:{self.qbt.port}")
        if self.ip:
            headers.setdefault("CF-Connecting-IP", self.ip)
        if form is not None:
            body = urlencode(form).encode()
            headers.setdefault("Content-Type", "application/x-www-form-urlencoded")
        if self.cookie:
            headers.setdefault("Cookie", self.cookie)
        connection = http.client.HTTPConnection("127.0.0.1", self.qbt.port, timeout=30)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            data = response.read()
            for key, value in response.getheaders():
                if key.lower() == "set-cookie" and value.startswith("QBT_SID_"):
                    pair = value.split(";", 1)[0]
                    self.cookie = pair if pair.split("=", 1)[1] else None
            return response.status, {k.lower(): v for k, v in response.getheaders()}, data
        finally:
            connection.close()

    def login(self, username, password):
        return self.request("POST", "/api/v2/auth/login", form={"username": username, "password": password})

    def access(self, action, method="GET", **params):
        if method == "GET":
            status, headers, data = self.request("GET", f"/api/v2/access/{action}?{urlencode(params)}")
        else:
            status, headers, data = self.request("POST", f"/api/v2/access/{action}", form=params)
        try:
            return status, json.loads(data) if data else None
        except ValueError:
            return status, data.decode("utf-8", "replace")

    def add_torrent(self, name, **fields):
        torrent, info_hash = make_torrent(name)
        boundary = "----testboundary" + secrets.token_hex(8)
        parts = []
        for key, value in {"stopped": "true", **fields}.items():
            parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{key}"\r\n\r\n{value}\r\n'.encode())
        parts.append((f'--{boundary}\r\nContent-Disposition: form-data; name="torrents"; filename="{name}.torrent"\r\n'
                      "Content-Type: application/x-bittorrent\r\n\r\n").encode() + torrent + b"\r\n")
        parts.append(f"--{boundary}--\r\n".encode())
        status, _, body = self.request("POST", "/api/v2/torrents/add", body=b"".join(parts),
                                       headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
        return status, info_hash, body

    def client_data(self, *keys):
        status, _, body = self.request("POST", "/api/v2/clientdata/load", form={"keys": json.dumps([f"qbt_{key}" for key in keys])})
        assert status == 200, (status, body)
        return json.loads(body)

    def store_client_data(self, data):
        return self.request("POST", "/api/v2/clientdata/store", form={"data": json.dumps(data)})[0]

    def torrent(self, info_hash):
        deadline = time.time() + 10
        while time.time() < deadline:
            status, _, body = self.request("GET", f"/api/v2/torrents/info?hashes={info_hash}")
            if status == 200 and json.loads(body):
                return json.loads(body)[0]
            time.sleep(0.2)
        return None


class AccessTestCase(unittest.TestCase):
    instance_options = {}

    @classmethod
    def setUpClass(cls):
        if not NOX.exists():
            raise unittest.SkipTest(f"{NOX} not built")
        cls.qbt = QbtInstance(**cls.instance_options)
        # tearDownClass isn't called when setUpClass fails, so never leave an instance running
        cls.addClassCleanup(cls.qbt.cleanup)
        cls.addClassCleanup(cls.qbt.stop)
        cls.qbt.start()

    def client(self, user=None, password=None, ip=None):
        client = Client(self.qbt, ip)
        if user:
            status, _, body = client.login(user, password or f"{user}-password")
            self.assertEqual(status, 200 if body else 204, body)
            self.assertIsNotNone(client.cookie)
        return client


class LoginTests(AccessTestCase):
    # qBittorrent's own ban per visitor address is on, as it is by default
    instance_options = {"max_auth_fail_count": 5}

    def test_anonymous_sees_stock_login_page(self):
        status, _, body = Client(self.qbt).request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(b'id="loginform"', body)
        self.assertIn(b"css/login.css", body)
        self.assertEqual(Client(self.qbt).request("GET", "/api/v2/torrents/info")[0], 403)

    def test_personal_login_and_legacy_hash_upgrade(self):
        with self.qbt.db() as con:
            self.assertTrue(con.execute("SELECT password_hash FROM users WHERE username='hashuser'").fetchone()[0]
                            .startswith("pbkdf2_sha256$"))
        client = self.client("hashuser")
        self.assertEqual(client.request("GET", "/api/v2/torrents/info")[0], 200)
        with self.qbt.db() as con:
            new_hash = con.execute("SELECT password_hash FROM users WHERE username='hashuser'").fetchone()[0]
        self.assertFalse(new_hash.startswith("pbkdf2_sha256$"))
        self.assertEqual(new_hash.count(":"), 1)
        # the upgraded hash keeps working
        self.assertEqual(self.client("hashuser").request("GET", "/api/v2/app/version")[0], 200)
        self.assertEqual(self.qbt.activity(username="hashuser", category="login")[0]["summary"], "Logged in")

    def test_wrong_password_bans_the_visitor(self):
        attacker = Client(self.qbt, ip="198.51.100.7")
        for _ in range(5):
            self.assertEqual(attacker.login("victor", "wrong")[0], 401)
        status, _, body = attacker.login("victor", "victor-password")
        self.assertEqual(status, 403)
        self.assertIn(b"banned", body)
        # others, and the account itself, are not affected
        self.assertEqual(Client(self.qbt, ip="198.51.100.8").login("victor", "victor-password")[0] // 100, 2)
        entry = self.qbt.activity(outcome="failed")[0]
        self.assertEqual(entry["ip"], "198.51.100.7")

    def test_account_lock_follows_the_setting(self):
        admin = self.client("morgan")
        status, user = admin.access("createUser", "POST", username="lockme", password="lockme-password",
                                    role_id=self.qbt.roles["Viewer"])
        self.assertEqual(status, 200, user)
        self.assertEqual(admin.request("POST", "/api/v2/app/setPreferences",
                                       form={"json": json.dumps({"web_ui_user_max_auth_fail_count": 3})})[0], 200)
        try:
            # the lock is for the address that failed, so a stranger can't lock the owner out
            for i in range(3):
                self.assertEqual(Client(self.qbt, ip="198.51.100.20").login("lockme", "wrong")[0], 401)
            status, _, body = Client(self.qbt, ip="198.51.100.20").login("lockme", "lockme-password")
            self.assertEqual(status, 403)
            self.assertIn(b"This account is locked", body)
            self.assertEqual(Client(self.qbt, ip="198.51.100.30").login("lockme", "lockme-password")[0] // 100, 2)
            # other accounts can still log in from the locked address
            self.assertEqual(Client(self.qbt, ip="198.51.100.20").login("alice", "alice-password")[0] // 100, 2)
        finally:
            admin.request("POST", "/api/v2/app/setPreferences",
                          form={"json": json.dumps({"web_ui_user_max_auth_fail_count": 10})})

    def test_forged_forwarded_for_does_not_hide_the_visitor(self):
        # The visitor wrote "1.1.1.1"; the trusted proxy appended the real address. Stock 5.2.4 would pick 1.1.1.1.
        client = Client(self.qbt)
        client.request("POST", "/api/v2/auth/login", form={"username": "nobody", "password": "x"},
                       headers={"X-Forwarded-For": "1.1.1.1, 203.0.113.1"})
        self.assertEqual(self.qbt.activity(username="nobody")[0]["ip"], "203.0.113.1")

    def test_shared_login_is_off_once_accounts_exist(self):
        self.assertEqual(Client(self.qbt, ip="198.51.100.40").login(SHARED_USER, SHARED_PASSWORD)[0], 401)
        admin = self.client("morgan")
        admin.request("POST", "/api/v2/app/setPreferences", form={"json": json.dumps({"web_ui_shared_login_allowed": True})})
        try:
            shared = self.client(SHARED_USER, SHARED_PASSWORD)
            self.assertEqual(shared.access("me")[1]["kind"], "shared")
        finally:
            admin.request("POST", "/api/v2/app/setPreferences",
                          form={"json": json.dumps({"web_ui_shared_login_allowed": False})})

    def test_basic_auth_does_not_pile_up_sessions(self):
        def stored_sessions():
            with self.qbt.db() as con:
                return con.execute("SELECT COUNT(*) FROM sessions WHERE user_id = ?", (self.qbt.ids["victor"],)).fetchone()[0]

        before = stored_sessions()
        token = base64.b64encode(b"victor:victor-password").decode()
        client = Client(self.qbt, ip="198.51.100.80")
        for _ in range(3):
            status, headers, _ = client.request("GET", "/api/v2/torrents/info", headers={"Authorization": f"Basic {token}"})
            self.assertEqual(status, 200)
            self.assertIsNone(client.cookie)
        # the account's permissions still apply
        status, _, _ = client.request("POST", "/api/v2/torrents/stop", form={"hashes": "all"},
                                      headers={"Authorization": f"Basic {token}"})
        self.assertEqual(status, 403)
        self.assertEqual(stored_sessions(), before)
        self.assertEqual(len(self.qbt.activity(username="victor", summary="Logged in with HTTP Basic authentication")), 1)

    def test_logout_ends_session(self):
        client = self.client("alice")
        client.request("POST", "/api/v2/auth/logout")
        self.assertEqual(client.request("GET", "/api/v2/torrents/info")[0], 403)


class PermissionTests(AccessTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        admin = Client(cls.qbt)
        admin.login("morgan", "morgan-password")
        status, cls.hash_a, _ = admin.add_torrent("Ubuntu-Desktop")
        status, cls.hash_b, _ = admin.add_torrent("Debian-netinst")
        assert admin.torrent(cls.hash_a) and admin.torrent(cls.hash_b)

    def test_viewer_can_view_but_not_control(self):
        viewer = self.client("victor")
        self.assertEqual(viewer.request("GET", "/api/v2/sync/maindata?rid=0")[0], 200)
        status, headers, body = viewer.request("POST", "/api/v2/torrents/start", form={"hashes": self.hash_a})
        self.assertEqual(status, 403)
        self.assertEqual(headers.get("x-access-denied"), "control")
        self.assertIn(b"Start / stop torrents", body)
        entry = self.qbt.activity(username="victor", outcome="denied")[0]
        self.assertEqual(entry["summary"], "Started 'Ubuntu-Desktop.txt'")

    def test_user_actions_are_logged_with_names(self):
        alice = self.client("alice", ip="192.0.2.44")
        status, _, _ = alice.request("POST", "/api/v2/torrents/reannounce", form={"hashes": f"{self.hash_a}|{self.hash_b}"})
        self.assertEqual(status // 100, 2)
        entry = self.qbt.activity(username="alice", category="torrent")[0]
        self.assertEqual(entry["summary"], "Reannounced 'Ubuntu-Desktop.txt', 'Debian-netinst.txt'")
        self.assertEqual((entry["outcome"], entry["ip"], entry["role"]), ("ok", "192.0.2.44", "User"))

    def test_user_cannot_delete_and_delete_files_is_separate(self):
        alice = self.client("alice")
        self.assertEqual(alice.request("POST", "/api/v2/torrents/delete", form={"hashes": self.hash_b, "deleteFiles": "false"})[0], 403)
        with self.qbt.db() as con:
            con.execute("UPDATE roles SET permissions = ? WHERE name = 'User'", (json.dumps(["view", "delete"]),))
        try:
            status, headers, _ = alice.request("POST", "/api/v2/torrents/delete", form={"hashes": self.hash_b, "deleteFiles": "true"})
            self.assertEqual((status, headers.get("x-access-denied")), (403, "delete_files"))
            self.assertIsNotNone(alice.torrent(self.hash_b))
        finally:
            with self.qbt.db() as con:
                con.execute("UPDATE roles SET permissions = ? WHERE name = 'User'",
                            (json.dumps(["add", "control", "edit", "rss_read", "search", "view"]),))

    def test_downloading_a_torrent_file_has_its_own_permission(self):
        alice = self.client("alice")
        status, headers, _ = alice.request("GET", f"/api/v2/torrents/downloadFile?hash={self.hash_a}&file=0")
        self.assertEqual((status, headers.get("x-access-denied")), (403, "download_files"))
        self.assertEqual(self.qbt.activity(username="alice", outcome="denied")[0]["endpoint"], "torrents/downloadFile")
        # an existing role gets it only when an admin ticks it
        url = f"/api/v2/torrents/downloadFile?hash={self.hash_a}&file=0"
        with self.qbt.db() as con:
            admin_permissions = json.loads(con.execute("SELECT permissions FROM roles WHERE name = 'Admin'").fetchone()[0])
        self.assertEqual(self.client("morgan").request("GET", url)[0], 403)
        for role, original, ticked in (("Admin", admin_permissions, admin_permissions + ["download_files"]),
                                       ("Viewer", ["rss_read", "view"], ["view", "download_files"])):
            with self.qbt.db() as con:
                con.execute("UPDATE roles SET permissions = ? WHERE name = ?", (json.dumps(ticked), role))
            try:
                user = self.client("morgan" if role == "Admin" else "victor")
                self.assertNotEqual(user.request("GET", url)[0], 403, role)
            finally:
                with self.qbt.db() as con:
                    con.execute("UPDATE roles SET permissions = ? WHERE name = ?", (json.dumps(original), role))

    def test_a_signed_in_user_survives_the_limit_on_open_sessions(self):
        # qBittorrent 5.3 keeps at most 15 sessions in memory and drops the least recently used ones
        alice = self.client("alice")
        self.assertEqual(alice.request("GET", "/api/v2/sync/maindata?rid=0")[0], 200)
        for i in range(20):
            self.assertEqual(Client(self.qbt).login("victor", "victor-password")[0] // 100, 2)
        # their own session is restored from the user database, so they are not signed out
        self.assertEqual(alice.request("GET", "/api/v2/sync/maindata?rid=0")[0], 200)

    def test_endpoints_added_in_5_3_follow_the_role(self):
        viewer = self.client("victor")
        for endpoint, missing in (("transfer/pauseSession", "control"), ("transfer/resumeSession", "control"),
                                  ("transfer/setSpeedLimits", "speed_limits"), ("rss/cloneRule", "rss_manage"),
                                  ("app/getFreeSpaceAtPath", "save_location")):
            status, headers, _ = viewer.request("POST", f"/api/v2/{endpoint}", form={})
            self.assertEqual((status, headers.get("x-access-denied")), (403, missing), endpoint)
        # importing rules can bring folders with it, so it needs both
        status, headers, _ = self.client("alice").request("POST", "/api/v2/rss/importRules", form={})
        self.assertEqual((status, headers.get("x-access-denied")), (403, "rss_manage,save_location"))
        # reading is part of "view" and "rss_read"
        self.assertEqual(viewer.request("GET", "/api/v2/transfer/getSpeedLimits")[0], 200)
        self.assertEqual(viewer.request("GET", "/api/v2/rss/exportRules")[0], 200)
        # an admin gets past the check (no file was sent, so the request itself is refused)
        self.assertEqual(self.client("morgan").request("POST", "/api/v2/rss/importRules", form={})[0], 400)

    def test_percent_encoded_action_is_still_checked(self):
        alice = self.client("alice")
        self.assertEqual(alice.request("POST", "/api/v2/torrents/%64elete", form={"hashes": self.hash_a})[0], 403)

    def test_unknown_and_admin_only_actions(self):
        alice = self.client("alice")
        self.assertEqual(alice.request("POST", "/api/v2/torrents/futureAction", form={})[0], 403)
        self.assertEqual(alice.request("POST", "/api/v2/app/shutdown", form={})[0], 403)
        self.assertEqual(alice.request("POST", "/api/v2/search/installPlugin", form={"sources": "http://x/p.py"})[0], 403)
        self.assertEqual(alice.request("GET", "/api/v2/app/getDirectoryContent?dirPath=C:\\")[0], 403)
        self.assertEqual(alice.request("POST", "/api/v2/app/setPreferences", form={"json": "{}"})[0], 403)

    def test_preferences_hidden_for_non_admins(self):
        prefs = json.loads(self.client("alice").request("GET", "/api/v2/app/preferences")[2])
        self.assertEqual(prefs["web_ui_username"], "")
        self.assertIn("save_path", prefs)
        admin_prefs = json.loads(self.client("morgan").request("GET", "/api/v2/app/preferences")[2])
        self.assertEqual(admin_prefs["web_ui_username"], SHARED_USER)

    def test_user_add_is_tagged_and_save_path_removed(self):
        alice = self.client("alice")
        elsewhere = self.qbt.root / "elsewhere"
        status, info_hash, _ = alice.add_torrent("Alice-ISO", savepath=str(elsewhere), tags="linux")
        self.assertEqual(status // 100, 2)
        torrent = alice.torrent(info_hash)
        self.assertEqual(set(torrent["tags"].split(", ")), {"linux", "added-by:alice"})
        self.assertNotIn("elsewhere", torrent["save_path"])
        self.assertEqual(self.qbt.activity(username="alice", endpoint="torrents/add")[0]["summary"],
                         "Added torrent: 'Alice-ISO.torrent'")

    def test_admin_delete_with_files_is_logged(self):
        admin = self.client("morgan")
        status, info_hash, _ = admin.add_torrent("Throwaway")
        self.assertIsNotNone(admin.torrent(info_hash))
        self.assertEqual(admin.request("POST", "/api/v2/torrents/delete", form={"hashes": info_hash, "deleteFiles": "true"})[0] // 100, 2)
        self.assertEqual(self.qbt.activity(username="morgan", endpoint="torrents/delete")[0]["summary"],
                         "Deleted 'Throwaway.txt' and their files")

    def test_rename_cannot_move_files_out_of_the_torrent(self):
        alice = self.client("alice")
        status, info_hash, _ = alice.add_torrent("Rename-Me")
        self.assertIsNotNone(alice.torrent(info_hash))
        for new_path in ("../escaped.txt", "..\\..\\escaped.txt", "sub/../../escaped.txt", "C:/escaped.txt", "C:escaped.txt"):
            for action in ("renameFile", "renameFolder"):
                status, headers, _ = alice.request("POST", f"/api/v2/torrents/{action}",
                                                   form={"hash": info_hash, "oldPath": "Rename-Me.txt", "newPath": new_path})
                self.assertEqual((status, headers.get("x-access-denied")), (403, "save_location"), (action, new_path))
        # renaming inside the torrent still works
        status, _, body = alice.request("POST", "/api/v2/torrents/renameFile",
                                        form={"hash": info_hash, "oldPath": "Rename-Me.txt", "newPath": "sub/Renamed.txt"})
        self.assertEqual(status // 100, 2, body)

    def test_rss_rule_with_own_folder_needs_save_locations(self):
        with self.qbt.db() as con:
            con.execute("UPDATE roles SET permissions = ? WHERE name = 'User'", (json.dumps(["view", "rss_manage"]),))
        try:
            alice = self.client("alice")
            rule = {"enabled": False, "mustContain": "x", "torrentParams": {"save_path": str(self.qbt.root / "rss")}}
            status, headers, _ = alice.request("POST", "/api/v2/rss/setRule",
                                               form={"ruleName": "alice-rule", "ruleDef": json.dumps(rule)})
            self.assertEqual((status, headers.get("x-access-denied")), (403, "save_location"))
            rule["torrentParams"]["save_path"] = ""
            status, _, body = alice.request("POST", "/api/v2/rss/setRule",
                                            form={"ruleName": "alice-rule", "ruleDef": json.dumps(rule)})
            self.assertEqual(status // 100, 2, body)
            alice.request("POST", "/api/v2/rss/removeRule", form={"ruleName": "alice-rule"})
        finally:
            with self.qbt.db() as con:
                con.execute("UPDATE roles SET permissions = ? WHERE name = 'User'",
                            (json.dumps(["add", "control", "edit", "rss_read", "search", "view"]),))

    def test_stored_cookies_are_not_logged(self):
        admin = self.client("morgan")
        cookies = [{"name": "uid", "value": "secret-cookie-value", "domain": "tracker.example", "path": "/"}]
        status, _, body = admin.request("POST", "/api/v2/app/setCookies", form={"cookies": json.dumps(cookies)})
        self.assertEqual(status // 100, 2, body)
        entry = self.qbt.activity(username="morgan", endpoint="app/setCookies")[0]
        self.assertNotIn("secret-cookie-value", entry["details"])
        admin.request("POST", "/api/v2/app/setCookies", form={"cookies": "[]"})

    def test_search_download_is_tagged(self):
        alice = self.client("alice")
        info_hash = secrets.token_hex(20)
        status, _, body = alice.request("POST", "/api/v2/search/downloadTorrent",
                                        form={"torrentUrl": f"magnet:?xt=urn:btih:{info_hash}&dn=Search-Find", "pluginName": "test"})
        self.assertEqual(status // 100, 2, body)
        torrent = alice.torrent(info_hash)
        self.assertIsNotNone(torrent)
        self.assertEqual(torrent["tags"], "added-by:alice")

    def test_cross_site_request_is_blocked(self):
        admin = self.client("morgan")
        status, _, _ = admin.request("POST", "/api/v2/torrents/reannounce", form={"hashes": self.hash_a},
                                     headers={"Origin": "https://evil.example"})
        self.assertEqual(status, 401)


class AdminApiTests(AccessTestCase):
    def test_non_admin_cannot_manage(self):
        alice = self.client("alice")
        self.assertEqual(alice.access("users")[0], 403)
        self.assertEqual(alice.access("activity")[0], 403)
        status, me = alice.access("me")
        self.assertEqual((status, me["role"], me["kind"]), (200, "User", "user"))

    def test_users_roles_settings(self):
        admin = self.client("morgan")
        status, role = admin.access("createRole", "POST", name="Family", permissions="view,add,bogus")
        self.assertEqual(status, 200, role)
        self.assertEqual(role["permissions"], ["add", "view"])
        status, user = admin.access("createUser", "POST", username="bob", password="bob-password", role_id=role["id"])
        self.assertEqual((status, user["role_name"]), (200, "Family"))
        self.assertEqual(admin.access("createUser", "POST", username="x", password="short", role_id=role["id"])[0], 400)
        # the settings are ordinary preferences, kept in range
        new_values = {"web_ui_user_sign_in_days": 7, "web_ui_activity_log_days": 99999}
        self.assertEqual(admin.request("POST", "/api/v2/app/setPreferences", form={"json": json.dumps(new_values)})[0], 200)
        prefs = json.loads(admin.request("GET", "/api/v2/app/preferences")[2])
        self.assertEqual((prefs["web_ui_user_sign_in_days"], prefs["web_ui_activity_log_days"]), (7, 3650))
        self.assertNotEqual(admin.access("setSettings", "POST", json="{}")[0], 200)
        self.assertEqual(self.client("bob").request("GET", "/api/v2/torrents/info")[0], 200)
        summaries = [e["summary"] for e in self.qbt.activity(category="admin")]
        self.assertIn("Created role 'Family'", summaries)
        self.assertIn("Created user 'bob' with role 'Family'", summaries)

    def test_cannot_remove_last_admin(self):
        admin = self.client("morgan")
        morgan = self.qbt.ids["morgan"]
        status, body = admin.access("updateUser", "POST", id=morgan, role_id=self.qbt.roles["User"])
        self.assertEqual(status, 400)
        self.assertIn("no active user", body)
        self.assertEqual(admin.access("updateRole", "POST", id=self.qbt.roles["Admin"], permissions="view")[0], 400)
        self.assertEqual(admin.access("deleteUser", "POST", id=morgan)[0], 400)

    def test_revoked_and_disabled_sessions_end_at_once(self):
        admin = self.client("morgan")
        victor = self.client("victor")
        status, data = admin.access("sessions")
        session = next(s for s in data["sessions"] if s["username"] == "victor")
        self.assertEqual(admin.access("revokeSession", "POST", id=session["id"])[0], 200)
        self.assertEqual(victor.request("GET", "/api/v2/torrents/info")[0], 403)
        victor = self.client("victor")
        self.assertEqual(admin.access("updateUser", "POST", id=self.qbt.ids["victor"], disabled="true")[0], 200)
        self.assertEqual(victor.request("GET", "/api/v2/torrents/info")[0], 403)
        self.assertEqual(admin.access("updateUser", "POST", id=self.qbt.ids["victor"], disabled="false")[0], 200)

    def test_change_own_password(self):
        phone, laptop = self.client("alice"), self.client("alice")
        self.assertEqual(phone.access("changePassword", "POST", current_password="nope", new_password="new-password-1")[0], 400)
        self.assertEqual(phone.access("changePassword", "POST", current_password="alice-password", new_password="new-password-1")[0], 200)
        self.assertEqual(phone.request("GET", "/api/v2/torrents/info")[0], 200)
        self.assertEqual(laptop.request("GET", "/api/v2/torrents/info")[0], 403)
        phone.access("changePassword", "POST", current_password="new-password-1", new_password="alice-password")

    def test_wrong_current_password_locks_the_account(self):
        admin = self.client("morgan")
        status, user = admin.access("createUser", "POST", username="guessme", password="guessme-password",
                                    role_id=self.qbt.roles["Viewer"])
        self.assertEqual(status, 200, user)
        admin.request("POST", "/api/v2/app/setPreferences", form={"json": json.dumps({"web_ui_user_max_auth_fail_count": 3})})
        try:
            stolen = self.client("guessme")
            for i in range(3):
                status, body = stolen.access("changePassword", "POST", current_password=f"guess-{i}", new_password="stolen-password")
                self.assertEqual(status, 400)
                self.assertIn("current password is wrong", body)
            status, body = stolen.access("changePassword", "POST", current_password="guessme-password",
                                         new_password="stolen-password")
            self.assertEqual(status, 400)
            self.assertIn("locked", body)
            # signing in elsewhere is not blocked by wrong passwords typed in a signed-in session
            self.assertEqual(Client(self.qbt, ip="198.51.100.70").login("guessme", "guessme-password")[0] // 100, 2)
            self.assertEqual(self.qbt.activity(username="guessme", endpoint="access/changePassword")[0]["outcome"], "failed")
        finally:
            admin.request("POST", "/api/v2/app/setPreferences",
                          form={"json": json.dumps({"web_ui_user_max_auth_fail_count": 10})})

    def test_display_preferences_are_per_person(self):
        admin = self.client("morgan")
        status, user = admin.access("createUser", "POST", username="prefs", password="prefs-password",
                                    role_id=self.qbt.roles["Viewer"])
        self.assertEqual(status, 200, user)
        phone, laptop, victor = self.client("prefs"), self.client("prefs"), self.client("victor")
        # everyone starts from the shared copy
        self.assertEqual(phone.client_data("date_format"), {"qbt_date_format": "shared-format"})
        self.assertEqual(phone.store_client_data({"qbt_color_scheme": "dark"}) // 100, 2)
        self.assertEqual(laptop.client_data("color_scheme", "date_format"),
                         {"qbt_color_scheme": "dark", "qbt_date_format": "shared-format"})
        self.assertEqual(laptop.store_client_data({"qbt_display_density": "compact"}) // 100, 2)
        # neither browser overwrites the other
        self.assertEqual(phone.client_data("color_scheme", "display_density"),
                         {"qbt_color_scheme": "dark", "qbt_display_density": "compact"})
        self.assertEqual(victor.client_data("color_scheme", "display_density"), {})
        with self.qbt.db() as con:
            self.assertEqual(con.execute("SELECT COUNT(*) FROM client_data WHERE user_id = ?", (user["id"],)).fetchone()[0], 1)
        self.assertEqual(admin.access("deleteUser", "POST", id=user["id"])[0], 200)
        with self.qbt.db() as con:
            self.assertEqual(con.execute("SELECT COUNT(*) FROM client_data WHERE user_id = ?", (user["id"],)).fetchone()[0], 0)

    def test_activity_query_and_csv(self):
        self.client("victor").request("POST", "/api/v2/torrents/stop", form={"hashes": "0" * 40})
        admin = self.client("morgan")
        status, data = admin.access("activity", outcome="denied")
        self.assertEqual(status, 200)
        self.assertTrue(data["items"])
        self.assertTrue(all(i["outcome"] == "denied" for i in data["items"]))
        status, headers, body = admin.request("GET", "/api/v2/access/exportActivity")
        self.assertEqual(status, 200)
        self.assertIn("attachment", headers["content-disposition"])
        self.assertTrue(body.decode("utf-8-sig").startswith("Time,User,Role"))

    def test_user_accounts_are_in_options(self):
        alice = self.client("alice")
        status, _, body = alice.request("GET", "/views/preferences.html")
        self.assertEqual(status, 200)
        self.assertIn(b'id="UserAccountsTab"', body)
        self.assertIn(b"window.qBittorrent.UserAccounts", body)
        self.assertIn(b'id="PrefUserAccountsLink"', body)
        # the separate window, its Tools menu entry and the old Options toolbar are gone
        for page in ("/views/useraccounts.html", "/views/useraccountsToolbar.html", "/access.html",
                     "/views/preferencesToolbar.html"):
            self.assertEqual(alice.request("GET", page)[0], 404, page)
        self.assertNotIn(b"userAccountsLink", alice.request("GET", "/")[2])


class PreferencesTests(AccessTestCase):
    def preferences(self, client):
        status, _, body = client.request("GET", "/api/v2/app/preferences")
        self.assertEqual(status, 200, body)
        return json.loads(body)

    def set_preferences(self, client, values):
        status, _, body = client.request("POST", "/api/v2/app/setPreferences", form={"json": json.dumps(values)})
        self.assertEqual(status, 200, body)

    def test_settings_that_used_to_be_desktop_only(self):
        admin = self.client("morgan")
        new_values = {"start_paused": True, "shutdown_timeout": 42}
        if sys.platform == "win32":
            new_values |= {"autorun_console": True, "memory_priority": "Low"}
        self.set_preferences(admin, new_values)
        prefs = self.preferences(admin)
        for key, value in new_values.items():
            self.assertEqual(prefs[key], value, key)
        # -1 means unlimited; anything lower is ignored
        self.set_preferences(admin, {"shutdown_timeout": -5})
        self.assertEqual(self.preferences(admin)["shutdown_timeout"], 42)
        # and the Options page has a row for each of them
        page = admin.request("GET", "/views/preferences.html")[2]
        for element_id in ("startPaused", "sessionShutdownTimeout", "autorunConsoleCheckbox", "memoryPriority",
                           "deleteCancelledTorrentFileCheckbox"):
            self.assertIn(f'id="{element_id}"'.encode(), page, element_id)

    def test_delete_torrent_files_keeps_all_three_modes(self):
        admin = self.client("morgan")
        for mode in (2, 1, 0):  # always (also when addition is cancelled), if added, never
            self.set_preferences(admin, {"auto_delete_mode": mode})
            self.assertEqual(self.preferences(admin)["auto_delete_mode"], mode)


SOURCE = Path(__file__).resolve().parents[2] / "src"


def webui_text(text):
    """The English text of a WebUI page, without the QBT_TR(...)QBT_TR[CONTEXT=...] wrappers."""
    return re.sub(r"QBT_TR\((.*?)\)QBT_TR\[CONTEXT=\w+\]", r"\1", text, flags=re.DOTALL)


def plain(text):
    return " ".join(text.replace("&", "").split())


class OptionsPage(HTMLParser):
    """Reads the WebUI Options page: the sidebar pages in order, each page's section titles ("Section" and
    "Section > Sub-section"), the Advanced row labels, and every element id."""

    VOID = {"input", "img", "br", "hr", "meta", "link"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.links = []       # (page name, page element id)
        self.sections = {}    # page element id -> [title]
        self.advanced_rows = []
        self.ids = set()
        self._stack = []      # open elements: (tag, attributes)
        self._text = None     # (kind, parts, extra) while reading an element's text

    def _page(self):
        pages = [attrs["id"] for (tag, attrs) in self._stack if "PrefTab" in attrs.get("class", "").split()]
        return pages[-1] if pages else None

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            self.ids.add(attrs["id"])
        if tag in self.VOID:
            return
        self._stack.append((tag, attrs))
        if self._text:
            return
        if (tag == "li") and ("data-page" in attrs):
            self._text = ("link", [], attrs["data-page"])
        elif (tag == "legend") and self._page():
            self._text = ("legend", [], sum(1 for (t, _) in self._stack if t == "fieldset"))
        elif (tag == "label") and (self._page() == "AdvancedTab"):
            self._text = ("row", [], None)

    def handle_endtag(self, tag):
        if tag not in [t for (t, _) in self._stack]:
            return
        while self._stack.pop()[0] != tag:
            pass
        if not self._text or (tag != {"link": "li", "legend": "legend", "row": "label"}[self._text[0]]):
            return
        kind, parts, extra = self._text
        self._text = None
        text = " ".join("".join(parts).split())
        if kind == "link":
            self.links.append((text, extra))
        elif kind == "legend":
            titles = self.sections.setdefault(self._page(), [])
            titles.append(text if (extra == 1) else f"{titles[-1].split(' > ')[0]} > {text}")
        else:
            self.advanced_rows.append(text)

    def handle_data(self, data):
        if self._text:
            self._text[1].append(data)


class OptionsParityTests(unittest.TestCase):
    """The desktop Options window and the WebUI Options page show the same pages and sections in the same order.

    Reads the source files, so it needs no running qBittorrent."""

    # sections that only make sense on one side ("Section" or "Section > Sub-section")
    DESKTOP_ONLY = {
        "Behavior": {"Interface > Use custom UI Theme", "Torrent Content View", "Desktop",
                     "Desktop > Show qBittorrent in notification area", "Desktop > File association",
                     "Power Management", "Search UI > Store opened tabs"},
        "Downloads": {"When adding a torrent > Display torrent content and some options"},
    }
    WEBUI_ONLY = {
        "Behavior": {"Custom WebUI settings"},
    }
    # Advanced rows that only change the desktop app
    DESKTOP_ONLY_ADVANCED_ROWS = {
        "PROGRAM_NOTIFICATIONS", "TORRENT_ADDED_NOTIFICATIONS", "NOTIFICATION_TIMEOUT",
        "DOWNLOAD_TRACKER_FAVICON", "SAVE_PATH_HISTORY_LENGTH",
        "ENABLE_SPEED_WIDGET", "ENABLE_ICONS_IN_MENUS", "USE_ATTACHED_ADD_NEW_TORRENT_DIALOG"}

    @classmethod
    def setUpClass(cls):
        cls.html = (SOURCE / "webui/www/private/views/preferences.html").read_text(encoding="utf-8")
        cls.webui = OptionsPage()
        cls.webui.feed(webui_text(cls.html))
        cls.ui = ET.parse(SOURCE / "gui/optionsdialog.ui").getroot()

    def desktop_pages(self):
        """(page name, [section titles]) of optionsdialog.ui, in sidebar order."""
        def prop(widget, name):
            for p in widget.findall("property"):
                if (p.get("name") == name) and len(p):
                    return p[0].text
            return None

        def widget(name):
            return next(w for w in self.ui.iter("widget") if w.get("name") == name)

        def walk(element, section, titles):
            for child in element:
                if (child.tag == "widget") and (child.get("class") == "QGroupBox"):
                    title = plain(prop(child, "title") or "")
                    # a box marked as a heading reads as a section of its own, wherever it sits
                    if (section is None) or (prop(child, "heading") == "true"):
                        titles.append(title)
                        walk(child, title, titles)
                    else:
                        titles.append(f"{section} > {title}")
                        walk(child, section, titles)
                else:
                    walk(child, section, titles)
            return titles

        names = [plain(prop(item, "text")) for item in widget("tabSelection").findall("item")]
        pages = widget("tabOption").findall("widget")
        self.assertEqual(len(names), len(pages))
        return [(name, walk(page, None, [])) for (name, page) in zip(names, pages)]

    def webui_pages(self):
        return [(name, self.webui.sections.get(page, [])) for (name, page) in self.webui.links]

    def test_same_pages_in_the_same_order(self):
        self.assertEqual([name for (name, _) in self.desktop_pages()], [name for (name, _) in self.webui_pages()])

    def test_same_sections_in_the_same_order(self):
        webui = dict(self.webui_pages())
        for name, desktop in self.desktop_pages():
            if name == "Advanced":
                continue  # a list of rows on both sides, compared below
            with self.subTest(page=name):
                self.assertEqual([t for t in desktop if t not in self.DESKTOP_ONLY.get(name, set())],
                                 [t for t in webui[name] if t not in self.WEBUI_ONLY.get(name, set())])

    def test_one_side_only_sections_still_exist(self):
        # a stale entry in the lists above could hide a real difference later
        desktop = dict(self.desktop_pages())
        webui = dict(self.webui_pages())
        for name, titles in self.DESKTOP_ONLY.items():
            self.assertLessEqual(titles, set(desktop[name]), name)
        for name, titles in self.WEBUI_ONLY.items():
            self.assertLessEqual(titles, set(webui[name]), name)

    def test_same_advanced_rows_in_the_same_order(self):
        source = (SOURCE / "gui/advancedsettings.cpp").read_text(encoding="utf-8")
        enum = source[source.index("enum AdvSettingsRows"):source.index("ROW_COUNT")]
        rows = [line.strip().rstrip(",") for line in enum.splitlines()[2:]]
        rows = [row for row in rows if row and not row.startswith(("#", "//")) and not row.endswith("_HEADER")]
        labels = dict(re.findall(r'addRow\((\w+),\s*\(?tr\("((?:[^"\\]|\\.)*)"', source))
        labels["ENABLE_MARK_OF_THE_WEB"] = re.search(r'motwLabel = tr\("(Enable Mark-of-the-Web[^"]*)"', source)[1]

        def normal(text):
            text = text.replace("%1", "μTP").replace("µ", "μ").replace("(?)", "")
            text = text.replace(" (require macOS or Windows)", "")  # the WebUI doesn't know the server's system
            return " ".join(text.split()).rstrip(":").strip().lower()

        desktop = [normal(labels[row]) for row in rows if row not in self.DESKTOP_ONLY_ADVANCED_ROWS]
        self.assertEqual(desktop, [normal(row) for row in self.webui.advanced_rows])

    def test_both_have_the_same_search_box(self):
        texts = {w.get("name"): [p[0].text for p in w.findall("property") if (p.get("name") in ("placeholderText", "text")) and len(p)]
                 for w in self.ui.iter("widget") if w.get("name") in ("searchEdit", "labelNoSearchMatch")}
        self.assertEqual(texts, {"searchEdit": ["Search settings"], "labelNoSearchMatch": ["No settings match"]})
        page = webui_text(self.html)
        self.assertRegex(page, r'<input type="search" id="preferencesSearch"[^>]* placeholder="Search settings"')
        self.assertIn('<div id="preferencesNoSearchMatch" class="invisible">No settings match</div>', page)

    def test_user_and_role_windows_match(self):
        """Add/Edit User and Add/Edit Role show the same fields in the same order, then OK and Cancel."""
        for (ui_name, html_name) in (("useraccountdialog.ui", "useraccount.html"), ("userroledialog.ui", "userrole.html")):
            with self.subTest(ui_name):
                ui = ET.parse(SOURCE / "gui" / ui_name).getroot()
                desktop = [plain(w.find("property[@name='text']/string").text) for w in ui.iter("widget")
                           if (w.get("class") in ("QLabel", "QCheckBox")) and (w.find("property[@name='text']/string") is not None)]
                buttons = next(w for w in ui.iter("widget") if w.get("class") == "QDialogButtonBox").find("property[@name='standardButtons']/set").text
                self.assertEqual(set(re.findall(r"StandardButton::(\w+)", buttons)), {"Ok", "Cancel"})

                page = webui_text((SOURCE / "webui/www/private" / html_name).read_text(encoding="utf-8"))
                body = page[page.index("<body"):]
                webui = [plain(text) for text in re.findall(r'<label for="[^"]+">([^<]+)</label>', body)]
                self.assertEqual(desktop, webui)
                self.assertRegex(body, r'<div class="dialogButtons">\s*<button type="button" id="\w+">OK</button>\s*<button type="button" id="\w+">Cancel</button>\s*</div>')

    def test_every_element_the_script_uses_exists(self):
        used = set(re.findall(r'getElementById\("([^"]+)"\)', self.html))
        used |= set(re.findall(r'querySelector(?:All)?\("#([\w-]+)', self.html))
        used -= {"preferencesPage"}  # the window around the page
        self.assertEqual(sorted(used - self.webui.ids), [])


class WebUIStyleTests(unittest.TestCase):
    """The WebUI's colors come only from the palette at the top of style.css, and the palette stays readable.

    Reads the source files, so it needs no running qBittorrent."""

    WWW = SOURCE / "webui/www"
    CSS_FILES = sorted((WWW / "private/css").glob("*.css")) + sorted((WWW / "public/css").glob("*.css"))
    # the "JavaScript is off" pages load no palette
    NO_PALETTE = {"noscript.css"}

    @staticmethod
    def tokens(css):
        """{"light": {...}, "dark": {...}} of the --color-* declarations, by the block they sit in."""
        found = {"light": {}, "dark": {}}
        stack = []
        for line in re.sub(r"/\*.*?\*/", "", css, flags=re.DOTALL).splitlines():
            line = line.strip()
            if line.endswith("{"):
                stack.append(line)
                continue
            if line == "}":
                stack.pop()
                continue
            match = re.match(r"(--color-[\w-]+):\s*(.+?);?$", line)
            if match and stack:
                scope = "dark" if any((".dark" in b) and ("not(.dark)" not in b) for b in stack) else "light"
                found[scope][match.group(1)] = match.group(2)
        return found

    @staticmethod
    def rgb(value):
        match = re.fullmatch(r"hsl\((\d+(?:\.\d+)?)deg (\d+(?:\.\d+)?)% (\d+(?:\.\d+)?)%\)", value)
        if not match:
            return None  # not a plain color (a filter, a shadow with transparency, ...)
        h, s, l = float(match.group(1)), float(match.group(2)) / 100, float(match.group(3)) / 100
        c = (1 - abs(2 * l - 1)) * s
        x = c * (1 - abs((h / 60) % 2 - 1))
        m = l - c / 2
        r, g, b = [(c, x, 0), (x, c, 0), (0, c, x), (0, x, c), (x, 0, c), (c, 0, x)][int(h // 60) % 6]
        return r + m, g + m, b + m

    @classmethod
    def luminance(cls, value):
        def linear(v):
            return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
        r, g, b = (linear(v) for v in cls.rgb(value))
        return 0.2126 * r + 0.7152 * g + 0.0722 * b

    @classmethod
    def contrast(cls, a, b):
        la, lb = sorted((cls.luminance(a), cls.luminance(b)), reverse=True)
        return (la + 0.05) / (lb + 0.05)

    def palette(self, scheme):
        found = self.tokens((self.WWW / "private/css/style.css").read_text(encoding="utf-8"))
        return {**found["light"], **found[scheme]} if scheme == "dark" else found["light"]

    def test_text_is_readable_in_light_and_dark(self):
        text = ("--color-text-default", "--color-text-blue", "--color-text-orange", "--color-text-red",
                "--color-text-green", "--color-tracker-disabled", "--color-tracker-not-contacted",
                "--color-tracker-working", "--color-tracker-updating", "--color-tracker-not-working")
        surfaces = ("--color-background-primary", "--color-background-popup", "--color-background-default",
                    "--color-background-control", "--color-background-alt-row")
        for scheme in ("light", "dark"):
            palette = self.palette(scheme)
            for name in text:
                for surface in surfaces:
                    ratio = self.contrast(palette[name], palette[surface])
                    self.assertGreaterEqual(ratio, 4.5, f"{scheme}: {name} on {surface} is {ratio:.2f}:1")
            # the default text also sits on the hover and selection shades, and white text on the progress bar's fill
            for shade in ("--color-background-hover", "--color-background-selected"):
                ratio = self.contrast(palette["--color-text-default"], palette[shade])
                self.assertGreaterEqual(ratio, 4.5, f"{scheme}: text on {shade} is {ratio:.2f}:1")
            ratio = self.contrast(palette["--color-text-white"], palette["--color-background-blue"])
            self.assertGreaterEqual(ratio, 4.5, f"{scheme}: white on the progress fill is {ratio:.2f}:1")
            # borders and the focus ring need 3:1 against the surfaces they sit on
            for surface in ("--color-background-primary", "--color-background-control", "--color-background-selected"):
                ratio = self.contrast(palette["--color-accent-blue"], palette[surface])
                self.assertGreaterEqual(ratio, 3, f"{scheme}: accent on {surface} is {ratio:.2f}:1")

    def test_the_line_above_the_status_bar_shows_in_light_and_dark(self):
        footer = (self.WWW / "private/css/Layout.css").read_text(encoding="utf-8")
        block = footer[footer.index("#desktopFooterWrapper {"):].split("}")[0]
        self.assertIn("border-top: 1px solid var(--color-border-status)", block)
        for scheme in ("light", "dark"):
            palette = self.palette(scheme)
            ratio = self.contrast(palette["--color-border-status"], palette["--color-background-default"])
            self.assertGreaterEqual(ratio, 2, f"{scheme}: the status bar line is only {ratio:.2f}:1 against the window")

    @staticmethod
    def without_blocks(css, openers):
        """css without the {...} blocks that start with one of the openers."""
        for opener in openers:
            while opener in css:
                start = css.index(opener)
                depth, end = 0, css.index("{", start)
                for end in range(end, len(css)):
                    depth += (css[end] == "{") - (css[end] == "}")
                    if depth == 0:
                        break
                css = css[:start] + css[end + 1:]
        return css

    def test_no_color_literals_outside_the_palette(self):
        literal = re.compile(r"#[0-9a-fA-F]{3,8}\b|\brgba?\(|\bhsla?\(")
        for path in self.CSS_FILES:
            if path.name in self.NO_PALETTE:
                continue
            css = re.sub(r"/\*.*?\*/", "", path.read_text(encoding="utf-8"), flags=re.DOTALL)
            css = re.sub(r"url\([^)]*\)", "", css)
            css = self.without_blocks(css, (":root {", "@media (prefers-color-scheme: dark) {"))
            self.assertEqual(literal.findall(css), [], f"{path.name} has a color outside the palette")

    def test_one_system_font(self):
        files = self.CSS_FILES + [self.WWW / "private/views/preferences.html", self.WWW / "private/scripts/mocha-init.js"]
        for path in files:
            text = path.read_text(encoding="utf-8")
            self.assertIsNone(re.search(r"Arial|Verdana|Helvetica|Courier", text), f"{path.name} names a web font")


class WebUIPopupTests(unittest.TestCase):
    """The popup pages share one layout: a title bar from the window frame, the fields, then the buttons at the bottom right.

    Reads the source files, so it needs no running qBittorrent."""

    WWW = SOURCE / "webui/www/private"
    # the multi-rename window lays out a file table next to its settings, with its own button row
    OWN_LAYOUT = {"index.html", "rename_files.html"}

    def popup_pages(self):
        return [p for p in sorted(self.WWW.glob("*.html")) if p.name not in self.OWN_LAYOUT]

    def test_every_popup_page_has_its_buttons_in_the_shared_row(self):
        for path in self.popup_pages():
            text = path.read_text(encoding="utf-8")
            self.assertRegex(text, r'class="[^"]*dialogButtons', f"{path.name} has no button row")
            self.assertRegex(text, r"closeFrameWindow|cancelBtn|Cancel", f"{path.name} has no way to cancel")

    def test_every_element_a_popup_page_looks_up_exists(self):
        for path in self.popup_pages():
            text = path.read_text(encoding="utf-8")
            used = set(re.findall(r'getElementById\("([\w-]+)"\)', text))
            present = set(re.findall(r'[ "]id="([\w-]+)"', text))
            self.assertEqual(sorted(used - present), [], f"{path.name} looks up an id the page doesn't have")

    def test_confirmations_and_larger_windows_use_the_shared_row(self):
        views = self.WWW / "views"
        for name in ("confirmdeletion", "confirmRecheck", "confirmAPIKey", "confirmAutoTMM", "cookies", "searchplugins",
                     "installsearchplugin"):
            text = (views / f"{name}.html").read_text(encoding="utf-8")
            self.assertRegex(text, r'class="[^"]*dialogButtons', f"{name}.html has no button row")

    def test_popup_frames_are_drawn_by_css_without_a_footer(self):
        text = (self.WWW / "scripts/monkeypatch.js").read_text(encoding="utf-8")
        for option in ("useCanvas = false", "useCanvasControls = false", "footerHeight = 0", "headerHeight = 30"):
            self.assertIn(option, text)
        self.assertIn("footerHeight: 0", (self.WWW / "scripts/mocha-init.js").read_text(encoding="utf-8"))


class WebUIPhoneTests(unittest.TestCase):
    """The phone layout (main window under 640 px): one width everywhere, and every piece it needs is there.

    Reads the source files, so it needs no running qBittorrent."""

    WWW = SOURCE / "webui"
    PRIVATE = WWW / "www/private"

    def read(self, name):
        return (self.PRIVATE / name).read_text(encoding="utf-8")

    def test_the_style_sheet_and_script_are_loaded_and_shipped(self):
        index = self.read("index.html")
        self.assertIn('css/Phone.css', index)
        self.assertIn('scripts/phone.js', index)
        # the script patches the windows at load, so it comes before the code that opens them
        self.assertLess(index.index("scripts/phone.js"), index.index("scripts/client.js"))
        qrc = (self.WWW / "www/webui.qrc").read_text(encoding="utf-8")
        self.assertIn("private/css/Phone.css", qrc)
        self.assertIn("private/scripts/phone.js", qrc)

    def test_the_narrow_width_is_the_same_in_the_style_sheet_and_the_scripts(self):
        self.assertEqual(re.findall(r"@media \(max-width: (\d+)px\)", self.read("css/Phone.css")), ["639"])
        for name in ("scripts/misc.js", "scripts/phone.js"):
            self.assertEqual(set(re.findall(r"\(max-width: (\d+)px\)", self.read(name))), {"639"}, name)

    def test_the_elements_the_script_looks_up_exist(self):
        index = self.read("index.html")
        present = set(re.findall(r'[ "]id="([\w-]+)"', index))
        script = self.read("scripts/phone.js")
        # elements the main window builds itself, or the script creates
        built = {"Filters", "transferList", "propertiesPanel", "propertiesPanel_wrapper", "propertiesPanel_header",
                 "transferList_handle", "pageWrapper", "propertiesSheetBar", "propertiesSheetTitle", "propertiesSheetClose"}
        wanted = set(re.findall(r'getElementById\("([\w-]+)"\)', script))
        self.assertEqual(sorted(wanted - present - built), [])
        # every item of the More menu stands for a menu item that exists
        for source in re.findall(r'data-source="(\w+)"', index):
            self.assertIn(source, present)

    def test_the_phone_columns_of_the_transfer_list_exist(self):
        table = self.read("scripts/dynamicTable.js")
        columns = re.search(r"phoneColumns = \{(.*?)\};", table, re.DOTALL).group(1)
        names = re.findall(r"(\w+):", columns)
        self.assertEqual(names, ["name", "progress", "status", "dlspeed", "upspeed"])
        for name in names:
            self.assertIn(f'this.newColumn("{name}"', table)

    def test_the_back_button_closes_what_is_on_top(self):
        script = self.read("scripts/phone.js")
        self.assertIn("history.pushState", script)
        self.assertIn('addEventListener("popstate"', script)
        # popups, the sheet and the sidebar each take part
        self.assertEqual(script.count("openLayer("), 3)
        # Download from URLs opens Add Torrent before it closes; its entry is taken away with the next one,
        # so adding a torrent from a link doesn't leave a Back press that does nothing
        self.assertEqual(script.count("dropClosedLayers();"), 2)
        # a reload closes the layers but not their history entries: each entry says how many are under it,
        # and the reloaded page goes back past them
        self.assertIn("{ qbtLayer: layers.length }", script)
        self.assertIn("history.go(-staleLayers)", script)

    def test_the_search_and_rss_downloader_bars_fit_their_controls(self):
        # the Search bar is as high as its controls (a fixed 24 px cut them off), and the RSS Downloader
        # measures the gaps in its column instead of assuming 15 px, so its Save button stays inside the column
        search = self.read("views/search.html")
        bar = search[search.index("#searchResultsHeaderContainer {"):]
        self.assertNotRegex(bar[:bar.index("}")], r"(?<!min-)height: 24px")
        downloader = self.read("views/rssDownloader.html")
        self.assertIn("marginsHeight(\"rssDownloaderFeeds\")", downloader)
        self.assertNotIn(".height + 15;", downloader)

    def test_the_touch_size_is_for_the_phone_layout_only(self):
        # a wider window on a touch screen keeps the bars (RSS, Search, Log) that fit the desktop sizes
        css = self.read("css/style.css")
        self.assertEqual(re.findall(r"@media \(pointer: coarse\)[^{]*", css), ["@media (pointer: coarse) and (max-width: 639px) "])

    def test_the_new_phone_strings_are_translated_in_every_language(self):
        for path in sorted((self.WWW / "www/translations").glob("webui_*.ts")):
            text = path.read_text(encoding="utf-8")
            self.assertIn("<source>More</source>", text, path.name)
            if path.name != "webui_en.ts":
                self.assertNotRegex(text, r"<source>More</source>\s*<translation type=\"unfinished\"", path.name)

    def test_the_layout_saved_for_a_wide_window_is_left_alone_on_a_phone(self):
        client = self.read("scripts/client.js")
        save = client[client.index("const saveColumnSizes"):]
        self.assertLess(save.index("isPhoneLayout()"), save.index("localPreferences.set"))
        self.assertIn("window.qBittorrent.Phone.setup()", client)

    def test_the_touch_size_does_not_stretch_the_category_arrow(self):
        # the 40 px minimum for buttons on touch screens would make every category row three times as tall
        css = self.read("css/style.css")
        rule = css[css.index(".categoryToggle {\n    background"):]
        rule = rule[:rule.index("}")]
        self.assertIn("min-height: 0", rule)


class PersistenceTests(AccessTestCase):
    def test_sessions_survive_a_restart(self):
        alice = self.client("alice")
        self.assertEqual(alice.request("GET", "/api/v2/torrents/info")[0], 200)
        self.qbt.stop()
        self.qbt.start()
        self.assertEqual(alice.request("GET", "/api/v2/torrents/info")[0], 200)


class LocalhostTests(AccessTestCase):
    instance_options = {"localhost_auth": False}

    def test_localhost_works_without_login_and_is_logged(self):
        local = Client(self.qbt)
        status, _, _ = local.request("POST", "/api/v2/torrents/reannounce", form={"hashes": "all"})
        self.assertEqual(status // 100, 2)
        self.assertEqual(self.qbt.activity(endpoint="torrents/reannounce")[0]["username"], "(localhost)")
        status, me = local.access("me")
        self.assertEqual(me["kind"], "localhost")

    def test_version_stays_plain_for_sonarr_and_radarr(self):
        # Sonarr and Radarr call this from localhost with no login and check the number
        status, _, body = Client(self.qbt).request("GET", "/api/v2/app/version")
        self.assertEqual((status, body), (200, b"v5.3.0"))

    def test_build_info_names_the_edition_version(self):
        # the edition number is separate from the qBittorrent one, which has to stay v5.3.0
        status, _, body = Client(self.qbt).request("GET", "/api/v2/app/buildInfo")
        self.assertEqual(status, 200)
        edition = json.loads(body)["edition_version"]
        self.assertRegex(edition, r"^\d+\.\d+\.\d+$")
        self.assertNotEqual(edition, "5.3.0")

    def test_a_request_a_proxy_passed_on_is_not_this_pc(self):
        # a proxy on this PC connects from 127.0.0.1; whoever is behind it must sign in, even when it names a loopback address
        for header in ({"X-Forwarded-For": "127.0.0.1"}, {"CF-Connecting-IP": "127.0.0.1"},
                       {"Forwarded": "for=127.0.0.1"}, {"X-Forwarded-For": "::1"}):
            status, _, _ = Client(self.qbt).request("GET", "/api/v2/app/version", headers=header)
            self.assertEqual(status, 403, header)
        self.assertEqual(Client(self.qbt).request("GET", "/api/v2/app/version")[0], 200)

    def test_signing_in_on_localhost_uses_the_account(self):
        local = Client(self.qbt)
        local.request("GET", "/api/v2/app/version")
        status, _, _ = local.login("victor", "victor-password")
        self.assertEqual(status // 100, 2)
        status, me = local.access("me")
        self.assertEqual((me["kind"], me["username"]), ("user", "victor"))
        self.assertEqual(local.request("POST", "/api/v2/torrents/stop", form={"hashes": "all"})[0], 403)


class AppInstallTests(AccessTestCase):
    # the browser reads the manifest and icons without the session cookie, also before anyone signs in
    APP_FILES = ("/manifest.json", "/images/qbittorrent-app-192.png", "/images/qbittorrent-app-512.png",
                 "/images/qbittorrent-tray.svg", "/scripts/applaunch.js")

    def manifest(self):
        status, _, body = Client(self.qbt).request("GET", "/manifest.json")
        self.assertEqual(status, 200)
        return json.loads(body)

    def test_app_files_load_with_and_without_signing_in(self):
        for signed_in, client in ((False, Client(self.qbt)), (True, self.client("alice"))):
            for path in self.APP_FILES:
                with self.subTest(path=path, signed_in=signed_in):
                    status, _, body = client.request("GET", path)
                    self.assertEqual(status, 200)
                    self.assertTrue(body)
        for size in (192, 512):
            _, headers, body = Client(self.qbt).request("GET", f"/images/qbittorrent-app-{size}.png")
            self.assertEqual(headers["content-type"], "image/png")
            # PNG header: width and height of the IHDR chunk
            self.assertEqual((int.from_bytes(body[16:20], "big"), int.from_bytes(body[20:24], "big")), (size, size))

    def test_manifest_declares_the_app(self):
        manifest = self.manifest()
        self.assertEqual((manifest["name"], manifest["short_name"]), ("qBittorrent WebUI", "qBittorrent"))
        self.assertEqual(manifest["display"], "standalone")
        self.assertEqual(manifest["file_handlers"], [{"action": "./", "accept": {"application/x-bittorrent": [".torrent"]}}])
        self.assertEqual(manifest["protocol_handlers"], [{"protocol": "magnet", "url": "./?magnet=%s"}])
        self.assertEqual(manifest["launch_handler"], {"client_mode": "focus-existing"})

        # every URL stays inside the app, so the app also works behind a reverse proxy sub-path
        urls = [manifest["id"], manifest["start_url"], manifest["scope"]]
        urls += [icon["src"] for icon in manifest["icons"]]
        urls += [handler["action"] for handler in manifest["file_handlers"]]
        urls += [handler["url"] for handler in manifest["protocol_handlers"]]
        for url in urls:
            with self.subTest(url=url):
                parts = urlsplit(url)
                self.assertFalse(parts.scheme or parts.netloc or url.startswith("/") or (".." in parts.path.split("/")))
        for icon in manifest["icons"]:
            with self.subTest(icon=icon["src"]):
                self.assertEqual(Client(self.qbt).request("GET", f"/{icon['src']}")[0], 200)

    def test_login_and_main_page_link_the_app(self):
        for signed_in, client in ((False, Client(self.qbt)), (True, self.client("alice"))):
            with self.subTest(signed_in=signed_in):
                status, _, body = client.request("GET", "/")
                self.assertEqual(status, 200)
                page = body.decode()
                self.assertIn('id="desktop"' if signed_in else 'id="loginform"', page)
                self.assertIn('<link rel="manifest" href="manifest.json">', page)
                self.assertRegex(page, r'<script defer src="scripts/applaunch\.js\?v=\w+"></script>')


class NoAccountsTests(AccessTestCase):
    instance_options = {"users": False}

    def test_shared_login_works_until_accounts_exist(self):
        client = self.client(SHARED_USER, SHARED_PASSWORD)
        status, me = client.access("me")
        self.assertEqual(me["kind"], "shared")
        self.assertEqual(client.access("users")[0], 200)

    def test_roles_can_be_edited_before_the_first_account_exists(self):
        # the "keep one admin" check only matters once there are accounts
        client = self.client(SHARED_USER, SHARED_PASSWORD)
        _, roles = client.access("roles")
        viewer = next(role for role in roles["roles"] if role["name"] == "Viewer")
        status, _ = client.access("updateRole", "POST", id=viewer["id"], description="Looks only")
        self.assertEqual(status, 200)
        status, role = client.access("createRole", "POST", name="Helper", permissions="view,add")
        self.assertEqual(status, 200)
        self.assertEqual(client.access("deleteRole", "POST", id=role["id"])[0], 200)



class BrokenDatabaseTests(AccessTestCase):
    instance_options = {"users": False, "max_auth_fail_count": 3, "broken_db": True}

    def test_wrong_password_still_bans_without_the_user_database(self):
        attacker = Client(self.qbt, ip="198.51.100.60")
        for _ in range(3):
            self.assertEqual(attacker.login(SHARED_USER, "wrong")[0], 401)
        status, _, body = attacker.login(SHARED_USER, SHARED_PASSWORD)
        self.assertEqual(status, 403)
        self.assertIn(b"banned", body)
        # the shared login keeps working for everyone else
        self.assertEqual(Client(self.qbt, ip="198.51.100.61").login(SHARED_USER, SHARED_PASSWORD)[0] // 100, 2)


class HardeningTests(AccessTestCase):
    def test_new_passwords_need_twelve_characters(self):
        admin = self.client("morgan")
        role = self.qbt.roles["Viewer"]
        status, _ = admin.access("createUser", "POST", username="eleven", password="x" * 11, role_id=role)
        self.assertEqual(status, 400)
        status, body = admin.access("createUser", "POST", username="twelve", password="x" * 12, role_id=role)
        self.assertEqual(status, 200, body)
        self.assertEqual(admin.access("catalog")[1]["password_min_length"], 12)

    def test_the_password_rule_is_written_once(self):
        # the pages read the minimum from the server's catalog and show it as a hint; none keeps a number of its own
        www = SOURCE / "webui/www/private"
        for name in ("useraccount.html", "userpassword.html"):
            page = (www / name).read_text(encoding="utf-8")
            self.assertIn("password_min_length", page, name)
            self.assertIn("At least %1 characters", page, name)
            self.assertIsNone(re.search(r"(minLength|MIN_LENGTH)\s*=\s*\d", page), name)
        self.assertIn("MIN_PASSWORD_LENGTH", (SOURCE / "gui/useraccountdialog.cpp").read_text(encoding="utf-8"))

    def test_the_first_account_defaults_to_a_role_that_can_manage_users(self):
        page = (SOURCE / "webui/www/private/useraccount.html").read_text(encoding="utf-8")
        self.assertIn('role.permissions.includes("manage_users")', page)
        self.assertIn("ADMIN_PERMISSION", (SOURCE / "gui/useraccountdialog.cpp").read_text(encoding="utf-8"))

    def test_the_activity_log_keeps_only_the_site_of_an_address(self):
        admin = self.client("morgan")
        magnet = ("magnet:?xt=urn:btih:" + "a" * 40 + "&dn=Some+Name"
                  "&tr=http%3A%2F%2Ftracker.invalid%2Fannounce%3Fpasskey%3DSECRETPASS")
        admin.request("POST", "/api/v2/torrents/add",
                      form={"urls": magnet + "\nhttps://files.invalid/get/SECRETPATH/a.torrent?authkey=SECRETKEY"})
        admin.request("POST", "/api/v2/rss/addFeed",
                      form={"url": "https://feed.invalid/rss/SECRETFEED?token=SECRETTOKEN", "path": "feed"})
        log = json.dumps(self.qbt.activity(username="morgan"))
        for secret in ("SECRETPASS", "SECRETPATH", "SECRETKEY", "SECRETFEED", "SECRETTOKEN"):
            self.assertNotIn(secret, log)
        for site in ("files.invalid", "feed.invalid"):
            self.assertIn(site, log)

    def test_only_an_admin_can_write_an_added_by_tag(self):
        alice = self.client("alice")
        status, info_hash, _ = alice.add_torrent("Forged", tags="added-by:morgan,fun")
        self.assertEqual(status // 100, 2)
        self.assertEqual(set(alice.torrent(info_hash)["tags"].split(", ")), {"fun", "added-by:alice"})
        status, _, _ = alice.request("POST", "/api/v2/torrents/addTags", form={"hashes": info_hash, "tags": "added-by:victor,more"})
        self.assertEqual(status // 100, 2)
        self.assertEqual(set(alice.torrent(info_hash)["tags"].split(", ")), {"fun", "more", "added-by:alice"})
        admin = self.client("morgan")
        admin.request("POST", "/api/v2/torrents/addTags", form={"hashes": info_hash, "tags": "added-by:morgan"})
        self.assertIn("added-by:morgan", alice.torrent(info_hash)["tags"])

    def test_only_an_admin_can_take_an_added_by_tag_off(self):
        alice = self.client("alice")
        status, info_hash, _ = alice.add_torrent("Keep-My-Name", tags="fun")
        self.assertEqual(status // 100, 2)
        self.assertEqual(set(alice.torrent(info_hash)["tags"].split(", ")), {"fun", "added-by:alice"})

        def tags():
            return set(alice.torrent(info_hash)["tags"].split(", "))

        # taking off just the added-by tag, clearing every tag and replacing all of them are all refused
        for endpoint, form in (("removeTags", {"hashes": info_hash, "tags": "added-by:alice"}),
                               ("removeTags", {"hashes": info_hash, "tags": ""}),
                               ("removeTags", {"hashes": "all", "tags": ""}),
                               ("setTags", {"hashes": info_hash, "tags": "other"}),
                               ("setTags", {"hashes": "all", "tags": "other"}),
                               ("deleteTags", {"tags": "added-by:alice"})):
            status, _, body = alice.request("POST", f"/api/v2/torrents/{endpoint}", form=form)
            self.assertEqual(status, 403, (endpoint, form, body))
            self.assertEqual(tags(), {"fun", "added-by:alice"}, (endpoint, form))
        # other tags can still be removed, also in the same call
        status, _, _ = alice.request("POST", "/api/v2/torrents/removeTags",
                                     form={"hashes": info_hash, "tags": "fun,added-by:alice"})
        self.assertEqual(status // 100, 2)
        self.assertEqual(tags(), {"added-by:alice"})
        # an admin may
        admin = self.client("morgan")
        status, _, _ = admin.request("POST", "/api/v2/torrents/removeTags", form={"hashes": info_hash, "tags": "added-by:alice"})
        self.assertEqual(status // 100, 2)
        self.assertEqual(alice.torrent(info_hash)["tags"], "")

    def with_role_permissions(self, role, permissions):
        with self.qbt.db() as con:
            original = con.execute("SELECT permissions FROM roles WHERE name = ?", (role,)).fetchone()[0]
            con.execute("UPDATE roles SET permissions = ? WHERE name = ?", (json.dumps(permissions), role))
        self.addCleanup(self.restore_role_permissions, role, original)

    def restore_role_permissions(self, role, original):
        with self.qbt.db() as con:
            con.execute("UPDATE roles SET permissions = ? WHERE name = ?", (original, role))

    def test_settings_alone_cannot_reach_sign_in_or_run_program_options(self):
        self.with_role_permissions("User", ["view", "settings"])
        alice = self.client("alice")
        admin = self.client("morgan")
        form = {"json": json.dumps({"max_connec": 321, "web_ui_username": "hacked", "autorun_enabled": True,
                                    "autorun_program": "calc.exe", "bypass_local_auth": True,
                                    "web_ui_reverse_proxies_list": "0.0.0.0/0"})}
        status, _, body = alice.request("POST", "/api/v2/app/setPreferences", form=form)
        self.assertEqual(status // 100, 2, body)
        prefs = json.loads(admin.request("GET", "/api/v2/app/preferences")[2])
        self.assertEqual(prefs["max_connec"], 321)  # ordinary options still apply
        self.assertEqual(prefs["web_ui_username"], SHARED_USER)
        self.assertFalse(prefs["autorun_enabled"])
        self.assertEqual(prefs["autorun_program"], "")
        self.assertFalse(prefs["bypass_local_auth"])
        self.assertNotEqual(prefs["web_ui_reverse_proxies_list"], "0.0.0.0/0")
        # an admin can
        status, _, _ = admin.request("POST", "/api/v2/app/setPreferences", form={"json": json.dumps({"web_ui_max_auth_fail_count": 7})})
        self.assertEqual(status // 100, 2)
        self.assertEqual(json.loads(admin.request("GET", "/api/v2/app/preferences")[2])["web_ui_max_auth_fail_count"], 7)

    def test_only_an_admin_can_see_or_replace_the_api_key(self):
        self.with_role_permissions("User", ["view", "settings"])
        alice = self.client("alice")
        admin = self.client("morgan")
        for endpoint in ("rotateAPIKey", "deleteAPIKey"):
            status, headers, _ = alice.request("POST", f"/api/v2/app/{endpoint}", form={})
            self.assertEqual((status, headers.get("x-access-denied")), (403, "manage_users"), endpoint)
        self.assertEqual(admin.request("POST", "/api/v2/app/rotateAPIKey", form={})[0] // 100, 2)
        self.assertNotEqual(json.loads(admin.request("GET", "/api/v2/app/preferences")[2])["web_ui_api_key"], "")
        prefs = json.loads(alice.request("GET", "/api/v2/app/preferences")[2])
        self.assertEqual(prefs["web_ui_api_key"], "")
        self.assertEqual(prefs["web_ui_username"], SHARED_USER)  # the rest is still visible with "settings"


class ActivityLogLimitTests(AccessTestCase):
    def test_the_log_keeps_at_most_the_newest_rows(self):
        limit = 200000
        self.qbt.stop()
        with self.qbt.db() as con:
            con.executemany("INSERT INTO activity (ts, category, summary, outcome) VALUES (?, 'other', 'filler', 'ok')",
                            [(time.time(),)] * (limit + 50))
            newest = con.execute("SELECT MAX(id) FROM activity").fetchone()[0]
        self.qbt.start()  # the clean-up runs when the user database opens
        with self.qbt.db() as con:
            count, oldest, latest = con.execute("SELECT COUNT(*), MIN(id), MAX(id) FROM activity").fetchone()
        self.assertEqual(count, limit)
        self.assertEqual(latest, newest)
        self.assertEqual(oldest, newest - limit + 1)


class DatabaseVersionTests(AccessTestCase):
    def user_version(self):
        with self.qbt.db() as con:
            return con.execute("PRAGMA user_version").fetchone()[0]

    def test_the_database_has_a_version_and_one_from_a_newer_program_is_left_alone(self):
        self.assertEqual(Client(self.qbt).login("morgan", "morgan-password")[0] // 100, 2)
        self.qbt.stop()
        self.assertEqual(self.user_version(), 1)

        with self.qbt.db() as con:
            con.execute("PRAGMA user_version=99")
        self.qbt.start()
        # its accounts are not used; the shared login still gets in, and the file is not touched
        self.assertEqual(Client(self.qbt).login("morgan", "morgan-password")[0], 401)
        self.assertEqual(Client(self.qbt).login(SHARED_USER, SHARED_PASSWORD)[0] // 100, 2)
        self.qbt.stop()
        self.assertEqual(self.user_version(), 99)


class SecondInstanceTests(unittest.TestCase):
    # a torrent opened while qBittorrent runs is handed to it by a short-lived second copy;
    # opening several files in Explorer starts one copy per file, all at the same moment
    COUNT = 10

    def setUp(self):
        if not NOX.exists():
            raise unittest.SkipTest(f"{NOX} not built")
        self.qbt = QbtInstance()
        self.addCleanup(self.qbt.cleanup)
        self.addCleanup(self.qbt.stop)

    def open_at_once(self, prefix):
        hashes, paths = [], []
        for i in range(self.COUNT):
            torrent, info_hash = make_torrent(f"{prefix}{i}")
            path = self.qbt.root / f"{prefix}{i}.torrent"
            path.write_bytes(torrent)
            hashes.append(info_hash)
            paths.append(path)
        processes = [subprocess.Popen([str(NOX), f"--profile={self.qbt.root}", "--skip-dialog=true", str(path)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) for path in paths]
        self.addCleanup(lambda: [p.kill() for p in processes if p.poll() is None])
        return hashes, processes

    def assert_all_added(self, hashes):
        admin = Client(self.qbt)
        self.assertEqual(admin.login("morgan", "morgan-password")[0] // 100, 2)
        self.assertEqual([h for h in hashes if admin.torrent(h) is None], [])

    def test_files_opened_at_once_while_running_are_all_added(self):
        self.qbt.start()
        hashes, processes = self.open_at_once("running")
        for process in processes:
            self.assertEqual(process.wait(60), 0)
        self.assert_all_added(hashes)

    def test_files_opened_at_once_while_not_running_are_all_added(self):
        hashes, processes = self.open_at_once("starting")
        # one copy becomes qBittorrent, the others hand their file to it and exit
        deadline = time.time() + 60
        while (sum(p.poll() is None for p in processes) > 1) and (time.time() < deadline):
            time.sleep(0.2)
        running = [p for p in processes if p.poll() is None]
        self.assertEqual(len(running), 1)
        self.qbt.process = running[0]
        self.qbt.wait_ready()
        self.assert_all_added(hashes)


if __name__ == "__main__":
    unittest.main()
