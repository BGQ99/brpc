#include "arguments.h"
#include "core.h"
#include "statistics.h"

#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace {
struct Stage {
    uint64_t success = 0, failures = 0;
    uint64_t elapsed_ns = 0;
    int64_t cpu_ns = -1;
    echo_proto::Histogram latency;
};

Stage Run(const std::shared_ptr<echo_proto::ClientCore>& client,
          const std::string& payload, int concurrency, int seconds, uint64_t* next_id) {
    Stage stats;
    const uint64_t start = echo_proto::NowNanos();
    const uint64_t deadline = start + static_cast<uint64_t>(seconds) * 1000000000ULL;
    const int64_t cpu_start = echo_proto::CpuNanos();
    std::unordered_map<uint64_t, uint64_t> in_flight;
    auto submit = [&] {
        const uint64_t id = (*next_id)++;
        const uint64_t began = echo_proto::NowNanos();
        in_flight.emplace(id, began);
        std::string error;
        if (!client->Submit(id, payload, &error)) throw std::runtime_error(error);
    };
    for (int i = 0; i < concurrency; ++i) submit();
    while (!in_flight.empty()) {
        echo_proto::Completion result;
        if (!client->Take(&result, 100)) continue;
        auto it = in_flight.find(result.id);
        if (it == in_flight.end()) throw std::runtime_error("duplicate or unknown completion");
        const bool valid = result.error.empty() && result.sequence == result.id && result.payload == payload;
        const uint64_t ended = echo_proto::NowNanos();  // Includes payload validation.
        if (valid) { ++stats.success; stats.latency.Add(ended - it->second); }
        else { ++stats.failures; }
        in_flight.erase(it);
        if (echo_proto::NowNanos() < deadline) submit();
    }
    stats.elapsed_ns = echo_proto::NowNanos() - start;  // Includes draining the tail.
    const int64_t cpu_end = echo_proto::CpuNanos();
    if (cpu_start >= 0 && cpu_end >= cpu_start) stats.cpu_ns = cpu_end - cpu_start;
    return stats;
}

void Number(std::ostream& stream, int64_t value) {
    if (value < 0) stream << "null"; else stream << value;
}
}

int main(int argc, char** argv) {
    try {
        echo_proto::Arguments args(argc, argv);
        const int size = args.Int("size", 1024, 0, static_cast<int>(echo_proto::kMaxPayload));
        const int concurrency = args.Int("concurrency", 1, 1, 65536);
        const int capacity = args.Int("capacity", concurrency, concurrency, 65536);
        const int warmup = args.Int("warmup", 1, 0, 3600);
        const int seconds = args.Int("seconds", 5, 1, 86400);
        const int timeout = args.Int("timeout-ms", 5000, 1, 600000);
        const std::string transport = args.Text("transport", "tcp");
        const std::string group = args.Text("group", "N");
        if (group != "N" && group != "S") throw std::invalid_argument("native client group must be N or S");
        echo_proto::ConfigureUrma(args.Text("device", ""), args.Polling());
        auto client = echo_proto::ClientCore::Create(args.Text("server", "127.0.0.1:8000"), transport, timeout, capacity);
        std::string payload(static_cast<size_t>(size), '\0');
        for (int i = 0; i < size; ++i) payload[static_cast<size_t>(i)] = static_cast<char>(i * 31 + 7);
        uint64_t next_id = 1;
        // A complete RPC is excluded from measurement, even when warmup=0.
        std::string failure;
        if (!client->Submit(next_id++, payload, &failure)) throw std::runtime_error(failure);
        echo_proto::Completion first;
        while (!client->Take(&first, 100)) {}
        if (!first.error.empty() || first.sequence != 1 || first.payload != payload) throw std::runtime_error("warm-up Echo failed: " + first.error);
        if (warmup) {
            Stage warming = Run(client, payload, concurrency, warmup, &next_id);
            if (warming.failures) throw std::runtime_error("warm-up had failures");
        }
        Stage stats = Run(client, payload, concurrency, seconds, &next_id);
        client->Shutdown();
        std::cout << std::setprecision(12)
                  << "{\"schema\":1,\"build_id\":\"" << ECHO_BUILD_ID << "\",\"group\":\"" << group << "\",\"client\":\"cpp\",\"transport_requested\":\"" << transport
                  << "\",\"path_verified\":false,\"load_model\":\"closed_loop\",\"percentiles\":\"log_histogram_upper_bound\""
                  << ",\"polling\":" << (args.Polling() ? "true" : "false")
                  << ",\"size\":" << size << ",\"concurrency\":" << concurrency << ",\"capacity\":" << capacity
                  << ",\"timeout_ms\":" << timeout << ",\"warmup_seconds\":" << warmup << ",\"requested_seconds\":" << seconds
                  << ",\"elapsed_ns\":" << stats.elapsed_ns << ",\"success\":" << stats.success << ",\"failures\":" << stats.failures
                  << ",\"qps\":" << (stats.success * 1e9 / stats.elapsed_ns) << ",\"p50_ns\":";
        Number(std::cout, stats.latency.Percentile(50));
        std::cout << ",\"p95_ns\":"; Number(std::cout, stats.latency.Percentile(95));
        std::cout << ",\"p99_ns\":"; Number(std::cout, stats.latency.Percentile(99));
        std::cout << ",\"client_cpu_ns\":"; Number(std::cout, stats.cpu_ns);
        std::cout << "}" << std::endl;
        return stats.failures || stats.success == 0 ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "NATIVE_CLIENT=FAIL " << error.what() << std::endl;
        return 1;
    }
}
