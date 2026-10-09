# Builds a WASD release: the setup file you hand over, and its checksum.
#
#   .\release.ps1                 the real thing -> dist\WASD-<version>-setup.exe (+ .sha256)
#   .\release.ps1 -CheckOnly      only the pre-flight checks (clean tree, version not yet tagged)
#   .\release.ps1 -DryRun -SkipTests -OutDir <folder>
#                                 what tests\pkg\cycle_release.py runs: no pre-flight checks,
#                                 the installer's test identity, output into <folder>
#
# Steps (it stops at the first failure):
#   1. Pre-flight: no uncommitted changes (so the setup file matches a commit),
#      and v<version> not already tagged (so there's one file per version).
#   2. Release build into build\release (build\ and its tracked exes are left alone),
#      and the bundled Python (tools\fetch_python.ps1).
#   3. Every test suite: the default run; -Only pkg with WASD_BIN_DIR=build\release,
#      so the exe checks and the full installer cycle run on the binaries being
#      shipped; -Only realdata (skipped by itself when the game isn't installed).
#   4. The installer from build\release, then its SHA-256 file, checked against it.
#   5. Prints the git tag and push commands. It never tags or pushes by itself.
#
# The version comes from src\version.h (WASD_VERSION_STRING).

param(
    [switch]$CheckOnly,
    [switch]$DryRun,
    [switch]$SkipTests,
    [string]$OutDir = "",
    [string]$RepoDir = ""   # -CheckOnly against another repo (tests use a throwaway one)
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$repo = if ($RepoDir) { (Resolve-Path $RepoDir).Path } else { $PSScriptRoot }

function Fail($message) {
    Write-Host "RELEASE STOPPED: $message" -ForegroundColor Red
    exit 1
}

function Get-Version($root) {
    $line = Select-String -Path (Join-Path $root "src\version.h") -Pattern '^#define WASD_VERSION_STRING "([^"]+)"' |
        Select-Object -First 1
    if (-not $line) { Fail "no WASD_VERSION_STRING in src\version.h" }
    return $line.Matches[0].Groups[1].Value
}

function Invoke-Step($title, [scriptblock]$body) {
    Write-Host "== $title" -ForegroundColor Cyan
    $global:LASTEXITCODE = 0
    & $body
    if ($LASTEXITCODE -ne 0) { Fail "$title (exit $LASTEXITCODE)" }
}

$version = Get-Version $repo
$tag = "v$version"

# ---- 1. pre-flight -------------------------------------------------------
if (-not $DryRun) {
    $ErrorActionPreference = "Continue"  # git writes progress to stderr
    $dirty = git -C $repo status --porcelain
    if ($LASTEXITCODE -ne 0) { Fail "git status failed in $repo" }
    if ($dirty) {
        Write-Host ($dirty | Out-String)
        Fail "uncommitted changes -- commit (or stash) them first, so the setup file matches a commit."
    }
    $existing = git -C $repo tag --list $tag
    if ($existing) { Fail "$tag is already tagged -- bump the version in src\version.h for a new release." }
    $ErrorActionPreference = "Stop"
    Write-Host "Pre-flight OK: clean tree, $tag not tagged yet." -ForegroundColor Green
}
if ($CheckOnly) { exit 0 }

$dist = if ($OutDir) { $OutDir } else { Join-Path $PSScriptRoot "dist" }
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$bin = Join-Path $PSScriptRoot "build\release"

# ---- 2. release build + bundled Python -----------------------------------
Invoke-Step "Release build into build\release" {
    & powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Release -OutDir build\release | Out-Host
}
Invoke-Step "Bundled Python" {
    & powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\fetch_python.ps1 | Out-Host
}

# ---- 3. tests -------------------------------------------------------------
if (-not $SkipTests) {
    Invoke-Step "Tests: default run" { & powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 | Out-Host }
    $env:WASD_BIN_DIR = $bin
    try {
        Invoke-Step "Tests: exe checks and installer cycle on the release binaries" {
            & powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -Only pkg | Out-Host
        }
    } finally {
        Remove-Item Env:\WASD_BIN_DIR -ErrorAction SilentlyContinue
    }
    Invoke-Step "Tests: real data (skipped without the game)" {
        & powershell -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -Only realdata | Out-Host
    }
}

# ---- 4. installer + checksum ---------------------------------------------
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { Fail "Inno Setup 6 not found (winget install JRSoftware.InnoSetup)" }
$isccArgs = @("/Q", "/DBinDir=$bin", "/O$dist")
if ($DryRun) { $isccArgs = @("/DTestBuild") + $isccArgs }
Invoke-Step "Installer" { & $iscc @isccArgs installer\wasd.iss | Out-Host }

$setupName = if ($DryRun) { "WASD-test-$version-setup.exe" } else { "WASD-$version-setup.exe" }
$setup = Join-Path $dist $setupName
if (-not (Test-Path $setup)) { Fail "the installer wasn't written: $setup" }
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
$hashFile = "$setup.sha256"
# The usual "<hash>  <file name>" line (sha256sum's format), ASCII, LF.
[IO.File]::WriteAllText($hashFile, "$hash  $setupName`n", [Text.Encoding]::ASCII)
$check = ([IO.File]::ReadAllText($hashFile) -split '\s+')[0]
if ($check -ne (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()) { Fail "the checksum file doesn't match the installer" }

# ---- 5. summary -----------------------------------------------------------
$mb = (Get-Item $setup).Length / 1MB
Write-Host ""
Write-Host ("Release {0}{1} ready:" -f $version, $(if ($DryRun) { " (dry run, test identity)" } else { "" })) -ForegroundColor Green
Write-Host ("  {0}  ({1:N1} MB)" -f $setup, $mb)
Write-Host "  $hashFile"
Write-Host "  SHA-256 $hash"
if (-not $DryRun) {
    Write-Host ""
    Write-Host "After it has been checked on a second PC (manual checks):"
    Write-Host "  git tag -a $tag -m `"WASD $version`""
    Write-Host "  git push origin $tag"
}
exit 0
