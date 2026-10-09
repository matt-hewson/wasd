"""release.ps1's pre-flight checks, on a throwaway
git repo via -CheckOnly -RepoDir: a clean tree with an untagged version
passes; uncommitted changes, or a version that's already tagged, stop it.
Fast, so it runs by default; the full dry run is tests/pkg/cycle_release.py.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RELEASE = os.path.join(ROOT, "release.ps1")


def have_git():
    return shutil.which("git") is not None


@unittest.skipUnless(os.name == "nt" and have_git(), "needs Windows PowerShell and git")
class PreflightTest(unittest.TestCase):
    def setUp(self):
        self.repo = tempfile.mkdtemp(prefix="wasd release tést ")
        os.makedirs(os.path.join(self.repo, "src"))
        shutil.copy2(os.path.join(ROOT, "src", "version.h"), os.path.join(self.repo, "src", "version.h"))
        with open(os.path.join(self.repo, "notes.txt"), "w") as f:
            f.write("one\n")
        self.git("init", "-q")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "first")

    def tearDown(self):
        shutil.rmtree(self.repo, ignore_errors=True)

    def git(self, *args):
        subprocess.run(["git", "-C", self.repo, "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                        "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *args], check=True, capture_output=True)

    def check(self):
        r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", RELEASE,
                            "-CheckOnly", "-RepoDir", self.repo], capture_output=True, timeout=120)
        return r.returncode, r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace")

    def test_clean_and_untagged_passes(self):
        code, out = self.check()
        self.assertEqual(code, 0, out)
        self.assertIn("Pre-flight OK", out)

    def test_uncommitted_changes_stop_it(self):
        with open(os.path.join(self.repo, "notes.txt"), "a") as f:
            f.write("two\n")
        code, out = self.check()
        self.assertEqual(code, 1, out)
        self.assertIn("uncommitted changes", out)
        os.remove(os.path.join(self.repo, "notes.txt"))  # a deletion counts too
        self.assertEqual(self.check()[0], 1)

    def test_untracked_file_stops_it(self):
        with open(os.path.join(self.repo, "new.txt"), "w") as f:
            f.write("x\n")
        self.assertEqual(self.check()[0], 1)

    def test_existing_tag_stops_it(self):
        version = None
        with open(os.path.join(self.repo, "src", "version.h"), encoding="utf-8") as f:
            for line in f:
                if line.startswith("#define WASD_VERSION_STRING"):
                    version = line.split('"')[1]
        self.git("tag", "v" + version)
        code, out = self.check()
        self.assertEqual(code, 1, out)
        self.assertIn("already tagged", out)

    def test_missing_version_stops_it(self):
        with open(os.path.join(self.repo, "src", "version.h"), "w") as f:
            f.write("// no version here\n")
        self.git("commit", "-q", "-am", "break")
        code, out = self.check()
        self.assertEqual(code, 1, out)
        self.assertIn("no WASD_VERSION_STRING", out)


if __name__ == "__main__":
    unittest.main()
