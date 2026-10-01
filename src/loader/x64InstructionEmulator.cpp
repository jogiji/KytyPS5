#include "loader/x64InstructionEmulator.h"

#include "common/common.h"

#include <Zydis/Zydis.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <mutex>
#include <unordered_map>
#include <vector>
#if !defined(__APPLE__)
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <intrin.h> // IWYU pragma: keep
#include <windows.h> // IWYU pragma: keep
#elif defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <sched.h>
#include <ucontext.h>
#if defined(__x86_64__)
#include <x86intrin.h> // IWYU pragma: keep
#endif
#endif

namespace Loader::X64InstructionEmulator {

static uint64_t ExtractBitField(uint64_t value, uint32_t length, uint32_t index) {
	length &= 0x3fu;
	index &= 0x3fu;

	if (length == 0) {
		length = 64;
	}

	if (index >= 64) {
		return 0;
	}

	auto available = 64u - index;
	if (length > available) {
		length = available;
	}

	const uint64_t mask = (length == 64 ? UINT64_MAX : ((uint64_t {1} << length) - 1u));
	return (value >> index) & mask;
}

static uint64_t InsertBitField(uint64_t dst, uint64_t src, uint32_t length, uint32_t index) {
	length &= 0x3fu;
	index &= 0x3fu;

	if (length == 0) {
		length = 64;
	}

	if (index >= 64) {
		return dst;
	}

	auto available = 64u - index;
	if (length > available) {
		length = available;
	}

	const uint64_t mask        = (length == 64 ? UINT64_MAX : ((uint64_t {1} << length) - 1u));
	const uint64_t shifted     = (index == 0 ? mask : (mask << index));
	const uint64_t src_shifted = (src & mask) << index;

	return (dst & ~shifted) | src_shifted;
}

struct XmmWords {
	uint32_t w[4];
};


static uint64_t ReadHostTsc() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || (defined(__x86_64__) && !defined(__APPLE__))
	return __rdtsc();
#else
	return 0;
#endif
}

static uint64_t EmulateRdpru(uint32_t selector) {
	// RDPRU exposes AMD performance counters in user mode. Kyty only needs a
	// stable monotonic source for the timing paths that use this instruction.
	switch (selector) {
		case 0:
		case 1: return ReadHostTsc();
		default: return 0;
	}
}

static void Sha1Msg1(XmmWords& dest, const XmmWords& src2) {
	const uint32_t w0 = dest.w[3];
	const uint32_t w1 = dest.w[2];
	const uint32_t w2 = dest.w[1];
	const uint32_t w3 = dest.w[0];
	const uint32_t w4 = src2.w[3];
	const uint32_t w5 = src2.w[2];
	dest.w[3]         = w2 ^ w0;
	dest.w[2]         = w3 ^ w1;
	dest.w[1]         = w4 ^ w2;
	dest.w[0]         = w5 ^ w3;
}

static void Sha1Msg2(XmmWords& dest, const XmmWords& src2) {
	const uint32_t w13 = src2.w[2];
	const uint32_t w14 = src2.w[1];
	const uint32_t w15 = src2.w[0];
	const uint32_t w16 = std::rotl(dest.w[3] ^ w13, 1);
	const uint32_t w17 = std::rotl(dest.w[2] ^ w14, 1);
	const uint32_t w18 = std::rotl(dest.w[1] ^ w15, 1);
	const uint32_t w19 = std::rotl(dest.w[0] ^ w16, 1);
	dest.w[3]          = w16;
	dest.w[2]          = w17;
	dest.w[1]          = w18;
	dest.w[0]          = w19;
}

static void Sha1Nexte(XmmWords& dest, const XmmWords& src2) {
	const uint32_t tmp = std::rotl(dest.w[3], 30);
	dest.w[3]          = src2.w[3] + tmp;
	dest.w[2]          = src2.w[2];
	dest.w[1]          = src2.w[1];
	dest.w[0]          = src2.w[0];
}

static uint32_t Sha1RoundFunc(uint8_t group, uint32_t b, uint32_t c, uint32_t d) {
	switch (group & 3u) {
		case 0: return (b & c) ^ ((~b) & d);
		case 1: return b ^ c ^ d;
		case 2: return (b & c) ^ (b & d) ^ (c & d);
		default: return b ^ c ^ d;
	}
}

static uint32_t Sha1RoundConstant(uint8_t group) {
	switch (group & 3u) {
		case 0: return 0x5a827999u;
		case 1: return 0x6ed9eba1u;
		case 2: return 0x8f1bbcdcu;
		default: return 0xca62c1d6u;
	}
}

static void Sha1Rnds4(XmmWords& dest, const XmmWords& src2, uint8_t imm8) {
	const uint8_t  group = imm8 & 3u;
	const uint32_t k     = Sha1RoundConstant(group);
	const uint32_t w[4]  = {src2.w[3], src2.w[2], src2.w[1], src2.w[0]};

	uint32_t a = dest.w[3];
	uint32_t b = dest.w[2];
	uint32_t c = dest.w[1];
	uint32_t d = dest.w[0];
	uint32_t e = 0;

	for (unsigned int round = 0; round < 4u; round++) {
		uint32_t term = Sha1RoundFunc(group, b, c, d) + std::rotl(a, 5) + w[round] + k;
		if (round > 0u) {
			term += e;
		}
		const uint32_t a1 = term;
		e                 = d;
		d                 = c;
		c                 = std::rotl(b, 30);
		b                 = a;
		a                 = a1;
	}

	dest.w[3] = a;
	dest.w[2] = b;
	dest.w[1] = c;
	dest.w[0] = d;
}

static uint32_t Sha256Sigma0(uint32_t x) {
	return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3u);
}

static uint32_t Sha256Sigma1(uint32_t x) {
	return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10u);
}

static uint32_t Sha256Sum0(uint32_t x) {
	return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
}

static uint32_t Sha256Sum1(uint32_t x) {
	return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
}

static uint32_t Sha256Ch(uint32_t e, uint32_t f, uint32_t g) {
	return (e & f) ^ ((~e) & g);
}

static uint32_t Sha256Maj(uint32_t a, uint32_t b, uint32_t c) {
	return (a & b) ^ (a & c) ^ (b & c);
}

static void Sha256Msg1(XmmWords& dest, const XmmWords& src2) {
	const uint32_t w4 = src2.w[0];
	const uint32_t w3 = dest.w[3];
	const uint32_t w2 = dest.w[2];
	const uint32_t w1 = dest.w[1];
	const uint32_t w0 = dest.w[0];
	dest.w[3]         = w3 + Sha256Sigma0(w4);
	dest.w[2]         = w2 + Sha256Sigma0(w3);
	dest.w[1]         = w1 + Sha256Sigma0(w2);
	dest.w[0]         = w0 + Sha256Sigma0(w1);
}

static void Sha256Msg2(XmmWords& dest, const XmmWords& src2) {
	const uint32_t w14 = src2.w[2];
	const uint32_t w15 = src2.w[3];
	const uint32_t w16 = dest.w[0] + Sha256Sigma1(w14);
	const uint32_t w17 = dest.w[1] + Sha256Sigma1(w15);
	const uint32_t w18 = dest.w[2] + Sha256Sigma1(w16);
	const uint32_t w19 = dest.w[3] + Sha256Sigma1(w17);
	dest.w[3]          = w19;
	dest.w[2]          = w18;
	dest.w[1]          = w17;
	dest.w[0]          = w16;
}

static void Sha256Rnds2(XmmWords& dest, const XmmWords& src2, const XmmWords& xmm0) {
	uint32_t a = src2.w[3];
	uint32_t b = src2.w[2];
	uint32_t c = dest.w[3];
	uint32_t d = dest.w[2];
	uint32_t e = src2.w[1];
	uint32_t f = src2.w[0];
	uint32_t g = dest.w[1];
	uint32_t h = dest.w[0];

	for (unsigned int round = 0; round < 2u; round++) {
		const uint32_t wk = xmm0.w[round];
		const uint32_t t1 = Sha256Ch(e, f, g) + Sha256Sum1(e) + wk + h;
		const uint32_t t2 = Sha256Maj(a, b, c) + Sha256Sum0(a);
		const uint32_t a1 = t1 + t2;
		const uint32_t e1 = t1 + d;
		const uint32_t b1 = a;
		const uint32_t c1 = b;
		const uint32_t d1 = c;
		const uint32_t f1 = e;
		const uint32_t g1 = f;
		const uint32_t h1 = g;
		a                 = a1;
		b                 = b1;
		c                 = c1;
		d                 = d1;
		e                 = e1;
		f                 = f1;
		g                 = g1;
		h                 = h1;
	}

	dest.w[3] = a;
	dest.w[2] = b;
	dest.w[1] = e;
	dest.w[0] = f;
}

struct ShaNiInsn {
	uint8_t escape;
	uint8_t opcode;
	uint8_t imm8;
	uint8_t rex;
	size_t  modrm_offset;
	size_t  length;
};

static bool DecodeShaNiInsn(const uint8_t* rip, ShaNiInsn& insn) {
	size_t  offset = 0;
	uint8_t rex    = 0;
	if ((rip[0] & 0xf0u) == 0x40u) {
		rex    = rip[0];
		offset = 1;
	}

	if (rip[offset] != 0x0f) {
		return false;
	}

	if (rip[offset + 1] == 0x38) {
		const uint8_t op = rip[offset + 2];
		if (op != 0xc8 && op != 0xc9 && op != 0xca && op != 0xcb && op != 0xcc && op != 0xcd) {
			return false;
		}
		insn.escape       = 0x38;
		insn.opcode       = op;
		insn.imm8         = 0;
		insn.rex          = rex;
		insn.modrm_offset = offset + 3;
	} else if (rip[offset + 1] == 0x3a && rip[offset + 2] == 0xcc) {
		insn.escape       = 0x3a;
		insn.opcode       = 0xcc;
		insn.rex          = rex;
		insn.modrm_offset = offset + 3;
	} else {
		return false;
	}

	const uint8_t modrm = rip[insn.modrm_offset];
	const uint8_t mod   = modrm >> 6u;
	const uint8_t rm    = modrm & 0x07u;
	size_t        end   = insn.modrm_offset + 1;

	if (mod != 3u) {
		uint8_t sib_base = 0xffu;
		if (rm == 4u) {
			sib_base = rip[end] & 0x07u;
			end++;
		}

		if (mod == 0u && (rm == 5u || (rm == 4u && sib_base == 5u))) {
			end += 4;
		} else if (mod == 1u) {
			end++;
		} else if (mod == 2u) {
			end += 4;
		}
	}

	if (insn.escape == 0x3a) {
		insn.imm8 = rip[end];
		end++;
	}

	insn.length = end;
	return true;
}

static bool ShaNiModrmIsRegister(uint8_t modrm) {
	return (modrm & 0xc0u) == 0xc0u;
}

static uint8_t ShaNiRegIndex(uint8_t modrm, uint8_t rex, bool reg_field) {
	if (reg_field) {
		return ((modrm >> 3u) & 0x07u) | ((rex & 0x04u) << 1u);
	}
	return (modrm & 0x07u) | ((rex & 0x01u) << 3u);
}

static bool ResolveShaNiMemoryAddress(const uint8_t* rip, const ShaNiInsn&    insn,
                                      const uint64_t (&gpr)[16], const void*& address) {
	const uint8_t modrm = rip[insn.modrm_offset];
	const uint8_t mod   = modrm >> 6u;
	const uint8_t rm    = modrm & 0x07u;
	if (mod == 3u) {
		return false;
	}

	size_t   offset = insn.modrm_offset + 1;
	uint64_t result = 0;

	if (rm == 4u) {
		const uint8_t sib       = rip[offset++];
		const uint8_t scale     = sib >> 6u;
		const uint8_t index_low = (sib >> 3u) & 0x07u;
		const uint8_t base_low  = sib & 0x07u;
		const bool    has_index = index_low != 4u || (insn.rex & 0x02u) != 0;
		const bool    has_base  = mod != 0u || base_low != 5u;

		if (has_base) {
			const uint8_t base = base_low | ((insn.rex & 0x01u) << 3u);
			result += gpr[base];
		}
		if (has_index) {
			const uint8_t index = index_low | ((insn.rex & 0x02u) << 2u);
			result += gpr[index] << scale;
		}

		if (!has_base) {
			int32_t displacement = 0;
			std::memcpy(&displacement, rip + offset, sizeof(displacement));
			result += static_cast<uint64_t>(static_cast<int64_t>(displacement));
			offset += sizeof(displacement);
		}
	} else if (mod == 0u && rm == 5u) {
		int32_t displacement = 0;
		std::memcpy(&displacement, rip + offset, sizeof(displacement));
		result = reinterpret_cast<uint64_t>(rip + insn.length) +
		         static_cast<uint64_t>(static_cast<int64_t>(displacement));
		offset += sizeof(displacement);
	} else {
		const uint8_t base = rm | ((insn.rex & 0x01u) << 3u);
		result             = gpr[base];
	}

	if (mod == 1u) {
		const auto displacement = static_cast<int8_t>(rip[offset]);
		result += static_cast<uint64_t>(static_cast<int64_t>(displacement));
	} else if (mod == 2u) {
		int32_t displacement = 0;
		std::memcpy(&displacement, rip + offset, sizeof(displacement));
		result += static_cast<uint64_t>(static_cast<int64_t>(displacement));
	}

	address = reinterpret_cast<const void*>(result);
	return true;
}

// Decode the memory operand of the SSE4a MOVNTSS/MOVNTSD instructions. The
// host Intel CPU raises #UD for these AMD instructions, so the exception path
// has to reproduce the guest store without relying on the non-temporal hint.
static bool DecodeSse4aMemoryOperand(const uint8_t* rip, size_t modrm_offset, uint8_t rex,
                                     size_t& instruction_length) {
	const uint8_t modrm = rip[modrm_offset];
	const uint8_t mod   = modrm >> 6u;
	const uint8_t rm    = modrm & 0x07u;
	if (mod == 3u) {
		return false;
	}

	size_t offset = modrm_offset + 1;
	if (rm == 4u) {
		const uint8_t sib      = rip[offset++];
		const uint8_t base_low = sib & 0x07u;
		const bool    has_base = mod != 0u || base_low != 5u || (rex & 0x01u) != 0;
		if (!has_base || mod == 2u) {
			offset += sizeof(int32_t);
		} else if (mod == 1u) {
			offset += sizeof(int8_t);
		}
	} else if (mod == 0u && rm == 5u && (rex & 0x01u) == 0) {
		// RIP-relative addressing.
		offset += sizeof(int32_t);
	} else if (mod == 1u) {
		offset += sizeof(int8_t);
	} else if (mod == 2u) {
		offset += sizeof(int32_t);
	}

	instruction_length = offset;
	return true;
}

static bool ResolveSse4aMemoryAddress(const uint8_t* rip, size_t modrm_offset, uint8_t rex,
                                      size_t instruction_length, const uint64_t (&gpr)[16],
                                      void*& address) {
	const uint8_t modrm = rip[modrm_offset];
	const uint8_t mod   = modrm >> 6u;
	const uint8_t rm    = modrm & 0x07u;
	if (mod == 3u) {
		return false;
	}

	size_t   offset = modrm_offset + 1;
	uint64_t result = 0;

	if (rm == 4u) {
		const uint8_t sib       = rip[offset++];
		const uint8_t scale     = sib >> 6u;
		const uint8_t index_low = (sib >> 3u) & 0x07u;
		const uint8_t base_low  = sib & 0x07u;
		const bool    has_index = index_low != 4u || (rex & 0x02u) != 0;
		const bool    has_base  = mod != 0u || base_low != 5u || (rex & 0x01u) != 0;

		if (has_base) {
			const uint8_t base = base_low | ((rex & 0x01u) << 3u);
			result += gpr[base];
		}
		if (has_index) {
			const uint8_t index = index_low | ((rex & 0x02u) << 2u);
			result += gpr[index] << scale;
		}
		if (!has_base) {
			int32_t displacement = 0;
			std::memcpy(&displacement, rip + offset, sizeof(displacement));
			result += static_cast<uint64_t>(static_cast<int64_t>(displacement));
			offset += sizeof(displacement);
		}
	} else if (mod == 0u && rm == 5u && (rex & 0x01u) == 0) {
		int32_t displacement = 0;
		std::memcpy(&displacement, rip + offset, sizeof(displacement));
		result = reinterpret_cast<uint64_t>(rip + instruction_length) +
		         static_cast<uint64_t>(static_cast<int64_t>(displacement));
	} else {
		const uint8_t base = rm | ((rex & 0x01u) << 3u);
		result             = gpr[base];
	}

	if (mod == 1u) {
		result += static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(rip[offset])));
	} else if (mod == 2u) {
		int32_t displacement = 0;
		std::memcpy(&displacement, rip + offset, sizeof(displacement));
		result += static_cast<uint64_t>(static_cast<int64_t>(displacement));
	}

	address = reinterpret_cast<void*>(result);
	return true;
}

static bool ExecuteShaNiInsn(const ShaNiInsn& insn, const XmmWords& src2, const XmmWords& xmm0,
                             XmmWords& dest) {
	if (insn.escape == 0x3a && insn.opcode == 0xcc) {
		Sha1Rnds4(dest, src2, insn.imm8);
		return true;
	}

	switch (insn.opcode) {
		case 0xc8: Sha1Nexte(dest, src2); return true;
		case 0xc9: Sha1Msg1(dest, src2); return true;
		case 0xca: Sha1Msg2(dest, src2); return true;
		case 0xcb: Sha256Rnds2(dest, src2, xmm0); return true;
		case 0xcc: Sha256Msg1(dest, src2); return true;
		case 0xcd: Sha256Msg2(dest, src2); return true;
		default: return false;
	}
}

// Keep instruction semantics shared; only access to the saved host context differs.
struct Context {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	PCONTEXT native;

	[[nodiscard]] uint64_t Rip() const { return native->Rip; }
	void                   Advance(size_t length) { native->Rip += length; }
	[[nodiscard]] void*    Xmm(uint8_t index) const {
		switch (index) {
#define KYTY_CONTEXT_XMM_CASE(index) \
			case index: return &native->Xmm##index;
			KYTY_CONTEXT_XMM_CASE(0)
			KYTY_CONTEXT_XMM_CASE(1)
			KYTY_CONTEXT_XMM_CASE(2)
			KYTY_CONTEXT_XMM_CASE(3)
			KYTY_CONTEXT_XMM_CASE(4)
			KYTY_CONTEXT_XMM_CASE(5)
			KYTY_CONTEXT_XMM_CASE(6)
			KYTY_CONTEXT_XMM_CASE(7)
			KYTY_CONTEXT_XMM_CASE(8)
			KYTY_CONTEXT_XMM_CASE(9)
			KYTY_CONTEXT_XMM_CASE(10)
			KYTY_CONTEXT_XMM_CASE(11)
			KYTY_CONTEXT_XMM_CASE(12)
			KYTY_CONTEXT_XMM_CASE(13)
			KYTY_CONTEXT_XMM_CASE(14)
			KYTY_CONTEXT_XMM_CASE(15)
#undef KYTY_CONTEXT_XMM_CASE
			default: return nullptr;
		}
	}

	void StoreRdpru(uint64_t value) {
		native->Rax = static_cast<uint32_t>(value);
		native->Rdx = static_cast<uint32_t>(value >> 32u);
	}

	void LoadGprs(uint64_t (&gpr)[16]) const {
		const uint64_t registers[] = {native->Rax, native->Rcx, native->Rdx, native->Rbx,
		                              native->Rsp, native->Rbp, native->Rsi, native->Rdi,
		                              native->R8,  native->R9,  native->R10, native->R11,
		                              native->R12, native->R13, native->R14, native->R15};
		std::memcpy(gpr, registers, sizeof(gpr));
	}

	void ClearUpperYmm(uint8_t index) const {
		if ((native->ContextFlags & CONTEXT_XSTATE) != CONTEXT_XSTATE) {
			return;
		}
		DWORD64 features = 0;
		if (!GetXStateFeaturesMask(native, &features) || (features & XSTATE_MASK_AVX) == 0) {
			return; // An absent AVX component restores zeroes.
		}
		DWORD size = 0;
		auto* ymm = static_cast<M128A*>(LocateXStateFeature(native, XSTATE_AVX, &size));
		if (ymm != nullptr && size >= (index + 1u) * sizeof(M128A)) {
			ymm[index] = {};
		}
	}
#elif defined(__APPLE__)
	ucontext_t* native;

	[[nodiscard]] uint64_t Rip() const {
		return static_cast<uint64_t>(native->uc_mcontext->__ss.__rip);
	}
	void Advance(size_t length) {
		native->uc_mcontext->__ss.__rip += static_cast<uint64_t>(length);
	}
	void LoadGprs(uint64_t (&gpr)[16]) const {
		const auto* ss = &native->uc_mcontext->__ss;
		const uint64_t registers[] = {
			static_cast<uint64_t>(ss->__rax), static_cast<uint64_t>(ss->__rcx),
			static_cast<uint64_t>(ss->__rdx), static_cast<uint64_t>(ss->__rbx),
			static_cast<uint64_t>(ss->__rsp), static_cast<uint64_t>(ss->__rbp),
			static_cast<uint64_t>(ss->__rsi), static_cast<uint64_t>(ss->__rdi),
			static_cast<uint64_t>(ss->__r8),  static_cast<uint64_t>(ss->__r9),
			static_cast<uint64_t>(ss->__r10), static_cast<uint64_t>(ss->__r11),
			static_cast<uint64_t>(ss->__r12), static_cast<uint64_t>(ss->__r13),
			static_cast<uint64_t>(ss->__r14), static_cast<uint64_t>(ss->__r15),
		};
		std::memcpy(gpr, registers, sizeof(gpr));
	}
	// Darwin names the XMM file __fpu_xmm0..__fpu_xmm15 instead of exposing an array.
	[[nodiscard]] void* Xmm(uint8_t index) const {
		auto* fs = &native->uc_mcontext->__fs;
		switch (index) {
			case 0: return &fs->__fpu_xmm0;
			case 1: return &fs->__fpu_xmm1;
			case 2: return &fs->__fpu_xmm2;
			case 3: return &fs->__fpu_xmm3;
			case 4: return &fs->__fpu_xmm4;
			case 5: return &fs->__fpu_xmm5;
			case 6: return &fs->__fpu_xmm6;
			case 7: return &fs->__fpu_xmm7;
			case 8: return &fs->__fpu_xmm8;
			case 9: return &fs->__fpu_xmm9;
			case 10: return &fs->__fpu_xmm10;
			case 11: return &fs->__fpu_xmm11;
			case 12: return &fs->__fpu_xmm12;
			case 13: return &fs->__fpu_xmm13;
			case 14: return &fs->__fpu_xmm14;
			case 15: return &fs->__fpu_xmm15;
			default: return nullptr;
		}
	}
#else
	ucontext_t* native;

	[[nodiscard]] uint64_t Rip() const {
		return static_cast<uint64_t>(native->uc_mcontext.gregs[REG_RIP]);
	}
	void Advance(size_t length) {
		native->uc_mcontext.gregs[REG_RIP] += static_cast<greg_t>(length);
	}
	[[nodiscard]] void* Xmm(uint8_t index) const {
		if (native->uc_mcontext.fpregs == nullptr) {
			return nullptr;
		}
		return native->uc_mcontext.fpregs->_xmm[index].element;
	}

	void LoadGprs(uint64_t (&gpr)[16]) const {
		constexpr int registers[] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
		                             REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
		                             REG_R12, REG_R13, REG_R14, REG_R15};
		for (size_t i = 0; i < 16; ++i) {
			gpr[i] = static_cast<uint64_t>(native->uc_mcontext.gregs[registers[i]]);
		}
	}

	void ClearUpperYmm(uint8_t index) const {
		// Linux signal frames use the standard XSAVE layout. An absent AVX component
		// already restores the architectural initial value (all zeroes).
		auto*    state    = reinterpret_cast<uint8_t*>(native->uc_mcontext.fpregs);
		uint32_t magic    = 0;
		uint32_t size     = 0;
		uint64_t features = 0;
		std::memcpy(&magic, state + 464, sizeof(magic));
		if (magic != 0x46505853) {
			return;
		}
		std::memcpy(&size, state + 480, sizeof(size));
		if (size < 832) {
			return;
		}
		std::memcpy(&features, state + 512, sizeof(features));
		if ((features & 4) != 0) {
			std::memset(state + 576 + index * 16, 0, 16);
		}
	}

	void StoreRdpru(uint64_t value) {
		native->uc_mcontext.gregs[REG_RAX] = static_cast<greg_t>(static_cast<uint32_t>(value));
		native->uc_mcontext.gregs[REG_RDX] =
		    static_cast<greg_t>(static_cast<uint32_t>(value >> 32u));
	}
#endif
};

#if !defined(__APPLE__)

static bool TryEmulateShaNi(Context& context) {
	const auto* rip = reinterpret_cast<const uint8_t*>(context.Rip());
	ShaNiInsn   insn {};
	if (!DecodeShaNiInsn(rip, insn)) {
		return false;
	}

	const uint8_t modrm_byte = rip[insn.modrm_offset];
	const uint8_t dest_index = ShaNiRegIndex(modrm_byte, insn.rex, true);
	auto*         dest_xmm   = context.Xmm(dest_index);
	auto*         xmm0       = context.Xmm(0);
	if (dest_xmm == nullptr || xmm0 == nullptr) {
		return false;
	}

	XmmWords dest {};
	XmmWords src2 {};
	XmmWords xmm0_words {};
	std::memcpy(&dest, dest_xmm, sizeof(dest));
	std::memcpy(&xmm0_words, xmm0, sizeof(xmm0_words));

	if (ShaNiModrmIsRegister(modrm_byte)) {
		const uint8_t src_index = ShaNiRegIndex(modrm_byte, insn.rex, false);
		auto*         src_xmm   = context.Xmm(src_index);
		if (src_xmm == nullptr) {
			return false;
		}
		std::memcpy(&src2, src_xmm, sizeof(src2));
	} else {
		uint64_t    gpr[16] {};
		const void* source = nullptr;
		context.LoadGprs(gpr);
		if (!ResolveShaNiMemoryAddress(rip, insn, gpr, source)) {
			return false;
		}
		std::memcpy(&src2, source, sizeof(src2));
	}

	if (!ExecuteShaNiInsn(insn, src2, xmm0_words, dest)) {
		return false;
	}

	std::memcpy(dest_xmm, &dest, sizeof(dest));
	context.Advance(insn.length);
	return true;
}

#endif

static bool TryEmulateSse4a(Context& context, InstructionType* out_type = nullptr) {
	const auto*   rip    = reinterpret_cast<const uint8_t*>(context.Rip());
	const uint8_t prefix = rip[0];
	if (prefix != 0x66 && prefix != 0xf2 && prefix != 0xf3) {
		return false;
	}

	size_t  offset = 1;
	uint8_t rex    = 0;
	if ((rip[offset] & 0xf0u) == 0x40u) {
		rex = rip[offset++];
	}
	if (rip[offset] != 0x0f) {
		return false;
	}
	const uint8_t opcode           = rip[offset + 1];
	const bool    register_extract = prefix == 0x66 && opcode == 0x79;
	if (opcode != 0x78 && !register_extract && opcode != 0x2b) {
		return false;
	}

	const uint8_t modrm = rip[offset + 2];
	const uint8_t reg   = ((modrm >> 3u) & 0x07u) | ((rex & 0x04u) << 1u);

	if (opcode == 0x2b) {
		// MOVNTSD/MOVNTSS have a memory destination and an XMM source in
		// ModRM.reg. The non-temporal hint is not guest-visible here.
		if (prefix != 0xf2 && prefix != 0xf3) {
			return false;
		}
		size_t instruction_length = 0;
		if (!DecodeSse4aMemoryOperand(rip, offset + 2, rex, instruction_length)) {
			return false;
		}

		uint64_t gpr[16] {};
		context.LoadGprs(gpr);
		void* address = nullptr;
		if (!ResolveSse4aMemoryAddress(rip, offset + 2, rex, instruction_length, gpr, address)) {
			return false;
		}

		auto* src_xmm = context.Xmm(reg);
		if (src_xmm == nullptr) {
			return false;
		}
		uint64_t source = 0;
		std::memcpy(&source, src_xmm, sizeof(source));
		if (prefix == 0xf2) {
			std::memcpy(address, &source, sizeof(source));
			if (out_type != nullptr) {
				*out_type = InstructionType::Movntsd;
			}
		} else {
			const uint32_t value = static_cast<uint32_t>(source);
			std::memcpy(address, &value, sizeof(value));
			if (out_type != nullptr) {
				*out_type = InstructionType::Movntss;
			}
		}
		context.Advance(instruction_length);
		return true;
	}

	if ((modrm & 0xc0u) != 0xc0u) {
		return false;
	}

	const uint8_t rm = (modrm & 0x07u) | ((rex & 0x01u) << 3u);

	// Immediate EXTRQ encodes its destination in r/m; the two-register form uses reg.
	uint8_t dest_index = reg;
	if (prefix == 0x66 && !register_extract) {
		dest_index = rm;
	}
	auto* dest_xmm = context.Xmm(dest_index);
	auto* src_xmm  = context.Xmm(rm);
	if (dest_xmm == nullptr || src_xmm == nullptr) {
		return false;
	}
	uint64_t dest[2] {};
	uint64_t source = 0;
	std::memcpy(dest, dest_xmm, sizeof(dest));
	std::memcpy(&source, src_xmm, sizeof(source));
	uint8_t length             = 0;
	uint8_t index              = 0;
	size_t  instruction_length = offset + 3;
	if (register_extract) {
		length = static_cast<uint8_t>(source);
		index  = static_cast<uint8_t>(source >> 8u);
	} else {
		length = rip[offset + 3];
		index  = rip[offset + 4];
		instruction_length += 2;
	}
	if (prefix == 0x66) {
		dest[0] = ExtractBitField(dest[0], length, index);
		dest[1] = 0;
		if (out_type != nullptr) {
			*out_type = InstructionType::Extrq;
		}
	} else {
		dest[0] = InsertBitField(dest[0], source, length, index);
		if (out_type != nullptr) {
			*out_type = InstructionType::Insertq;
		}
	}
	std::memcpy(dest_xmm, dest, sizeof(dest));
	context.Advance(instruction_length);
	return true;
}

#if !defined(__APPLE__)

static bool TryEmulateMonitorxMwaitx(Context& context) {
	const auto* rip = reinterpret_cast<const uint8_t*>(context.Rip());
	if (rip[0] != 0x0f || rip[1] != 0x01 || (rip[2] != 0xfa && rip[2] != 0xfb)) {
		return false;
	}

	// Approximate AMD MONITORX/MWAITX as no-op/yield.
	if (rip[2] == 0xfb) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SwitchToThread();
#else
		::sched_yield();
#endif
	}
	context.Advance(3);
	return true;
}

static bool TryEmulateAmdSystem(Context& context, InstructionType* out_type = nullptr) {
	const auto* rip = reinterpret_cast<const uint8_t*>(context.Rip());
	if (rip[0] != 0x0f || rip[1] != 0x01) {
		return false;
	}

	uint64_t gpr[16] {};
	context.LoadGprs(gpr);
	switch (rip[2]) {
		case 0xfc: { // CLZERO: clear the 64-byte cache line containing RAX.
			const uint64_t address = gpr[0] & ~uint64_t {63};
			if (address == 0) {
				return false;
			}
			std::memset(reinterpret_cast<void*>(address), 0, 64);
			context.Advance(3);
			if (out_type != nullptr) {
				*out_type = InstructionType::Clzero;
			}
			return true;
		}
		case 0xfd: // RDPRU: return an approximate host counter in EDX:EAX.
			context.StoreRdpru(EmulateRdpru(static_cast<uint32_t>(gpr[1])));
			context.Advance(3);
			if (out_type != nullptr) {
				*out_type = InstructionType::Rdpru;
			}
			return true;
		default: return false;
	}
}

static uint32_t ReciprocalSquareRoot(uint32_t bits) {
	const uint32_t magnitude = bits & 0x7fffffffu;
	const uint32_t exponent  = magnitude & 0x7f800000u;
	if (exponent == 0) {
		// RSQRT treats denormals as signed zero regardless of MXCSR.DAZ.
		return (bits & 0x80000000u) | 0x7f800000u;
	}
	if (magnitude > 0x7f800000u) {
		return bits | 0x00400000u; // Quiet NaNs without raising an exception.
	}
	if ((bits & 0x80000000u) != 0) {
		return 0xffc00000u;
	}
	if (magnitude == 0x7f800000u) {
		return 0;
	}

	// A deterministic accurate estimate meets the instruction's relative-error
	// bound without relying on the host vendor's approximation table.
	const __m128d input  = _mm_set_sd(static_cast<double>(std::bit_cast<float>(bits)));
	const __m128d result = _mm_div_sd(_mm_set_sd(1.0), _mm_sqrt_sd(input, input));
	return std::bit_cast<uint32_t>(_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(), result)));
}

static bool TryEmulateReciprocalSquareRoot(Context& context) {
	const auto* rip            = reinterpret_cast<const uint8_t*>(context.Rip());
	size_t      prefix_size    = 0;
	uint8_t     dest_extension = 0;
	uint8_t     src_extension  = 0;
	if (rip[0] == 0xc5 && (rip[1] & 0x7fu) == 0x70u) {
		prefix_size    = 2;
		dest_extension = (~rip[1] & 0x80u) >> 4u;
	} else if (rip[0] == 0xc4 && (rip[1] & 0x1fu) == 1 && (rip[2] & 0x7fu) == 0x70u) {
		prefix_size    = 3;
		dest_extension = (~rip[1] & 0x80u) >> 4u;
		src_extension  = (~rip[1] & 0x20u) >> 2u;
	} else {
		return false;
	}
	if (rip[prefix_size] != 0x52 || (rip[prefix_size + 1] & 0xc0u) != 0xc0u) {
		return false;
	}

	const uint8_t modrm    = rip[prefix_size + 1];
	const uint8_t dest     = ((modrm >> 3u) & 7u) | dest_extension;
	const uint8_t source   = (modrm & 7u) | src_extension;
	auto*         dest_xmm = context.Xmm(dest);
	auto*         src_xmm  = context.Xmm(source);
	if (dest_xmm == nullptr || src_xmm == nullptr) {
		return false;
	}
	XmmWords result {};
	std::memcpy(&result, src_xmm, sizeof(result));
	// RSQRT ignores the rounding mode and never changes guest exception flags.
	// Mask host exceptions while calculating, then restore the handler's state.
	const uint32_t mxcsr = _mm_getcsr();
	_mm_setcsr(0x1f80);
	for (auto& word: result.w) {
		word = ReciprocalSquareRoot(word);
	}
	_mm_setcsr(mxcsr);
	std::memcpy(dest_xmm, &result, sizeof(result));
	context.ClearUpperYmm(dest);
	context.Advance(prefix_size + 2);
	return true;
}

#endif

bool IsReciprocalSquareRoot(const ZydisDecodedInstruction& instruction,
                            const ZydisDecodedOperand* operands) {
	return instruction.mnemonic == ZYDIS_MNEMONIC_VRSQRTPS &&
	       instruction.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX &&
	       instruction.raw.vex.offset == 0 && operands[0].size == 128 &&
	       operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER;
}

uint64_t PatchReciprocalSquareRoots(uint64_t address, uint64_t size) {
	uint64_t patched = 0;
#if !defined(__APPLE__)
	ZydisDecoder decoder {};
	if (!ZYAN_SUCCESS(
	        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
		return 0;
	}
	for (uint64_t offset = 0; offset < size;) {
		auto*                   code = reinterpret_cast<uint8_t*>(address + offset);
		ZydisDecodedInstruction instruction {};
		ZydisDecodedOperand     operands[ZYDIS_MAX_OPERAND_COUNT] {};
		if (!ZYAN_SUCCESS(
		        ZydisDecoderDecodeFull(&decoder, code, size - offset, &instruction, operands))) {
			++offset;
			continue;
		}
		if (IsReciprocalSquareRoot(instruction, operands)) {
			// vvvv is reserved (must be 1111b). Clear one bit to route this
			// otherwise intact instruction through the illegal-instruction emulator.
			code[instruction.raw.vex.size - 1] &= ~0x08u;
			++patched;
		}
		offset += instruction.length;
	}
#else
	(void)address;
	(void)size;
#endif
	return patched;
}

bool TryEmulate(void* native_context, InstructionType* out_type) {
	if (out_type != nullptr) {
		*out_type = InstructionType::Unknown;
	}
	if (native_context == nullptr) {
		return false;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	Context context {static_cast<PCONTEXT>(native_context)};
#elif defined(__APPLE__)
	auto* saved_context = static_cast<ucontext_t*>(native_context);
	if (saved_context->uc_mcontext == nullptr) {
		return false;
	}
	Context context {saved_context};
#else
	Context context {static_cast<ucontext_t*>(native_context)};
#endif
#if !defined(__APPLE__)
	if (TryEmulateReciprocalSquareRoot(context)) {
		if (out_type != nullptr) {
			*out_type = InstructionType::Vrsqrtps;
		}
		return true;
	}
	if (TryEmulateMonitorxMwaitx(context)) {
		if (out_type != nullptr) {
			*out_type = InstructionType::MonitorxMwaitx;
		}
		return true;
	}
	if (TryEmulateAmdSystem(context, out_type)) {
		return true;
	}
	if (TryEmulateSse4a(context, out_type)) {
		return true;
	}
	if (TryEmulateShaNi(context)) {
		if (out_type != nullptr) {
			*out_type = InstructionType::ShaNi;
		}
		return true;
	}
	return false;
#else
	return TryEmulateSse4a(context, out_type);
#endif
}

const char* InstructionTypeName(InstructionType type) {
	switch (type) {
		case InstructionType::Vrsqrtps: return "vrsqrtps";
		case InstructionType::Rdpru: return "rdpru";
		case InstructionType::Clzero: return "clzero";
		case InstructionType::Movntss: return "movntss";
		case InstructionType::Movntsd: return "movntsd";
		case InstructionType::Extrq: return "extrq";
		case InstructionType::Insertq: return "insertq";
		case InstructionType::MonitorxMwaitx: return "monitorx_mwaitx";
		case InstructionType::ShaNi: return "shani";
		case InstructionType::Unhandled: return "unhandled";
		default: return "unknown";
	}
}

namespace {

struct CumulativeEmulationStats {
	std::array<std::atomic<uint64_t>, static_cast<size_t>(InstructionType::Count)> counts {};
	std::array<std::atomic<uint64_t>, static_cast<size_t>(InstructionType::Count)> total_ns {};
	std::array<std::atomic<uint64_t>, static_cast<size_t>(InstructionType::Count)> max_ns {};
	std::atomic<uint64_t> total_ud_count {0};
	std::atomic<uint64_t> total_ud_ns {0};
};

struct EmulationSite {
	uint64_t        pc         = 0;
	InstructionType type       = InstructionType::Unknown;
	std::string     thread_name;
	uint64_t        count      = 0;
	uint64_t        total_ns   = 0;
	uint64_t        max_ns     = 0;
};

static CumulativeEmulationStats                      g_cum_stats;
static std::mutex                                     g_emu_site_mutex;
static std::unordered_map<uint64_t, EmulationSite>    g_emu_sites;

struct EmulationSnapshot {
	std::array<uint64_t, static_cast<size_t>(InstructionType::Count)> counts {};
	std::array<uint64_t, static_cast<size_t>(InstructionType::Count)> total_ns {};
	uint64_t                                                          total_ud_count = 0;
	uint64_t                                                          total_ud_ns    = 0;
};

static EmulationSnapshot g_last_snapshot {};

} // namespace

void RecordEmulation(InstructionType type, uint64_t pc, uint64_t duration_ns,
                     const char* thread_name) {
	const auto idx = static_cast<size_t>(type);
	if (idx < static_cast<size_t>(InstructionType::Count)) {
		g_cum_stats.counts[idx].fetch_add(1, std::memory_order_relaxed);
		g_cum_stats.total_ns[idx].fetch_add(duration_ns, std::memory_order_relaxed);
		auto prev_max = g_cum_stats.max_ns[idx].load(std::memory_order_relaxed);
		while (duration_ns > prev_max &&
		       !g_cum_stats.max_ns[idx].compare_exchange_weak(prev_max, duration_ns,
		                                                      std::memory_order_relaxed)) {}
	}
	g_cum_stats.total_ud_count.fetch_add(1, std::memory_order_relaxed);
	g_cum_stats.total_ud_ns.fetch_add(duration_ns, std::memory_order_relaxed);

	std::lock_guard lock(g_emu_site_mutex);
	auto&           site = g_emu_sites[pc];
	if (site.count == 0) {
		site.pc          = pc;
		site.type        = type;
		site.thread_name = (thread_name != nullptr && thread_name[0] != '\0') ? thread_name : "(unnamed)";
	}
	site.count++;
	site.total_ns += duration_ns;
	if (duration_ns > site.max_ns) {
		site.max_ns = duration_ns;
	}
}

std::string FormatEmulationReport(uint64_t frames, double seconds, bool interval) {
	EmulationSnapshot current {};
	for (size_t i = 0; i < static_cast<size_t>(InstructionType::Count); ++i) {
		current.counts[i]   = g_cum_stats.counts[i].load(std::memory_order_relaxed);
		current.total_ns[i] = g_cum_stats.total_ns[i].load(std::memory_order_relaxed);
	}
	current.total_ud_count = g_cum_stats.total_ud_count.load(std::memory_order_relaxed);
	current.total_ud_ns    = g_cum_stats.total_ud_ns.load(std::memory_order_relaxed);

	uint64_t delta_ud_count = 0;
	uint64_t delta_ud_ns    = 0;
	std::array<uint64_t, static_cast<size_t>(InstructionType::Count)> delta_counts {};
	std::array<uint64_t, static_cast<size_t>(InstructionType::Count)> delta_ns {};

	if (interval) {
		delta_ud_count = current.total_ud_count - g_last_snapshot.total_ud_count;
		delta_ud_ns    = current.total_ud_ns - g_last_snapshot.total_ud_ns;
		for (size_t i = 0; i < static_cast<size_t>(InstructionType::Count); ++i) {
			delta_counts[i] = current.counts[i] - g_last_snapshot.counts[i];
			delta_ns[i]     = current.total_ns[i] - g_last_snapshot.total_ns[i];
		}
		g_last_snapshot = current;
	} else {
		delta_ud_count = current.total_ud_count;
		delta_ud_ns    = current.total_ud_ns;
		for (size_t i = 0; i < static_cast<size_t>(InstructionType::Count); ++i) {
			delta_counts[i] = current.counts[i];
			delta_ns[i]     = current.total_ns[i];
		}
	}

	std::unordered_map<uint64_t, EmulationSite> sites;
	{
		std::lock_guard lock(g_emu_site_mutex);
		if (interval) {
			sites.swap(g_emu_sites);
		} else {
			sites = g_emu_sites;
		}
	}

	const auto per = [frames](double value) -> double {
		return frames == 0 ? 0.0 : value / static_cast<double>(frames);
	};

	const double total_emu_ms = static_cast<double>(delta_ud_ns) / 1e6;
	const double ud_per_sec   = seconds > 0.0 ? static_cast<double>(delta_ud_count) / seconds : 0.0;
	const double ud_per_frame = per(static_cast<double>(delta_ud_count));
	const double emu_ms_per_frame = per(total_emu_ms);

	// Estimated Windows x64 VEH hardware #UD trap roundtrip overhead is ~2.5 µs (0.0025 ms) per trap.
	const double est_veh_ms = total_emu_ms + static_cast<double>(delta_ud_count) * 0.0025;
	const double est_veh_ms_per_frame = per(est_veh_ms);

	std::string text;
	if (delta_ud_count == 0) {
		text = fmt::format(
		    "cpu-emulation: {:.1f}s #UD=0 (0.0/s, 0.0/frame) | total=0.00ms (0.00ms/frame) | no illegal instruction traps\n",
		    seconds);
	} else {
		text = fmt::format(
		    "cpu-emulation: {:.1f}s #UD={} ({:.1f}/s, {:.1f}/frame) | total={:.2f}ms ({:.2f}ms/frame) | est-veh={:.2f}ms ({:.2f}ms/frame) | vrsqrtps={} rdpru={} clzero={} movntss={} movntsd={} extrq={} insertq={} monitorx_mwaitx={} shani={} unhandled={}\n",
		    seconds, delta_ud_count, ud_per_sec, ud_per_frame,
		    total_emu_ms, emu_ms_per_frame,
		    est_veh_ms, est_veh_ms_per_frame,
		    delta_counts[static_cast<size_t>(InstructionType::Vrsqrtps)],
		    delta_counts[static_cast<size_t>(InstructionType::Rdpru)],
		    delta_counts[static_cast<size_t>(InstructionType::Clzero)],
		    delta_counts[static_cast<size_t>(InstructionType::Movntss)],
		    delta_counts[static_cast<size_t>(InstructionType::Movntsd)],
		    delta_counts[static_cast<size_t>(InstructionType::Extrq)],
		    delta_counts[static_cast<size_t>(InstructionType::Insertq)],
		    delta_counts[static_cast<size_t>(InstructionType::MonitorxMwaitx)],
		    delta_counts[static_cast<size_t>(InstructionType::ShaNi)],
		    delta_counts[static_cast<size_t>(InstructionType::Unhandled)]);

		std::vector<const EmulationSite*> site_rows;
		for (const auto& [pc, site]: sites) {
			site_rows.push_back(&site);
		}
		std::sort(site_rows.begin(), site_rows.end(),
		          [](const EmulationSite* a, const EmulationSite* b) {
			          return a->total_ns > b->total_ns;
		          });

		const size_t print_count = std::min<size_t>(site_rows.size(), 50);
		for (size_t i = 0; i < print_count; ++i) {
			const auto&  s       = *site_rows[i];
			const double site_ms = static_cast<double>(s.total_ns) / 1e6;
			const double avg_ms  = s.count == 0 ? 0.0 : site_ms / static_cast<double>(s.count);
			const double max_ms  = static_cast<double>(s.max_ns) / 1e6;
			text += fmt::format(
			    "  emu-site       {:<14} pc={:#014x} thread={:<24} n={:<6} {:8.2f}ms avg={:.3f}ms max={:.3f}ms\n",
			    InstructionTypeName(s.type), s.pc, s.thread_name, s.count, site_ms, avg_ms, max_ms);
		}
	}
	return text;
}

} // namespace Loader::X64InstructionEmulator
