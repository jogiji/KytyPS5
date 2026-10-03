#include "graphics/host_gpu/renderer/gpuGapTrace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Libs::Graphics::GpuGapTrace {
namespace {

constexpr size_t RingCapacity = 65536;
using ThreadRing = Ring<RingCapacity>;

std::mutex             g_rings_mutex;
std::vector<ThreadRing*> g_rings;
std::mutex             g_output_mutex;
std::mutex             g_mapping_mutex;
std::ofstream          g_output;
bool                   g_header_written = false;
bool                   g_mapping_valid = false;
uint64_t               g_mapping_ticks = 0;
uint64_t               g_mapping_ns = 0;
uint64_t               g_mapping_deviation_ns = 0;
double                 g_mapping_period_ns = 0.0;

uint64_t CpuClockNs() noexcept {
#ifdef _WIN32
	static const int64_t frequency = [] {
		LARGE_INTEGER value {};
		QueryPerformanceFrequency(&value);
		return value.QuadPart;
	}();
	LARGE_INTEGER value {};
	QueryPerformanceCounter(&value);
	return frequency > 0
	           ? static_cast<uint64_t>(static_cast<long double>(value.QuadPart) * 1000000000.0L /
	                                  static_cast<long double>(frequency))
	           : 0;
#else
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
#endif
}

uint64_t QpcToNs(uint64_t qpc) noexcept {
#ifdef _WIN32
	static const uint64_t frequency = [] {
		LARGE_INTEGER value {};
		QueryPerformanceFrequency(&value);
		return static_cast<uint64_t>(value.QuadPart);
	}();
	return frequency != 0 ? static_cast<uint64_t>(static_cast<long double>(qpc) * 1000000000.0L /
	                                               static_cast<long double>(frequency))
	                      : 0;
#else
	return qpc;
#endif
}

ThreadRing* LocalRing() {
	thread_local ThreadRing* ring = [] {
		auto* value = new ThreadRing;
		std::lock_guard lock(g_rings_mutex);
		g_rings.push_back(value);
		return value;
	}();
	return ring;
}

const char* Name(Event event) noexcept {
	switch (event) {
		case Event::ThreadRole: return "thread_role";
		case Event::ProducerEnqueue: return "producer_enqueue";
		case Event::ProducerDequeue: return "producer_dequeue";
		case Event::ProducerCommandEnqueue: return "producer_command_enqueue";
		case Event::ProducerCommandDequeue: return "producer_command_dequeue";
		case Event::ThreadGpuIdleBegin: return "thread_gpu_idle_begin";
		case Event::ThreadGpuIdleEnd: return "thread_gpu_idle_end";
		case Event::ThreadGpuBlockedBegin: return "thread_gpu_blocked_begin";
		case Event::ThreadGpuBlockedEnd: return "thread_gpu_blocked_end";
		case Event::ThreadGpuProcessBegin: return "thread_gpu_process_begin";
		case Event::ThreadGpuProcessEnd: return "thread_gpu_process_end";
		case Event::ProducerIdleWaitBegin: return "producer_idle_wait_begin";
		case Event::ProducerIdleWaitEnd: return "producer_idle_wait_end";
		case Event::ExplicitSemaphoreWaitBegin: return "explicit_semaphore_wait_begin";
		case Event::ExplicitSemaphoreWaitEnd: return "explicit_semaphore_wait_end";
		case Event::Pm4Begin: return "pm4_begin";
		case Event::Pm4End: return "pm4_end";
		case Event::Pm4DrawHandlerSlow: return "pm4_draw_handler_slow";
		case Event::Pm4DispatchHandlerSlow: return "pm4_dispatch_handler_slow";
		case Event::DrawPhaseSlow: return "draw_phase_slow";
		case Event::StreamPublish: return "stream_publish";
		case Event::StreamDepthSample: return "stream_depth_sample";
		case Event::StreamConsumeBegin: return "stream_consume_begin";
		case Event::StreamConsumeEnd: return "stream_consume_end";
		case Event::RecordBatchBegin: return "record_batch_begin";
		case Event::RecordBatchEnd: return "record_batch_end";
		case Event::StreamEmptyBegin: return "stream_empty_begin";
		case Event::StreamEmptyEnd: return "stream_empty_end";
		case Event::StreamBackpressureBegin: return "stream_backpressure_begin";
		case Event::StreamBackpressureEnd: return "stream_backpressure_end";
		case Event::SubmitQueued: return "submit_queued";
		case Event::SubmitDequeued: return "submit_dequeued";
		case Event::SubmitQueueSample: return "submit_queue_sample";
		case Event::SubmitBegin: return "submit_begin";
		case Event::SubmitEnd: return "submit_end";
		case Event::GpuGap: return "gpu_gap";
		case Event::ClockCalibration: return "clock_calibration";
		case Event::ClockCalibrationFailure: return "clock_calibration_failure";
	}
	return "unknown";
}

void At(uint64_t timestamp_ns, Event event, uint64_t value0, uint64_t value1) noexcept {
	if (!Enabled()) {
		return;
	}
	static thread_local const uint64_t thread_id =
	    static_cast<uint64_t>(std::hash<std::thread::id> {}(std::this_thread::get_id()));
	LocalRing()->Push({timestamp_ns, value0, value1, thread_id, static_cast<uint16_t>(event), 0});
}

std::string OutputPath() {
	if (const char* path = std::getenv("KYTY_GPU_GAP_TRACE_FILE"); path != nullptr && path[0] != '\0') {
		return path;
	}
	return "_perf_gpu_gap_trace.csv";
}

} // namespace

bool Enabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_GAP_TRACE");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

uint64_t NowNs() noexcept {
	return CpuClockNs();
}

uint64_t HostCounterToNs(uint64_t counter) noexcept {
	return QpcToNs(counter);
}

void Instant(Event event, uint64_t value0, uint64_t value1) noexcept {
	At(NowNs(), event, value0, value1);
}

void Span(Event begin, Event end, uint64_t start_ns, uint64_t value0, uint64_t value1) noexcept {
	At(start_ns, begin, value0, value1);
	At(NowNs(), end, value0, value1);
}

void DurationAt(Event event, uint64_t end_ns, uint64_t duration_ns, uint64_t value1) noexcept {
	At(end_ns, event, duration_ns, value1);
}

void GpuGap(uint64_t start_ns, uint64_t end_ns, uint64_t max_deviation_ns) noexcept {
	At(start_ns, Event::GpuGap, end_ns >= start_ns ? end_ns - start_ns : 0, max_deviation_ns);
}

void SetGpuClockMapping(uint64_t gpu_ticks, uint64_t cpu_ns, double timestamp_period_ns,
                        uint64_t max_deviation_ns) noexcept {
	{
		std::lock_guard lock(g_mapping_mutex);
		g_mapping_ticks = gpu_ticks;
		g_mapping_ns = cpu_ns;
		g_mapping_period_ns = timestamp_period_ns;
		g_mapping_deviation_ns = max_deviation_ns;
		g_mapping_valid = timestamp_period_ns > 0.0;
	}
	At(cpu_ns, Event::ClockCalibration, gpu_ticks, max_deviation_ns);
}

bool GpuTicksToNs(uint64_t ticks, uint64_t& ns, uint64_t& max_deviation_ns) noexcept {
	std::lock_guard lock(g_mapping_mutex);
	if (!g_mapping_valid) {
		return false;
	}
	const auto delta = static_cast<long double>(ticks) - static_cast<long double>(g_mapping_ticks);
	const auto mapped = static_cast<long double>(g_mapping_ns) + delta * g_mapping_period_ns;
	if (mapped < 0.0L || mapped > static_cast<long double>(UINT64_MAX)) {
		return false;
	}
	ns = static_cast<uint64_t>(mapped);
	max_deviation_ns = g_mapping_deviation_ns;
	return true;
}

void SnapshotAndWrite() {
	if (!Enabled()) {
		return;
	}
	std::lock_guard output_lock(g_output_mutex);
	if (!g_output.is_open()) {
		g_output.open(OutputPath(), std::ios::out | std::ios::app);
		if (!g_output) {
			return;
		}
	}
	if (!g_header_written) {
		g_output << "timestamp_ns,event,thread_id,value0,value1\n";
		g_header_written = true;
	}
	std::lock_guard rings_lock(g_rings_mutex);
	for (auto* ring: g_rings) {
		uint64_t cursor = ring->Read();
		const auto limit = ring->Published();
		while (cursor < limit) {
			const auto* record = ring->Peek(cursor);
			if (record == nullptr) {
				break;
			}
			g_output << record->timestamp_ns << ',' << Name(static_cast<Event>(record->event)) << ','
			         << record->thread_id << ',' << record->value0 << ',' << record->value1 << '\n';
		}
		ring->Consume(cursor);
		g_output << NowNs() << ",ring_dropped,0," << ring->Dropped() << ",0\n";
	}
	g_output.flush();
}

} // namespace Libs::Graphics::GpuGapTrace
