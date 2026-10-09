# Builds both exes from src\main.cpp with MSVC, with their resources
# (version info, icon, manifest: src\app.rc):
#   <OutDir>\wasd-cli.exe   the command-line tool (console)
#   <OutDir>\WASD.exe       the app: window, overlay, live page (no console)
# main.cpp is compiled once; the two exes differ only in how they're linked
# (subsystem and entry point: wmain vs. wWinMain) and in their resources.
# Run from a normal PowerShell prompt:
#   .\build.ps1                              debug-friendly build into build\ (as before)
#   .\build.ps1 -Release                     optimised (/O2)
#   .\build.ps1 -Release -OutDir build\release   what release.ps1 does, leaving build\ alone
# The exes find data\ and templates\ one folder up, so OutDir must be one
# folder below the repo root for them to run from there (build\release is
# two below: release.ps1 only packages those, it doesn't run them in place).
# Both link the C++ runtime statically (cl's default /MT), so neither needs
# a Visual C++ Redistributable; tests\pkg checks that it stays that way.

param([switch]$Release, [string]$OutDir = "build")

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

. "$PSScriptRoot\tools\find_vcvars.ps1"
$vcvars = Find-Vcvars

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$opt = if ($Release) { "/O2 /DNDEBUG" } else { "" }
# vcvars' stderr goes to nul too: it prints a harmless "'vswhere.exe' is not
# recognized" that looked like a failure (papercuts, 2026-10-07).
# /MANIFEST:NO: the manifest comes from app.rc instead of the linker's default.
$o = $OutDir.TrimEnd('\')
$link = "link /nologo `"$o\main.obj`" /MANIFEST:NO"
$cmd = "call `"$vcvars`" >nul 2>nul" +
       " && cl /nologo /c /EHsc /std:c++17 /W4 /utf-8 $opt `"/Fo:$o\main.obj`" src\main.cpp" +
       " && rc /nologo /fo `"$o\app.res`" src\app.rc" +
       " && rc /nologo /d WASD_GUI /fo `"$o\app-gui.res`" src\app.rc" +
       " && $link `"$o\app.res`" `"/OUT:$o\wasd-cli.exe`" /SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup" +
       " && $link `"$o\app-gui.res`" `"/OUT:$o\WASD.exe`" /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup"
cmd /c $cmd

if ($LASTEXITCODE -ne 0) {
    throw "Build failed (exit $LASTEXITCODE)"
}

$kind = if ($Release) { "release" } else { "debug" }
Write-Host "Build OK ($kind) -> $o\wasd-cli.exe, $o\WASD.exe" -ForegroundColor Green
