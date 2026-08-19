@echo off
rem ============================================================
rem  视频弱网场景测试（综合：丢包/延迟/慢包 + 带宽变化，循环执行）
rem  拓扑：video-sender(node:9999) -> udp-proxy(5555->9999, scenario) -> video-player(leaf)
rem ============================================================
cd /d "%~dp0"

set DUR=130

start "video-sender" "build\video-sender.exe" --port 9999 --seconds %DUR%
timeout /t 2 /nobreak >nul

start "udp-proxy" "build\udp-proxy.exe" --listen 0.0.0.0:5555 --target 127.0.0.1:9999 --scenario "scenarios\video_weaknet.txt" --l4s --stats-interval 3
timeout /t 1 /nobreak >nul

start "video-player" "build\video-player.exe" --connect 127.0.0.1:5555 --seconds 120

echo.
echo === running. press any key to stop all processes. ===
pause >nul
taskkill /f /im video-sender.exe >nul 2>&1
taskkill /f /im udp-proxy.exe   >nul 2>&1
taskkill /f /im video-player.exe >nul 2>&1
