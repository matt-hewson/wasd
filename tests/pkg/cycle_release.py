"""release.ps1 end to end as a dry run:
`release.ps1 -DryRun -SkipTests -OutDir <temp>` -- a release build into
build\\release, the installer with the test identity, and its checksum file.

Runs with `.\\test.ps1 -Only pkg` (about a minute and a half: an optimised
compile). The pre-flight checks are tests/pkg/test_release.py.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cycle_installer import iscc  # noqa: E402
from test_static import version_h, version_info  # noqa: E402


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


@unittest.skipUnless(os.name == "nt" and iscc(), "needs Inno Setup 6")
class ReleaseDryRunTest(unittest.TestCase):
    def test_dry_run(self):
        out = tempfile.mkdtemp(prefix="wasd release dry ")
        dev_exes = {f: sha256(os.path.join(ROOT, "build", f)) for f in ("WASD.exe", "wasd-cli.exe")
                   if os.path.exists(os.path.join(ROOT, "build", f))}
        try:
            r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                os.path.join(ROOT, "release.ps1"), "-DryRun", "-SkipTests", "-OutDir", out],
                               capture_output=True, timeout=900)
            log = r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace")
            self.assertEqual(r.returncode, 0, log)

            version = version_h()["WASD_VERSION_STRING"]
            setup = os.path.join(out, "WASD-test-%s-setup.exe" % version)
            self.assertEqual(sorted(os.listdir(out)), sorted([os.path.basename(setup), os.path.basename(setup) + ".sha256"]))
            with open(setup + ".sha256", "rb") as f:
                line = f.read()
            self.assertEqual(line, ("%s  %s\n" % (sha256(setup), os.path.basename(setup))).encode("ascii"))
            self.assertIn("SHA-256 " + sha256(setup), log)

            release = os.path.join(ROOT, "build", "release")
            for exe in ("WASD.exe", "wasd-cli.exe"):
                path = os.path.join(release, exe)
                strings, _, _ = version_info(path)
                self.assertEqual(strings["ProductVersion"], version)
                if os.path.exists(os.path.join(ROOT, "build", exe)):  # /O2: smaller than the debug build
                    self.assertLess(os.path.getsize(path), os.path.getsize(os.path.join(ROOT, "build", exe)))
            for f, h in dev_exes.items():  # the dev exes in build are left alone
                self.assertEqual(sha256(os.path.join(ROOT, "build", f)), h, "release.ps1 changed build\\" + f)
            self.assertNotIn("git tag", log)  # a dry run never suggests tagging
        finally:
            shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
