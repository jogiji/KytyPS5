#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUGAPTRACE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUGAPTRACE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics::GpuGapTrace {

enum class Event : uint16_t {
	ThreadRole,
	ProducerEnqueue,
	ProducerDequeue,
	ProducerCommandEnqueue,
	ProducerCommandDequeue,
	ThreadGpuIdleBegin,
	ThreadGpuIdleEnd,
	ThreadGpuBlockedBegin,
	ThreadGpuBlockedEnd,
	ThreadGpuProcessBegin,
	ThreadGpuProcessEnd,
	ProducerIdleWaitBegin,
	ProducerIdleWaitEnd,
	ExplicitSemaphoreWaitBegin,
	ExplicitSemaphoreWaitEnd,
	Pm4Begin,
	Pm4End,
	Pm4DrawHandlerSlow,
	Pm4DispatchHandlerSlow,
	DrawPhaseSlow,
	StreamPublish,
	StreamDepthSample,
	StreamConsumeBegin,
	StreamConsumeEnd,
	RecordBatchBegin,
	RecordBatchEnd,
	StreamEmptyBegin,
	StreamEmptyEnd,
	StreamBackpressureBegin,
	StreamBackpressureEnd,
	SubmitQueued,
	SubmitDequeued,
	SubmitQueueSample,
	SubmitBegin,
	SubmitEnd,
	GpuGap,
	ClockCalibration,
	ClockCalibrationFailure,
};

struct Record {
	uint64_t timestamp_ns = 0;
	uint64_t value0 = 0;
	uint64_t value1 = 0;
	uint64_t thread_id = 0;
	uint16_t event = 0;
	uint16_t reserved = 0;
};

// A bounded single-writer buffer. A full buffer drops new records instead of blocking
// an emulator thread. The consumer may snapshot [read, published) and advance read.
template <size_t Capacity>
class Ring {
public:
	bool Push(const Record& record) noexcept {
		const auto head = published_.load(std::memory_order_relaxed);
		if (head - read_.load(std::memory_order_acquire) >= Capacity) {
			dropped_.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		records_[head % Capacity] = record;
		published_.store(head + 1, std::memory_order_release);
		return true;
	}

	const Record* Peek(uint64_t& cursor) const noexcept {
		if (cursor >= published_.load(std::memory_order_acquire)) {
			return nullptr;
		}
		return &records_[cursor++ % Capacity];
	}

	void Consume(uint64_t cursor) noexcept { read_.store(cursor, std::memory_order_release); }
	uint64_t Read() const noexcept { return read_.load(std::memory_order_acquire); }
	uint64_t Published() const noexcept { return published_.load(std::memory_order_acquire); }
	uint64_t Dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
	Record records_[Capacity] {};
	std::atomic<uint64_t> published_ {0};
	std::atomic<uint64_t> read_ {0};
	std::atomic<uint64_t> dropped_ {0};
};

[[nodiscard]] bool Enabled() noexcept;
[[nodiscard]] uint64_t NowNs() noexcept;
[[nodiscard]] uint64_t HostCounterToNs(uint64_t counter) noexcept;
void Instant(Event event, uint64_t value0 = 0, uint64_t value1 = 0) noexcept;
void Span(Event begin, Event end, uint64_t start_ns, uint64_t value0 = 0,
          uint64_t value1 = 0) noexcept;
void DurationAt(Event event, uint64_t end_ns, uint64_t duration_ns, uint64_t value1 = 0) noexcept;
void SnapshotAndWrite();
void GpuGap(uint64_t start_ns, uint64_t end_ns, uint64_t max_deviation_ns) noexcept;

// Publish a mapping from Vulkan timestamp ticks to the same monotonic nanosecond domain used
// by NowNs(). max_deviation_ns is retained as an uncertainty bound from Vulkan calibration.
void SetGpuClockMapping(uint64_t gpu_ticks, uint64_t cpu_ns, double timestamp_period_ns,
                        uint64_t max_deviation_ns) noexcept;
[[nodiscard]] bool GpuTicksToNs(uint64_t ticks, uint64_t& ns,
                                uint64_t& max_deviation_ns) noexcept;

} // namespace Libs::Graphics::GpuGapTrace

#endif
