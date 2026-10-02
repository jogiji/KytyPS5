# KytyPS5 Upstream Integration & Astro Bot Certification (2026-10-02)

## 1. Base Information
- **Integration Branch:** `exp/astrobot-upstream-20261002`
- **Previous Certified Fastpath Base SHA:** `296240c473e708fe26ebc414eac85ffb7a711220` (`exp/astrobot-vrsqrtps-trampoline`)
- **Upstream Target SHA Integrated:** `70a475ec8cd08575a41b31d234578e9c3e672dfa` (through PR #966 audio accumulated timing drift fix)
- **Primary Remotes:**
  - `personal`: `https://github.com/jogiji/KytyPS5.git` (writable)
  - `upstream`: `https://github.com/KytyPS5/KytyPS5.git` (read-only)
  - `perfref`: `https://github.com/micolee221/KytyPS5.git` (read-only)

---

## 2. Integrated Upstream Changes & Conflict Resolutions
- Merged upstream commits up to `70a475ec8cd08575a41b31d234578e9c3e672dfa`:
  - Ray tracing SPIR-V emission (`spirvEmitterBvh.cpp`) and ray tracing tests.
  - Image atomic compare swap 32-bit (`IMAGE_ATOMIC_CMPSWAP`).
  - Audio popping and time-stretching drift fixes in output.
- All certified Intel fastpaths preserved:
  - `EXTRQ` inline rewrite trampoline.
  - `VRSQRTPS` load-time trampoline with exact `sqrt` semantics and jump-table discovery.
  - SysV Red Zone leaf-frame protection (`--redzone`).
- All `micolee` performance architecture features preserved:
  - Async submit queue thread (`--async-submit`).
  - Record thread for command submission (`--record-thread`).
  - Speculative draw prep (`--speculative-draws`).
  - Selective/epoch BDA synchronization (`--bda-sync Selective`).
  - GPU DCC clear (`--dcc-gpu-clear`).
  - GPU mesh indirect draw builder (`--gpu-mesh-indirect`).
  - Label flush interval batching (`--label-flush-interval-us 2000`).
  - GPU synchronization zones (`KYTY_GPU_ZONES=1`).

---

## 3. Conformance & Test Suite Fixes
- **Shader Recompiler Compute Tests (100% Passed):**
  - Resolved `VectorWaveOrReduceWithUnlaunchedLanesW64T20`: Added `lane_count == 2` fallback to low half in `EmitLaunchedLaneAtOrBelow` when high half has no launched lanes.
  - Resolved `CheckWave64WholeWaveResults`: Preserved upstream 2-shuffle pattern in `EmitReadLane` and enabled ballot caching for native single-half shaders in `ValueEmitContext::Ballot`.
  - Resolved `VectorSinCosMaxFiniteSpecialCases`: Added `large_finite` clamp in `EmitTrigCycleF32` to avoid `NaN` generation from `GLSLstd450Fract` on extreme negative finite floats.
  - Resolved `ConstantPropagation`: Guarded `ObservedOnlyWhere` against empty instruction uses.
  - Resolved `SrtWalker`: Maintained full branch-reachability evaluation in `SrtEvaluator::RefreshFlatBuffer`.
- **AudioOut2 Port Tests:**
  - Resolved stack out-of-bounds read in `TestSynchronousDevicePushBypassesModelledQueue` and `TestAsynchronousDevicePushKeepsQueueBounded` by sizing test PCM arrays to `512 * 2` floats to match the 2-channel float format.

---

## 4. Test Suite Results
- **Test Framework:** CTest (`ctest --test-dir _Build/windows --output-on-failure -j 1`)
- **Total Tests:** 64
- **Passed:** 64 (100%)
- **Failed:** 0
- **Pass Rate:** 100%

---

## Runtime Certification

Manual runtime validation performed by Rajiv.

- Astro Bot PPSA21567 v01.018 boots successfully.
- Menus and gameplay are functional.
- Sky Garden is playable.
- No obvious tree/foliage/visual regression observed during manual testing.
- Observed frame rate varies approximately 15–50 FPS depending on scene/load.
- 64/64 automated tests already pass.

Verdict:
CERTIFIED — NEW WORKING ASTRO BOT BASELINE
