@echo off
rem ============================================================
rem  视频带宽阶梯场景（验证视频码率随 proxy 带宽跟随）
rem  拓扑：video-sender(node:9999) -> udp-proxy(5555->9999, scenario) -> video-player(leaf)
rem ============================================================
cd /d "%~dp0"

set DUR=100

start "video-sender" "build\video-sender.exe" --port 9999 --seconds %DUR%
timeout /t 2 /nobreak >nul

start "udp-proxy" "build\udp-proxy.exe" --listen 0.0.0.0:5555 --target 127.0.0.1:9999 --scenario "scenarios\video_ramp.txt" --l4s --stats-interval 3
timeout /t 1 /nobreak >nul

start "video-player" "build\video-player.exe" --connect 127.0.0.1:5555 --seconds 90

echo.
echo === running. press any key to stop all processes. ===
pause >nul
taskkill /f /im video-sender.exe >nul 2>&1
taskkill /f /im udp-proxy.exe   >nul 2>&1
taskkill /f /im video-player.exe >nul 2>&1
