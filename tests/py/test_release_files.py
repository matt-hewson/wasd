"""What goes into a release: data/ holds only our own files, the old copied data is gone, and the licence is in place.
Run: python -I -m unittest discover -s tests/py
"""
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# data/: our own guidance tables and the notices. generated/ is the user's own extract (git-ignored, never shipped).
EXPECTED_DATA = {"bosses.tsv", "key_items.tsv", "missables.tsv", "region_areas.tsv", "route.tsv",
                 "THIRD_PARTY_NOTICES.md"}
# Files earlier versions had, now removed: must not come back.
REMOVED = ["data/item_names.tsv", "data/region_names.tsv", "data/bonfire_names.tsv", "data/LICENSE-AGPL-3.0.txt",
           "data/area_levels.tsv", "build/captures/getinventoryitem.bin"]


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


class DataFolderTest(unittest.TestCase):
    def test_data_holds_only_our_own_files(self):
        found = {n for n in os.listdir(os.path.join(ROOT, "data")) if n != "generated"}
        self.assertEqual(found, EXPECTED_DATA, "data/ changed: update EXPECTED_DATA, the installer and the notices")

    def test_old_copied_data_is_gone(self):
        for path in REMOVED:
            self.assertFalse(os.path.exists(os.path.join(ROOT, path)), path)

    def test_captures_are_not_kept_in_git(self):
        self.assertNotIn("!build/captures/", read(".gitignore"))


class LicenceTest(unittest.TestCase):
    def test_licence_is_the_gpl_3_text(self):
        text = read("LICENSE")
        self.assertTrue(text.lstrip().startswith("GNU GENERAL PUBLIC LICENSE"))
        self.assertIn("Version 3, 29 June 2007", text)
        self.assertEqual(len(text.splitlines()), 674)

    def test_notices_state_the_licence_and_credit_the_archive_keys(self):
        notices = read("data/THIRD_PARTY_NOTICES.md")
        self.assertIn("GPL-3.0-or-later", notices)
        self.assertIn("Copyright (c) 2015 Atvaark", notices)  # BinderTool's MIT notice travels with its keys


@unittest.skipUnless(os.path.exists(os.path.join(ROOT, ".publicignore")), "the public copy has no private files")
class PublicFilesTest(unittest.TestCase):
    """In the development copy: no file that gets published mentions a private document or place (the export
    script's own rule). Code comments explain themselves; plans stay in the private documents."""

    def test_published_files_mention_nothing_private(self):
        import subprocess
        sys_path = os.path.join(ROOT, "tools")
        import sys
        sys.path.insert(0, sys_path)
        try:
            import export_public as ep
        finally:
            sys.path.remove(sys_path)
        ignore = ep.read_ignore(ROOT)
        tracked = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
        files = [f for f in tracked.splitlines() if not ep.is_private(f, ignore) and os.path.exists(os.path.join(ROOT, f))]

        def read(f):
            with open(os.path.join(ROOT, f), "rb") as fh:
                return fh.read()

        found = ep.leaks(files, read, ignore)
        self.assertEqual(found, [], "\n".join("%s:%d: %s" % x for x in found))

    def test_private_list_matches_paths_and_folders(self):
        import sys
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        try:
            import export_public as ep
        finally:
            sys.path.pop(0)
        ignore = ["PLANS.md", "tests/fixture/"]  # made-up names: real ones here would be leaks themselves
        self.assertTrue(ep.is_private("PLANS.md", ignore))
        self.assertTrue(ep.is_private("tests/fixture/test_x.py", ignore))
        self.assertFalse(ep.is_private("tests/py/test_tools.py", ignore))
        rx = ep.private_pattern(ignore)
        self.assertTrue(rx.search("see PLANS.md 2.3"))
        self.assertTrue(rx.search(r"run tests\fixture"))
        self.assertFalse(rx.search("a fixture of the tests"))  # a folder's last word alone isn't a mention
        self.assertFalse(rx.search("see docs/TECHNICAL.md"))


class InstallerTest(unittest.TestCase):
    def test_installer_ships_the_licence_and_no_removed_file(self):
        iss = read("installer/wasd.iss")
        files = iss[iss.index("[Files]"):iss.index("[Icons]")]
        self.assertRegex(files, r'Source: "\{#Root\}\\LICENSE";.*DestName: "LICENSE.txt"')
        self.assertIn("THIRD_PARTY_NOTICES.md", files)
        for path in REMOVED:
            self.assertNotIn(os.path.basename(path), files)

    def test_upgrades_delete_the_removed_files(self):
        iss = read("installer/wasd.iss")
        deletes = iss[iss.index("[InstallDelete]"):iss.index("[UninstallDelete]")]
        for path in REMOVED:
            if path.startswith("data/"):
                self.assertRegex(deletes, r'Type: files; Name: "\{app\}\\data\\%s"' % re.escape(os.path.basename(path)))


if __name__ == "__main__":
    unittest.main()
