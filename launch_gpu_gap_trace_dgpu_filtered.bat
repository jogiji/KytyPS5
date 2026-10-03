@echo off
setlocal
cd /d "C:\Users\rajiv\Development\KytyPS5-perf-astrobot"

echo ======================================================================
echo KytyPS5 Astro Bot: dGPU GPU-gap filtered correlation trace
echo GPU-gap CSV: _perf_gpu_gap_correlation_filtered.csv
echo Emulator log: _perf_gpu_gap_correlation_filtered.log
echo ======================================================================

set KYTY_GPU_ZONES=1
set KYTY_GPU_GAP_TRACE=1
set KYTY_GPU_GAP_TRACE_FILE=_perf_gpu_gap_correlation_filtered.csv

powershell -NoProfile -ExecutionPolicy Bypass -Command "$env:KYTY_GPU_ZONES='1'; $env:KYTY_GPU_GAP_TRACE='1'; $env:KYTY_GPU_GAP_TRACE_FILE='_perf_gpu_gap_correlation_filtered.csv'; & 'C:\Users\rajiv\Development\KytyPS5-perf-astrobot\_Build\windows\install\kyty_emulator.exe' --game 'D:\ps5\PPSA21567 - ASTRO BOT_extracted' --game-patch 'C:\Users\rajiv\Development\KytyPS5-perf-astrobot\_Build\windows\install\_Patches\PPSA21567.json' --gpu 0 --amd-cpu --redzone --rewrite-vrsqrtps true --rewrite-extrq true --dcc-gpu-clear true --async-submit true --gpu-mesh-indirect true --label-flush-interval-us 2000 --pipeline-libraries true --speculative-draws true --record-thread true --hardware-buffer-bounds true --relaxed-readback true --gpu-timestamp-scale 115 --drain-stats 1 2>&1 | Tee-Object -FilePath '_perf_gpu_gap_correlation_filtered.log'"

pause
