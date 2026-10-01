# Builds the Release app + plugin, then packages them into dist\:
#   GuitarPedalboard-<version>-Setup.exe             (installer, Inno Setup)
#   GuitarPedalboard-<version>-Windows-Portable.zip  (no install needed)
#
# Usage:  powershell -ExecutionPolicy Bypass -File packaging\build-windows.ps1
$ErrorActionPreference = "Stop"

$root    = Split-Path $PSScriptRoot
$version = "1.2.1"
$cmake   = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$iscc    = Join-Path $root "tools\InnoSetup6\ISCC.exe"
$release = Join-Path $root "build\GuitarPedalboard_artefacts\Release"
$dist    = Join-Path $root "dist"

Write-Host "== Building Release" -ForegroundColor Cyan
& $cmake -S $root -B (Join-Path $root "build") | Out-Null
& $cmake --build (Join-Path $root "build") --config Release --target GuitarPedalboard_VST3 GuitarPedalboard_Standalone --parallel -- /v:minimal
if ($LASTEXITCODE -ne 0) { throw "build failed" }

New-Item -ItemType Directory -Force $dist | Out-Null

Write-Host "== Portable zip" -ForegroundColor Cyan
$stage = Join-Path $dist "GuitarPedalboard-$version-Windows-Portable"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage "VST3") | Out-Null
Copy-Item (Join-Path $release "Standalone\Guitar Pedalboard.exe") $stage
Copy-Item -Recurse (Join-Path $release "VST3\Guitar Pedalboard.vst3") (Join-Path $stage "VST3")
Copy-Item (Join-Path $PSScriptRoot "QuickStart-zh-TW.txt") $stage
Copy-Item (Join-Path $PSScriptRoot "Install-VST3.cmd") $stage
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
$zip = "$stage.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Remove-Item -Recurse -Force $stage

Write-Host "== Installer" -ForegroundColor Cyan
& $iscc /Q (Join-Path $PSScriptRoot "GuitarPedalboard.iss")
if ($LASTEXITCODE -ne 0) { throw "installer build failed" }

Get-ChildItem $dist -File | Select-Object Name, @{ n = "MB"; e = { [math]::Round($_.Length / 1MB, 1) } } | Format-Table -AutoSize
