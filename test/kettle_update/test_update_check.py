"""Tests for the Kettle Edition update check, against a real qBittorrent with a throw-away profile.

A local web server stands in for GitHub, and the instance is pointed at it with KETTLE_UPDATE_URL
and KETTLE_UPDATE_PUBLIC_KEY. Those two settings only work outside the Program Files folders, and one
test checks exactly that. The user's real qBittorrent is never touched.

Run with:
    python -m unittest discover -s test/kettle_update -v
Environment:
    QBT_NOX       full path of the qbittorrent-ke.exe under test (the desktop build; it has the updater)
    QBT_OPENSSL   openssl.exe used to make test keys and signatures (default: the one in Git for Windows)
"""

import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "webui_access"))
import test_access  # noqa: E402  (a module import only, so its test classes are not collected here)

OPENSSL = os.environ.get("QBT_OPENSSL", r"C:\Program Files\Git\mingw64\bin\openssl.exe")
REAL_PRIVATE_KEY = Path(__file__).resolve().parents[3] / "signing" / "kettle-update.key"


class Signer:
    """An Ed25519 key pair made with openssl, signing the same way publish-update.ps1 does."""

    def __init__(self, key_path=None):
        self.folder = Path(tempfile.mkdtemp(prefix="kettle-update-keys-"))
        self.key = Path(key_path) if key_path else self.folder / "key.pem"
        if not key_path:
            subprocess.run([OPENSSL, "genpkey", "-algorithm", "ed25519", "-out", str(self.key)], check=True)
        der = subprocess.run([OPENSSL, "pkey", "-in", str(self.key), "-pubout", "-outform", "DER"],
                             check=True, capture_output=True).stdout
        self.public_key_hex = der[-32:].hex()

    def sign(self, data):
        source, signature = self.folder / "data", self.folder / "data.sig"
        source.write_bytes(data)
        subprocess.run([OPENSSL, "pkeyutl", "-sign", "-inkey", str(self.key), "-rawin",
                        "-in", str(source), "-out", str(signature)], check=True)
        return signature.read_bytes()

    def cleanup(self):
        shutil.rmtree(self.folder, ignore_errors=True)


class ReleaseServer:
    """Serves the release files from memory and remembers what was asked for."""

    def __init__(self):
        self.files = {}
        self.redirects = {}
        self.requests = []
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                outer.requests.append(self.path)
                if self.path in outer.redirects:
                    self.send_response(302)
                    self.send_header("Location", outer.redirects[self.path])
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                body = outer.files.get(self.path)
                if body is None:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}/"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def publish(self, signer, edition, level="tweak", notes="a small fix", sign_with=None, files=None, served=None):
        """files: {path in the program folder: content}, listed in the signed info and served under the tag's folder.
        served: {path: content} to hand out instead of the listed content, to play a damaged or swapped download."""
        tag = f"kettle-v{edition}"
        listed = []
        for path, content in (files or {}).items():
            flat = path.replace("/", ".")
            listed.append({"path": path, "name": flat, "size": len(content), "sha256": hashlib.sha256(content).hexdigest()})
            self.files[f"/download/{tag}/{flat}"] = (served or {}).get(path, content)
        manifest = json.dumps({"edition": edition, "level": level, "notes": notes, "base": "5.3.0",
                               "tag": tag, "files": listed}).encode()
        self.files["/kettle-update.json"] = manifest
        self.files["/kettle-update.json.sig"] = (sign_with or signer).sign(manifest)

    def asked_for(self, name):
        return self.requests.count(f"/{name}")

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class UpdateCheckTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not test_access.NOX.exists():
            raise unittest.SkipTest(f"{test_access.NOX} not built")
        cls.signer = Signer()
        cls.other_signer = Signer()
        cls.addClassCleanup(cls.signer.cleanup)
        cls.addClassCleanup(cls.other_signer.cleanup)

    def setUp(self):
        self.server = ReleaseServer()
        self.addCleanup(self.server.close)

    def start(self, *, exe=None, use_built_in_key=False):
        """Starts a copy with the update check on, and returns an admin client."""
        env = {"KETTLE_UPDATE_URL": self.server.url, "KETTLE_UPDATE_PUBLIC_KEY": self.signer.public_key_hex}
        qbt = test_access.QbtInstance(users=True, extra_ini="Advanced\\updateCheck=true")
        self.qbt = qbt
        self.addCleanup(qbt.cleanup)
        self.addCleanup(qbt.stop)
        with mock.patch.dict(os.environ, env), mock.patch.object(test_access, "NOX", exe or test_access.NOX):
            if use_built_in_key:
                del os.environ["KETTLE_UPDATE_PUBLIC_KEY"]  # patch.dict puts it back afterwards
            qbt.start()
        admin = test_access.Client(qbt)
        self.assertEqual(admin.login("morgan", "morgan-password")[0] // 100, 2)
        return admin

    def wait_for_check(self, names=("kettle-update.json", "kettle-update.json.sig")):
        deadline = time.time() + 30
        while time.time() < deadline:
            if all(self.server.asked_for(name) for name in names):
                time.sleep(1.5)  # the answers are processed a moment after they are fetched
                return
            time.sleep(0.2)
        self.fail(f"the update check never asked for {names}: {self.server.requests}")

    def update_folder(self, edition):
        return self.qbt.root / "qBittorrent" / "cache" / "kettle-update" / edition

    @staticmethod
    def log_lines(admin):
        status, _, body = admin.request("GET", "/api/v2/log/main?normal=true&info=true&warning=true&critical=true")
        assert status == 200, status
        return [entry["message"] for entry in json.loads(body)]

    @staticmethod
    def edition_in_source():
        header = (Path(__file__).resolve().parents[2] / "src" / "base" / "version.h.in").read_text(encoding="utf-8")
        return ".".join(re.search(rf"#define QBT_EDITION_VERSION_{part} (\d+)", header).group(1)
                        for part in ("MAJOR", "MINOR", "PATCH"))

    def assert_logged(self, admin, text):
        self.assertTrue(any(text in line for line in self.log_lines(admin)), self.log_lines(admin))

    def assert_not_logged(self, admin, text):
        self.assertFalse(any(text in line for line in self.log_lines(admin)), self.log_lines(admin))


class EditionCheckTests(UpdateCheckTestCase):
    def test_newer_signed_edition_is_reported_with_its_notes(self):
        self.server.publish(self.signer, "99.1.0", level="feature", notes="faster Options window")
        admin = self.start()
        self.wait_for_check()
        self.assert_logged(admin, "Kettle Edition 99.1.0 is available. faster Options window")

    def test_official_release_list_is_not_asked_for(self):
        self.server.files["/versions.json"] = json.dumps({"win": {"version": "99.0.0"}, "macos": {"version": "99.0.0"}}).encode()
        admin = self.start()
        self.wait_for_check()
        self.assertEqual(self.server.asked_for("versions.json"), 0, self.server.requests)
        self.assert_not_logged(admin, "Official qBittorrent")

    def test_same_edition_is_not_reported(self):
        self.server.publish(self.signer, self.edition_in_source())
        admin = self.start()
        self.wait_for_check()
        self.assert_not_logged(admin, "is available")

    def test_older_edition_is_not_reported(self):
        self.server.publish(self.signer, "0.0.1")
        admin = self.start()
        self.wait_for_check()
        self.assert_not_logged(admin, "Kettle Edition 0.0.1")

    def test_wrong_signature_is_ignored_and_logged(self):
        self.server.publish(self.signer, "99.1.0", sign_with=self.other_signer)
        admin = self.start()
        self.wait_for_check()
        self.assert_logged(admin, "signature is not valid")
        self.assert_not_logged(admin, "Kettle Edition 99.1.0 is available")

    def test_changed_manifest_is_ignored(self):
        self.server.publish(self.signer, "99.1.0")
        self.server.files["/kettle-update.json"] = self.server.files["/kettle-update.json"].replace(b"99.1.0", b"99.9.9")
        admin = self.start()
        self.wait_for_check()
        self.assert_logged(admin, "signature is not valid")
        self.assert_not_logged(admin, "is available")

    def test_missing_release_files_only_log_a_failed_download(self):
        admin = self.start()
        self.wait_for_check()
        self.assert_logged(admin, "Failed to download the program update info")
        self.assert_not_logged(admin, "is available")

    def test_oversized_release_file_is_refused_and_the_check_still_finishes(self):
        # a bigger-than-expected file is refused once, and the check still finishes
        self.server.publish(self.signer, "99.1.0")
        self.server.files["/kettle-update.json.sig"] = b"x" * (3 * 1024 * 1024)
        admin = self.start()
        self.wait_for_check()
        lines = self.log_lines(admin)
        self.assertEqual(sum("The file is larger than expected" in line for line in lines), 1, lines)
        self.assert_not_logged(admin, "Kettle Edition 99.1.0 is available")

    @unittest.skipUnless(REAL_PRIVATE_KEY.exists(), "the real signing key is not on this PC")
    def test_the_key_built_into_the_app_matches_the_real_signing_key(self):
        # KETTLE_UPDATE_PUBLIC_KEY is not set: the app uses the key compiled into it
        self.server.publish(Signer(REAL_PRIVATE_KEY), "99.2.0", notes="signed with the real key")
        admin = self.start(use_built_in_key=True)
        self.wait_for_check()
        self.assert_logged(admin, "Kettle Edition 99.2.0 is available. signed with the real key")


class DownloadTests(UpdateCheckTestCase):
    """The files of a newer release are fetched from the release's tag, checked, and only then kept."""

    def program_file(self):
        # a file that really is installed next to the exe under test, to see that unchanged files are not fetched again
        path = Path(test_access.NOX).parent / "Qt6Core.dll"
        return path.name, path.read_bytes()

    def wait_for_log(self, admin, text):
        deadline = time.time() + 30
        while time.time() < deadline:
            if any(text in line for line in self.log_lines(admin)):
                return
            time.sleep(0.3)
        self.fail(f"never logged {text!r}: {self.log_lines(admin)}")

    def test_changed_files_are_downloaded_checked_and_marked_ready(self):
        name, content = self.program_file()
        files = {"qbittorrent-ke.exe": b"new exe " * 1000, "plugins/inner.bin": b"inner file", name: content}
        self.server.publish(self.signer, "99.1.0", files=files)
        admin = self.start()
        self.wait_for_log(admin, "Kettle Edition 99.1.0 is downloaded and ready to install.")

        folder = self.update_folder("99.1.0")
        self.assertEqual((folder / "qbittorrent-ke.exe").read_bytes(), files["qbittorrent-ke.exe"])
        self.assertEqual((folder / "plugins" / "inner.bin").read_bytes(), b"inner file")
        self.assertFalse((folder / name).exists(), "a file that is already installed was downloaded")
        self.assertEqual(self.server.asked_for(f"download/kettle-v99.1.0/{name}"), 0)
        self.assertEqual(self.server.asked_for("download/kettle-v99.1.0/plugins.inner.bin"), 1)  # tag folder, flat name
        ready = json.loads((folder / "ready.json").read_text(encoding="utf-8"))
        self.assertEqual(ready["edition"], "99.1.0")
        self.assertEqual(sorted(item["path"] for item in ready["files"]), ["plugins/inner.bin", "qbittorrent-ke.exe"])

    def test_file_behind_a_redirect_is_downloaded(self):
        # GitHub hands release files over through a redirect to another host
        self.server.publish(self.signer, "99.1.0", files={"qbittorrent-ke.exe": b"new exe " * 5000})
        self.server.files["/storage/exe"] = self.server.files.pop("/download/kettle-v99.1.0/qbittorrent-ke.exe")
        self.server.redirects["/download/kettle-v99.1.0/qbittorrent-ke.exe"] = "/storage/exe"
        admin = self.start()
        self.wait_for_log(admin, "Kettle Edition 99.1.0 is downloaded and ready to install.")
        self.assertEqual((self.update_folder("99.1.0") / "qbittorrent-ke.exe").read_bytes(), b"new exe " * 5000)

    def test_file_with_the_wrong_hash_is_not_kept_and_the_update_is_not_ready(self):
        files = {"qbittorrent-ke.exe": b"new exe", "plugins/inner.bin": b"inner file"}
        self.server.publish(self.signer, "99.1.0", files=files, served={"plugins/inner.bin": b"inner FILE"})
        admin = self.start()
        self.wait_for_log(admin, "does not match its checksum")
        time.sleep(1)
        folder = self.update_folder("99.1.0")
        self.assertFalse((folder / "plugins" / "inner.bin").exists(), "a file with a wrong hash was written")
        self.assertFalse((folder / "ready.json").exists())
        self.assert_not_logged(admin, "ready to install")

    def test_file_with_the_wrong_size_is_not_kept(self):
        self.server.publish(self.signer, "99.1.0", files={"qbittorrent-ke.exe": b"0123456789"}, served={"qbittorrent-ke.exe": b"01234"})
        admin = self.start()
        self.wait_for_log(admin, "does not have the expected size")
        self.assertFalse((self.update_folder("99.1.0") / "qbittorrent-ke.exe").exists())
        self.assert_not_logged(admin, "ready to install")

    def test_missing_file_is_a_failed_download_and_not_ready(self):
        self.server.publish(self.signer, "99.1.0", files={"qbittorrent-ke.exe": b"new exe"})
        del self.server.files["/download/kettle-v99.1.0/qbittorrent-ke.exe"]
        admin = self.start()
        self.wait_for_log(admin, "Failed to download Kettle Edition 99.1.0")
        self.assertFalse((self.update_folder("99.1.0") / "ready.json").exists())

    def test_path_that_leaves_the_update_folder_is_refused_before_anything_is_fetched(self):
        # the info is signed, but a wrong path must still never be written outside the update folder
        self.server.publish(self.signer, "99.1.0", files={"qbittorrent-ke.exe": b"new exe", "../escape.bin": b"x"})
        admin = self.start()
        self.wait_for_log(admin, "The release info is not valid")
        self.assertEqual([r for r in self.server.requests if r.startswith("/download/")], [])
        self.assertFalse((self.qbt.root / "qBittorrent" / "cache" / "kettle-update" / "escape.bin").exists())

    def test_nothing_is_downloaded_for_a_version_that_is_not_newer(self):
        self.server.publish(self.signer, self.edition_in_source(), files={"qbittorrent-ke.exe": b"new exe"})
        admin = self.start()
        self.wait_for_check()
        self.assertEqual([r for r in self.server.requests if r.startswith("/download/")], [])
        self.assert_not_logged(admin, "Downloading Kettle Edition")

    def test_download_from_an_unsigned_change_never_starts(self):
        self.server.publish(self.signer, "99.1.0", files={"qbittorrent-ke.exe": b"new exe"}, sign_with=self.other_signer)
        admin = self.start()
        self.wait_for_check()
        self.assertEqual([r for r in self.server.requests if r.startswith("/download/")], [])


class InstalledCopyTests(UpdateCheckTestCase):
    def test_test_settings_do_nothing_in_a_program_files_folder(self):
        # a copy that sits in a Program Files folder must ignore both settings, so it can't be pointed at another server
        source = Path(test_access.NOX).parent
        target = Path(tempfile.mkdtemp(prefix="kettle-update-pf-")) / "Program Files" / "qBittorrent"
        self.addCleanup(shutil.rmtree, target.parents[1], True)
        target.mkdir(parents=True)
        shutil.copy2(source / "qbittorrent-ke.exe", target)
        for dll in source.glob("*.dll"):
            shutil.copy2(dll, target)
        for folder in ("generic", "iconengines", "imageformats", "networkinformation", "platforms", "sqldrivers", "styles", "tls"):
            if (source / folder).is_dir():
                shutil.copytree(source / folder, target / folder)

        self.server.publish(self.signer, "99.1.0")
        admin = self.start(exe=target / "qbittorrent-ke.exe")
        time.sleep(8)  # long enough for the startup check to have run
        self.assertEqual(self.server.requests, [], "the copy in Program Files asked the test server")
        self.assert_not_logged(admin, "Kettle Edition 99.1.0 is available")


class InstallTests(UpdateCheckTestCase):
    """Installing: kettle-update-helper.exe replaces the files of a throw-away copy of the program, and undoes it
    (files and profile) when the new version is no good. The copy's exe is the real one plus a zero byte, so it runs
    like the real one but is a different file, and the "new" exe can be the real one."""

    HELPER_NAME = "kettle-update-helper.exe"

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.source = Path(test_access.NOX).parent
        if not (cls.source / cls.HELPER_NAME).exists():
            raise unittest.SkipTest(f"{cls.HELPER_NAME} not built")
        cls.fake_exe = cls.compile_fake_app()

    @classmethod
    def compile_fake_app(cls):
        """A tiny exe that stands in for a broken new version. Needs the Visual Studio compiler."""
        work = Path(tempfile.mkdtemp(prefix="kettle-update-fake-"))
        cls.addClassCleanup(shutil.rmtree, work, True)
        exe = work / "fake.exe"
        script = work / "compile.ps1"
        script.write_text(
            '$vs = & "${env:ProgramFiles(x86)}\\Microsoft Visual Studio\\Installer\\vswhere.exe" -products * -latest -property installationPath\n'
            '& "$vs\\Common7\\Tools\\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null\n'
            f'cl /nologo /O1 /Fo"{work}\\\\" /Fe"{exe}" "{Path(__file__).with_name("fakeapp.c")}" user32.lib\n',
            encoding="utf-8")
        subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)],
                       capture_output=True, cwd=work)
        if not exe.exists():
            raise unittest.SkipTest("the Visual Studio compiler is not available to build the fake exe")
        return exe.read_bytes()

    def setUp(self):
        super().setUp()
        self.real_exe = (self.source / "qbittorrent-ke.exe").read_bytes()
        self.old_exe = self.real_exe + b"\0"
        self.program = Path(tempfile.mkdtemp(prefix="kettle update prog ")) / "qBittorrent"
        self.addCleanup(shutil.rmtree, self.program.parent, True)
        self.program.mkdir()
        for item in self.source.iterdir():
            if item.suffix == ".dll" or item.name == self.HELPER_NAME:
                shutil.copy2(item, self.program)
        for folder in ("generic", "iconengines", "imageformats", "networkinformation", "platforms", "sqldrivers", "styles", "tls"):
            if (self.source / folder).is_dir():
                shutil.copytree(self.source / folder, self.program / folder)
        (self.program / "qbittorrent-ke.exe").write_bytes(self.old_exe)
        self.process_roots = [str(self.program.parent)]
        self.addCleanup(self.kill_test_processes)

    def kill_test_processes(self):
        """Ends every process that runs from the throw-away folders (never any other qBittorrent)."""
        tests = " -or ".join(f"$_.Path.StartsWith('{root}', 'OrdinalIgnoreCase')" for root in self.process_roots)
        subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                        f"Get-Process | Where-Object {{ $_.Path -and ({tests}) }} | Stop-Process -Force"], capture_output=True)
        time.sleep(1)

    # ---- running it ----

    @property
    def data(self):
        return self.qbt.root / "qBittorrent" / "data"

    @property
    def update_root(self):
        return self.qbt.root / "qBittorrent" / "cache" / "kettle-update"

    def create_instance(self):
        self.qbt = test_access.QbtInstance(users=True, localhost_auth=False, extra_ini="Advanced\\updateCheck=true")
        self.addCleanup(self.qbt.cleanup)
        self.process_roots.append(str(self.qbt.root))
        self.addCleanup(self.kill_test_processes)  # runs before the profile folder is removed
        (self.data / "marker.txt").write_text("original", encoding="utf-8")

    def launch(self, *, install_in=None, **hooks):
        """Starts the throw-away program with the throw-away profile. install_in: seconds from now until the install time."""
        env = {**os.environ, "KETTLE_UPDATE_URL": self.server.url, "KETTLE_UPDATE_PUBLIC_KEY": self.signer.public_key_hex,
               "KETTLE_UPDATE_HEALTHY_SECONDS": "5", "KETTLE_UPDATE_HEALTHY_TIMEOUT": "40", **hooks}
        if install_in is not None:
            at = datetime.datetime.now() + datetime.timedelta(seconds=install_in)
            env["KETTLE_UPDATE_INSTALL_AT"] = at.strftime("%H:%M:%S")
        self.qbt.process = subprocess.Popen(
            [str(self.program / "qbittorrent-ke.exe"), f"--profile={self.qbt.root}", "--confirm-legal-notice"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def start(self, **kwargs):
        self.create_instance()
        self.launch(**kwargs)
        self.qbt.wait_ready()

    def admin(self):
        client = test_access.Client(self.qbt)
        self.assertEqual(client.login("morgan", "morgan-password")[0] // 100, 2)
        return client

    def update_log(self):
        path = self.data / "update.log"
        return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""

    def wait_for_update_log(self, text, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if text in self.update_log():
                return
            time.sleep(0.5)
        self.fail(f"update.log never said {text!r}:\n{self.update_log()}")

    def wait_for_api(self, timeout=60):
        """The WebUI of the program the helper started; no login is needed from this PC."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                status, _, body = test_access.Client(self.qbt).request("GET", "/api/v2/app/version")
                if status == 200:
                    return body.decode()
            except OSError:
                pass
            time.sleep(0.5)
        self.fail("the program did not answer after the update")

    def wait_for_log(self, text, timeout=60):
        deadline = time.time() + timeout
        lines = []
        while time.time() < deadline:
            try:
                lines = self.log_lines(self.admin())
            except (OSError, AssertionError):
                lines = []
            if any(text in line for line in lines):
                return
            time.sleep(0.5)
        self.fail(f"never logged {text!r}: {lines}")

    def wait_for_ready(self):
        self.wait_for_log("is downloaded and ready to install.")

    def stop_program(self):
        admin = self.admin()
        admin.request("POST", "/api/v2/app/shutdown", form={})
        self.qbt.process.wait(60)

    def publish(self, files, **kwargs):
        self.server.publish(self.signer, "99.1.0", files=files, **kwargs)

    def skipped_editions(self):
        path = self.update_root / "skipped.txt"
        return path.read_text(encoding="utf-8").split() if path.exists() else []

    def assert_old_version_is_back(self, *, skipped=True):
        self.assertEqual((self.program / "qbittorrent-ke.exe").read_bytes(), self.old_exe)
        self.assertEqual(sorted(self.program.rglob("*.old")), [])
        self.assertEqual((self.data / "marker.txt").read_text(encoding="utf-8"), "original")
        self.assertFalse((self.data / "changed.txt").exists(), "a file the new version made in the profile was kept")
        self.assertFalse((self.qbt.root / "qBittorrent" / "data-update-backup").exists())
        self.assertEqual("99.1.0" in self.skipped_editions(), skipped)

    # ---- an update that works ----

    def test_update_is_installed_at_the_install_time_and_the_old_files_are_kept_for_one_step_back(self):
        self.publish({"qbittorrent-ke.exe": self.real_exe}, level="feature", notes="faster Options window")
        self.start(install_in=6)
        self.wait_for_update_log("Update installed.")

        self.assertEqual((self.program / "qbittorrent-ke.exe").read_bytes(), self.real_exe)
        self.assertEqual(sorted(self.program.rglob("*.old")), [])
        self.assertEqual((self.data / "kettle-previous" / "qbittorrent-ke.exe").read_bytes(), self.old_exe)
        self.assertFalse((self.qbt.root / "qBittorrent" / "data-update-backup").exists())
        self.assertFalse((self.update_root / "99.1.0").exists())
        self.assertEqual((self.data / "marker.txt").read_text(encoding="utf-8"), "original")

        # the program that is running now is the new one; Sonarr and Radarr see the same plain version, without a login
        self.assertEqual(self.wait_for_api(), "v5.3.0")
        # this test's "new" exe is the real build, which reports 1.0.0 and not the 99.1.0 of the release info: it is not offered again
        self.wait_for_log("was installed but reports itself as")
        self.assertIn("99.1.0", (self.update_root / "skipped.txt").read_text(encoding="utf-8").split())

    def test_update_that_is_waiting_is_not_installed_once_the_setting_is_turned_off(self):
        self.publish({"qbittorrent-ke.exe": self.real_exe})
        self.start()
        self.wait_for_ready()
        self.stop_program()
        ini = self.qbt.root / "qBittorrent" / "config" / "qBittorrent.ini"
        text = ini.read_text(encoding="utf-8")
        self.assertIn("updateCheck=true", text)
        ini.write_text(text.replace("updateCheck=true", "updateCheck=false"), encoding="utf-8")

        self.launch(install_in=3)
        self.qbt.wait_ready()
        time.sleep(15)
        self.assertEqual(self.update_log(), "")
        self.assertEqual((self.program / "qbittorrent-ke.exe").read_bytes(), self.old_exe)

    def test_quitting_before_the_check_is_over_is_not_a_failure(self):
        self.publish({"qbittorrent-ke.exe": self.real_exe})
        self.create_instance()
        self.launch(install_in=6, KETTLE_UPDATE_HEALTHY_SECONDS="300")
        self.qbt.wait_ready()
        self.wait_for_update_log("Starting the new version.")
        self.assertEqual(self.wait_for_api(), "v5.3.0")
        time.sleep(3)
        self.stop_program()  # the user quits the new version well before the 300 seconds are over
        self.wait_for_update_log("Update installed.")
        self.assertEqual((self.program / "qbittorrent-ke.exe").read_bytes(), self.real_exe)

    def test_update_waits_when_the_install_time_is_over(self):
        self.publish({"qbittorrent-ke.exe": self.real_exe})
        self.start(install_in=-3 * 3600)  # three hours ago: the two hours are over, so the next night
        self.wait_for_ready()
        time.sleep(15)
        self.assertEqual((self.program / "qbittorrent-ke.exe").read_bytes(), self.old_exe)
        self.assertEqual(self.update_log(), "")

    # ---- install on start, never on quit ----

    def test_update_is_installed_when_the_program_starts_again_and_not_when_it_quits(self):
        self.publish({"qbittorrent-ke.exe": self.real_exe})
        self.start()
        self.wait_for_ready()
        self.stop_program()
        time.sleep(8)
        self.assertEqual((self.program / "qbittorrent-ke.exe").read_bytes(), self.old_exe, "quitting installed the update")
        self.assertEqual(self.update_log(), "")

        self.launch()
        self.wait_for_update_log("Update installed.")
        self.assertEqual((self.program / "qbittorrent-ke.exe").read_bytes(), self.real_exe)
        self.assertEqual(self.wait_for_api(), "v5.3.0")

    def test_damaged_download_is_discarded_and_the_old_version_starts_again(self):
        self.publish({"qbittorrent-ke.exe": self.real_exe})
        self.start()
        self.wait_for_ready()
        self.stop_program()
        with open(self.update_root / "99.1.0" / "qbittorrent-ke.exe", "r+b") as damaged:
            damaged.seek(1000)
            first = damaged.read(1)
            damaged.seek(1000)
            damaged.write(bytes([first[0] ^ 1]))  # one bit changed after it was checked: same size, other hash

        self.launch()
        self.wait_for_update_log("was not installed: a downloaded file was damaged")
        self.wait_for_log("was not installed: a downloaded file was damaged")
        self.assertEqual((self.program / "qbittorrent-ke.exe").read_bytes(), self.old_exe)
        self.assertFalse((self.data / "kettle-previous").exists())
        self.assertFalse((self.qbt.root / "qBittorrent" / "data-update-backup").exists())
        self.assertFalse((self.update_root / "skipped.txt").exists(), "a damaged download is no reason to skip the version")

    def test_version_that_failed_before_is_not_downloaded_again(self):
        self.create_instance()
        self.update_root.mkdir(parents=True)
        (self.update_root / "skipped.txt").write_text("99.1.0\n", encoding="utf-8")
        self.publish({"qbittorrent-ke.exe": self.real_exe})
        self.launch(install_in=3)
        self.qbt.wait_ready()
        self.wait_for_log("installing it failed earlier")
        time.sleep(5)
        self.assertEqual([r for r in self.server.requests if r.startswith("/download/")], [])
        self.assertEqual(self.update_log(), "")

    # ---- an update that does not work is undone, with the profile ----

    def failed_update(self, mode, *, install_in=6, **hooks):
        self.publish({"qbittorrent-ke.exe": self.fake_exe})
        self.create_instance()
        self.launch(install_in=install_in, KETTLE_FAKE_MODE=mode, KETTLE_FAKE_DATA=str(self.data), **hooks)
        self.qbt.wait_ready()
        self.wait_for_update_log("was undone")

    def test_new_version_that_exits_at_once_is_undone_with_the_profile(self):
        self.failed_update("exit")
        self.assertIn("the new version stopped (exit code 3)", self.update_log())
        self.assert_old_version_is_back()
        # the old version runs again, tells why, and does not try that version again
        self.assertEqual(self.wait_for_api(), "v5.3.0")
        self.wait_for_log("failed and was undone")
        self.wait_for_log("installing it failed earlier")

    def test_new_version_that_crashes_before_it_is_healthy_is_undone(self):
        self.failed_update("crash:2", KETTLE_UPDATE_HEALTHY_SECONDS="60")
        self.assertIn("the new version stopped (exit code 1)", self.update_log())
        self.assert_old_version_is_back()
        self.assertEqual(self.wait_for_api(), "v5.3.0")

    def test_new_version_that_never_reports_is_stopped_and_undone(self):
        self.failed_update("hang", KETTLE_UPDATE_HEALTHY_TIMEOUT="10")
        self.assertIn("did not start properly within 10 seconds", self.update_log())
        self.assert_old_version_is_back()
        self.assertEqual(self.wait_for_api(), "v5.3.0")

    def test_locked_file_undoes_everything_and_the_old_version_runs(self):
        (self.program / "locked.bin").write_bytes(b"old locked file")
        self.publish({"qbittorrent-ke.exe": self.real_exe, "locked.bin": b"new locked file"})
        self.create_instance()
        with open(self.program / "locked.bin", "rb"):  # a file that can't be renamed while it is open
            self.launch(install_in=6, KETTLE_FAKE_DATA=str(self.data))
            self.qbt.wait_ready()
            self.wait_for_update_log("program files could not be replaced", timeout=150)
        self.assertIn("Could not move aside locked.bin", self.update_log())
        # a locked file may pass: the version is tried again, and only skipped when it keeps failing
        self.assert_old_version_is_back(skipped=False)
        self.assertEqual((self.update_root / "failures.txt").read_text(encoding="utf-8").split(), ["99.1.0", "1"])
        self.assertEqual((self.program / "locked.bin").read_bytes(), b"old locked file")
        self.assertEqual(self.wait_for_api(), "v5.3.0")

    def test_version_that_keeps_failing_to_install_is_skipped_the_third_time(self):
        (self.program / "locked.bin").write_bytes(b"old locked file")
        self.publish({"qbittorrent-ke.exe": self.real_exe, "locked.bin": b"new locked file"})
        self.create_instance()
        self.update_root.mkdir(parents=True)
        (self.update_root / "failures.txt").write_text("99.1.0 2\r\n", encoding="utf-8")
        with open(self.program / "locked.bin", "rb"):
            self.launch(install_in=6, KETTLE_FAKE_DATA=str(self.data))
            self.qbt.wait_ready()
            self.wait_for_update_log("program files could not be replaced", timeout=150)
        self.assert_old_version_is_back(skipped=True)
        self.assertEqual(self.wait_for_api(), "v5.3.0")


class HelperStateTests(unittest.TestCase):
    """kettle-update-helper.exe deletes the folders its state file names, so it must refuse a state file that names
    the wrong ones. A good state file gets as far as trying to start the (fake) program, which fails and is undone."""

    def setUp(self):
        self.helper = Path(test_access.NOX).parent / "kettle-update-helper.exe"
        if not self.helper.exists():
            self.skipTest("kettle-update-helper.exe not built")
        self.work = Path(tempfile.mkdtemp(prefix="kettle helper state "))
        self.addCleanup(shutil.rmtree, self.work, True)
        self.addCleanup(self.stop_programs_of_the_test)  # runs before the folder is removed
        (self.work / "program").mkdir()
        (self.work / "program" / "app.exe").write_bytes(b"app")
        (self.work / "cache" / "99.1.0").mkdir(parents=True)
        (self.work / "cache" / "99.1.0" / "app.exe").write_bytes(b"new")
        (self.work / "data").mkdir()
        (self.work / "victim").mkdir()
        (self.work / "victim" / "keep.txt").write_text("keep", encoding="utf-8")

    def stop_programs_of_the_test(self):
        """Ends every process that runs from the throw-away folder (never any other program)."""
        subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                        f"Get-Process | Where-Object {{ $_.Path -and $_.Path.StartsWith('{self.work}', 'OrdinalIgnoreCase') }}"
                        " | Stop-Process -Force"], capture_output=True)

    def run_helper(self, **override):
        w = self.work
        values = {"pid": "4000000", "old": "1.0.0", "new": "99.1.0", "level": "tweak", "notes": "", "healthy_timeout": "5",
                  "program": str(w / "program"), "exe": "app.exe", "update": str(w / "cache" / "99.1.0"),
                  "root": str(w / "cache"), "previous": str(w / "data" / "kettle-previous"),
                  "backup": str(w / "data-update-backup"), "log": str(w / "data" / "update.log"),
                  "profile": str(w / "data"), "file": f"app.exe|3|{hashlib.sha256(b'new').hexdigest()}"}
        values.update(override)
        state = w / "cache" / "state.txt"
        state.write_bytes("".join(f"{key}={value}\r\n" for key, value in values.items()).encode("utf-8"))
        return subprocess.run([str(self.helper), str(state)], timeout=90).returncode

    def assert_nothing_touched(self):
        self.assertEqual((self.work / "program" / "app.exe").read_bytes(), b"app")
        self.assertEqual((self.work / "victim" / "keep.txt").read_text(encoding="utf-8"), "keep")
        self.assertTrue((self.work / "cache" / "99.1.0" / "app.exe").exists())

    def test_a_good_state_file_is_accepted(self):
        # the fake program is no working version, so the helper undoes the swap: exit code 1, the old file is back
        self.assertEqual(self.run_helper(), 1)
        self.assertEqual((self.work / "program" / "app.exe").read_bytes(), b"app")
        self.assertTrue((self.work / "cache" / "result.txt").exists())

    def test_state_files_that_name_the_wrong_places_are_refused(self):
        w = self.work
        for what, override in (
                ("a file that leaves the program folder", {"file": f"..\\escape.txt|3|{hashlib.sha256(b'new').hexdigest()}"}),
                ("a file with a drive", {"file": f"C:\\escape.txt|3|{hashlib.sha256(b'new').hexdigest()}"}),
                ("downloads outside the update folder", {"update": str(w / "victim")}),
                ("the update folder itself", {"update": str(w / "cache")}),
                ("another folder as the old files", {"previous": str(w / "victim")}),
                ("another folder as the backup", {"backup": str(w / "victim")}),
                ("another file as the log", {"log": str(w / "victim" / "keep.txt")}),
                ("a profile that holds the program", {"profile": str(w)}),
                ("a drive as the profile", {"profile": "Z:\\"}),  # a drive that is not there, in case the check fails
                ("an exe with a path", {"exe": "..\\app.exe"})):
            self.assertEqual(self.run_helper(**override), 2, what)
            self.assert_nothing_touched()
            self.assertFalse((w / "escape.txt").exists(), what)


if __name__ == "__main__":
    unittest.main()
