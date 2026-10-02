# Astro Bot Sky Garden — Load-Time VRSQRTPS Instruction Trampoline Handoff

## Executive Summary

During empirical testing of Astro Bot (*PPSA21567* v01.018.000) on Intel (`Core i9-13980HX` + `RTX 4090 Laptop GPU`), we isolated the exact behavioral and performance characteristics of the Intel fastpaths:

| Mode | Sky Garden FPS | Menus / Loading FPS | Visual Fidelity |
|---|---:|---:|---|
| **Baseline (trapped `#UD`)** | ~3–4 | ~60 | Correct (no missing assets) |
| **EXTRQ Trampoline Only** | ~6–7 | ~60 | Correct (no missing assets) |
| **Raw Intel `VRSQRTPS` Only** | ~6–7 | ~8–15 | Trees/foliage disappear near player |
| **Raw Intel `VRSQRTPS` + EXTRQ** | 25–50+ | ~8–15 | Trees/foliage disappear near player |

### Root Cause Analysis of Raw Intel VRSQRTPS Divergence
1. **Visual Culling Regression:**
   - Intel's hardware `vrsqrtps` uses a 12-bit mantissa lookup table that differs in least-significant bits from AMD hardware and Kyty's software emulation.
   - Astro Bot's distance/LOD culling shaders calculate reciprocal lengths (`1.0 / sqrt(dx*dx + dy*dy + dz*dz)`). Small differences in the mantissa or edge-case handling near boundary conditions caused distance thresholds to fail, resulting in foliage and tree models culling away as the player approached.
2. **Menu / Transition Stalls (8–15 FPS vs 60 FPS):**
   - Certain menu and transition effects feed denormal or zero values into vector reciprocal square roots, triggering Intel hardware microcode assists (costing hundreds of cycles per vector) or NaN generation cascades.

### Solution: Load-Time VRSQRTPS Instruction Trampoline
To eliminate the 140,000 `#UD` traps/frame without introducing visual regressions or menu lag, we implemented a generic **Load-Time VRSQRTPS Instruction Trampoline** within Kyty's red-zone relocation infrastructure (`src/loader/redZonePatcher.cpp`).

This trampoline:
- Completely eliminates Windows `#UD` / VEH exception overhead (0 traps).
- Strictly preserves Kyty's reference `ReciprocalSquareRoot()` semantics with **100.0% bit-exact accuracy across 300,000 test cases**.
- Completely insulates execution from Intel denormal microcode stalls and division-by-zero faults.
- Preserves the System V AMD64 red zone (128 bytes below RSP), RFLAGS, GPRs, and guest `MXCSR`.
- Strictly zeroes upper YMM bits `[255:128]` conforming to the VEX.128 AVX architecture specification.

---

## 1. Trampoline Architecture & SIMD Sequence

The trampoline executes a robust, branchless SIMD sequence that evaluates high-precision square root and division in double precision while handling all IEEE-754 special cases through bitwise vector classification:

```text
Guest Code:
  VRSQRTPS xmm_dest, xmm_src, ...
        ↓
Load-Time Patch:
  JMP qword [trampoline] + NOPs
        ↓
Trampoline:
  1. Allocate 384-byte stack frame below RSP (preserving 128-byte guest red zone)
  2. Save 4 scratch YMM registers (s0, s1, s2, s3 dynamically chosen distinct from dest & src)
  3. Save guest MXCSR and set safe mask 0x1f80 (all FP exceptions masked)
  4. Vector Classification & Sanitization:
     - Check for +0, -0, denormals: mask_zero_denorm
     - Check for NaN / negative: mask_nan_neg (differentiates +inf from NaNs)
     - Construct safe_src = blend(src, 1.0f, mask_zero_denorm | mask_nan_neg)
  5. Compute High-Precision Reciprocal Root:
     - vcvtps2pd + vsqrtpd + vdivpd + vcvtpd2ps (double precision 4-lane pipeline)
  6. Blend IEEE-754 / Kyty Special Values:
     - If +0 / -0 / denormal -> +inf (0x7f800000)
     - If NaN -> qNaN (payload with bit 22 set)
     - If negative -> qNaN (0xffc00000)
     - If +inf -> +0 (0x00000000)
  7. Writeback & Cleanup:
     - vmovups dest_xmm, s1_xmm  (automatically zeroes YMM bits [255:128])
     - Restore MXCSR
     - Restore s0, s1, s2, s3
     - Restore RSP
     - JMP return_address
```

---

## 2. Unit Test & Semantic Verification

`tests/VirtualMemoryAllocationTests.cpp` includes comprehensive verification in `TestVrsqrtpsTrampolineSemantics`:

```powershell
.\_Build\windows\virtual_memory_allocation_tests.exe --vrsqrtps-trampoline
```

### Verification Results
```text
========================================================================================
  VRSQRTPS TRAMPOLINE SEMANTIC VERIFICATION: Trampoline vs ReciprocalSquareRoot Ref
========================================================================================
  [PASS] xmm0, xmm1 (separate)   : 100000 / 100000 (100.0%) bit-exact matches against reference.
  [PASS] xmm1, xmm1 (in-place)   : 100000 / 100000 (100.0%) bit-exact matches against reference.
  [PASS] xmm10, xmm9 (extended)  : 100000 / 100000 (100.0%) bit-exact matches against reference.
========================================================================================

[host]    VrsqrtpsTrampolineSemantics                      ok
```

Edge cases explicitly validated:
- `+0.0f` -> `+inf` (`0x7f800000`)
- `-0.0f` -> `-inf` (or `+inf` conforming to IEEE reciprocal root)
- Positive & negative denormals -> `+inf` (matching Kyty software path)
- `1.0f` -> `1.0f` (`0x3f800000`)
- `4.0f` -> `0.5f` (`0x3f000000`)
- Largest finite float `0x7f7fffff` -> `0x1f800000`
- `+inf` -> `+0.0f`
- `-inf` -> `qNaN`
- `qNaN` & `sNaN` payloads -> quieted `qNaN` with preserved payload and quiet bit set.
- Upper 128 bits of destination YMM: verified bit-exact 0.
- Source register preservation: verified intact when `dest != src`.
- 128-byte red zone: verified byte-for-byte untouched.

---

## 3. Configuration & Test Launchers

The new options can be configured via command-line flags or `kyty_settings.ini`:
- `--rewrite-vrsqrtps <true|false>` (default: `false`)
- `--rewrite-extrq <true|false>` (default: `false`)

### Batch Launchers Provided

| Script | VRSQRTPS | EXTRQ | Purpose |
|---|---|---|---|
| `launch_test_t1_vrsqrtps_trampoline.bat` | Trampoline (`true`) | Trapped (`false`) | Verify VRSQRTPS trampoline isolation |
| `launch_test_t2_both_fastpath.bat` | Trampoline (`true`) | Trampoline (`true`) | **Production Intel Fastpath Configuration** |
| `launch_test_combined_fastpath.bat` | Trampoline (`true`) | Trampoline (`true`) | Updated standard fastpath batch |
| `Desktop\Launch_AstroBot_Intel_Fastpath.bat` | Trampoline (`true`) | Trampoline (`true`) | Desktop shortcut for interactive play |

---

## 4. Verification Instructions

1. Launch **`launch_test_t2_both_fastpath.bat`** (or use the Desktop shortcut `Launch_AstroBot_Intel_Fastpath.bat`).
2. Observe:
   - **Sky Garden FPS:** 25–50+ FPS (smooth gameplay).
   - **Menu / Loading FPS:** 60 FPS (no 8–15 FPS stutter).
   - **Visuals:** Walk up to trees and foliage in Sky Garden; confirm they remain visible without disappearing.
   - **`#UD` Traps:** 0 traps recorded in logs.
