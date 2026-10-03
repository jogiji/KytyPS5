#include "graphics/host_gpu/renderer/gpuGapTrace.h"

#include <cstdio>

int main() {
	using namespace Libs::Graphics::GpuGapTrace;
	int failures = 0;
	const auto check = [&failures](bool condition, const char* message) {
		if (!condition) {
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}

	};
	check(!Enabled(), "disabled environment leaves tracing inactive");
	Ring<2> ring;
	check(ring.Push({1, 2, 3, 4, 5, 0}), "first bounded event is accepted");
	check(ring.Push({2, 3, 4, 5, 6, 0}), "second bounded event is accepted");
	check(!ring.Push({3, 4, 5, 6, 7, 0}), "full ring drops without blocking");
	check(ring.Dropped() == 1, "full ring records a dropped-event count");
	uint64_t cursor = 0;
	const auto* first = ring.Peek(cursor);
	check(first != nullptr && first->timestamp_ns == 1, "consumer reads published records in order");
	ring.Consume(cursor);
	check(ring.Push({3, 4, 5, 6, 7, 0}), "consumer progress releases bounded capacity");
	uint64_t mapped_ns = 0;
	uint64_t deviation_ns = 0;
	check(!GpuTicksToNs(110, mapped_ns, deviation_ns), "clock mapping is unavailable before calibration");
	SetGpuClockMapping(100, 1000, 2.0, 7);
	check(GpuTicksToNs(110, mapped_ns, deviation_ns), "calibrated GPU timestamp maps to CPU time");
	check(mapped_ns == 1020 && deviation_ns == 7, "mapping preserves scale, offset and uncertainty");
	if (failures == 0) {
		std::puts("GpuGapTraceTests: all passed");
	}
	return failures == 0 ? 0 : 1;
}
