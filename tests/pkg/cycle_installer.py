"""The installer, installed and uninstalled for real.

Run with `.\\test.ps1 -Only pkg` (not in the default run: it writes to your
user registry and Start menu, then removes everything again). Everything
uses the installer's TestBuild identity -- its own AppId and uninstall
entry, "WASD (test)" names and shortcut, its own single-instance mutex and
data folder -- so a real WASD install is never touched.

Needs Inno Setup 6 (winget install JRSoftware.InnoSetup), build\\WASD.exe,
build\\wasd-cli.exe and build\\python (test.ps1 makes them).
"""
import ctypes
import os
import shutil
import subprocess
import tempfile
import time
import unittest
import winreg

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ISS = os.path.join(ROOT, "installer", "wasd.iss")
TEST_GUID = "{6F0C5E1A-3B7D-4C2E-9A41-7E2B9D0F5C13}"
UNINSTALL_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\%s_is1" % TEST_GUID
SHORTCUT = os.path.join(os.environ.get("APPDATA", ""), r"Microsoft\Windows\Start Menu\Programs", "WASD (test).lnk")
TEST_DATA = os.path.join(os.environ.get("LOCALAPPDATA", ""), "WASD (test)")  # what a test uninstall may remove
FULL_NAME = "WASD: World Awareness & State Display"

EXPECTED_FILES = sorted([
    "LICENSE.txt", "README.txt", "unins000.dat", "unins000.exe",
    r"bin\WASD.exe", r"bin\wasd-cli.exe",
    r"data\bosses.tsv", r"data\key_items.tsv", r"data\missables.tsv", r"data\region_areas.tsv",
    r"data\route.tsv", r"data\THIRD_PARTY_NOTICES.md",
    r"python\libffi-8.dll", r"python\LICENSE.txt", r"python\python.exe", r"python\python314.dll",
    r"python\python314.zip", r"python\python314._pth", r"python\vcruntime140.dll", r"python\vcruntime140_1.dll",
    r"python\_ctypes.pyd",
    r"templates\live.html", r"templates\results_template.html",
    r"tools\ds3_archive.py", r"tools\extract_treasures.py",
])


def iscc():
    for c in (os.path.join(os.environ.get("LOCALAPPDATA", ""), r"Programs\Inno Setup 6\ISCC.exe"),
              os.path.join(os.environ.get("ProgramFiles(x86)", ""), r"Inno Setup 6\ISCC.exe"),
              os.path.join(os.environ.get("ProgramFiles", ""), r"Inno Setup 6\ISCC.exe")):
        if os.path.exists(c):
            return c
    return None


def reg_values(hive=winreg.HKEY_CURRENT_USER, key=UNINSTALL_KEY):
    try:
        with winreg.OpenKey(hive, key) as k:
            out, i = {}, 0
            while True:
                try:
                    name, value, _ = winreg.EnumValue(k, i)
                except OSError:
                    return out
                out[name] = value
                i += 1
    except OSError:
        return None


def files_under(folder):
    return sorted(os.path.relpath(os.path.join(d, f), folder) for d, _, fs in os.walk(folder) for f in fs)


def shortcut_info(path):
    ps = ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{0}'); $s.TargetPath; "
          "$f=(New-Object -ComObject Shell.Application).Namespace('{1}').ParseName('{2}'); "
          "$f.ExtendedProperty('System.AppUserModel.ID')").format(path, os.path.dirname(path), os.path.basename(path))
    out = subprocess.run(["powershell", "-NoProfile", "-Command", "[Console]::OutputEncoding=[Text.Encoding]::UTF8; " + ps],
                         capture_output=True, timeout=60)
    lines = out.stdout.decode("utf-8").splitlines()
    return (lines + ["", ""])[:2]


def wait_until(cond, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.25)
    return cond()


def uninstall(install_dir, *extra):
    """Silent uninstall; returns the uninstaller's exit code once it has finished."""
    r = subprocess.run([os.path.join(install_dir, "unins000.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                        *extra], timeout=120)
    return r.returncode


def remove_leftover_test_install():
    v = reg_values()
    if v and "InstallLocation" in v and os.path.exists(os.path.join(v["InstallLocation"], "unins000.exe")):
        uninstall(v["InstallLocation"], "/REMOVEUSERDATA=1")
        wait_until(lambda: reg_values() is None, 60)
    if reg_values() is not None:  # an entry without its uninstaller: remove the entry itself
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY)
    if os.path.exists(SHORTCUT):
        os.remove(SHORTCUT)
    shutil.rmtree(TEST_DATA, ignore_errors=True)


@unittest.skipUnless(os.name == "nt" and iscc(), "needs Inno Setup 6 (winget install JRSoftware.InnoSetup)")
class InstallerCycleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # WASD_BIN_DIR: package another build's exes (release.ps1: build\release),
        # so the shipped binaries go through this whole cycle too.
        bin_dir = os.environ.get("WASD_BIN_DIR") or os.path.join(ROOT, "build")
        for need in (os.path.join(bin_dir, "WASD.exe"), os.path.join(bin_dir, "wasd-cli.exe"),
                     os.path.join(ROOT, "build", "python", "python.exe")):
            if not os.path.exists(need):
                raise unittest.SkipTest("%s missing: run test.ps1 -Only pkg (it builds and fetches)" % need)
        cls.out = tempfile.mkdtemp(prefix="wasd setups ")
        cls.setup, cls.setup_next = (os.path.join(cls.out, "WASD-test-%s-setup.exe" % v) for v in ("0.1.0", "0.1.1"))
        for extra in ([], ["/DVersionOverride=0.1.1"]):
            r = subprocess.run([iscc(), "/Q", "/DTestBuild", "/DBinDir=" + bin_dir, *extra, "/O" + cls.out, ISS],
                               capture_output=True, text=True)
            if r.returncode != 0:
                raise AssertionError("ISCC failed:\n" + r.stdout + r.stderr)
        remove_leftover_test_install()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="wasd install tést ")
        self.dir = os.path.join(self.tmp, "Programs", "WASD tést (test)")
        self.user = os.path.join(self.tmp, "user data")
        self.app = None

    def tearDown(self):
        if self.app and self.app.poll() is None:
            self.app.kill()
            self.app.wait(timeout=10)
        remove_leftover_test_install()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def install(self, setup, *args):
        r = subprocess.run([setup, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", *args], timeout=180)
        return r.returncode

    def run_installed(self, exe, *args):
        env = dict(os.environ, WASD_USER_DIR=self.user, WASD_INSTANCE="installertest", WASD_NO_BROWSER="1")
        return subprocess.run([os.path.join(self.dir, "bin", exe), *args], capture_output=True, timeout=60, env=env)

    def test_install_upgrade_uninstall(self):
        # -- install ---------------------------------------------------------
        self.assertEqual(self.install(self.setup, "/DIR=" + self.dir), 0)
        self.assertEqual(files_under(self.dir), EXPECTED_FILES)

        v = reg_values()
        self.assertIsNotNone(v, "no uninstall entry under HKCU (what Settings -> Apps lists)")
        self.assertEqual(v["DisplayName"], FULL_NAME + " (test)")
        self.assertEqual(v["DisplayVersion"], "0.1.0")
        self.assertEqual(v["Publisher"], "AmishGoose")
        self.assertEqual(v["DisplayIcon"], os.path.join(self.dir, "bin", "WASD.exe"))
        self.assertEqual(v["InstallLocation"], self.dir + "\\")
        self.assertEqual(v["UninstallString"], '"%s"' % os.path.join(self.dir, "unins000.exe"))
        self.assertTrue(v["QuietUninstallString"].startswith(v["UninstallString"]))
        self.assertGreater(v["EstimatedSize"], 10000)  # KB: ~18 MB with Python
        self.assertEqual((v["NoModify"], v["NoRepair"]), (1, 1))
        self.assertIsNone(reg_values(winreg.HKEY_LOCAL_MACHINE), "an all-users (admin) entry was written")

        self.assertTrue(os.path.exists(SHORTCUT), "no Start menu shortcut")
        target, aumid = shortcut_info(SHORTCUT)
        self.assertEqual(target, os.path.join(self.dir, "bin", "WASD.exe"))
        self.assertEqual(aumid, "AmishGoose.WASD")  # pinned shortcut + running window = one taskbar button

        r = self.run_installed("wasd-cli.exe", "--version")
        self.assertEqual(r.stdout.decode().strip(), FULL_NAME + " 0.1.0")
        r = self.run_installed("WASD.exe", "--smoke-test")
        self.assertEqual(r.returncode, 0, r.stdout.decode())
        r = self.run_installed("wasd-cli.exe", "paths")
        self.assertIn("mode       installed", r.stdout.decode("utf-8"))
        self.assertIn("assets     " + self.dir, r.stdout.decode("utf-8"))

        # -- upgrade: same folder, one entry, your data untouched -------------
        os.makedirs(TEST_DATA, exist_ok=True)
        marker = os.path.join(TEST_DATA, "settings.ini")
        with open(marker, "w") as f:
            f.write("spoiler_tier=Full\n")
        self.assertEqual(self.install(self.setup_next), 0)  # no /DIR: it finds the previous folder itself
        v = reg_values()
        self.assertEqual(v["DisplayVersion"], "0.1.1")
        self.assertEqual(v["InstallLocation"], self.dir + "\\")
        self.assertEqual(files_under(self.dir), EXPECTED_FILES)
        self.assertTrue(os.path.exists(marker), "the upgrade touched your data")

        # -- uninstall while the app runs: refused, nothing half-removed ------
        env = dict(os.environ, WASD_USER_DIR=self.user, WASD_INSTANCE="installertest", WASD_NO_BROWSER="1")
        self.app = subprocess.Popen([os.path.join(self.dir, "bin", "WASD.exe")], env=env)
        u32 = ctypes.WinDLL("user32")
        u32.FindWindowW.restype = ctypes.c_void_p
        self.assertTrue(wait_until(lambda: u32.FindWindowW("WasdMainWindow.installertest", None), 15))
        self.assertNotEqual(uninstall(self.dir), 0, "uninstall went ahead with the app running")
        self.assertEqual(files_under(self.dir), EXPECTED_FILES)
        self.assertIsNotNone(reg_values())
        u32.PostMessageW(ctypes.c_void_p(u32.FindWindowW("WasdMainWindow.installertest", None)), 0x0010, 0, 0)
        self.assertEqual(self.app.wait(timeout=15), 0)

        # -- uninstall: program, shortcut and entry gone; your data kept ------
        self.assertEqual(uninstall(self.dir), 0)
        self.assertTrue(wait_until(lambda: reg_values() is None), "the uninstall entry is still there")
        self.assertTrue(wait_until(lambda: not os.path.exists(self.dir)), "files left: %s" % (
            files_under(self.dir) if os.path.exists(self.dir) else []))
        self.assertFalse(os.path.exists(SHORTCUT), "the Start menu shortcut is still there")
        self.assertTrue(os.path.exists(marker), "a silent uninstall deleted your data")

    def test_remove_user_data_on_request(self):
        self.assertEqual(self.install(self.setup, "/DIR=" + self.dir), 0)
        os.makedirs(os.path.join(TEST_DATA, "sessions"), exist_ok=True)
        with open(os.path.join(TEST_DATA, "sessions", "x.jsonl"), "w") as f:
            f.write("{}\n")
        self.assertEqual(uninstall(self.dir, "/REMOVEUSERDATA=1"), 0)
        self.assertTrue(wait_until(lambda: reg_values() is None))
        self.assertTrue(wait_until(lambda: not os.path.exists(TEST_DATA)), "the data folder wasn't removed")


if __name__ == "__main__":
    unittest.main()
