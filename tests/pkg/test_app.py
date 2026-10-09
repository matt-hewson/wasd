"""The app (build\\WASD.exe) on the real exe.

- `WASD.exe --smoke-test`: the window comes up as an app window with a
  taskbar button and the AmishGoose.WASD identity, finds its assets, then
  closes -- in the dev build and in a simulated installed layout.
- Running it for real, under a test identity (WASD_INSTANCE) with its own
  data folder and no browser tab: the window appears, a second launch exits
  at once with code 3 and leaves the first running, closing the window ends
  the app cleanly, and the day's log file records the run.

If DS3 with Seamless Co-op happens to be running, the test copy attaches
to it read-only, as the real app would. Windows only.
"""
import ctypes
import ctypes.wintypes as wt
import os
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "build", "WASD.exe")
CLI = os.path.join(ROOT, "build", "wasd-cli.exe")
WM_CLOSE = 0x0010


def find_window(cls, timeout=10.0):
    u32 = ctypes.WinDLL("user32")
    u32.FindWindowW.restype = wt.HWND
    end = time.time() + timeout
    while time.time() < end:
        hwnd = u32.FindWindowW(cls, None)
        if hwnd:
            return hwnd
        time.sleep(0.1)
    return None


def smoke(exe, env=None):
    r = subprocess.run([exe, "--smoke-test"], capture_output=True, text=True, timeout=30, env=env)
    return r.returncode, r.stdout


@unittest.skipUnless(os.name == "nt" and os.path.exists(APP), "needs build\\WASD.exe on Windows")
class SmokeTest(unittest.TestCase):
    def test_dev_build(self):
        code, out = smoke(APP)
        self.assertEqual(code, 0, out)
        for check in ("window shown", "taskbar button", "AppUserModelID AmishGoose.WASD", "title", "assets found",
                      "controls"):
            self.assertRegex(out, r"(?m)^ok\s+" + check, out)

    def test_installed_layout(self):
        tmp = tempfile.mkdtemp(prefix="wasd app tést ")
        try:
            app = os.path.join(tmp, "Programs", "WASD")
            os.makedirs(os.path.join(app, "bin"))
            shutil.copy2(APP, os.path.join(app, "bin", "WASD.exe"))
            shutil.copytree(os.path.join(ROOT, "templates"), os.path.join(app, "templates"))
            shutil.copytree(os.path.join(ROOT, "data"), os.path.join(app, "data"), ignore=shutil.ignore_patterns("generated"))
            env = dict(os.environ, LOCALAPPDATA=os.path.join(tmp, "Local"))
            env.pop("WASD_USER_DIR", None)
            code, out = smoke(os.path.join(app, "bin", "WASD.exe"), env)
            self.assertEqual(code, 0, out)
            self.assertRegex(out, r"(?m)^ok\s+assets found")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


@unittest.skipUnless(os.name == "nt" and os.path.exists(APP), "needs build\\WASD.exe on Windows")
class RunningAppTest(unittest.TestCase):
    def setUp(self):
        self.user = tempfile.mkdtemp(prefix="wasd app run ")
        self.instance = "test%d" % os.getpid()
        self.env = dict(os.environ, WASD_INSTANCE=self.instance, WASD_NO_BROWSER="1", WASD_USER_DIR=self.user)
        self.proc = None

    def tearDown(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=10)
        shutil.rmtree(self.user, ignore_errors=True)

    def start(self):
        return subprocess.Popen([APP], env=self.env)

    def test_one_instance_and_a_clean_close(self):
        self.proc = self.start()
        hwnd = find_window("WasdMainWindow." + self.instance)
        self.assertTrue(hwnd, "the app window didn't appear")

        second = subprocess.run([APP], env=self.env, timeout=15)
        self.assertEqual(second.returncode, 3, "a second launch should hand over to the first and exit")
        self.assertIsNone(self.proc.poll(), "the first copy stopped")

        ctypes.WinDLL("user32").PostMessageW(hwnd, WM_CLOSE, 0, 0)  # the window's X button
        self.assertEqual(self.proc.wait(timeout=10), 0)

        logs = os.listdir(os.path.join(self.user, "logs"))
        self.assertEqual(len(logs), 1, logs)
        self.assertRegex(logs[0], r"^wasd-\d{8}\.log$")
        with open(os.path.join(self.user, "logs", logs[0]), encoding="utf-8", errors="replace") as f:
            log = f.read()
        for line in ("WASD: World Awareness & State Display 0.1.0", "Your data: " + self.user,
                     "Live page: http://localhost:", "Quit from the WASD window.", "Closed."):
            self.assertIn(line, log)

    def test_another_identity_runs_alongside(self):
        # A test copy never meets a real one: a different WASD_INSTANCE is a separate app.
        self.proc = self.start()
        self.assertTrue(find_window("WasdMainWindow." + self.instance))
        other_env = dict(self.env, WASD_INSTANCE=self.instance + "b")
        other = subprocess.Popen([APP], env=other_env)
        try:
            hwnd = find_window("WasdMainWindow." + self.instance + "b")
            self.assertTrue(hwnd, "the second identity's window didn't appear")
            ctypes.WinDLL("user32").PostMessageW(hwnd, WM_CLOSE, 0, 0)
            self.assertEqual(other.wait(timeout=10), 0)
        finally:
            if other.poll() is None:
                other.kill()
        ctypes.WinDLL("user32").PostMessageW(find_window("WasdMainWindow." + self.instance), WM_CLOSE, 0, 0)
        self.assertEqual(self.proc.wait(timeout=10), 0)


if __name__ == "__main__":
    unittest.main()
