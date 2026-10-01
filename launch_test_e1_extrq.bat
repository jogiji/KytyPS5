@echo off
setlocal
cd /d "C:\Users\rajiv\Development\KytyPS5-perf-astrobot"

echo ======================================================================
echo KytyPS5 Astro Bot: Test E1 (EXTRQ Trampoline Rewrite Only)
echo Flags: --native-vrsqrtps false --rewrite-extrq true
echo Logging to: _perf_intel_e1_extrq.log
echo ======================================================================

set KYTY_GPU_ZONES=1

powershell -NoProfile -ExecutionPolicy Bypass -Command "$env:KYTY_GPU_ZONES='1'; & 'C:\Users\rajiv\Development\KytyPS5-perf-astrobot\_Build\windows\install\kyty_emulator.exe' --game 'D:\ps5\PPSA21567 - ASTRO BOT_extracted' --game-patch 'C:\Users\rajiv\Development\KytyPS5-perf-astrobot\_Build\windows\install\_Patches\PPSA21567.json' --gpu 0 --amd-cpu --redzone --native-vrsqrtps false --rewrite-extrq true --dcc-gpu-clear true --async-submit true --gpu-mesh-indirect true --label-flush-interval-us 2000 --pipeline-libraries true --speculative-draws true --record-thread true --hardware-buffer-bounds true --relaxed-readback true --gpu-timestamp-scale 115 --drain-stats 5 2>&1 | Tee-Object -FilePath '_perf_intel_e1_extrq.log'"

pause
