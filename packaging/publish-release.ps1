# Puts this version's installer and portable zip (from dist\, built by build-windows.ps1) on GitHub Releases:
#   https://github.com/<your-account>/guitar-pedalboard/releases
# Creates release v<version> with packaging\release-notes.md as its text, or replaces the files of an existing one.
#
# Usage:  powershell -ExecutionPolicy Bypass -File packaging\publish-release.ps1
param([string]$Repo = "guitar-pedalboard")
$ErrorActionPreference = "Continue" # native tools report through exit codes, checked below

$root = Split-Path $PSScriptRoot
$gh = Join-Path $root "tools\gh\bin\gh.exe"
$version = (Select-String -Path (Join-Path $PSScriptRoot "build-windows.ps1") -Pattern '^\$version\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value
$tag = "v$version"

$files = @("GuitarPedalboard-$version-Setup.exe", "GuitarPedalboard-$version-Windows-Portable.zip") | ForEach-Object { Join-Path $root "dist\$_" }
foreach ($f in $files) {
    if (-not (Test-Path $f)) { Write-Host "missing $f - run packaging\build-windows.ps1 first" -ForegroundColor Red; exit 1 }
}

& $gh auth status *> $null
if ($LASTEXITCODE -ne 0) { Write-Host "Not logged in to GitHub. Run:  tools\gh\bin\gh.exe auth login --web" -ForegroundColor Red; exit 1 }
$owner = (& $gh api user --jq .login).Trim()
$full = "$owner/$Repo"

& $gh release view $tag --repo $full *> $null
if ($LASTEXITCODE -eq 0) {
    Write-Host "== Replacing the files of release $tag" -ForegroundColor Cyan
    & $gh release upload $tag @files --repo $full --clobber
} else {
    Write-Host "== Creating release $tag" -ForegroundColor Cyan
    & $gh release create $tag @files --repo $full --title "Guitar Pedalboard $version" --notes-file (Join-Path $PSScriptRoot "release-notes.md")
}
if ($LASTEXITCODE -ne 0) { Write-Host "release upload failed" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "Published: https://github.com/$full/releases/tag/$tag" -ForegroundColor Green
