# Finds vcvars64.bat (the MSVC x64 build environment) for build.ps1 and test.ps1. Dot-source it:
#   . "$PSScriptRoot\tools\find_vcvars.ps1"; $vcvars = Find-Vcvars
# Asks vswhere (installed with every Visual Studio since 2017) for the newest install that has the C++ x64 tools,
# whatever its version or edition; falls back to Visual Studio 2026 Community's default path.
function Find-Vcvars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null | Select-Object -First 1
        if ($vs) {
            $bat = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $bat) { return $bat }
        }
    }
    $fallback = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $fallback) { return $fallback }
    throw "No Visual Studio with the C++ x64 build tools found (vswhere found none, and $fallback is missing). " +
        "Install Visual Studio with the 'Desktop development with C++' workload."
}
