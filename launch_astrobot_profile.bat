@echo off
cd /d "C:\Users\rajiv\Development\KytyPS5-perf-astrobot"
set KYTY_GPU_ZONES=1
set KYTY_DEBUG_DRAW_STATS=1
echo ============================================================
echo Starting KytyPS5 Astro Bot
echo Output is being logged to _perf_gameplay_003.log
echo ============================================================
powershell -NoProfile -ExecutionPolicy Bypass -Command "$env:KYTY_GPU_ZONES='1'; $env:KYTY_DEBUG_DRAW_STATS='1'; & '.\_Build\windows\install\kyty_emulator.exe' --game 'D:\ps5\PPSA21567 - ASTRO BOT_extracted' --game-patch 'C:\Users\rajiv\Development\KytyPS5-perf-astrobot\_Build\windows\install\_Patches\PPSA21567.json' --gpu 0 --amd-cpu --redzone --drain-stats 5 2>&1 | Tee-Object -FilePath '_perf_gameplay_003.log'"
pause
