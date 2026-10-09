"""Static checks on the built exe, run by test.ps1
after a fresh build.ps1: what Windows, Explorer and the installer will read
from build/wasd-cli.exe.

- the version resource matches src/version.h (numbers and strings)
- the embedded manifest asks for asInvoker (never admin), and it's the only one
- the app icon is embedded
- the exe is x64 and imports only Windows' own DLLs: no Visual C++ runtime,
  so it runs on a PC without the Redistributable
- `wasd-cli.exe --version` prints the version

Windows only; skipped elsewhere. Run: python -I -m unittest discover -s tests/pkg -p "test_static*.py"
"""
import ctypes
import os
import re
import struct
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# WASD_BIN_DIR: check another build's exes -- release.ps1 points it at
# build\release, so the shipped binaries get these checks too.
BIN = os.environ.get("WASD_BIN_DIR") or os.path.join(ROOT, "build")
EXE = os.path.join(BIN, "wasd-cli.exe")
APP = os.path.join(BIN, "WASD.exe")
# Both exes come from one object file (build.ps1): the console tool, and the
# app with its window. PE subsystem: 3 console, 2 GUI.
EXES = {"wasd-cli.exe": (EXE, 3), "WASD.exe": (APP, 2)}
IS_WINDOWS = os.name == "nt"


def version_h():
    """The #defines of src/version.h as {name: value}."""
    out = {}
    with open(os.path.join(ROOT, "src", "version.h"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r'#define (WASD_\w+) (?:"([^"]*)"|(\d+))\s*(?://.*)?$', line.strip())
            if m:
                out[m.group(1)] = m.group(2) if m.group(2) is not None else int(m.group(3))
    return out


# ---- PE parsing (imports, machine) -------------------------------------------

def pe_imports(path):
    """(machine, [imported DLL names]) from the PE import and delay-import tables."""
    with open(path, "rb") as f:
        data = f.read()
    (pe,) = struct.unpack_from("<I", data, 0x3C)
    assert data[pe:pe + 4] == b"PE\0\0"
    machine, nsections, _, _, _, opt_size = struct.unpack_from("<HHIIIH", data, pe + 4)
    opt = pe + 24
    (magic,) = struct.unpack_from("<H", data, opt)
    (subsystem,) = struct.unpack_from("<H", data, opt + 68)
    dirs = opt + (112 if magic == 0x20B else 96)
    sections = []
    for i in range(nsections):
        s = opt + opt_size + 40 * i
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((vaddr, max(vsize, rawsize), rawptr))

    def off(rva):
        for vaddr, size, rawptr in sections:
            if vaddr <= rva < vaddr + size:
                return rva - vaddr + rawptr
        raise ValueError("rva %#x not in any section" % rva)

    def cstr(rva):
        o = off(rva)
        return data[o:data.index(b"\0", o)].decode("ascii")

    names = []
    imp_rva, _ = struct.unpack_from("<II", data, dirs + 8 * 1)
    if imp_rva:
        o = off(imp_rva)
        while True:
            name_rva = struct.unpack_from("<5I", data, o)[3]
            if not name_rva:
                break
            names.append(cstr(name_rva))
            o += 20
    delay_rva, _ = struct.unpack_from("<II", data, dirs + 8 * 13)
    if delay_rva:
        o = off(delay_rva)
        while True:
            name_rva = struct.unpack_from("<8I", data, o)[1]
            if not name_rva:
                break
            names.append(cstr(name_rva))
            o += 32
    return machine, names, subsystem


# ---- Win32 resources ---------------------------------------------------------

if IS_WINDOWS:
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    ver = ctypes.WinDLL("version", use_last_error=True)
    k32.LoadLibraryExW.restype = ctypes.c_void_p
    k32.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
    k32.FindResourceW.restype = ctypes.c_void_p
    k32.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    k32.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    k32.LoadResource.restype = ctypes.c_void_p
    k32.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    k32.LockResource.restype = ctypes.c_void_p
    k32.LockResource.argtypes = [ctypes.c_void_p]
    k32.FreeLibrary.argtypes = [ctypes.c_void_p]
    ver.VerQueryValueW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_void_p),
                                   ctypes.POINTER(ctypes.c_uint)]

RT_ICON_GROUP, RT_MANIFEST = 14, 24


def resource(path, rtype, rid):
    """Raw bytes of resource (type, id), or None."""
    h = k32.LoadLibraryExW(path, None, 0x00000002 | 0x00000020)  # AS_DATAFILE | AS_IMAGE_RESOURCE
    if not h:
        raise OSError(ctypes.get_last_error(), "LoadLibraryExW")
    try:
        r = k32.FindResourceW(h, ctypes.c_void_p(rid), ctypes.c_void_p(rtype))
        if not r:
            return None
        size = k32.SizeofResource(h, r)
        p = k32.LockResource(k32.LoadResource(h, r))
        return ctypes.string_at(p, size)
    finally:
        k32.FreeLibrary(h)


def version_info(path):
    """({string name: value}, (file version 4-tuple), (product version 4-tuple))."""
    size = ver.GetFileVersionInfoSizeW(path, None)
    if not size:
        raise OSError(ctypes.get_last_error(), "no version resource")
    buf = ctypes.create_string_buffer(size)
    ver.GetFileVersionInfoW(path, 0, size, buf)
    p, n = ctypes.c_void_p(), ctypes.c_uint()
    ver.VerQueryValueW(buf, "\\", ctypes.byref(p), ctypes.byref(n))
    fixed = struct.unpack_from("<13I", ctypes.string_at(p, n.value))
    fv = (fixed[2] >> 16, fixed[2] & 0xFFFF, fixed[3] >> 16, fixed[3] & 0xFFFF)
    pv = (fixed[4] >> 16, fixed[4] & 0xFFFF, fixed[5] >> 16, fixed[5] & 0xFFFF)
    strings = {}
    for key in ("CompanyName", "ProductName", "ProductVersion", "FileVersion", "FileDescription",
                "LegalCopyright", "OriginalFilename"):
        if ver.VerQueryValueW(buf, "\\StringFileInfo\\040904B0\\" + key, ctypes.byref(p), ctypes.byref(n)) and n.value:
            strings[key] = ctypes.wstring_at(p, n.value).rstrip("\0")
    return strings, fv, pv


@unittest.skipUnless(IS_WINDOWS, "inspects a Windows exe")
class BuiltExeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(EXE) or not os.path.exists(APP):
            raise unittest.SkipTest("build\\wasd-cli.exe / WASD.exe missing: run .\\build.ps1 (test.ps1 does)")
        cls.v = version_h()

    def test_version_resource_matches_version_h(self):
        v = self.v
        nums = (v["WASD_VERSION_MAJOR"], v["WASD_VERSION_MINOR"], v["WASD_VERSION_PATCH"], 0)
        for name, (path, _) in EXES.items():
            with self.subTest(name):
                strings, fv, pv = version_info(path)
                self.assertEqual(fv, nums)
                self.assertEqual(pv, nums)
                self.assertEqual(strings["FileVersion"], v["WASD_VERSION_STRING"])
                self.assertEqual(strings["ProductVersion"], v["WASD_VERSION_STRING"])
                self.assertEqual(strings["ProductName"], v["WASD_APP_NAME"])
                self.assertEqual(strings["CompanyName"], v["WASD_APP_PUBLISHER"])
                self.assertEqual(strings["CompanyName"], "AmishGoose")
                self.assertEqual(strings["LegalCopyright"], v["WASD_APP_COPYRIGHT"])
                self.assertEqual(strings["OriginalFilename"], name)

    def test_subsystem_console_tool_vs_app(self):
        for name, (path, subsystem) in EXES.items():
            with self.subTest(name):
                self.assertEqual(pe_imports(path)[2], subsystem)  # WASD.exe opens no console window

    def test_manifest_is_as_invoker(self):
        for name, (path, _) in EXES.items():
            with self.subTest(name):
                manifest = resource(path, RT_MANIFEST, 1)
                self.assertIsNotNone(manifest, "no RT_MANIFEST resource 1")
                text = manifest.decode("utf-8")
                self.assertIn('level="asInvoker"', text)
                self.assertNotIn("requireAdministrator", text)
                self.assertNotIn("highestAvailable", text)
                self.assertIn('name="Microsoft.Windows.Common-Controls" version="6.0.0.0"', text)  # native controls
                self.assertIsNone(resource(path, RT_MANIFEST, 2), "a second manifest (the linker's default?)")

    def test_manifest_is_well_formed_xml(self):
        # Windows refuses to start an exe whose manifest doesn't parse; the
        # "&" in the app name has to be &amp; there (caught 2026-10-07).
        import xml.dom.minidom
        for name, (path, _) in EXES.items():
            with self.subTest(name):
                doc = xml.dom.minidom.parseString(resource(path, RT_MANIFEST, 1))
                (desc,) = doc.getElementsByTagName("description")
                self.assertEqual(desc.firstChild.data, version_h()["WASD_APP_NAME"])

    def test_icon_is_embedded_at_every_size(self):
        for name, (path, _) in EXES.items():
            with self.subTest(name):
                group = resource(path, RT_ICON_GROUP, 1)
                self.assertIsNotNone(group, "no icon group 1")
                _, _, count = struct.unpack_from("<HHH", group, 0)
                sizes = sorted((struct.unpack_from("<B", group, 6 + 14 * i)[0] or 256) for i in range(count))
                self.assertEqual(sizes, [16, 20, 24, 32, 40, 48, 64, 256])

    def test_x64_and_only_windows_dlls(self):
        system32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
        for name, (path, _) in EXES.items():
            with self.subTest(name):
                machine, dlls, _ = pe_imports(path)
                self.assertEqual(machine, 0x8664, "not an x64 exe")
                self.assertTrue(dlls)
                for dll in dlls:
                    self.assertNotRegex(dll.lower(), r"^(vcruntime|msvcp|ucrtbase|concrt|vccorlib|api-ms-win-crt)",
                                        "%s: the Visual C++ runtime must be linked statically (/MT)" % dll)
                    self.assertTrue(os.path.exists(os.path.join(system32, dll)), "%s isn't a Windows system DLL" % dll)

    def test_version_command(self):
        out = subprocess.run([EXE, "--version"], capture_output=True, text=True, timeout=30)
        self.assertEqual(out.returncode, 0)
        self.assertEqual(out.stdout.strip(), "%s %s" % (self.v["WASD_APP_NAME"], self.v["WASD_VERSION_STRING"]))

    def test_failure_from_a_script_returns_at_once(self):
        # A shared console (this test's): no "Press Enter" wait, so scripts can't hang.
        out = subprocess.run([EXE, "not-a-command"], capture_output=True, text=True, timeout=15,
                             stdin=subprocess.DEVNULL)
        self.assertEqual(out.returncode, 64)
        self.assertIn("unknown mode 'not-a-command'", out.stderr)
        self.assertNotIn("Press Enter", out.stdout)

    def test_failure_when_double_clicked_waits_for_enter(self):
        # Stand-in for Explorer: the exe gets a console of its own (hidden here),
        # so after a failure it keeps the window open until Enter.
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0  # SW_HIDE
        p = subprocess.Popen([EXE, "not-a-command"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, startupinfo=si,
                             creationflags=subprocess.CREATE_NEW_CONSOLE)
        try:
            with self.assertRaises(subprocess.TimeoutExpired):
                p.wait(timeout=2)  # still waiting for Enter
            out, _ = p.communicate("\n", timeout=10)
            self.assertEqual(p.returncode, 64)
            self.assertIn("Press Enter to close this window", out)
        finally:
            if p.poll() is None:
                p.kill()


class PeParserTest(unittest.TestCase):
    """The import reader above, on Python's own executable (any OS with a PE python is Windows)."""

    @unittest.skipUnless(IS_WINDOWS, "needs a Windows PE file")
    def test_reads_pythons_own_imports(self):
        import sys
        machine, dlls, _ = pe_imports(sys.executable)
        self.assertIn(machine, (0x8664, 0x14C, 0xAA64))
        self.assertTrue(any(d.lower().startswith("python") or d.lower() == "kernel32.dll" for d in dlls))


if __name__ == "__main__":
    unittest.main()
