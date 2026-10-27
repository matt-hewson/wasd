"""Removing the tests' temp folders on Windows.

shutil.rmtree(ignore_errors=True) quietly leaves two kinds of folder
behind: git's object files are read-only, and a file an exe has only just
closed can't be deleted for a moment. This clears the read-only flag and
retries for a few seconds, then says what it couldn't remove.
"""
import os
import shutil
import stat
import sys
import time


def remove_tree(path, attempts=10):
    def make_writable(func, p, exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)

    for k in range(attempts):
        if not os.path.exists(path):
            return
        try:
            shutil.rmtree(path, onexc=make_writable)
            return
        except OSError:
            time.sleep(0.2 * (k + 1))
    if os.path.exists(path):
        print("warning: couldn't remove " + path, file=sys.stderr)
