# Compiles and runs the standalone test of one effect engine (no JUCE, a few seconds):
#   Tests\standalone\<Name>_test.cpp  ->  build-standalone\<Name>\<Name>_test.exe
# The test writes its reference data to test_output\standalone\<Name>\ for web\test\standalone.mjs.
#
# Usage:  powershell -ExecutionPolicy Bypass -File Tests\standalone\build.ps1 -Name volume
param([Parameter(Mandatory)][string]$Name)
$ErrorActionPreference = "Stop"

$root   = Split-Path (Split-Path $PSScriptRoot)
$out    = Join-Path $root "build-standalone\$Name"
$source = Join-Path $PSScriptRoot "${Name}_test.cpp"
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
New-Item -ItemType Directory -Force $out | Out-Null

# Same floating-point model and optimisation as the plugin build (/Ox, default /fp:precise).
cmd /c "`"$vcvars`" >nul 2>nul && cl /nologo /std:c++20 /Ox /EHsc /utf-8 /W4 /Fo`"$out\\`" /Fe`"$out\${Name}_test.exe`" `"$source`""
if ($LASTEXITCODE -ne 0) { Write-Host "COMPILE FAILED"; exit 2 }

& "$out\${Name}_test.exe" (Join-Path $root "test_output\standalone\$Name")
exit $LASTEXITCODE
