@echo off
rem ============================================================
rem  Direct video test: video-sender (MF 1080p) -> ffplay window
rem ============================================================
cd /d "%~dp0"

start "video-sender" "build\video-sender.exe" --port 9999
timeout /t 2 /nobreak >nul

start "video-player" "build\video-player.exe" --connect 127.0.0.1:9999

echo.
echo === running. ffplay window should show video. press any key to stop ===
pause >nul
taskkill /f /im video-sender.exe >nul 2>&1
taskkill /f /im video-player.exe >nul 2>&1
taskkill /f /im ffplay.exe >nul 2>&1
