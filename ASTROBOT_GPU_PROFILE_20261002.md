# Astro Bot GPU Profile — 2026-10-02

> **Capture completed 2026-10-03.** Three stable 5-second windows were collected for each accepted scene, including Rajiv-confirmed Sky Garden. This report profiles the canonical binary/source configuration; it makes no runtime changes.

## 1. Canonical baseline

- **Title:** Astro Bot, PPSA21567, version 01.018.000.
- **Source branch:** `exp/astrobot-gpu-profile-20261002`.
- **Source SHA:** `4ee64cba382050597ee7b25e1d9f4ad8c12d7c65`.
- The installed Release binary reports build `6881861`, matching the runtime-source parent of the canonical documentation-only commit.
- **MEASURED:** The profiling branch points at the canonical SHA. The working tree contained only this report before it was staged; no runtime source was changed.
- Baseline certification (64/64 tests and manual playability) is inherited from the supplied canonical-baseline record; the test suite was not rerun for this profiling task.

## 2. Hardware

- Alienware m18 R1; Intel Core i9-13980HX; 32 hardware threads; 64 GB RAM; NVIDIA RTX 4090 Laptop GPU, 16 GB; Windows 11.
- Runtime window identified NVIDIA Vulkan device vendor `0x10de`, device `0x2757`.

## 3. Runtime configuration

The visible run used the arguments from `launch_test_combined_fastpath.bat`:

```text
--game "D:\ps5\PPSA21567 - ASTRO BOT_extracted"
--game-patch "_Build\windows\install\_Patches\PPSA21567.json"
--gpu 0 --amd-cpu --redzone
--rewrite-vrsqrtps true --rewrite-extrq true
--dcc-gpu-clear true --async-submit true --gpu-mesh-indirect true
--label-flush-interval-us 2000 --pipeline-libraries true
--speculative-draws true --record-thread true
--hardware-buffer-bounds true --relaxed-readback true
--gpu-timestamp-scale 115 --drain-stats 5
```

Environment for this capture:

```text
KYTY_GPU_ZONES=1
KYTY_DEBUG_DRAW_STATS=1
KYTY_DEBUG_DRAW_PHASES=all
```

The `PPSA21567.json` patch set selected the existing non-tiled deferred-lighting renderer and disabled GI probes and lighting shaders. `--relaxed-readback true` is a **performance-semantic relaxation**: selected hot guest reads can observe earlier bytes while newer GPU data is in flight. It was retained unchanged. Draw-phase and GPU-zone instrumentation add measurement overhead; its magnitude was not separately calibrated.

**MEASURED:** Startup logged EXTRQ rewrite of 97 instructions, VRSQRTPS trampoline rewrite of 11,044 instructions, and a 43-instruction VRSQRTPS fallback sweep. All selected steady-state windows report total `#UD=0`. Per-instruction counters are not emitted separately; zero total traps means neither instruction trapped in these windows.

## 4. Instrumentation methodology

- Emulator remained visible. Rajiv confirmed a stable menu for Scene A, stable gameplay for Scene B, and stable Sky Garden for Scene C. Each accepted sample is three consecutive 5-second `--drain-stats` windows. Transitions and scene movement were excluded by the operator's stability confirmations.
- The retained raw log is `_perf_gpu_capture_20261003.log` (ignored by Git).
- `gpu-busy` is the union of submitted command-buffer execution intervals. `gpu-gap` is time between command buffers when none is executing. `gpu-thread-idle` is time `Thread_Gpu` waits for a submission or command; zero does not mean the physical GPU stayed busy.
- GPU zones use bottom-of-pipe timestamps. Adjacent identical `(zone, key)` marks are coalesced. The report prints only the 24 highest-time zone keys, so `n/frame` is a recorded-span count, not a guest draw or guaranteed dispatch count. Zone sums approximately reconcile with `gpu-busy`; small differences occur from timestamp and interval accounting.
- `draw-stats` and PM4 opcode accounting measure guest draw/dispatch activity. Exact host Vulkan draw-command count is not exposed. CPU phase and PM4 handler durations are cumulative CPU work; overlapping totals must not be added or treated as frame wall time.
- `suspend-wait` is separate from `tick-wait`. A guest thread waits on a semaphore released after the relevant scheduler tick completes. This synchronization wait overlaps GPU/producer work and is not additive to GPU busy/gap.
- Frame-time percentiles use integer-millisecond buckets; the final bucket collects longer samples. The display is a bucketed distribution, not an exact per-frame duration series.
- `readback` prints count/bytes, not a readback-wait duration. A distinct readback wait-ms value is unavailable.

## 5. Scene definitions

| Scene | Definition | Capture state |
|---|---|---|
| A — Fast | Menu, identified by Rajiv | Three accepted stable windows |
| B — Gameplay sample | Rajiv-confirmed stable gameplay; level name not confirmed | Three accepted stable windows; this was the slowest sampled gameplay state |
| C — Sky Garden | Rajiv-confirmed heavy gameplay area | Three accepted stable windows |

Scene B's measured FPS is lower than Sky Garden's, so it is not an intermediate point by measured frame time. Its exact area is unconfirmed; do not present it as the slowest reproducible named area.

## 6. Fast scene results

Three windows: 300, 297, and 300 frames over 5 seconds each; all report no illegal-instruction traps.

| Metric | Mean / median | Range | Evidence |
|---|---:|---:|---|
| FPS | 59.8 mean; 60.0 median window | 59.4–60.0 | MEASURED |
| Implied frame interval | 16.72 ms mean; 16.67 ms median | 16.67–16.84 ms | INFERRED as capture seconds / presents |
| Frame-time p50 | 16 ms median | 16 ms | MEASURED |
| Frame-time p95 | 17 ms median | 17 ms | MEASURED |
| Frame-time p99 | 17 ms median | 17–27 ms | MEASURED |
| Frames >=25 ms | 4 / 897 (0.45%) | 0–4 per window | MEASURED |
| GPU busy | 12.62 ms/frame | 11.54–13.73 | MEASURED |
| GPU gap | 4.12 ms/frame | 3.13–5.13 | MEASURED |
| Thread_Gpu idle | 0.25 ms/frame mean | 0–0.75 | MEASURED |
| Tick wait | 0.27 ms/frame | 0.08–0.61 | MEASURED |
| Suspend wait | 13.05 ms/frame | 12.82–13.47 | MEASURED; overlaps other work |
| Game draw zones | 8.73 ms/frame | 7.72–9.70 | MEASURED |
| Game dispatch zones | 2.05 ms/frame | 1.97–2.13 | MEASURED |
| Tiler | 0.11 ms/frame | 0.08–0.15 | MEASURED |
| Image + buffer copy zones | 1.06 ms/frame | 0.65–1.11 | MEASURED |
| Guest draws | ~620/frame | 594–655/frame | INFERRED from `draw-stats` rate / presents |
| Compute dispatch packets | ~77.6/frame | 77.1–77.8 | INFERRED from PM4 direct + indirect packet rates / presents |
| Total #UD | 0/frame | 0 in all windows | MEASURED |

## 7. Mid-load gameplay results

Scene B had 57, 55, and 49 frames in the three 5-second windows (161 presents / 15 seconds total). The level name was not confirmed; this is a representative held gameplay state, not Sky Garden.

| Metric | Mean / median | Range | Evidence |
|---|---:|---:|---|
| FPS | 10.73 mean; 11.0 median window | 9.8–11.4 | MEASURED |
| Implied frame interval | 93.17 ms mean | 87.72–102.04 ms | INFERRED as capture seconds / presents |
| Frame-time p50 | 83 ms median | 83–100 ms | MEASURED |
| Frame-time p95 | 100 ms | 100 ms | MEASURED; top histogram bucket |
| Frame-time p99 / max | 100 ms | 100 ms | MEASURED; top histogram bucket |
| Frames >=25 ms | 161 / 161 (100%) | 100% each window | MEASURED |
| GPU busy | 30.51 ms/frame | 29.33–32.84 | MEASURED |
| GPU gap | 62.78 ms/frame | 58.52–69.02 | MEASURED |
| Approximate GPU busy fraction | 32.8% of capture time | — | INFERRED: summed busy / 15 s |
| Approximate GPU gap fraction | 67.4% of capture time | — | INFERRED: summed gap / 15 s |
| Thread_Gpu idle | 0.00 ms/frame | 0 | MEASURED |
| Tick wait | 0.14 ms/frame | 0.07–0.18 | MEASURED |
| Submit / queue-lock wait | 1.32 / 0.006 ms/frame | 1.26–1.45 / 0.004–0.006 | MEASURED |
| Suspend wait | 71.27 ms/frame | 67.07–78.48 | MEASURED; overlaps GPU/producer activity |
| Game draw zones | 19.70 ms/frame | 19.06–20.96 | MEASURED |
| Game dispatch zones | 4.68 ms/frame | 4.49–5.05 | MEASURED |
| Tiler | 0.62 ms/frame | 0.55–0.68 | MEASURED |
| Image copy | 2.42 ms/frame | 2.23–2.81 | MEASURED |
| Buffer copy | 2.47 ms/frame | 2.29–2.68 | MEASURED |
| Guest draws | ~5,059/frame | 4,633–5,523 | INFERRED from `draw-stats` rate / presents |
| Compute dispatch packets | ~260/frame | 238–284 | INFERRED from PM4 direct + indirect packet rates / presents |
| Total #UD | 0/frame | 0 in all windows | MEASURED |

**MEASURED:** draw-phase CPU work was 919.6–921.1 ms/s; PM4 handler work was 978.5–978.9 ms/s. These are accumulated CPU-work measurements and overlap; they are not added or substituted for frame time.

## 8. Sky Garden results

Sky Garden windows contained 79, 77, and 79 frames (235 presents / 15 seconds). FPS was stable within 15.4–15.8 across windows. All three report total `#UD=0`.

| Metric | Mean / median | Range | Evidence |
|---|---:|---:|---|
| FPS | 15.67 mean; 15.8 median window | 15.4–15.8 | MEASURED |
| Implied frame interval | 63.83 ms mean | 63.29–64.94 ms by window | INFERRED as 5 seconds / presents |
| Frame-time p50 | 66 ms median | 66 ms | MEASURED |
| Frame-time p95 | 67 ms median | 67–80 ms | MEASURED |
| Frame-time p99 | 82 ms median | 68–83 ms | MEASURED |
| Frame-time max | 83 ms | 83 ms | MEASURED |
| Frames >=25 ms | 235 / 235 (100%) | 100% each window | MEASURED |
| GPU busy | 32.35 ms/frame | 29.91–34.48 | MEASURED |
| GPU gap | 31.61 ms/frame | 30.53–33.41 | MEASURED |
| Approximate GPU busy fraction | 50.7% of nominal capture time | — | INFERRED: summed busy / 15 s |
| Approximate GPU gap fraction | 49.5% of nominal capture time | — | INFERRED: summed gap / 15 s; busy + gap sum to 100.2% from interval accounting |
| Thread_Gpu idle | 0.00 ms/frame | 0 | MEASURED |
| Tick wait | 0.82 ms/frame | 0.44–1.54 | MEASURED |
| Readback-wait duration | Not separately emitted | — | Not available |
| Submit / queue-lock wait | 1.18 / 0.020 ms/frame | 1.14–1.22 / 0.01–0.03 | MEASURED |
| Suspend wait | 46.42 ms/frame | 45.84–47.25 | MEASURED; overlaps GPU/producer activity |
| Game draw zones | 22.36 ms/frame | 20.09–24.39 | MEASURED |
| Game dispatch zones | 4.44 ms/frame | 4.37–4.49 | MEASURED |
| Tiler | 0.60 ms/frame | 0.52–0.71 | MEASURED |
| Image copy | 2.14 ms/frame | 1.95–2.32 | MEASURED |
| Buffer copy | 2.21 ms/frame | 2.15–2.27 | MEASURED |
| Guest draws | ~3,899/frame | 3,785–3,997 | INFERRED from `draw-stats` rate / presents |
| Compute dispatch packets | ~266/frame | 257–276 | INFERRED from PM4 opcodes `0x15` + `0x16` rates / presents |
| Total #UD | 0/frame | 0 in all windows | MEASURED |

### Scene-level comparison

| Metric | Fast menu | Gameplay sample B | Sky Garden |
|---|---:|---:|---:|
| FPS | 59.8 | 10.73 | 15.67 |
| Frame time | 16.72 ms | 93.17 ms | 63.83 ms |
| GPU busy | 12.62 ms | 30.51 ms | 32.35 ms |
| GPU gap | 4.12 ms | 62.78 ms | 31.61 ms |
| Thread_Gpu idle | 0.25 ms | 0.00 ms | 0.00 ms |
| Tick wait / separate readback-wait duration | 0.27 ms / not reported | 0.14 ms / not reported | 0.82 ms / not reported |
| Game draw zones | 8.73 ms | 19.70 ms | 22.36 ms |
| Game dispatch zones | 2.05 ms | 4.68 ms | 4.44 ms |
| Tiler | 0.11 ms | 0.62 ms | 0.60 ms |
| Image + buffer copies | 1.06 ms | 4.89 ms | 4.34 ms |
| Guest draws/frame | ~620 | ~5,059 | ~3,899 |
| Compute dispatch packets/frame | ~77.6 | ~260 | ~266 |
| Total #UD/frame | 0 | 0 | 0 |

## 9. CPU/GPU feed analysis

- **MEASURED — Sky Garden:** frame interval was 63.83 ms; GPU busy was 32.35 ms and GPU gap 31.61 ms. Their sum closely matches the interval, though the independently aggregated counters total about 0.2% above nominal wall time. The GPU busy fraction was about half the capture interval.
- **MEASURED — Gameplay sample B:** frame interval was 93.17 ms, GPU busy 30.51 ms, and GPU gap 62.78 ms. The GPU work is close to Sky Garden's, while B has about 31.2 ms/frame more GPU gap and about 29.3 ms/frame more frame time. This is the strongest measured explanation for why B was slower than Sky Garden.
- **MEASURED — Sky Garden:** draw-phase CPU work was 899.5–909.2 ms/s (weighted 899.9 ms/s); PM4 handler work was 970.0–976.6 ms/s (weighted 973.3 ms/s). These independently accumulated counters may overlap and are not CPU wall time per frame.
- **MEASURED — Sky Garden:** submit took about 1.18 ms/frame, queue-lock wait 0.02 ms/frame, tick wait 0.82 ms/frame, and `Thread_Gpu` idle was zero. `suspend-wait` was 46.42 ms/frame, a separate synchronization measure that overlaps the GPU/producer timeline.
- **INFERRED:** both GPU execution and GPU feed gaps limit Sky Garden. GPU execution alone exceeds the 16.67 ms 60-FPS budget, while the approximately 31.6 ms/frame non-executing interval also materially lengthens frames. In B, the much larger GPU gap is the dominant difference from Sky Garden.
- **INFERRED:** high draw/PM4 CPU work while command buffers have long gaps is consistent with CPU command-generation/feed starvation. The current counters do not identify whether each gap is caused by CPU preparation, guest memory-fault work, or a dependency/synchronization wait.
- **HYPOTHESIS:** per-draw preparation and protected-memory fault handling contribute to B's longer feed gaps. The aggregate counters cannot prove either on the critical path.

## 10. Compute hotspots

Sky Garden's aggregate `game-dispatch` zone was 4.44 ms/frame (about 13.7% of measured GPU busy). PM4 opcode rates imply about 266 direct plus indirect dispatch packets per presented frame. That total is not a per-hash count.

| Visible compute zone key | GPU time | Recorded spans/frame | Mean per span | Evidence / caveat |
|---|---:|---:|---:|---|
| `3276e23cce1be33c` | ~0.646 ms/frame | ~7.0 | ~92 µs | MEASURED zone key/span statistics |
| `173677e49330bd65` | ~0.496 ms/frame | ~2.8 | ~174 µs | MEASURED zone key/span statistics |
| `01d6f21611218e74` | ~0.443 ms/frame | ~1.0 | ~443 µs | MEASURED zone key/span statistics |
| `a572ee17a880e71c` | ~0.277 ms/frame | ~13.8 | ~20 µs | MEASURED zone key/span statistics |

The key rows are only the compute entries visible in the top-24 mixed-zone output; they are not a complete top-20 compute-shader list. The `b57099b84b0b69e3`, `900aba8df9448d3d`, and `838a104ff34fa882` historical candidates did not appear in the emitted Sky Garden top-24 rows. That does not establish zero cost. Adjacent same-key zones coalesce, so their span counts are not exact dispatch counts. Dispatch dimensions, workgroup sizes, invocation counts, and bound target dimensions were not emitted.

## 11. Draw hotspots

- **MEASURED — Sky Garden:** aggregate game-draw time was 22.36 ms/frame (about 69.1% of GPU busy). The highest visible game-draw zone key was `ef31694ed8d87754` at 6.42 ms/frame, about 12.33 recorded spans/frame and 521 µs/span. Its logged render-area annotation averaged about 1.09 Mpx; this is not a target width/height or full surface dimension.
- Other highest visible game-draw keys were `13495e6ee1376edc` (~3.75 ms/frame), `4a6940ae373ad4e6` (~1.50), `6996d4e234bd5b8e` (~1.34), and `3cf24e1b171c5ee5` (~1.22). These are key-zone GPU times, not necessarily one-to-one shader-pair timings.
- **MEASURED — guest draw rate:** about 3,899 guest draws/frame. Top draw-stat rows repeatedly included PS `0000000000000000` (~1,279/frame, with VS key varying between `95b66894c562bfee` and `70c651c06dbbf1ce`), PS `c924afb0821b68c8` / VS `977341379220c149` (~379/frame), PS `f112e0ba527945ad` (~347/frame; VS key varied between `8e4a146647cf8029` and `8812d3a78cf7b565`), and PS `ca11de665702d6d9` / VS `7c5921c705e23c93` (~205/frame). These counts rank guest draws, not GPU time.
- The top GPU-time key `ef31694ed8d87754` is not in the emitted top six by guest draw count, so its exact guest draw count is unavailable.
- **Why counts differ:** `draw-stats` counts guest PM4 draw packets; GPU-zone counts are timestamped spans after adjacent identical `(zone,key)` marks coalesce. Sky Garden had about 3,899 guest draws/frame but roughly 483 `game-draw` zone spans/frame. They are different units. Host Vulkan draw-command count is not exposed.

## 12. Tiler analysis

**MEASURED — Sky Garden:** tiler zones were 0.60 ms/frame across about 19.7 recorded spans/frame (about 30.5 µs/span). This is about 1.9% of GPU busy and not a leading measured cost in this scene. Span count is not guaranteed to equal dispatch count. Tiler target identities, surface dimensions, formats, and processed bytes are unavailable from the current counters.

## 13. Copy analysis

**MEASURED — Sky Garden:**

| Copy zone | GPU time | Recorded spans/frame | Bytes |
|---|---:|---:|---|
| Image copy | 2.14 ms/frame | ~25.7 | Not reported per zone |
| Buffer copy | 2.21 ms/frame | ~37.3 | Not reported per zone |
| Combined | 4.34 ms/frame | ~63.0 | Not reported per zone |

The copy zones are part of the measured GPU-zone execution and account for about 13.4% of GPU busy; they are real GPU work in this sample, though the counters do not quantify transfer bytes or overlap with CPU work. `readback` reports 1.2–1.5 MiB per 5-second window, not total GPU-copy traffic and not a wait duration. Upload/readback copy time is not separately reported as a GPU zone.

## 14. Internal resolution

**Not determined.** Exact main color, G-buffer, depth, and dominant compute-target dimensions and any dynamic-resolution range are not exposed by these counters. GPU-zone `area=...Mpx` annotations describe marked render area per span, not the underlying target width and height. Sky Garden's ~1.09 Mpx annotation on one draw key must not be treated as internal resolution or proof of 4K rendering.

## 15. Page-fault observations

**MEASURED — Sky Garden:** for the 20 write-fault sites printed in each 5-second interval:

| Window | Printed-site fault events | Rate | Sum of printed-site durations |
|---|---:|---:|---:|
| 1 | 297,788 | 59.6k/s | 4.21 s |
| 2 | 291,872 | 58.4k/s | 4.11 s |
| 3 | 295,043 | 59.0k/s | 4.26 s |

Durations are accumulated across faulting threads/sites and can overlap; they are not seconds added to frame time. The list is limited to the 20 highest-time sites. **INFERRED:** faults are a substantial CPU-work signal but the available telemetry does not estimate their critical-path wall-time effect, so the volume alone does not make memory/fault handling the primary verdict.

## 16. Top five measured costs

Sky Garden GPU-zone ranking. Percentages use measured GPU busy (32.35 ms/frame) as denominator; small percentage mismatch reflects aggregate zone/busy accounting.

| Rank | Component | Identifier | ms/frame | % GPU time | Evidence |
|---:|---|---|---:|---:|---|
| 1 | Game draw aggregate | All visible game-draw keys | 22.36 | 69.1% | MEASURED |
| 2 | Game dispatch aggregate | All visible game-dispatch keys | 4.44 | 13.7% | MEASURED |
| 3 | Buffer copy | All buffer-copy zones | 2.21 | 6.8% | MEASURED |
| 4 | Image copy | All image-copy zones | 2.14 | 6.6% | MEASURED |
| 5 | Tiler | All tiler zones | 0.60 | 1.9% | MEASURED |

A separate frame-pacing cost is `gpu-gap`: 31.61 ms/frame in Sky Garden and 62.78 ms/frame in the slower, unnamed Scene B. It is not GPU execution and is not included in the table above.

## 17. Bottleneck verdict

**MIXED.**

- **MEASURED:** Sky Garden spends about 32.35 ms/frame executing GPU commands and 31.61 ms/frame in GPU gaps, against a 63.83 ms/frame interval. The GPU execution component alone is almost twice the 16.67 ms 60-FPS budget.
- **MEASURED:** Scene B has similar GPU execution (30.51 ms/frame) but a much larger gap (62.78 ms/frame), producing 93.17 ms/frame. This is the slowest sampled gameplay state, though its level is unnamed.
- **INFERRED:** GPU execution prevents 60 FPS even with perfect feeding; CPU-side command generation or synchronization gaps add a second substantial limit. The data therefore supports MIXED rather than a pure GPU or pure CPU classification.
- **HYPOTHESIS:** protected-memory faults or guest draw preparation account for some of B's excess GPU gap. Current aggregate counters cannot separate those causes from synchronization dependencies.

## 18. Recommended next investigation

**Trace the 62.78 ms/frame GPU gap in the slower gameplay sample (Scene B) to its CPU producer or dependency wait.** This is the largest measured frame-time opportunity, is much larger than the 1.32 ms/frame submit wait, and appears alongside about 0.92 CPU-seconds/second of draw-phase work and 0.98 CPU-seconds/second of PM4 handler work. Correlate GPU-gap intervals with the command-producing thread, guest-read/write fault handlers, and suspend-point/tick dependencies in the same held scene; first record the scene location so the sample can be reproduced. This is an investigation recommendation only. It does not imply that eliminating the gap alone would reach 60 FPS, because measured GPU work is already about 30.5 ms/frame in that sample.

## 19. Limitations / uncertain measurements

- Scene B's level/location is unconfirmed. It is the slowest sampled gameplay state, while Sky Garden is the named, operator-confirmed heavy-scene sample.
- GPU zones emit only the top 24 mixed-category keys and coalesce adjacent same-key marks. A complete top-20 compute list and exact per-hash dispatch count are unavailable.
- Exact host Vulkan draw-command count, per-copy GPU bytes, readback-wait duration, internal target dimensions/resolution range, and tiler target details are unavailable from current counters.
- CPU phase, PM4 handler, suspend-wait, and fault-site durations overlap or aggregate across threads; they are not additive critical-path frame time.
- Diagnostic instrumentation overhead was not calibrated against an uninstrumented control.
- Frame-time percentiles are integer-ms buckets; do not treat them as exact frame-time values.
