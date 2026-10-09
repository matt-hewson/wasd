# Runs every automated test. No game needed.
#   .\test.ps1              all suites
#   .\test.ps1 -Only cpp    one suite: cpp, js, py, pkg or realdata
#   .\test.ps1 -Filter route   C++ tests whose name contains "route"
#
# cpp: builds build\wasd-tests.exe (src\main.cpp without its entry point plus
#      tests\cpp\*) with MSVC and runs it. Windows only.
# js:  node --test on tests\js (the live page and results page scripts).
# py:  python unittest on tests\py (the map-file tools).
# pkg: rebuilds build\wasd-cli.exe with build.ps1 and fetches the bundled
#      Python (tools\fetch_python.ps1: downloaded once, cached, SHA-256
#      checked), then checks what
#      Windows and the installer read from it, and runs it (tests\pkg\test_*.py:
#      version resource, manifest, icon, DLL imports, --version, double-click
#      behaviour; where it reads and writes, incl. a simulated installed
#      layout; the trimmed Python and `extract` there; WASD.exe's window).
#      Windows only. With -Only pkg it also runs the installer cycle
#      (tests\pkg\cycle_installer.py: a real install, upgrade and uninstall under
#      a test identity; needs Inno Setup 6) and a release.ps1 dry run
#      (cycle_release.py). Not in "all": the cycle writes to your user registry
#      and Start menu while it runs, and the dry run takes a minute or two.
#      WASD_BIN_DIR points the exe checks and the cycle at another build
#      (release.ps1 uses build\release).
# realdata: not part of "all". The bundled and the dev Python run the map
#      extractor on your real DS3 install; the output must be identical
#      (tests\realdata). Skipped when the game isn't found.
# Exits non-zero if any suite fails. Native output goes through Out-Host: a
# PowerShell function otherwise returns it, which would swallow the results.

param(
    [ValidateSet("all", "cpp", "js", "py", "pkg", "realdata")] [string]$Only = "all",
    [string]$Filter = ""
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$failed = @()

function Run-Cpp {
    $ErrorActionPreference = "Continue"  # judge native tools by exit code, not stderr
    . "$PSScriptRoot\tools\find_vcvars.ps1"
    $vcvars = Find-Vcvars
    New-Item -ItemType Directory -Force -Path "build\tests" | Out-Null
    Write-Host "== C++: building build\wasd-tests.exe" -ForegroundColor Cyan
    $cmd = "call `"$vcvars`" >nul 2>nul && cl /nologo /EHsc /std:c++17 /W4 /utf-8 /Fo:build\tests\ /Fe:build\wasd-tests.exe tests\cpp\run_tests.cpp /link /SUBSYSTEM:CONSOLE"
    cmd /c $cmd | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "C++ test build FAILED" -ForegroundColor Red; return $false }
    Write-Host "== C++: running" -ForegroundColor Cyan
    if ($Filter) { & .\build\wasd-tests.exe $Filter | Out-Host } else { & .\build\wasd-tests.exe | Out-Host }
    return $LASTEXITCODE -eq 0
}

function Run-Js {
    $ErrorActionPreference = "Continue"
    Write-Host "== JS: node --test tests\js" -ForegroundColor Cyan
    node --test "tests/js/*.test.mjs" | Out-Host
    return $LASTEXITCODE -eq 0
}

function Run-Py {
    $ErrorActionPreference = "Continue"
    Write-Host "== Python: unittest tests\py" -ForegroundColor Cyan
    # unittest reports on stderr; no 2>&1 here -- in Windows PowerShell 5.1 that turns
    # each line into an error record, which stops the script.
    python -I -W error::ResourceWarning -m unittest discover -s tests/py -v
    return $LASTEXITCODE -eq 0
}

function Run-Pkg {
    $ErrorActionPreference = "Continue"
    Write-Host "== Packaging: rebuilding build\wasd-cli.exe" -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "Build FAILED" -ForegroundColor Red; return $false }
    Write-Host "== Packaging: bundled Python (tools\fetch_python.ps1)" -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\fetch_python.ps1 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "Couldn't fetch Python (offline?): its tests will be skipped" -ForegroundColor Yellow }
    Write-Host "== Packaging: exe checks (tests\pkg)" -ForegroundColor Cyan
    python -I -W error::ResourceWarning -m unittest discover -s tests/pkg -p "test_*.py" -v
    $ok = $LASTEXITCODE -eq 0
    if ($Only -eq "pkg") {
        # Only when asked for: installs and uninstalls for real (under a test
        # identity), writing to your user registry and Start menu meanwhile.
        Write-Host "== Packaging: installer cycle (tests\pkg\cycle_installer.py)" -ForegroundColor Cyan
        python -I -W error::ResourceWarning -m unittest discover -s tests/pkg -p "cycle_*.py" -v
        $ok = $ok -and ($LASTEXITCODE -eq 0)
    }
    return $ok
}

if ($Only -in "all", "cpp") { if (-not (Run-Cpp)) { $failed += "cpp" } }
if ($Only -in "all", "js")  { if (-not (Run-Js))  { $failed += "js" } }
if ($Only -in "all", "py")  { if (-not (Run-Py))  { $failed += "py" } }
if ($Only -in "all", "pkg") { if (-not (Run-Pkg)) { $failed += "pkg" } }
function Run-RealData {
    $ErrorActionPreference = "Continue"
    & powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\fetch_python.ps1 | Out-Host
    Write-Host "== Real data: bundled vs. dev Python on your DS3 install (tests\realdata)" -ForegroundColor Cyan
    python -I -m unittest discover -s tests/realdata -v
    return $LASTEXITCODE -eq 0
}
if ($Only -eq "realdata") { if (-not (Run-RealData)) { $failed += "realdata" } }

if ($failed.Count) {
    Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "All tests passed." -ForegroundColor Green
