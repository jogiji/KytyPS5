# Astro Bot GPU Gap Investigation

**Status:** profiling-only instrumentation; no production behavior changes.
**Capture date:** 2026-10-03.  
**Evidence labels:** MEASURED, INFERRED, HYPOTHESIS.

## 1. Baseline

- Canonical source: `exp/astrobot-upstream-20261002` at `4ee64cba382050597ee7b25e1d9f4ad8c12d7c65`.
- Investigation started from the certified profile commit `792e7da018576433252c1fc06051347fe8356376`; branch: `exp/astrobot-gpu-gap`.
- The initial profile used the installed `_Build/windows/install/kyty_emulator.exe`. This correlation follow-up added gated diagnostic instrumentation and rebuilt the diagnostic binary; no production graphics or synchronization semantics were changed.
- The earlier profile measured an unnamed gameplay state at 10.73 FPS, 30.51 ms GPU busy and 62.78 ms GPU gap per frame. Rajiv had not identified that location, so those numbers are historical evidence, not a reproducible benchmark.

## 2. Reproducible heavy scene

The original unnamed location was not reproduced or identified. Rajiv instead supplied a screenshot at the opening of Sky Garden. It shows Astro on the center of the pink stone path, facing the glass tower; the title bar showed 9.2 FPS at that instant. The character and camera were held still for the accepted capture windows.

The fresh capture log is `_perf_gpu_gap_existing_20261003.log` (ignored by Git). Three adjacent, post-warm-up 5-second windows were used. The earlier loading and transition windows were excluded. This is a reproducible scene description, but not proof that it is the exact camera/position from the earlier Sky Garden profile.

## 3. Meaning of the existing gap metric

`CommandScheduler::WriteStartTimestamp` writes a Vulkan top-of-pipe timestamp at the start of a command buffer; `WriteEndTimestamp` writes a bottom-of-pipe timestamp at its end. Once the submission tick completes, the async completion path reads the pair. The GPU timestamp period converts ticks to nanoseconds.

`ReadTimestamps` maintains `m_gpu_last_end`. If the next start is later, it records the interval between the previous end and this start as `gpu-gap`. It records `gpu-busy` only for the newly covered part of the command-buffer interval (`end - max(start, previous end)`). This is union accounting, so overlapping command-buffer intervals are not double-counted. The values are accumulated over a drain-stat window and divided by presented frames for `ms/frame`.

**MEASURED definition:** `gpu-gap` is time between timestamped command-buffer intervals on the GPU queue. It says the queue had no timestamped command-buffer interval in that span; by itself it does not identify why no next buffer was ready. It is not a CPU timer and is not automatically proof that the guest producer was idle.

`gpu-thread-idle` is a separate host-side timer. In `GuestGpu::ThreadRun`, it runs only while the internal command queue is empty, no guest submission is available, and the GPU thread is not stopping. It does not include active PM4 processing, other command work, or time in the all-front-queues-blocked polling path. `blocked-poll` times that last path. Neither counter is synchronized one-for-one with GPU timestamp gaps.

## 4. CPU/GPU timeline

The configured path is:

```text
guest/main and TBB worker threads
  -> guest GPU submission / PM4 queues
  -> Thread_Gpu: PM4 handlers, draw/dispatch translation and host commands
  -> deferred host command recording (record thread enabled)
  -> async queue submission (async submit enabled)
  -> Vulkan command-buffer execution on the NVIDIA GPU
```

`submit` and `queue-lock-wait` are CPU-side submission measurements. `gpu-busy` and `gpu-gap` come from GPU timestamps. `draw-phases` and `pm4-ops` report accumulated CPU work over the interval; they can nest/overlap and must not be added together or treated as separate wall-clock portions of a frame.

## 5. Thread_Gpu accounting

| Capture | FPS | GPU busy ms/frame | GPU gap ms/frame | Thread_Gpu idle ms/frame | blocked-poll ms/frame |
|---|---:|---:|---:|---:|---:|
| Prior fast menu, three-window profile | 59.8 | 12.62 | 4.12 | 0.25 | See prior profile |
| Prior unnamed slow gameplay, three-window profile | 10.73 | 30.51 | 62.78 | 0.00 | See prior profile |
| Prior Sky Garden, three-window profile | 15.67 | 32.35 | 31.61 | 0.00 | See prior profile |
| Fresh Sky Garden start, latest three windows | 11.67 | 82.65 | 3.09 | 1.17 | 0.00 |

**MEASURED:** in the fresh capture, queue-empty `Thread_Gpu` idle averaged 1.17 ms/frame while GPU timestamp gaps averaged 3.09 ms/frame. In the earlier two gameplay captures, `Thread_Gpu` idle was zero despite 31.61–62.78 ms/frame of GPU gap. Thus those earlier holes were not the thread waiting in the instrumented empty-command condition.

**LIMIT:** the difference between those two aggregate values is not an exact gap attribution. Their event boundaries are not aligned per frame. A zero empty-queue timer also does not distinguish active translation from every possible lock, dependency, or wait inside the work being processed.

## 6. PM4 producer/consumer analysis

**MEASURED, fresh Sky Garden start:** PM4 handler CPU work was 955.2–978.3 ms/s over the matched windows, with a three-window mean of 965.2 ms/s. PM4 opcode `0x35` accounted for 808–831 ms/s; `0x2d` accounted for 92–97 ms/s. At 11.67 presents/s, this is approximately 82.7 ms of accumulated handler CPU work per present. This is not extra elapsed time to add to the frame.

**MEASURED, earlier unnamed slow gameplay:** PM4 handler work was 978.5–978.9 ms/s at 10.73 FPS, or approximately 91.2 ms of handler CPU work per presented frame when normalized by presents. The guest-facing queue counters and handler timers are interval totals, not a timestamp of when each packet arrived relative to a GPU gap.

**INFERRED:** the high handler occupancy with `Thread_Gpu` idle at zero is more consistent with Kyty actively processing/feeding PM4 work than with the GPU thread simply waiting for the guest queue to become nonempty. It does not prove which handler or producer caused each historical gap.

## 7. CPU draw cost

**MEASURED, fresh Sky Garden start:** `draw-phases` reported 917.2, 900.3 and 890.1 ms/s. At 11.67 presents/s this is approximately 77.4 ms of accumulated draw-phase CPU work per presented frame. Mean reported draw rate was about 56.2k/s, roughly 4.8k draw-phase events per present. This timer measures CPU work and may overlap PM4 handler accounting.

**MEASURED, earlier unnamed slow gameplay:** draw-phase CPU work was 919.6–921.1 ms/s at 10.73 FPS, approximately 85.8 ms/frame by normalization. This is a large CPU-side workload alongside only 30.51 ms/frame of GPU busy time and 62.78 ms/frame of GPU gap.

**INFERRED:** draw preparation/translation is the leading measured CPU-side candidate for the earlier GPU starvation. The available timer is aggregate phase work, not a direct interval match against each gap, so it cannot establish that all 62.78 ms came from draw preparation.

## 8. Pipeline/resource preparation

The fresh draw-phase counters report per-second CPU work for setup, shader/parameter lookup, target binding, image/buffer views, pipeline lookup, recording and probes. They do not expose an exact, non-overlapping critical-path total by present. The matched windows show pipeline phase around 0.33–0.34 ms/s and no evidence here of steady-state pipeline creation dominating the capture. Startup/warm-up was excluded.

No separate per-frame counter was available for every resource-materialization or descriptor-preparation substage. The largest individual named CPU draw phase in the fresh sample was shader program lookup (`ps-program` about 5.0–5.3 ms/s), far below the total draw-phase CPU work.

## 9. Explicit GPU waits

For the fresh three windows, `tick-wait` was 12.29–18.76 ms/frame, mean 15.80 ms/frame. This is a host wait counter and overlaps the producer/GPU timeline; it is not evidence that the GPU timestamp gap itself was a tick wait. `full-drain` and `blocked-poll` were zero. The separate `suspend-wait` value is a guest synchronization measure and overlaps other work; it is not additive to frame time or GPU gap.

For the earlier unnamed slow sample, tick-wait was 0.14 ms/frame, `full-drain` was zero, and `Thread_Gpu` idle was zero. These measured counters do not account for its 62.78 ms GPU gap as an explicit scheduler wait. Readback telemetry reports bytes/count, not readback-wait duration.

## 10. Memory faults

The fresh log includes fault-site summaries. The largest listed protected-write site in the sampled windows reports roughly 72k–75k events per 5 seconds and about 1.55–1.58 seconds of accumulated site time. Fault-site durations can overlap across TBB/draw threads and the output is limited to top sites; it is not critical-path time. Read-fault sites also appear, including `Draw Extra Geometry`.

**MEASURED limitation:** the diagnostics do not align individual fault intervals with GPU gaps. **INFERRED:** fault counts alone cannot establish that memory faults caused the historical gaps. Do not rank them above draw/PM4 translation on count or aggregate time alone.

## 11. Submission behaviour

`--async-submit true` and `--record-thread true` were active from the existing launch configuration. In the fresh windows, `submit` averaged 1.30 ms/frame and `queue-lock-wait` averaged 0.024 ms/frame. The earlier unnamed slow sample reported 1.32 ms/frame submit and 0.006 ms/frame queue-lock wait.

**INFERRED:** queue-lock contention and CPU queue-submit cost are too small to explain tens of milliseconds of GPU gap. There is no counter for the number of already-recorded buffers waiting to be submitted, so submission starvation cannot be ruled out absolutely, but the measured submit path is not a leading candidate.

## 12. Fast vs heavy comparison

The fast and earlier heavy values below come from the committed profile run, not the fresh queue-diagnostics run. The new capture retakes the fallback Sky Garden area only; the original unnamed slow location remains unidentified.

| Metric | Fast menu, prior run | Unnamed slow gameplay, prior run | Sky Garden prior run | Sky Garden opening, fresh run |
|---|---:|---:|---:|---:|
| FPS | 59.8 | 10.73 | 15.67 | 11.67 |
| Implied frame time | 16.72 ms | 93.17 ms | 63.83 ms | 85.71 ms |
| GPU busy | 12.62 ms | 30.51 ms | 32.35 ms | 82.65 ms |
| GPU gap | 4.12 ms | 62.78 ms | 31.61 ms | 3.09 ms |
| Thread_Gpu idle | 0.25 ms | 0.00 ms | 0.00 ms | 1.17 ms |
| Draw-phase CPU work | Not used here | ~85.8 ms/frame | ~57.4 ms/frame | ~77.4 ms/frame |
| Submit / queue-lock | Not used here | 1.32 / 0.006 ms | 1.18 / 0.020 ms | 1.30 / 0.024 ms |
| Total #UD | 0 | 0 | 0 | 0 |

The fresh Sky Garden opening capture is not a repeat of the prior Sky Garden timing profile: FPS is lower, timestamped GPU busy time is much higher, and gap is much lower. The scene label matches, but the exact in-level camera/position and scene state from the prior run were not recorded. Treat this as a measured condition difference, not a proven emulator regression or a valid direct A/B.

## 13. Gap attribution

The table separates direct measurements from causal interpretation. Percentages are not forced because CPU timers and GPU intervals are not frame-aligned and some counters overlap.

| Component | Fresh Sky Garden, ms/frame | Earlier slow capture, ms/frame | Share of earlier 62.78 ms gap | Evidence |
|---|---:|---:|---:|---|
| Waiting for guest PM4 / empty Thread_Gpu condition | 1.17 | 0.00 | 0% measured in that timer | MEASURED timer; not per-gap aligned |
| PM4 processing | ~82.7 CPU-work estimate | ~91.2 CPU-work estimate | Not directly assignable | MEASURED handlers/s normalized by presents; includes nested work |
| Draw/resource preparation | ~77.4 CPU-work estimate | ~85.8 CPU-work estimate | Not directly assignable | MEASURED draw-phase work normalized by presents; overlaps PM4 accounting |
| Explicit scheduler waits | Tick wait ~15.8; blocked-poll 0 | Tick wait ~0.14; blocked-poll 0 | Not a measured share | MEASURED host wait timers; overlap and are not GPU-gap partitions |
| Submission / queue lock | 1.30 / 0.024 | 1.32 / 0.006 | Not a measured share | MEASURED; small relative to the gap |
| Timestamped GPU gap | 3.09 | 62.78 | 100% by definition | MEASURED GPU timestamp interval; cause unknown to this counter |
| Remaining causal attribution | Not applicable | Unknown | Unknown | No per-gap PM4 arrival/Thread_Gpu state timeline |

**INFERRED:** the strongest explanation for the earlier tens-of-milliseconds gap is Kyty CPU command generation, especially draw preparation within PM4 processing. The evidence is high CPU draw/handler work per frame, zero queue-empty idle, zero blocked-poll/full-drain time, and very low submit/queue-lock time. These measurements narrow the cause to active CPU-side work more strongly than to empty guest queues, explicit scheduler waits or submit serialization.

**Not measured:** exactly how much of a particular 62.78 ms interval is spent in draw translation, other PM4 handling, resource preparation, memory fault handling, or an uninstrumented dependency. No causal percentage is claimed.

## 14. Root-cause verdict

**MEASURED:** the earlier unnamed slow capture had 62.78 ms/frame of GPU timestamp gap, zero `Thread_Gpu` empty-queue idle, 0.14 ms/frame tick wait, zero full drain, 1.32 ms/frame submit and 0.006 ms/frame queue-lock wait. Its draw-phase work was about 85.8 CPU ms/frame and PM4 handler work about 91.2 CPU ms/frame. The Intel illegal-instruction trap count remained zero.

**INFERRED — primary category: CASE B, Kyty CPU translation bottleneck.** Active PM4/draw translation is the best-supported explanation for why the GPU lacked the next command buffer in the earlier slow sample. The measured wait and submission counters are far too small to explain the gap, and the instrumented empty-queue timer is zero. Confidence is **medium**, because CPU phase totals are aggregate and not correlated to each individual GPU gap.

**MEASURED — fresh fallback scene:** the Sky Garden opening ran at 11.67 FPS (85.71 ms/frame) with 82.65 ms/frame GPU busy and 3.09 ms/frame gap. This capture does not exhibit the prior tens-of-milliseconds idle gap; it is almost fully covered by timestamped command-buffer intervals. Its slow frame rate is consistent with high GPU execution plus high CPU translation work, but this retake does not reproduce the old gap event.

## 15. Recommended single next task

Add one gated, low-overhead per-gap producer/consumer trace for the reproducible Sky Garden opening: record command-buffer GPU gap boundaries against wall-clock timestamps for PM4 availability, `Thread_Gpu` state (`Process`/draw preparation/blocked/empty), and record/submit queue depth. That direct correlation is the missing measurement needed to validate the CPU-translation inference and quantify each cause. Do not optimize until that trace is reviewed.

## 16. Limitations

- The original 10.73 FPS location was unidentified and not reproduced. The fallback opening of Sky Garden is identified, but it does not reproduce a large gap in this run.
- The prior and fresh Sky Garden captures have a large busy/gap decomposition difference. Exact camera, character coordinates, and scene state were not recorded in the earlier capture.
- CPU draw-phase, PM4-handler, fault-site, tick-wait and suspend-wait timers have different boundaries; some overlap and none should be summed into frame time.
- Queue snapshots are periodic diagnostics, not continuous stage-queue depth timelines. A blocked PM4 queue entry does not establish that all GPU work was blocked.
- Readback bytes/count are available, but readback wait duration is not. Full per-frame readback, GPU-fence dependency, lock and guest-producer critical-path attribution is unavailable.
- This is an instrumented visible run. The cost of diagnostic instrumentation was not calibrated against a no-diagnostics control.
- No emulator behavior, shaders, tiler, resolution, memory protection or synchronization settings were changed.

## Optimus Baseline Reconciliation

### Invocation and identity

The historical profile and both reconciliation runs used the same game and patch paths and the same command-line arguments as `launch_test_combined_fastpath.bat`. The captures invoked the emulator directly from PowerShell rather than executing the `.bat` wrapper; arguments and the `KYTY_GPU_ZONES` setting matched. The batch wrapper itself only sets `KYTY_GPU_ZONES=1`, launches the emulator with these options, and tees output to a log.

| Setting | Earlier Sky Garden, 15.67 FPS | Fresh retake, 11.67 FPS | Run A, normal profile | Run B, diagnostics |
|---|---|---|---|---|
| `KYTY_GPU_ZONES=1` | Yes | Yes | Yes | Yes |
| `--gpu 0` | Yes | Yes | Yes | Yes |
| `--amd-cpu` | Yes | Yes | Yes | Yes |
| `--redzone` | Yes | Yes | Yes | Yes |
| `--rewrite-vrsqrtps true` | Yes | Yes | Yes | Yes |
| `--rewrite-extrq true` | Yes | Yes | Yes | Yes |
| `--dcc-gpu-clear true` | Yes | Yes | Yes | Yes |
| `--async-submit true` | Yes | Yes | Yes | Yes |
| `--gpu-mesh-indirect true` | Yes | Yes | Yes | Yes |
| `--label-flush-interval-us 2000` | Yes | Yes | Yes | Yes |
| `--pipeline-libraries true` | Yes | Yes | Yes | Yes |
| `--speculative-draws true` | Yes | Yes | Yes | Yes |
| `--record-thread true` | Yes | Yes | Yes | Yes |
| `--hardware-buffer-bounds true` | Yes | Yes | Yes | Yes |
| `--relaxed-readback true` | Yes | Yes | Yes | Yes |
| `--gpu-timestamp-scale 115` | Yes | Yes | Yes | Yes |
| `--drain-stats 5` | Yes | Yes | Yes | Yes |
| Extra diagnostic environment | `KYTY_DEBUG_DRAW_STATS=1`; `KYTY_DEBUG_DRAW_PHASES=all`; no queue snapshots | Same, plus `KYTY_DEBUG_QUEUES=1` | None beyond `KYTY_GPU_ZONES=1` | Draw stats, all draw phases, and queue snapshots enabled |
| Executable | `_Build\windows\install\kyty_emulator.exe`; build 6881861 Release per profile record | Same path/build per prior record | Same file, SHA256 `BF8C4CA9C058E8AD40E66CDB5F975AB6A17E230227718B85A06CEA26CFF456D` | Same file/hash; no rebuild between A and B |
| Pipeline-cache load | 42,658,066 bytes; 674 recorded permutations replayed | 42,658,066 bytes; 679 replayed | 42,927,848 bytes; 679 replayed | Same 42,927,848 bytes and 679 replayed |

The earlier two capture logs do not contain per-run executable hashes. Their identity is supported by the profile record and the unchanged installed Release build; it is not an independently logged historical hash. The Run A and Run B logs both show the same current executable path/build. Both A and B started from the same preserved cache snapshot (SHA256 `8831a88abf8dca3588ed862fc5cbdacb34b485c6b5ace7b148a4833e46d25beb`). The historical cache was populated in both runs; the five-permutation difference between the two old logs and the larger A/B cache file are recorded facts, not evidence that cache state caused the busy/gap reversal.

### Run A / Run B method

Run A used the batch arguments with only `KYTY_GPU_ZONES=1`; `KYTY_DEBUG_DRAW_STATS`, `KYTY_DEBUG_DRAW_PHASES`, and `KYTY_DEBUG_QUEUES` were unset. Run B used the identical executable, command line and cache snapshot, with those three diagnostic variables enabled. Each was a visible interactive run. Loading and navigation windows were excluded. Rajiv confirmed the reference position for A. For B, Rajiv reported the scene loaded after receiving the same navigation instruction, but did not separately confirm the exact character and camera position; this limits how tightly the A/B scene state can be matched.

**Reference scene:** Astro Bot, Sky Garden opening path. Astro stood centered on the pink stone path, facing the glass tower with the camera behind Astro. The large pink-leaf tree and blue bot bubble are to the right; grass borders the path. Screenshot reference: `codex-clipboard-380af6bb-a010-4903-bd9d-941dc726719a.png`.

### Stable windows

The values below are each complete 5-second `--drain-stats` interval. Frame time is inferred as 5 seconds divided by presents. All six windows report `#UD=0`, `full-drain=0`, `blocked-poll=0`, and `gpu-thread-idle=0`.

| Run | Window | Presents / FPS | Implied frame ms | GPU busy ms/frame | GPU gap ms/frame |
|---|---:|---:|---:|---:|---:|
| A | 1 | 60 / 12.0 | 83.33 | 25.63 | 57.89 |
| A | 2 | 60 / 12.0 | 83.33 | 24.51 | 58.70 |
| A | 3 | 63 / 12.6 | 79.37 | 26.06 | 52.94 |
| B | 1 | 65 / 13.0 | 76.92 | 29.49 | 47.11 |
| B | 2 | 65 / 13.0 | 76.92 | 29.91 | 46.99 |
| B | 3 | 66 / 13.2 | 75.76 | 29.87 | 45.92 |

| Summary | FPS mean | Implied frame ms | GPU busy ms/frame | GPU gap ms/frame |
|---|---:|---:|---:|---:|
| A, normal profile | 12.20 | 81.97 | 25.41 | 56.45 |
| B, diagnostics enabled | 13.07 | 76.53 | 29.76 | 46.67 |

Busy plus gap sums to 81.86 ms/frame in A and 76.43 ms/frame in B, close to the respective inferred frame intervals. A had submit 1.29 ms/frame and queue-lock wait about 0.005 ms/frame; B had submit about 1.05 ms/frame and queue-lock wait below 0.01 ms/frame. These remain small in both runs.

### Diagnostics overhead and discrepancy

**MEASURED:** B was 0.87 FPS faster than A (+7.1%), with 5.44 ms/frame lower inferred frame time, 4.35 ms/frame more timestamped GPU busy time, and 9.78 ms/frame less GPU gap. The large gap persisted with diagnostics disabled in A and enabled in B. This result does not support extra diagnostics as the cause of the 32–63 ms gaps.

**MEASURED limitation:** the A/B comparison did not directly time the CPU cost of the diagnostic code. A had no draw-phase/PM4 timers; B reported approximately 0.90–0.92 accumulated draw CPU-seconds per second and 0.96–0.98 PM4-handler CPU-seconds per second, which measure emulator workload and overlap each other. The queue diagnostic logs snapshots every five seconds. The A/B FPS delta is the observed net difference, not a direct isolated overhead measurement.

**INFERRED:** the A/B is inconclusive for a small diagnostics overhead because the diagnostics-on run was faster and its busy/gap split changed substantially despite the same intended reference scene and identical cache. Exact B positioning was not reconfirmed, so scene-state variation remains a confound. The measured large gap itself is repeatable across both diagnostic settings. The earlier 31.61 ms Sky Garden gap is reproduced in the broader sense of a large gap: the new stable windows measured 45.92–58.70 ms/frame. The exact 31.61 ms value was not reproduced.

**INFERRED:** the 3.09 ms gap from the prior fresh retake is not reproduced by either controlled run. All launch flags, executable/build identity, GPU selection, and A/B cache contents are matched here, so these do not explain the new A/B difference. The old retake did not record the exact held character/camera coordinates; a different scene state or natural frame-to-frame workload remains plausible.

**HYPOTHESIS:** dynamic scene work or runtime GPU/CPU clock variation contributed to the A/B timing difference. Clock and power state were not captured, so this is not established.

### Decision

```text
OPTIMUS BASELINE STATUS:
REPRODUCIBLE

REFERENCE SCENE:
Sky Garden opening path; Astro centered on pink stone path, camera behind, facing glass tower; screenshot codex-clipboard-380af6bb-a010-4903-bd9d-941dc726719a.png

REFERENCE CONFIG:
launch_test_combined_fastpath.bat arguments; KYTY_GPU_ZONES=1; no draw/PM4/queue-state diagnostics; warm cache 42,927,848 bytes, 679 permutations

REFERENCE FPS:
12.20 mean (three 5-second windows)

REFERENCE FRAME MS:
81.97 ms inferred from 183 presents / 15 seconds

REFERENCE GPU BUSY:
25.41 ms/frame

REFERENCE GPU GAP:
56.45 ms/frame

DIAGNOSTIC OVERHEAD:
Not directly isolated. Diagnostics-on Run B measured +7.1% FPS, +4.35 ms/frame GPU busy, and -9.78 ms/frame GPU gap versus A; the result is inconclusive for overhead magnitude.

EARLIER 31.61-MS GAP:
REPRODUCED (large-gap behavior; new range 45.92–58.70 ms/frame)

SAFE TO BEGIN dGPU-ONLY A/B:
YES
```

## Dedicated NVIDIA vs Optimus A/B

### dGPU-only graphics environment

After the reboot into dedicated NVIDIA mode, `vulkaninfo --summary` enumerated one Vulkan device: GPU 0, NVIDIA GeForce RTX 4090 Laptop GPU, vendor `0x10de`, device `0x2757`, driver `617.14`. The Intel UHD adapter remained as a Windows PnP phantom/unknown display entry and did not appear in the Vulkan device list or active `Win32_VideoController` list. `nvidia-smi topo -m` showed only GPU 0; NVIDIA reported display active, and Windows reported 2560x1600. During the run, `nvidia-smi` listed `kyty_emulator.exe` PID 53164 as a C+G process on GPU 0. The window title identified build 6881861 Release, Astro Bot PPSA21567 01.018.000, and the RTX 4090. This confirms `--gpu 0` selected the RTX 4090 for the observed run; no Intel Vulkan device was exposed.

The executable SHA256 was `BF8C4CA9C058E8AD40E66CDB5F975AB6A17E230227718B85A06CEA26CFF456D`, matching the pre-run binary. The run used `launch_test_combined_fastpath.bat` and its normal-profile flags, with `KYTY_GPU_ZONES=1`; no draw-phase, PM4, or queue diagnostics were enabled. Startup loaded 42,927,848 cache bytes and replayed all 679 recorded permutations, skipping none. The on-disk cache was 42,927,995 bytes with SHA256 `8831a88abf8dca3588ed862fc5cbdacb34b485c6b5ace7b148a4833e46d25beb` before and after the run, including after normal emulator exit. The cache remained warm and unchanged.

### Scene and capture method

Rajiv confirmed the dGPU scene held after the instruction used for the Optimus reference: Sky Garden opening path, Astro centered on the pink stone path, camera behind Astro facing the glass tower. The same nearby pink-leaf tree and blue bot bubble were to the right of the path in the Optimus screenshot reference. No numerical character/camera coordinates or new dGPU screenshot were recorded, so the scene match is practical and visual rather than coordinate-exact. The emulator was visible and operated interactively. The 60 prior five-second intervals were treated as warm-up/navigation; the next three complete intervals were accepted. The complete ignored telemetry copy is `_perf_gpu_dgpu_only_20261003.log`.

### Individual dGPU-only windows

| Window | Presents / FPS | Implied frame ms | GPU busy ms/frame | GPU gap ms/frame | #UD/frame |
|---|---:|---:|---:|---:|---:|
| 1 | 67 / 13.4 | 74.63 | 27.46 | 47.35 | 0 |
| 2 | 67 / 13.4 | 74.63 | 26.62 | 47.73 | 0 |
| 3 | 68 / 13.6 | 73.53 | 26.89 | 46.68 | 0 |

Combined over 202 presents in 15 seconds, the representative dGPU-only values are 13.47 FPS, 74.26 ms/frame, 26.99 ms GPU busy, and 47.25 ms GPU gap. The per-window median is 13.4 FPS, 74.63 ms/frame, 26.89 ms busy, and 47.35 ms gap. Each interval reported `full-drain=0`, `blocked-poll=0`, `gpu-thread-idle=0`, and `#UD=0`. `tick-wait` was 1.69, 2.06, and 1.71 ms/frame; `submit` was 1.17, 1.19, and 1.15 ms/frame; queue-lock wait was 0.00 ms/frame after rounding. `suspend-wait` was 54.01, 54.12, and 53.14 ms/frame. These CPU synchronization counters overlap other work and are not additive to frame time or GPU gap.

Five one-second `nvidia-smi` samples during the held scene reported GPU utilization of 33%, 46%, 39%, 37%, and 34% (mean 37.8%). The same samples showed 14,908–14,916 MiB used of 16,376 MiB, 1,050–1,290 MHz graphics clocks, and 65–66 C. This is device-wide WDDM telemetry, not per-process utilization or a direct substitute for Kyty's GPU timestamps. No equivalent Optimus utilization samples exist, so it cannot establish a mode-to-mode utilization change.

### Comparison and interpretation

| Metric | Optimus Run A | dGPU-only | Absolute change | Relative change |
|---|---:|---:|---:|---:|
| FPS | 12.20 | 13.47 | +1.27 | +10.4% |
| Frame time | 81.97 ms | 74.26 ms | -7.71 ms | -9.4% |
| GPU busy | 25.41 ms/frame | 26.99 ms/frame | +1.58 ms | +6.2% |
| GPU gap | 56.45 ms/frame | 47.25 ms/frame | -9.20 ms | -16.3% |

Changes use `(dGPU-only - Optimus) / Optimus`. The dGPU capture's busy-plus-gap is 74.24 ms/frame, close to its 74.26 ms implied frame time. In the Optimus reference, busy plus gap was 81.86 ms/frame versus 81.97 ms implied. Timestamped GPU busy rose while timestamped gap fell. The result is therefore not a simple GPU execution-speed increase: most of the observed frame-time reduction coincides with a smaller gap between timestamped GPU command-buffer intervals.

**MEASURED:** the GPU gap remains large at 47.25 ms/frame in dGPU-only mode, although it is 9.20 ms/frame (16.3%) lower than normal-profile Optimus Run A. Timestamped GPU busy is 1.58 ms/frame (6.2%) higher; FPS is 10.4% higher. The fast Intel instruction path remains healthy at zero illegal-instruction traps.

**MEASURED:** dGPU-only `gpu-thread-idle`, `blocked-poll`, and `full-drain` remain zero, and `tick-wait` and `submit` remain around 1–2 ms/frame. These counters do not attribute each GPU gap to a specific producer, wait, or presentation event. The 53.75 ms/frame combined `suspend-wait` is a separate guest synchronization measure; it overlaps other work and cannot be assigned as the cause of the GPU gap. Optimus Run A's accepted windows averaged about 60.26 ms/frame for this counter, but the difference is not a per-gap correlation.

**INFERRED:** this capture supports a moderate graphics-topology-associated effect: dGPU-only mode reduced GPU gap and frame time while increasing measured GPU execution. It does not identify presentation-copy overhead or another specific mechanism. The measured improvement is not large enough to explain the remaining low frame rate; the GPU timestamp gap still accounts for about 63.6% of the implied dGPU frame interval.

**LIMITATION:** this is one sequential capture per graphics mode, with three adjacent windows in each. The scene was visually matched by Rajiv's hold confirmation, but no coordinates or dGPU screenshot establish identical scene state. Scene-work variation, clocks/power state, and run order remain possible contributors. The prior diagnostics-enabled Optimus capture also showed a gap near 46.67 ms/frame, demonstrating that a similar value occurred in a separate Optimus run; because it had extra diagnostics and no exact B position confirmation, it is not a clean counterexample or control.

### Decision

```text
DEDICATED-NVIDIA A/B RESULT:

OPTIMUS REFERENCE:
FPS: 12.20
FRAME MS: 81.97
GPU BUSY: 25.41 ms/frame
GPU GAP: 56.45 ms/frame

DGPU-ONLY:
FPS: 13.47
FRAME MS: 74.26
GPU BUSY: 26.99 ms/frame
GPU GAP: 47.25 ms/frame

GPU GAP CHANGE:
-9.20 ms / -16.3%

GPU BUSY CHANGE:
+1.58 ms / +6.2%

BOTTLENECK CLASSIFICATION AFTER A/B:
MIXED: moderate topology-associated change measured, with a large unexplained GPU queue gap persisting.

OPTIMUS CONTRIBUTION:
SUPPORTED (moderate association in this capture; specific mechanism and repeatability unproven)

NEXT INVESTIGATION:
Correlate per-gap GPU timestamp boundaries with producer availability, Thread_Gpu state, and record/submit queue depth in the held dGPU-only Sky Garden scene.
```

## GPU Gap Critical-Path Correlation

### Capture and exact configuration

**MEASURED:** the emulator ran visibly in dGPU-only mode on the NVIDIA RTX 4090, using `launch_gpu_gap_trace_dgpu_filtered.bat`, the same warm game/cache and the same runtime arguments as the established dGPU reference. Effective runtime options were `--gpu 0 --amd-cpu --redzone --rewrite-vrsqrtps true --rewrite-extrq true --dcc-gpu-clear true --async-submit true --gpu-mesh-indirect true --label-flush-interval-us 2000 --pipeline-libraries true --speculative-draws true --record-thread true --hardware-buffer-bounds true --relaxed-readback true --gpu-timestamp-scale 115 --drain-stats 1`, plus the game and patch paths in the batch file. The trace run added `KYTY_GPU_GAP_TRACE=1`, `KYTY_GPU_ZONES=1`, and the filtered trace policy: queue-depth sampling at 50 ms and retaining record-batch, empty-stream, and consumer spans only when at least 250 microseconds long. `--relaxed-readback true` remains the known performance-semantic relaxation, and was unchanged in both captures. No production behavior or graphics semantics were changed. The emulator was closed normally before analysis.

**MEASURED:** the selected stable-position trace interval is QPC-nanoseconds `[22889352397300, 22904352397300)`, exactly 15 seconds. It contains 164 presents, zero `#UD` traps in each accepted one-second telemetry window, 304 clock calibrations in the full trace, maximum calibration deviation 55.328 microseconds, and zero cumulative ring drops. Loading and navigation were excluded. The character/camera match is based on Rajiv's visual hold confirmation, not numerical coordinates.

### Instrumentation overhead A/B

| Capture | Presents / time | FPS | Frame ms | GPU busy ms/frame | GPU gap ms/frame |
|---|---:|---:|---:|---:|---:|
| Normal dGPU reference | 202 / 15 s | 13.47 | 74.26 | 26.99 | 47.25 |
| Filtered correlation trace | 164 / 15 s | 10.93 | 91.46 | 30.93 | 61.42 |

**MEASURED:** with correlation tracing, FPS was 18.8% lower, implied frame time 17.20 ms higher, GPU busy 3.94 ms/frame higher, and GPU gap 14.17 ms/frame higher. The trace window also varied across its three consecutive 5-second groups: 12.8, 10.6, and 9.4 FPS. This is a material net capture effect. The comparison is sequential rather than simultaneous and scene coordinates were not recorded, so it does not isolate profiler overhead from scene/runtime variation. The trace should be used to classify the active pipeline state, not as the normal-profile performance number.

### Gap and queue measurements

**MEASURED:** the one-second telemetry for the selected interval aggregates to 164 presents in 15 seconds: 10.93 FPS, 91.46 ms implied frame time, 30.93 ms/frame GPU busy, and 61.42 ms/frame GPU gap. These drain-window GPU metrics are close in aggregate but are not exactly aligned to trace event boundaries.

The timeline contains 4,618 positive-duration GPU-gap records totaling 9,524.938 ms. Their median duration is 0.0143 ms and p95 is 19.72 ms. The 403 gaps longer than 5 ms total 8,259.52 ms; mean duration is 20.495 ms, median 20.420 ms, and p95 27.289 ms. The longest was 30.148 ms. The trace event total divided by 164 presents is 58.08 ms of timestamped gap per present; the independent drain-stat average is 61.42 ms/frame. The approximately 3.34 ms/frame mismatch reflects different interval/accounting boundaries and is retained as uncertainty rather than forced to reconcile.

| Queue/state | MEASURED result in selected interval | Meaning and limit |
|---|---|---|
| Guest GPU submissions awaiting Thread_Gpu | Event-updated depth time-weighted mean 11.40, peak 15; depth was zero for 29.7 ms of 15 s (0.20%). At gap starts, 4 of 4,618 samples were zero; mean depth 8.88, median 9. | Guest submission work was available through nearly all observed gaps. This is submission count, not PM4 packet count. |
| Thread_Gpu callback/work queue | Pending callback depth was zero for 99.998% of the interval, peak 1; it was zero at every selected gap start. | This queue counts separately posted callbacks. It does not mean the main guest-submission queue was empty. |
| Thread_Gpu execution | `thread_gpu_process` overlapped 9,523.318 ms of 9,524.938 ms of all captured positive GPU-gap durations (99.983%). PM4 spans on the same Thread_Gpu ID overlapped 9,503.950 ms (99.780%). | Direct temporal overlap on the GPU thread. Overlap supports association but alone does not prove causation. |
| Host command-record stream | Periodic publish-time samples: 599, all nonzero; mean 1,052 bytes / 5.77 record packets, median 112 bytes / 1 packet, peak 22,912 bytes / 157 packets. Exact consumer-empty spans totaled 1,364.111 ms over 15 s (9.09%); 884.223 ms overlapped captured GPU gaps (9.28% of gap duration). | Publish-time samples are biased to publishing and cannot establish an empty fraction. Empty spans are the direct empty-time measure; record batches over the trace threshold overlapped only 1.797 ms of gaps. |
| Async submit queue | 299 periodic samples; 297 showed zero jobs/packets, two showed one. It was empty in 4,587 of 4,619 gap-start states. | Sampled at 50 ms; the sample ratio is not exact empty duration. It provides no evidence of a sustained backlog waiting for the submitter. |

At the start of each of the three longest gaps, the carried queue state was: guest submission depth 12, callback/work depth 0, command stream 112 bytes / one record packet, and submit queue 0. For the 30.148 ms example, same-thread PM4 processing overlapped 30.102 ms; command-stream consumer idle overlapped 0.716 ms; Vulkan submit overlapped 0.076 ms. Thus the GPU gap begins while guest submissions are pending and Thread_Gpu is processing PM4, with little downstream queued work.

**MEASURED:** the analyzer finds 3,399 explicit semaphore waits totaling 14.945 seconds across all threads during the selected interval. None was on the identified Thread_Gpu ID, so same-thread semaphore-wait overlap with GPU gaps is zero. The earlier all-thread overlap is coincident activity and is not evidence that these waits caused the gaps. Vulkan submit CPU spans overlapped 86.608 ms of captured gap time (0.91%); record batches overlapped 1.797 ms. Submit time and queue-lock wait in the matched drain statistics averaged approximately 1.46 ms/frame and 0.01 ms/frame, respectively. `gpu-thread-idle`, `blocked-poll`, and `full-drain` were zero in the accepted telemetry windows.

### PM4 and translation correlation

**MEASURED:** same-thread PM4 spans occupy 9.504 seconds during the 9.525 seconds of captured positive GPU-gap duration. This equals 57.95 ms per present across 164 presents, or 99.78% of the trace's timestamped gap event duration. The broader `Thread_Gpu` processing span overlaps 99.983%. These are single-thread wall spans coincident with GPU timestamp gaps, not additive CPU totals.

Slow-handler events are threshold-filtered samples, not complete PM4 accounting. In this window, the trace recorded 831 slow draw-handler events totaling 648.468 ms (3.95 ms per present), 48 slow dispatch-handler events totaling 21.205 ms (0.13 ms/present), and 41 slow draw-phase events totaling 41.264 ms (0.25 ms/present). These subsets cannot explain the remaining PM4 span and do not identify a specific opcode as the cause. In particular, opcode `0x35` was not independently established as the critical handler in this trace.

**INFERRED:** the earliest likely limiting stage is Thread_Gpu PM4 processing and its production of downstream recorded/submitted host work. The guest submission queue is almost never empty and has a substantial backlog during gaps; meanwhile Thread_Gpu is active in PM4 processing, the record stream is shallow at gap starts, and the submit queue is almost always empty. Sustained CPU submit work, queue-lock contention, and Thread_Gpu idle do not explain the measured gap. The precise handoff between PM4 handling and command recording is not proven because stream samples are periodic/publish-biased and no per-buffer readiness marker directly links a specific PM4 packet to the next Vulkan timestamp interval.

### Attribution table

| Cause/state | ms/frame or share | Evidence and interpretation |
|---|---:|---|
| Thread_Gpu PM4 processing overlaps GPU timestamp gaps | 57.95 ms/present; 99.78% of captured gap duration | MEASURED temporal overlap, not a causal partition. |
| Producer unavailable | 0.20% of interval empty; 4/4,618 gap starts empty | MEASURED from submission-depth transitions; producer starvation is not supported. |
| Record consumer empty during gaps | 884.223 ms total; 9.28% of captured gap duration | MEASURED; overlaps the Thread_Gpu/PM4 spans and must not be added to them. |
| Submit queue backlog / submit CPU spans | 32 positive gap-start samples; 86.608 ms submit-span overlap | MEASURED; no evidence of sustained submit-stage backlog. |
| Thread_Gpu semaphore wait | 0 ms overlap | MEASURED same-thread matching; waits on unrelated threads excluded. |
| Unattributed / exact causality | Not separated | PM4 overlap explains temporal state for almost all gap events, but does not prove which PM4 work or producer dependency caused each interval. |

**INFERRED:** `Thread_Gpu PM4 processing → low downstream command/submit supply` is the leading explanation for why the GPU queue is often without timestamped work. The evidence partially attributes the gaps to that active stage, but does not confirm that translation/preparation consumes the entire GPU-idle critical path. The trace's material performance effect limits extrapolation to uninstrumented play.

**HYPOTHESIS:** the high guest-submission backlog reflects PM4 processing throughput that cannot emit/record host work fast enough for the GPU. It is testable by opcode/handler-duration and per-command-buffer readiness correlation; the current trace does not identify the actionable handler.

### Correlation result

```text
GPU GAP ROOT-CAUSE STATUS:
PARTIALLY ATTRIBUTED

DGPU REFERENCE:
FPS: 13.47 (normal-profile reference; correlation run 10.93)
FRAME MS: 74.26 (normal-profile reference; correlation run 91.46)
GPU BUSY: 26.99 ms/frame (normal-profile reference; correlation run 30.93)
GPU GAP: 47.25 ms/frame (normal-profile reference; correlation run 61.42)

DOMINANT GAP STATE:
Thread_Gpu actively processing PM4 while guest submissions remain queued and downstream submit depth is usually zero.

FIRST STARVING PIPELINE BOUNDARY:
Likely Thread_Gpu PM4 processing to host command-record/submit supply; the exact handoff is not proven.

DOMINANT CRITICAL-PATH COST:
57.95 ms/frame of same-thread PM4 overlap with captured GPU gaps

PERCENT OF GPU GAP EXPLAINED:
99.78% temporally overlapped by PM4 spans in the trace (not a causal percentage).

TRANSLATION HYPOTHESIS:
PARTIAL

SUBMISSION BOTTLENECK:
NOT SUPPORTED

PRODUCER STARVATION:
NOT SUPPORTED

NEXT OPTIMIZATION TARGET:
NONE — further attribution required; next investigation should identify which PM4 handlers delay downstream recorded/submitted work.

CONFIDENCE:
MEDIUM
```
