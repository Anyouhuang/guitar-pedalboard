@echo off
rem Copies the VST3 plugin into the standard Windows VST3 folder (needs administrator rights).

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Asking for administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "SRC=%~dp0VST3\Guitar Pedalboard.vst3"
set "DST=%CommonProgramFiles%\VST3\Guitar Pedalboard.vst3"

xcopy /E /I /Y /Q "%SRC%" "%DST%" >nul
if %errorlevel% equ 0 (
    echo Installed: %DST%
    echo Rescan plugins in your DAW to find "Guitar Pedalboard".
) else (
    echo Copy failed. Close your DAW and try again.
)
pause
