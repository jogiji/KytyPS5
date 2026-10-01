#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <Zydis/DecoderTypes.h>
#include <cstdint>
#include <string>

namespace Loader::X64InstructionEmulator {

enum class InstructionType : uint8_t {
	Unknown = 0,
	Vrsqrtps,
	Rdpru,
	Clzero,
	Movntss,
	Movntsd,
	Extrq,
	Insertq,
	MonitorxMwaitx,
	ShaNi,
	Unhandled,
	Count
};

const char* InstructionTypeName(InstructionType type);

[[nodiscard]] bool IsReciprocalSquareRoot(const ZydisDecodedInstruction& instruction,
                                         const ZydisDecodedOperand* operands);
uint64_t           PatchReciprocalSquareRoots(uint64_t address, uint64_t size);
[[nodiscard]] bool TryEmulate(void* native_context, InstructionType* out_type = nullptr);

void RecordEmulation(InstructionType type, uint64_t pc, uint64_t duration_ns,
                     const char* thread_name);

std::string FormatEmulationReport(uint64_t frames, double seconds, bool interval = true);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
