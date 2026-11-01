#pragma once

// Clocks and CPU pinning for the feed tools (Linux).
// The kernel stamps received packets with CLOCK_REALTIME (SO_TIMESTAMPNS), so
// latency measured from that stamp must use realtime_ns(). Pacing uses
// monotonic_ns(). Both go through the vDSO (tens of ns, no syscall).

#include <sched.h>
#include <time.h>

#include <cstdint>

namespace net {

[[nodiscard]] inline std::uint64_t to_ns(const timespec& t) noexcept {
    return static_cast<std::uint64_t>(t.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(t.tv_nsec);
}

[[nodiscard]] inline std::uint64_t realtime_ns() noexcept {
    timespec t;
    ::clock_gettime(CLOCK_REALTIME, &t);
    return to_ns(t);
}

[[nodiscard]] inline std::uint64_t monotonic_ns() noexcept {
    timespec t;
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return to_ns(t);
}

// Pins the calling thread to one CPU. Needs no privileges.
inline bool pin_to_cpu(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::sched_setaffinity(0, sizeof set, &set) == 0;
}

}  // namespace net
