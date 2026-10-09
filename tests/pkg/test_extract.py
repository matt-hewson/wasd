"""Map data with the bundled Python, on the real exe.

- The trimmed embeddable Python (build\\python, from tools\\fetch_python.ps1)
  has what the extractor imports and not what was cut.
- In a simulated installed layout (bin\\, data\\, templates\\, python\\,
  tools\\), `wasd-cli.exe extract --game <synthetic game>` writes the map
  data into %LOCALAPPDATA%\\WASD\\generated, remembers the folder, and
  writes nothing into the program folder.
- No Python, or no game: an error code and a message, at once.

The synthetic game (tests/shared/ds3_fixtures.py) is signed with a toy key,
passed through the test-only WASD_RSA_KEYS_FILE.
Windows only; needs build\\wasd-cli.exe and build\\python (test.ps1 makes both).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(ROOT, "build", "wasd-cli.exe")
PYTHON_DIR = os.path.join(ROOT, "build", "python")
sys.path.insert(0, os.path.join(ROOT, "tests", "shared"))
import ds3_fixtures  # noqa: E402
from test_paths import files_under, run  # noqa: E402

HAVE_PYTHON = os.path.exists(os.path.join(PYTHON_DIR, "python.exe"))


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq DarkSoulsIII.exe", "/NH"], capture_output=True, text=True)
    return "DarkSoulsIII.exe" in out.stdout


@unittest.skipUnless(os.name == "nt" and HAVE_PYTHON, "needs build\\python (tools\\fetch_python.ps1)")
class TrimmedPythonTest(unittest.TestCase):
    PY = os.path.join(PYTHON_DIR, "python.exe")

    def py(self, *args):
        return subprocess.run([self.PY, "-I", "-B", *args], capture_output=True, text=True, timeout=60)

    def test_has_what_the_extractor_imports(self):
        out = self.py("-c", "import argparse, base64, ctypes, json, os, re, struct, sys, zlib; "
                            "ctypes.WinDLL('bcrypt'); print('ok')")
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(out.stdout.strip(), "ok")

    def test_what_was_cut_is_gone(self):
        for module in ("ssl", "sqlite3", "socket", "lzma", "bz2", "_hashlib", "unicodedata", "asyncio"):
            out = self.py("-c", "import %s" % module)
            self.assertNotEqual(out.returncode, 0, "%s is still importable" % module)

    def test_runs_the_extractor(self):
        out = self.py(os.path.join(ROOT, "tools", "extract_treasures.py"), "--help")
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("--out", out.stdout)

    def test_files(self):
        self.assertEqual(sorted(os.listdir(PYTHON_DIR)), sorted([
            "python.exe", "python314.dll", "python314.zip", "python314._pth", "_ctypes.pyd", "libffi-8.dll",
            "vcruntime140.dll", "vcruntime140_1.dll", "LICENSE.txt"]))


@unittest.skipUnless(os.name == "nt" and os.path.exists(EXE) and HAVE_PYTHON,
                     "needs build\\wasd-cli.exe and build\\python on Windows")
class InstalledExtractTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="wasd extract tést ")
        self.app = os.path.join(self.tmp, "Programs", "WASD")
        self.local = os.path.join(self.tmp, "Local")
        os.makedirs(os.path.join(self.app, "bin"))
        os.makedirs(os.path.join(self.app, "tools"))
        os.makedirs(self.local)
        shutil.copy2(EXE, os.path.join(self.app, "bin", "wasd-cli.exe"))
        shutil.copytree(os.path.join(ROOT, "templates"), os.path.join(self.app, "templates"))
        shutil.copytree(os.path.join(ROOT, "data"), os.path.join(self.app, "data"),
                        ignore=shutil.ignore_patterns("generated"))
        shutil.copytree(PYTHON_DIR, os.path.join(self.app, "python"))
        for f in ("ds3_archive.py", "extract_treasures.py"):
            shutil.copy2(os.path.join(ROOT, "tools", f), os.path.join(self.app, "tools", f))
        self.game = os.path.join(self.tmp, "Steam Library", "DARK SOULS III", "Game")
        keys = ds3_fixtures.make_game(self.game)
        self.exe = os.path.join(self.app, "bin", "wasd-cli.exe")
        self.env = {"WASD_USER_DIR": None, "LOCALAPPDATA": self.local, "WASD_RSA_KEYS_FILE": keys}
        self.generated = os.path.join(self.local, "WASD", "generated")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_extract_writes_map_data_into_your_data_folder(self):
        program_files = files_under(self.app)
        code, out, err = run([self.exe, "extract", "--game", self.game], self.env, cwd=self.tmp)
        self.assertEqual(code, 0, out + err)
        self.assertEqual(ds3_fixtures.read_tsv(os.path.join(self.generated, "treasures.tsv")),
                         ds3_fixtures.EXPECTED_TREASURES)
        self.assertEqual(ds3_fixtures.read_tsv(os.path.join(self.generated, "enemies.tsv")),
                         ds3_fixtures.EXPECTED_ENEMIES)
        self.assertEqual(files_under(self.app), program_files, "something was written into the program folder")

    def test_extract_remembers_the_game_folder(self):
        code, out, err = run([self.exe, "extract", "--game", self.game], self.env)
        self.assertEqual(code, 0, out + err)
        with open(os.path.join(self.local, "WASD", "settings.ini"), encoding="utf-8") as f:
            self.assertIn("game_dir=" + self.game, f.read().splitlines())
        # Next time no --game is needed: the saved folder comes before Steam. A
        # running DS3 comes first of all (by design), so that half needs the game closed.
        if game_running():
            self.skipTest("DS3 is running, and the running game is looked at before the saved folder")
        os.remove(os.path.join(self.generated, "treasures.tsv"))
        code, out, err = run([self.exe, "extract"], self.env)
        self.assertEqual(code, 0, out + err)
        self.assertIn("from settings.ini", out)
        self.assertTrue(os.path.exists(os.path.join(self.generated, "treasures.tsv")))

    def test_missing_game_folder(self):
        code, out, err = run([self.exe, "extract", "--game", os.path.join(self.tmp, "nope")], self.env)
        self.assertEqual(code, 2)
        self.assertIn("Couldn't find Dark Souls III", out)
        self.assertFalse(os.path.exists(self.generated))

    def test_missing_bundled_python(self):
        shutil.rmtree(os.path.join(self.app, "python"))
        code, out, err = run([self.exe, "extract", "--game", self.game], self.env)
        self.assertEqual(code, 5)
        self.assertIn("bundled Python is missing", out)

    def test_bad_archive_reports_the_extractor_error(self):
        with open(os.path.join(self.game, "Data5.bhd"), "wb") as f:
            f.write(b"\0" * 27)
        code, out, err = run([self.exe, "extract", "--game", self.game], self.env)
        self.assertEqual(code, 4)
        self.assertIn("exit code 4", out)


if __name__ == "__main__":
    unittest.main()
