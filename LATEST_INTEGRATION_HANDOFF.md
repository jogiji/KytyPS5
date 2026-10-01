# Latest KytyPS5 Upstream + micolee Performance + Intel Compatibility Integration Handoff

## 1. Base Information
- **Known-Good Reference Base SHA:** `668388a842ce117b8b8ae5cef8c9ff8357aea839` (branch `exp/astrobot-micolee-intel`, preserved untouched)
- **micolee Performance Fork Base SHA:** `26d62f802e7ade8362569dcf989d347ccb4014c4`
- **Upstream Merge Base SHA:** `59a17604274e55c0380bee823c61d3fd0a4717ff`
- **Upstream SHA Merged:** `b7a1fac898be93bfe0c282752a7a386fd5202486` (`KytyPS5-2026-09-30-b7a1fac`)

---

## 2. Merge Details
- **Upstream Commits Integrated:** 16 commits (`650dc1e0` through `b7a1fac8`):
  1. `650dc1e0` graphics: upload streamed texture mips from sparse reservations
  2. `f9fd400d` memory: validate sparse uploads against the guest address-space owner
  3. `5bf9218b` ampr: keep commands in one append-ordered sequence
  4. `c045f089` ampr: schedule address waits without blocking other priorities
  5. `da9b38af` ampr: reuse file-read staging storage on the APR worker
  6. `18a5f04d` graphics: retain oversized readbacks through GPU completion
  7. `2650478d` profiling: preserve emulator fault handlers when Tracy connects
  8. `9d4da483` Linux: Strip debug symbols from launcher (#913)
  9. `af4edc28` window: explain why each GPU was rejected in device selection (#910)
  10. `05057c94` launcher: configure and preview DualSense lightbar color (#912)
  11. `840b9f57` Fix unnecessary queue drains in predication and suspend points
  12. `48b52d31` libs: libScePngEnc on stb_image_write
  13. `8ba2caf5` videoout: WQHD detection and VRR peg/unpeg
  14. `17100ec9` shareplay: return initialized connection information
  15. `bc193f52` shader: V_CMPX_LT_I16 + fix SDWA with SEXT
  16. `b7a1fac8` input: correct gyro orientation drift with the accelerometer (#899)

- **Conflicted Files & Resolutions:**
  1. `src/graphics/guest_gpu/command_processor/commandProcessor.h`:
     - *Resolution:* Preserved micolee's `BufferFlushIfGpuIdle()`, `BufferFlushForInterrupt()`, and `BufferFlushAndWait()` required by `pm4Handlers.cpp` and `SynchronizePredicate`.
  2. `src/graphics/guest_gpu/graphicsRun.h`:
     - *Resolution:* Adopted upstream's clean asynchronous `SuspendPoint()` and public `WaitForIdle()`; added inline alias `Done() { SuspendPoint(); }` for compatibility; removed obsolete `WaitForSubmissionsBefore`.
  3. `src/graphics/guest_gpu/graphicsRun.cpp`:
     - *Resolution:* Adopted upstream's `SuspendPoint()`; retained micolee's `BufferFlushIfGpuIdle()`, `BufferFlushForInterrupt()`, and `BufferFlushAndWait()`; adopted upstream's Z-pass comment while preserving micolee's `SynchronizePredicate`.
  4. `src/graphics/host_gpu/renderer/cache/bufferCache.cpp`:
     - *Resolution:* Retained micolee's full in-flight download tracking and memory tracker finalization (`token`, `pages`, `owner` lifetime extension), which is functionally identical to and strictly more complete than upstream's `18a5f04d`.
  5. `src/kernel/memory.h`:
     - *Resolution:* Replaced deprecated `TryReadPrtBacking` with upstream's generalized `TryReadSparseBacking`; preserved micolee's `ReadGuestOnGpuThread` and `TryReadGuestPlainOnGpuThread`.
  6. `src/libs/libAmpr.cpp`:
     - *Resolution:* Adopted upstream's asynchronous `CommandEngine` worker and append-ordered sequence architecture; integrated micolee's texture streaming thrash detection (`DebugAprRead`, `NoteRepeatedRead`, and `Graphics::NoteStreamingThrash()`) directly into the new `ExecuteCommand` read pathway.

---

## 3. Intel Compatibility Verification
- **`SceSndzAudioOutMain` crash remains resolved:** **YES**
  - Confirmed via live runtime testing on Intel Core i9-13980HX.
  - Red Zone patcher (`src/loader/redZonePatcher.cpp`) and AMD instruction emulator (`src/loader/x64InstructionEmulator.cpp`) preserved intact.
  - Zero `#UD` exception corruption of SysV Red Zone leaf frames.

---

## 4. Test Suite Results
- **Framework:** CTest (`ctest --test-dir _Build/windows --output-on-failure`)
- **Total Tests:** 63
- **Passed:** 61 (97%)
- **Failed:** 2 (`36 - shader_recompiler_compute`, `48 - shader_recompiler_compute_hw_bounds`)
  - *Failure Detail:* Pre-existing Vulkan subgroup reduction edge case (`VectorWaveOrReduceWithUnlaunchedLanesW64T20`) on Nvidia Ada Lovelace (native 32-lane subgroups vs emulated Wave64 with 20 unlaunched lanes).
- **Skipped:** 0
- **Key Passing Tests:**
  - `audio_out2_port`: PASSED
  - `virtual_memory_allocation`: PASSED
  - `kernel_file_system`: PASSED
  - `png_enc`: PASSED
  - `resource_tracking`: PASSED
  - `resource_materialization`: PASSED
  - `buffer_cache_ranges`: PASSED
  - `texture_cache_image_overlap`: PASSED
  - `gpu_tiler`: PASSED
  - `memory_tracker`: PASSED
  - `pad_haptics`: PASSED

---

## 5. Build Artifacts
- **Emulator Binary:** `C:\Users\rajiv\Development\KytyPS5-perf-astrobot\_Build\windows\install\kyty_emulator.exe`
- **Launcher Binary:** `C:\Users\rajiv\Development\KytyPS5-perf-astrobot\_Build\windows\install\launcher.exe`
- **Build Type:** RelWithDebInfo (`clang-cl 22.1.3`, `lld-link 22.1.3`, `Ninja 1.13.2`, `Qt 6.11.2`)

---

## 6. Astro Bot Runtime Certification
- **Environment:**
  - CPU: Intel Core i9-13980HX (24C / 32T)
  - GPU: NVIDIA GeForce RTX 4090 Laptop GPU (16GB VRAM)
  - Game: Astro Bot (`D:\ps5\PPSA21567 - ASTRO BOT_extracted`)
  - Command: `.\_Build\windows\install\kyty_emulator.exe --game "D:/ps5/PPSA21567 - ASTRO BOT_extracted" --gpu 0 --amd-cpu --redzone`
- **Observations:**
  - Intro Behaviour: PlayStation / Team ASOBI startup and video intro play smoothly through completion (`AvPlayer video started playing` -> `AvPlayer video stopped`).
  - Audio Out: Clean audio playback with no exceptions or ring buffer pointer faults in `SceSndzAudioOutMain`.
  - Shader Pipeline: Smooth background compilation and execution of vertex, pixel, compute, and geometry shaders (e.g. `Shaders: VS 18 | PS 27 | CS 38 | GS 2 | LS 0 | HS 0 | TES 0`).
  - Visual Regressions: None observed.
  - Stability: 100% stable; survived extended run beyond all intro milestones without crashing.

---

## 7. Git & Remote Status
- **Integration Branch:** `exp/astrobot-micolee-intel-latest`
- **Local HEAD SHA:** `d80d0faa86457eafe282aef8592101d482f922e1`
- **Remote Branch:** `personal/exp/astrobot-micolee-intel-latest`
- **Remote SHA:** `d80d0faa86457eafe282aef8592101d482f922e1` (verified 1:1 match)
- **Reference Remotes:** `upstream`, `perfref`, `intelref` remain strictly `DISABLED` for push.

---

## 8. Recommendation
The integrated build on branch `exp/astrobot-micolee-intel-latest` is **fully certified and suitable** to become the new baseline for upcoming profiling and performance analysis of the 4–5 FPS gameplay drop.
