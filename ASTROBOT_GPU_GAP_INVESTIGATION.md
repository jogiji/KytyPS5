# Astro Bot GPU Gap Investigation

**Status:** profiling only; no emulator/runtime source changes.  
**Capture date:** 2026-10-03.  
**Evidence labels:** MEASURED, INFERRED, HYPOTHESIS.

## 1. Baseline

- Canonical source: `exp/astrobot-upstream-20261002` at `4ee64cba382050597ee7b25e1d9f4ad8c12d7c65`.
- Investigation started from the certified profile commit `792e7da018576433252c1fc06051347fe8356376`; branch: `exp/astrobot-gpu-gap`.
- The emulator binary was the existing `_Build/windows/install/kyty_emulator.exe`. No rebuild or runtime source change was made.
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
