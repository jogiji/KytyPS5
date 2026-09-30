# Astro Bot (PPSA21567) — AudioOut2 & SceSndz Investigation Journal

## Environment Baseline
- **Host**: Intel Core i9-13980HX (24 cores / 32 threads)
- **Primary GPU**: NVIDIA GeForce RTX 4090 Laptop GPU (Index 0)
- **Secondary GPU**: Intel Raptor Lake-S Mobile Graphics Controller (Index 1)
- **Host OS**: Windows 11
- **Toolchain**: Visual Studio 2026 Developer Shell, clang-cl 22.1.3, lld-link 22.1.3, CMake 4.4.3, Ninja 1.13.2, Qt 6.11.2 MSVC2022 x64, Vulkan SDK 1.4.357
- **Game**: Astro Bot (PPSA21567 v01.018.000) at `D:/ps5/PPSA21567 - ASTRO BOT_extracted`
- **Official Upstream SHA**: `05057c9441dbf3972d49f9143b90213a43a01a00`
- **Branch**: `exp/astrobot-audio-fault`

---

## Root Cause Analysis: The AMD-vs-Intel Differential

### 1. The Symptom
On Intel CPUs (e.g. Intel Core i9-13980HX), Astro Bot consistently crashed immediately following the ASOBI intro on thread `SceSndzAudioOutMain` with an access violation executing:
```asm
vmovdqu xmmword ptr [rdi], xmm0
```
with `rdi = 0x3f80000026800000` (or `0x000000003e000000`), values corresponding to IEEE-754 floats (`1.0f`, `0.125f`) stored on the stack frame. Conversely, on AMD CPUs (Zen 2 / 3 / 4), the exact same build ran smoothly through this sequence without crashing.

### 2. The Architectural Differential
PS5 guest binaries are compiled under the **System V AMD64 ABI**, which specifies a 128-byte **Red Zone** immediately below the stack pointer (`[RSP - 128 .. RSP]`). Functions are free to use this 128-byte region for temporary variables, float matrices, and pointers without moving `RSP`.

On Windows, the Microsoft x64 ABI does **not** recognize a red zone. When a CPU exception occurs in user mode (such as `#UD` - Invalid Opcode):
1. The Windows NT kernel dispatches the exception via `KiUserExceptionDispatcher`.
2. Windows pushes a 1.2 KB `CONTEXT` structure directly below the current user `RSP`.
3. This push **overwrites and obliterates the System V Red Zone**.

### 3. Why Did This Happen on Intel but Not AMD?
- **AMD CPUs**: AMD Zen hardware natively supports AMD-specific instruction set extensions including **SSE4a** (`EXTRQ`, `INSERTQ`, `MOVNTSD`, `MOVNTSS`), **CLZERO**, and **RDPRU**. In Astro Bot, these instructions execute directly in hardware without generating exceptions. The Windows kernel exception dispatcher is never invoked, and the guest red zone remains intact.
- **Intel CPUs**: Intel x86-64 hardware does **not** support SSE4a (`MOVNTSS`, `MOVNTSD`, `EXTRQ`), `CLZERO`, or `RDPRU`. When an Intel CPU hits one of these opcodes, it immediately raises `#UD`.

### 4. The Defect in Upstream KytyPS5
Kyty has a static binary patcher (`RedZonePatcher`) designed to wrap risky instructions in trampolines:
```asm
lea rsp, [rsp - 128]
<instruction>
lea rsp, [rsp + 128]
```
By moving `RSP` down by 128 bytes before `<instruction>`, any subsequent exception pushes the Windows `CONTEXT` below the red zone, preserving the guest stack.

However, in upstream Kyty:
1. `redZonePatcher.cpp` filtered rewrite sites with:
   ```cpp
   if (!decoded.accesses_memory || !decoded.red_zone_live.any() || rewrite_sites.contains(address))
   ```
   Because `EXTRQ`, `INSERTQ`, `CLZERO`, and `RDPRU` are register or implicit instructions, `accesses_memory` was false. Upstream **completely omitted trampolines for emulated instructions**!
2. When an Intel CPU hit an AMD instruction like `EXTRQ` or `MOVNTSS`, the `#UD` dispatch occurred with `RSP` unadjusted, corrupting the red zone where Astro Bot's audio mixer stored its stream pointers and float gains.
3. `x64InstructionEmulator.cpp` was also missing emulation for `MOVNTSS` (`F3 0F 2B /r`), `MOVNTSD` (`F2 0F 2B /r`), `CLZERO` (`0F 01 FC`), and `RDPRU` (`0F 01 FD`).
4. Additionally, `AudioOut2ContextCreate` left caller-provided context arenas uninitialized, which allowed poison patterns to be read if uninitialized slots were accessed.

---

## Solution & Implementation

The fixes were ported cleanly onto upstream `main` across 6 files:

1. **`src/loader/redZonePatcher.cpp`**:
   - Added `MayEmulateInstruction(ZydisMnemonic mnemonic)` covering `EXTRQ`, `INSERTQ`, `MOVNTSD`, `MOVNTSS`, `CLZERO`, `RDPRU`, `MONITORX`, `MWAITX`, and `SHA*`.
   - Updated `CollectRedZoneMemoryInstructions` to ensure all instructions that may raise `#UD` and be emulated receive red-zone trampolines (`lea rsp, [rsp - 128] ... lea rsp, [rsp + 128]`).

2. **`src/loader/x64InstructionEmulator.cpp`**:
   - Added emulation for `MOVNTSS` and `MOVNTSD` memory decoding and emulation.
   - Added emulation for `CLZERO` (cache-line zeroing) and `RDPRU` (monotonic timer read via `__rdtsc()`).
   - Implemented safe `Context::Xmm()` indexing and `StoreRdpru()`.

3. **`src/libs/libAudio2.cpp`**:
   - Zero-initialized caller-supplied context arena up to 16 MB in `AudioOut2ContextCreate` to prevent stale pointer interpretation.
   - Added memory range validation for `AudioOut2Attribute` arrays and PCM payloads in `AudioOut2PortSetAttributes`.

4. **`src/libs/audio.cpp`**:
   - Added bounds and readability check `Graphics::HostMemoryRangeIsReadable` before queuing guest PCM data to SDL.
   - Protected `AudioOutOutputs` with `Common::LockGuard lock(m_mutex)` to avoid use-after-free with concurrent port destruction.

5. **`src/graphics/host_gpu/hostMemory.cpp`** & **`CMakeLists.txt`**:
   - Linked `hostMemory.cpp` into test targets and ensured proper query support.

---

## Experiment Matrix

| Exp ID | Kyty Commit | Source Changes | CLI Arguments | Host CPU / GPU | Milestone Reached | Crash / Result |
|---|---|---|---|---|---|---|
| **EXP-AUD-000** | `05057c9` (Official) | None | `--game ... --gpu 0` | Intel i9-13980HX / RTX 4090 | ASOBI Intro | Crash in `SceSndzAudioOutMain` (`vmovdqu [rdi], xmm0`, `rdi=3f80000026800000`) |
| **EXP-AUD-001** | `05057c9` (Clean local) | None | `--game ... --gpu 0` | Intel i9-13980HX / RTX 4090 | ASOBI Intro | Reproduces exact same crash (`vmovdqu [rdi], xmm0`) |
| **EXP-AUD-008** | `1cfc969` (Local commit) | RedZone `#UD` protection + SSE4a/CLZERO/RDPRU emulation + AudioOut2 validation | `--game ... --gpu 0 --amd-cpu --redzone` | Intel i9-13980HX / RTX 4090 | **PASSED ASOBI intro**, boots into main title runtime, compiles shaders, dispatches compute jobs | **NO CRASH**. Successfully running continuously. |
