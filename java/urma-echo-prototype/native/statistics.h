#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>

namespace echo_proto {
inline uint64_t NowNanos() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline int64_t CpuNanos() {
    timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return -1;
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// Same bounded-memory histogram in Java/C++. Percentiles report bucket upper
// bounds (64 subdivisions per power of two; relative bucket width <= 1/64).
class Histogram {
public:
    void Add(uint64_t nanos) {
        nanos = std::max<uint64_t>(1, std::min<uint64_t>(nanos, (1ULL << 55) - 1));
        unsigned power = 63U - static_cast<unsigned>(__builtin_clzll(nanos));
        uint64_t base = 1ULL << power;
        unsigned fraction = static_cast<unsigned>((nanos - base) * 64 / base);
        ++buckets_[power * 64 + fraction];
        ++count_;
    }
    int64_t Percentile(unsigned percent) const {
        if (count_ == 0) return -1;
        uint64_t rank = (count_ * percent + 99) / 100;
        uint64_t seen = 0;
        for (unsigned i = 0; i < buckets_.size(); ++i) {
            seen += buckets_[i];
            if (seen >= rank) {
                uint64_t base = 1ULL << (i / 64);
                return static_cast<int64_t>(base + ((i % 64 + 1) * base + 63) / 64 - 1);
            }
        }
        return -1;
    }
private:
    std::array<uint64_t, 56 * 64> buckets_{};
    uint64_t count_ = 0;
};
}
