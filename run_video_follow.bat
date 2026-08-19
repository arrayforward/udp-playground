@echo off
rem ============================================================
rem  Video follow test: good -> weak -> recover -> weak -> recover (loop)
rem  Topology: video-sender(9999) -> udp-proxy(5555, follow scenario) -> video-player
rem  Full link: H.264 720p (ch0) + Opus audio (ch1, FEC + 300ms jitter buffer)
rem  Usage : run_video_follow.bat [seconds]   (default 90)
rem  Logs  : test-results\follow-<stamp>\  (sender/proxy/player .log/.err)
rem ============================================================
cd /d "%~dp0"

set DUR=90
if not "%~1"=="" set DUR=%~1

for /f %%a in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%a
set OUT=test-results\follow-%STAMP%
if not exist "%OUT%" mkdir "%OUT%"

echo ============================================================
echo  follow scenario test: %DUR%s  logs: %OUT%
echo  good 8s - weak 2Mbps 8s - good 8s - 4Mbps 8s - ... loop
echo ============================================================
echo.

start "video-sender" cmd /c ""build\video-sender.exe" --port 9999 --seconds %DUR% >"%OUT%\sender.log" 2>"%OUT%\sender.err""
timeout /t 2 /nobreak >nul

start "udp-proxy" cmd /c ""build\udp-proxy.exe" --listen 0.0.0.0:5555 --target 127.0.0.1:9999 --scenario "scenarios\video_follow.txt" --stats-interval 5 >"%OUT%\proxy.log" 2>"%OUT%\proxy.err""
timeout /t 1 /nobreak >nul

start "video-player" cmd /c ""build\video-player.exe" --connect 127.0.0.1:5555 --seconds %DUR% >"%OUT%\player.log" 2>"%OUT%\player.err""

echo  running... (ffplay window: video + audio)
echo  press any key to stop early
pause >nul

taskkill /f /im video-sender.exe >nul 2>&1
taskkill /f /im udp-proxy.exe   >nul 2>&1
taskkill /f /im video-player.exe >nul 2>&1
taskkill /f /im ffplay.exe >nul 2>&1

echo.
echo === player summary ===
findstr /c:"player 5s" "%OUT%\player.log"
echo.
echo === sender summary ===
findstr /c:"src 5s" "%OUT%\sender.log"
echo.
echo === proxy scenario timeline ===
findstr /c:"scenario[" "%OUT%\proxy.log"
echo.
echo logs: %OUT%
pause
