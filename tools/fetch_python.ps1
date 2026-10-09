# Fetches the embeddable Python that WASD's installer bundles to run the
# map-data extractor, and trims it to what the
# extractor needs. Output: build\python\ (git-ignored), which the installer
# copies to <install>\python\.
#
#   .\tools\fetch_python.ps1          download once (cached), verify, unpack, trim
#
# The version and SHA-256 are pinned below, copied from python.org's
# release page; a download that doesn't match is deleted and the script
# fails. To move to a newer Python, change both values together.
#
# Kept: what tools\extract_treasures.py and tools\ds3_archive.py import.
# zlib, struct, json, base64 and re are built into python3XX.dll; ctypes
# (for Windows' bcrypt) needs _ctypes.pyd and libffi. Everything else in the
# package (ssl, sqlite, sockets, ...) is left out. tests\pkg checks both.

$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)

$Version = "3.14.8"
$Sha256 = "a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310"  # python.org, 2026-09-30 release
$Url = "https://www.python.org/ftp/python/$Version/python-$Version-embed-amd64.zip"
$Tag = "python" + ($Version.Split(".")[0..1] -join "")  # python314

$Keep = @(
    "python.exe", "$Tag.dll", "$Tag.zip", "$Tag._pth",
    "_ctypes.pyd", "libffi-8.dll",
    "vcruntime140.dll", "vcruntime140_1.dll",
    "LICENSE.txt"
)

$cache = "build\python-dist"
$zip = Join-Path $cache "python-$Version-embed-amd64.zip"
$out = "build\python"
New-Item -ItemType Directory -Force -Path $cache | Out-Null

function Test-Hash($path) { (Get-FileHash $path -Algorithm SHA256).Hash.ToLower() -eq $Sha256 }

if (-not (Test-Path $zip) -or -not (Test-Hash $zip)) {
    Write-Host "Downloading $Url"
    Invoke-WebRequest -UseBasicParsing $Url -OutFile $zip
    if (-not (Test-Hash $zip)) {
        Remove-Item $zip
        throw "SHA-256 mismatch for $Url -- not using it. Expected $Sha256."
    }
}

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path $zip).Path, (Join-Path (Get-Location) $out))
Get-ChildItem $out -File | Where-Object { $Keep -notcontains $_.Name } | Remove-Item
$missing = $Keep | Where-Object { -not (Test-Path (Join-Path $out $_)) }
if ($missing) { throw "Not in the package: $($missing -join ', ')" }

$size = (Get-ChildItem $out -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("Python {0} ready in {1}: {2} files, {3:N1} MB" -f $Version, $out, $Keep.Count, $size) -ForegroundColor Green
