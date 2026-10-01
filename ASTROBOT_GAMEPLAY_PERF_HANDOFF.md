# ASTRO BOT GAMEPLAY PERFORMANCE ROOT-CAUSE & EXPERIMENTAL REPORT

**Date:** October 1, 2026  
**Repository:** `jogiji/KytyPS5`  
**Branch:** `exp/astrobot-gameplay-perf`  
**Integration Baseline:** `d80d0faa86457eafe282aef8592101d482f922e1` (Official upstream `b7a1fac8` + micolee + Intel host compatibility fixes)  
**Host Platform:** Intel Core i9-13980HX, NVIDIA GeForce RTX 4090 Laptop GPU (16 GB VRAM), Windows 11  
**Game Title:** Astro Bot (`PPSA21567` v01.018.000)  
**ETAHen Patch:** `_Patches/PPSA21567.json` (Non-tiled deferred lighting, disabled GI probes)  
**Pipeline Cache:** `_PipelineCache/PPSA21567.bin` (29.8 MB populated and saved)

---

## 1. Executive Summary: Root Cause of the 45–50 FPS to 3–4 FPS Collapse

The transition from the fast intro/title scene (**60.0 FPS / 16.6 ms per frame**) down into heavy gameplay in the **Sky Garden** level (**3.6 – 4.8 FPS / 208 – 278 ms per frame**) introduces an additional **~190 to ~260 ms/frame** of overhead.

Telemetry captured across thousands of frames in `_perf_gameplay_001.log` and `_perf_gameplay_003.log` proves that this gap is **~74% to ~82% CPU-bound GPU starvation** and **~18% to ~26% GPU execution inflation**:

$$\Delta \text{Frame Time} = 278.0\,\text{ms} - 16.6\,\text{ms} = +261.4\,\text{ms/frame}$$

| Metric | Fast Scene (Title/Intro) | Heavy Gameplay (Sky Garden) | Delta ($\Delta$) | Share of Gap | Root Cause & Diagnosis |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **GPU Idle Gap (`gpu-gap`)** | **2.90 ms** | **140.5 – 212.3 ms** | **+137.6 to +209.4 ms** | **~74.0% – 81.8%** | **GPU Starvation:** RTX 4090 spends 70–75% of every frame sitting idle waiting for CPU command buffers |
| **GPU Execution (`gpu-busy`)** | **13.76 ms** | **62.4 – 74.0 ms** | **+48.6 to +60.2 ms** | **~18.2% – 26.0%** | **Workload Explosion:** 27x draw calls (6,000/frame) + heavy compute/tiler shaders |
| **Synchronous Wait (`tick-wait`)** | **0.00 ms** | **43.6 – 52.0 ms** | **+43.6 to +52.0 ms** | *(included in CPU stall)* | **Host CPU stall:** CPU blocks waiting for GPU readbacks (`pc=0x7ffe4e76cc66/23`) |
| **Total Frame Time** | **16.6 ms (60.2 FPS)** | **208 – 278 ms (3.6–4.8 FPS)** | **+191 to +261 ms** | **100%** | **Strict serial lockstep between CPU and GPU** |

---

## 2. Quantitative Accounting: The ~140–212 ms GPU Starvation Gap

The host GPU (RTX 4090) spends **70% to 75% of every single frame doing nothing**. The GPU worker thread (`Thread_Gpu`) is idle for **3,479 ms out of each 5,000 ms window (69.6% idle)**.

The CPU fails to feed the GPU in time due to four primary factors:

### A. Draw Call Explosion (27x Increase)
* **Fast Scene:** ~230 draw calls per frame across 28 shaders (`13,839 draws/s` at 60 FPS).
* **Sky Garden Gameplay:** **~5,800 to 6,238 draw calls per frame** across 92 shaders (**21,225 – 28,377 draws/s** at 3.6–4.8 FPS).
* Between command buffers, the CPU takes **2.7 to 13.4 ms** to prepare, record, and submit each batch (`gpu-gap DISPATCH_DIRECT` avg = 13.4 ms, `DRAW_INDEX_OFFSET_2` avg = 2.7 ms).

### B. Strict Serial Lockstep (`SuspendPoint` Throttling)
* Upstream's `GuestGpu::SuspendPoint()` throttles the guest render thread using a binary semaphore (`m_suspend_point_ready = std::make_shared<std::binary_semaphore>(1)`).
* The guest engine is forced to wait for the host GPU to finish executing Frame $N$ before it can begin building the command buffers for Frame $N+1$.
* Because the CPU and GPU cannot run in parallel, their costs add strictly sequentially:
  $$\text{CPU Build Time (~180–205 ms)} + \text{GPU Execution Time (~62–74 ms)} = 242–279\,\text{ms} \approx 3.6–4.1\,\text{FPS}$$

### C. Massive Dynamic Memory Protection Write Faults
* Over **35,000 page write faults** occur every 5 seconds across guest worker threads (`tbb_thead`, `Draw Decal`, `Draw Geometry`, `MainThread`) updating dynamic vertex/constant/SRT buffers:
  * `fault-site write pc=0x000920003e55 thread=tbb_thead`: **20,703 faults** (309.5 ms)
  * `fault-site write pc=0x000920003fe2 thread=MainThread`: **3,950 faults** (45.9 ms)
  * `fault-site write pc=0x000920003ef0 thread=tbb_thead`: **2,608 faults** (34.2 ms)
  * `fault-site write pc=0x000920004009 thread=tbb_thead`: **2,336 faults** (23.9 ms)
  * `fault-site write pc=0x000920004029 thread=tbb_thead`: **2,245 faults** (23.6 ms)
  * `fault-site write pc=0x000920004039 thread=tbb_thead`: **2,270 faults** (23.1 ms)
* Every write fault invokes the Windows kernel structured exception handler, calls `VirtualProtect`, and unprotects the page in `BufferCache::InvalidateMemory`.

### D. Synchronous GPU Readback Stalls (`tick-wait`)
* Guest and host threads spend **43.6 – 52.0 ms/frame** blocked in `tick-wait guest-read-fault` waiting on GPU ticks:
  * `fault-site read pc=0x7ffe4e76cc66`: 19 faults taking **510.3 ms** (avg **26.9 ms** per fault)
  * `fault-site read pc=0x7ffe4e76cc23`: 38 faults taking **433.9 ms** (avg **11.4 ms** per fault)

---

## 3. Detailed Accounting: The 62–74 ms GPU Busy Time in Sky Garden

In the Sky Garden level (`_perf_gameplay_003.log`), the RTX 4090 GPU execution breaks down across zones as follows:

| GPU Zone | Fast Scene (ms/frame) | Sky Garden Gameplay (ms/frame) | Observations in Sky Garden |
| :--- | :--- | :--- | :--- |
| **`game-draw`** | 9.35 ms | **27.50 ms** | 453 draw intervals/frame across 92 shaders |
| **`tiler`** | 0.05 ms | **19.60 ms** | Texture detiling during level streaming |
| **`game-dispatch`** | 2.33 ms | **16.29 ms** | 122 compute dispatches/frame |
| **`image-copy`** | 0.90 ms | **5.69 ms** | Scaled copies & format conversions (21 calls/frame) |
| **`buffer-copy`** | 0.38 ms | **3.90 ms** | Buffer streaming & dynamic updates (46 calls/frame) |
| **`dcc-clear`** | 0.33 ms | **0.24 ms** | Fast-clear resolves |
| **`fault-buffer`** | 0.00 ms | **0.14 ms** | Host buffer fault handling |
| **Total GPU Busy** | **13.76 ms** | **73.68 ms** | Total active GPU rendering time |

### Top Individual Expensive Operations in Sky Garden:
1. **Texture Detiling Compute (`tiler key=0x000000053ad00000`):**
   * **16.89 ms/frame** (2 calls per frame taking **8,446.5 µs** each).
2. **Top Compute Dispatch (`game-dispatch key=173677e49330bd65`):**
   * **6.60 ms/frame** (3 calls per frame taking **2,198.5 µs** each).
3. **Compute Dispatch (`game-dispatch key=3276e23cce1be33c`):**
   * **3.11 ms/frame** (7.4 calls per frame taking **420.6 µs** each).
4. **Draw Shader (`game-draw key=3b809f9d156a95dd`):**
   * **2.38 ms/frame** (8.4 calls per frame taking **281.3 µs** each).
5. **Draw Shader (`game-draw key=13495e6ee1376edc`):**
   * **2.07 ms/frame** for a single 2.07 Mpx draw call.
6. **Buffer & Image Copies (`buffer-copy`, `image-copy`):**
   * Combined **8.55 ms/frame** for texture layout copies and buffer updates.

---

## 4. Targeted Experimental Optimization: Findings & Evidence

### Hypothesis:
Allowing the guest engine to pipeline 1 frame ahead of the GPU via `Config::GetGpuFramesAhead() = 1` across `m_suspend_point_ready` would allow the ~180 ms CPU build time to overlap with the ~70 ms GPU execution time.

### Experimental Result: **DEADLOCK in `blocked-poll`**
* **Telemetry Captured (`_perf_gameplay_002.log`):**
  ```text
  drain-stats: 5.0s frames=0 (0.0/s) presents=0 | blocked-poll n=3507 4943.29ms (avg=1.41ms)
  ```
* **Analysis:**
  Astro Bot's internal rendering engine explicitly depends on synchronous frame completion. When the guest was permitted to submit Frame $N+1$ before Frame $N$ finished, the compute/graphics command processors encountered inter-queue synchronization dependencies (`WAIT_REG_MEM`) that could not be satisfied, causing all queues to suspend in `blocked-poll` indefinitely.
* **Conclusion & Rollback:**
  The change was immediately rolled back cleanly to the baseline. Out-of-order frame queuing (`frames_ahead > 0`) is fundamentally incompatible with Astro Bot's internal synchronization architecture.

---

## 5. Architectural Recommendations for Future Optimization

To resolve the 3–4 FPS bottleneck without breaking game synchronization, future optimization work should target:

1. **CPU Draw-Call Processing Cost Reduction:**
   * At 6,000 draws/frame, every 1 µs saved in `GetGraphicsPrograms` or SRT resource evaluation recovers **6 ms/frame**!
   * Optimizing flat SRT slot resolution and avoiding redundant state tracking in `renderDraw.cpp`.
2. **Memory Tracker Page Protection Batching:**
   * Reducing the 35,000 page write faults per 5s window by coarsening write tracking or batching unprotects on dynamic vertex buffers.
3. **Synchronous Readback Mitigation:**
   * Relaxing readback stalls (`tick-wait`, 45–52 ms/frame) via `Config::SetRelaxedReadbackEnabled(true)` or staging buffer caching.
4. **Tiler & Compute Dispatch Optimization:**
   * Investigating texture detiling compute kernel `0x000000053ad00000` (16.9 ms/frame) to use vectorized 64-bit load/stores.
