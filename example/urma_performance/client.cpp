// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gflags/gflags.h>

#include "butil/atomicops.h"
#include "butil/logging.h"
#include "butil/time.h"
#include "brpc/channel.h"
#include "brpc/controller.h"
#include "brpc/urma/urma_helper.h"
#include "bthread/bthread.h"
#include "bvar/latency_recorder.h"
#include "bvar/variable.h"
#include "test.pb.h"

#if BRPC_WITH_URMA

DEFINE_string(server, "127.0.0.1:8003", "IP Port of urma performance server");
DEFINE_int32(thread_num, 0, "How many threads are used");
DEFINE_int32(queue_depth, 1, "How many requests can be pending in the queue");
DEFINE_int32(expected_qps, 0, "The expected QPS");
DEFINE_int32(max_thread_num, 16, "The max number of threads are used");
DEFINE_int32(attachment_size, -1, "Attachment size is used (in Bytes)");
DEFINE_int32(rpc_timeout_ms, 5000, "Timeout for each RPC in milliseconds");
DEFINE_int32(test_seconds, 30, "Test running time in seconds");
DEFINE_bool(echo_attachment, false, "Select whether attachment should be echo");
DEFINE_bool(use_urma, true, "Use URMA transport (true) or TCP (false)");
DEFINE_string(connection_type, "single", "Connection type of the channel");
DEFINE_string(protocol, "baidu_std", "Protocol type");

bvar::LatencyRecorder* g_latency = nullptr;
bvar::Adder<int64_t> g_error_count("client_error_count");
butil::atomic<int64_t> g_latency_sum(0);
butil::atomic<int64_t> g_success_count(0);
butil::atomic<int64_t> g_min_latency_us(
    std::numeric_limits<int64_t>::max());
butil::atomic<int64_t> g_server_cpu_sum_milli_percent(0);
butil::atomic<int64_t> g_server_cpu_samples(0);
butil::atomic<int64_t> g_client_cpu_sum_milli_percent(0);
butil::atomic<int64_t> g_client_cpu_samples(0);
butil::atomic<bool> g_stop(false);
butil::atomic<int64_t> g_token(10000);

namespace brpc {
namespace urma {
DECLARE_bool(urma_use_polling);
}  // namespace urma
}  // namespace brpc

static void record_latency(int64_t latency_us) {
    *g_latency << latency_us;
    g_latency_sum.fetch_add(latency_us, butil::memory_order_relaxed);
    g_success_count.fetch_add(1, butil::memory_order_relaxed);

    int64_t old_min = g_min_latency_us.load(butil::memory_order_relaxed);
    while (latency_us < old_min &&
           !g_min_latency_us.compare_exchange_weak(
               old_min, latency_us, butil::memory_order_relaxed)) {
    }
}

static bool parse_cpu_percent(const std::string& value,
                              int64_t* milli_percent) {
    if (value.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double ratio = std::strtod(value.c_str(), &end);
    if (errno != 0 || end == value.c_str() || *end != '\0' ||
        !std::isfinite(ratio) || ratio < 0) {
        return false;
    }
    const double scaled = ratio * 100000.0;
    if (scaled > static_cast<double>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    *milli_percent = static_cast<int64_t>(std::llround(scaled));
    return true;
}

static void record_server_cpu(const std::string& value) {
    int64_t milli_percent = 0;
    if (parse_cpu_percent(value, &milli_percent)) {
        g_server_cpu_sum_milli_percent.fetch_add(
            milli_percent, butil::memory_order_relaxed);
        g_server_cpu_samples.fetch_add(1, butil::memory_order_relaxed);
    }
}

static void record_client_cpu() {
    int64_t milli_percent = 0;
    if (parse_cpu_percent(
            bvar::Variable::describe_exposed("process_cpu_usage"),
            &milli_percent)) {
        g_client_cpu_sum_milli_percent.fetch_add(
            milli_percent, butil::memory_order_relaxed);
        g_client_cpu_samples.fetch_add(1, butil::memory_order_relaxed);
    }
}

static void* generate_token(void*) {
    const int64_t start_us = butil::monotonic_time_us();
    int64_t generated = 0;
    while (!g_stop.load(butil::memory_order_relaxed)) {
        bthread_usleep(100000);
        const int64_t elapsed_us = butil::monotonic_time_us() - start_us;
        const int64_t target =
            static_cast<int64_t>(FLAGS_expected_qps) * elapsed_us / 1000000;
        if (target > generated) {
            g_token.fetch_add(target - generated, butil::memory_order_relaxed);
            generated = target;
        }
    }
    return nullptr;
}

// Match rdma_performance's load model: each logical client thread owns one
// Channel and keeps queue_depth RPCs in flight by refilling from callbacks.
class PerformanceTest {
public:
    PerformanceTest()
        : _stub(nullptr)
        , _deadline_us(0)
        , _stop_issuing(false)
        , _outstanding(0) {
        if (FLAGS_attachment_size > 0) {
            _attachment.resize(FLAGS_attachment_size, 'a');
        }
    }

    int Init() {
        brpc::ChannelOptions options;
        options.socket_mode = FLAGS_use_urma ? brpc::SOCKET_MODE_URMA
                                              : brpc::SOCKET_MODE_TCP;
        options.protocol = FLAGS_protocol;
        options.connection_type = FLAGS_connection_type;
        options.connect_timeout_ms = FLAGS_rpc_timeout_ms;
        options.timeout_ms = FLAGS_rpc_timeout_ms;
        options.max_retry = 0;
        if (_channel.Init(FLAGS_server.c_str(), &options) != 0) {
            LOG(ERROR) << "Fail to initialize channel to " << FLAGS_server;
            return -1;
        }
        _stub.reset(new test::PerfTestService_Stub(&_channel));

        brpc::Controller cntl;
        cntl.set_timeout_ms(FLAGS_rpc_timeout_ms);
        test::PerfTestRequest request;
        test::PerfTestResponse response;
        request.set_echo_attachment(FLAGS_echo_attachment);
        _stub->Test(&cntl, &request, &response, nullptr);
        if (cntl.Failed()) {
            LOG(ERROR) << "Warm-up RPC failed after timeout_ms="
                       << FLAGS_rpc_timeout_ms << ": " << cntl.ErrorText();
            return -1;
        }
        LOG(INFO) << "Warm-up RPC to " << FLAGS_server
                  << " succeeded, latency=" << cntl.latency_us() << "us";
        return 0;
    }

    void Start(int64_t deadline_us) {
        _deadline_us = deadline_us;
        for (int i = 0; i < FLAGS_queue_depth; ++i) {
            if (!SendRequest()) {
                break;
            }
        }
    }

    void Stop() {
        _stop_issuing.store(true, butil::memory_order_release);
    }

    void Wait() const {
        while (_outstanding.load(butil::memory_order_acquire) != 0) {
            bthread_usleep(1000);
        }
    }

private:
    struct CallContext {
        PerformanceTest* test;
        brpc::Controller cntl;
        test::PerfTestRequest request;
        test::PerfTestResponse response;
    };

    bool CanIssue() const {
        return !brpc::IsAskedToQuit() &&
               !g_stop.load(butil::memory_order_acquire) &&
               !_stop_issuing.load(butil::memory_order_acquire) &&
               butil::monotonic_time_us() < _deadline_us;
    }

    bool AcquireToken() const {
        if (FLAGS_expected_qps <= 0) {
            return true;
        }
        while (CanIssue()) {
            int64_t token = g_token.load(butil::memory_order_relaxed);
            if (token > 0 && g_token.compare_exchange_weak(
                                 token, token - 1,
                                 butil::memory_order_relaxed)) {
                return true;
            }
            bthread_usleep(10);
        }
        return false;
    }

    bool SendRequest() {
        if (!CanIssue() || !AcquireToken()) {
            return false;
        }
        std::unique_ptr<CallContext> call(new CallContext);
        call->test = this;
        call->request.set_echo_attachment(FLAGS_echo_attachment);
        call->cntl.request_attachment().append(_attachment);
        _outstanding.fetch_add(1, butil::memory_order_release);
        _stub->Test(&call->cntl, &call->request, &call->response,
                    brpc::NewCallback(&PerformanceTest::HandleResponse,
                                      call.release()));
        return true;
    }

    static void HandleResponse(CallContext* call) {
        std::unique_ptr<CallContext> call_guard(call);
        PerformanceTest* test = call->test;
        if (call->cntl.Failed()) {
            g_error_count << 1;
            LOG_EVERY_SECOND(WARNING)
                << "RPC failed: " << call->cntl.ErrorText();
            test->_stop_issuing.store(true, butil::memory_order_release);
        } else {
            record_latency(call->cntl.latency_us());
            record_server_cpu(call->response.cpu_usage());
        }

        const bool refill = test->CanIssue();
        call_guard.reset();
        if (refill) {
            // Refill before retiring this completion so the configured depth
            // stays continuous instead of leaving a gap between batches.
            test->SendRequest();
        }
        test->_outstanding.fetch_sub(1, butil::memory_order_release);
    }

    brpc::Channel _channel;
    std::unique_ptr<test::PerfTestService_Stub> _stub;
    butil::IOBuf _attachment;
    int64_t _deadline_us;
    butil::atomic<bool> _stop_issuing;
    butil::atomic<int> _outstanding;
};

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_test_seconds <= 0) {
        LOG(ERROR) << "test_seconds must be positive";
        return -1;
    }
    const int64_t stats_window_seconds =
        static_cast<int64_t>(FLAGS_test_seconds) +
        (FLAGS_rpc_timeout_ms > 0
             ? (static_cast<int64_t>(FLAGS_rpc_timeout_ms) + 999) / 1000
             : 0) +
        2;
    if (stats_window_seconds > 3600) {
        LOG(ERROR) << "test_seconds plus RPC drain and sampling allowance must "
                      "not exceed 3600 seconds";
        return -1;
    }
    int thread_num = FLAGS_thread_num;
    if (thread_num == 0) {
        thread_num = FLAGS_max_thread_num;
    }
    if (thread_num <= 0 || FLAGS_queue_depth <= 0) {
        LOG(ERROR) << "thread_num and queue_depth must be positive";
        return -1;
    }
    if (FLAGS_use_urma) {
        brpc::urma::GlobalUrmaInitializeOrDie();
    }
    bvar::LatencyRecorder latency("client", stats_window_seconds);
    g_latency = &latency;

    std::vector<std::unique_ptr<PerformanceTest> > tests;
    tests.reserve(thread_num);
    for (int i = 0; i < thread_num; ++i) {
        std::unique_ptr<PerformanceTest> test(new PerformanceTest);
        if (test->Init() != 0) {
            return -1;
        }
        tests.push_back(std::move(test));
    }

    const int64_t start_us = butil::monotonic_time_us();
    const int64_t deadline_us =
        start_us + FLAGS_test_seconds * 1000000LL;
    bthread_t token_tid = 0;
    if (FLAGS_expected_qps > 0 &&
        bthread_start_background(&token_tid, nullptr, generate_token, nullptr) !=
            0) {
        LOG(ERROR) << "Fail to start QPS token generator";
        return -1;
    }
    for (int i = 0; i < thread_num; ++i) {
        tests[i]->Start(deadline_us);
    }
    LOG(INFO) << "URMA performance client started (server=" << FLAGS_server
              << ", use_urma=" << FLAGS_use_urma
              << ", threads=" << thread_num
              << ", queue_depth=" << FLAGS_queue_depth
              << ", rpc_timeout_ms=" << FLAGS_rpc_timeout_ms << ")";
    while (!brpc::IsAskedToQuit() &&
           butil::monotonic_time_us() < deadline_us) {
        const int64_t remaining_us =
            deadline_us - butil::monotonic_time_us();
        if (remaining_us > 0) {
            bthread_usleep(static_cast<uint64_t>(
                std::min<int64_t>(remaining_us, 1000000)));
        }
        record_client_cpu();
        LOG(INFO) << "rps=" << g_latency->qps(1)
                  << " avg=" << g_latency->latency(1) << "us"
                  << " errors=" << g_error_count.get_value();
    }
    g_stop.store(true, butil::memory_order_release);
    for (int i = 0; i < thread_num; ++i) {
        tests[i]->Stop();
    }
    for (int i = 0; i < thread_num; ++i) {
        tests[i]->Wait();
    }
    if (FLAGS_expected_qps > 0) {
        bthread_join(token_tid, nullptr);
    }

    const int64_t end_us = butil::monotonic_time_us();
    const int64_t requests =
        g_success_count.load(butil::memory_order_relaxed);
    const double elapsed_seconds = (end_us - start_us) / 1000000.0;
    const double avg_us = requests > 0
                              ? static_cast<double>(g_latency_sum.load(
                                    butil::memory_order_relaxed)) / requests
                              : 0;
    const double rps = elapsed_seconds > 0 ? requests / elapsed_seconds : 0;
    const double throughput_mb_s =
        elapsed_seconds > 0 && FLAGS_attachment_size > 0
            ? requests * static_cast<double>(FLAGS_attachment_size) /
                  elapsed_seconds / 1000000.0
            : 0;
    const int64_t server_cpu_samples =
        g_server_cpu_samples.load(butil::memory_order_relaxed);
    const int64_t client_cpu_samples =
        g_client_cpu_samples.load(butil::memory_order_relaxed);
    const double server_cpu_percent =
        server_cpu_samples > 0
            ? g_server_cpu_sum_milli_percent.load(
                  butil::memory_order_relaxed) /
                  (1000.0 * server_cpu_samples)
            : 0;
    const double client_cpu_percent =
        client_cpu_samples > 0
            ? g_client_cpu_sum_milli_percent.load(
                  butil::memory_order_relaxed) /
                  (1000.0 * client_cpu_samples)
            : 0;

    // LatencyRecorder samples once per second. Allow the final partial second
    // to be sampled before reading whole-run percentiles and maximum latency.
    bthread_usleep(1100000);
    int64_t min_us = g_min_latency_us.load(butil::memory_order_relaxed);
    if (requests == 0) {
        min_us = 0;
    }
    std::cout << std::fixed << std::setprecision(2)
              << "RESULT transport=" << (FLAGS_use_urma ? "URMA" : "TCP")
              << " polling=" << (brpc::urma::FLAGS_urma_use_polling
                                       ? "true" : "false")
              << " payload=" << FLAGS_attachment_size
              << " thread_num=" << thread_num
              << " avg_us=" << avg_us
              << " min_us=" << min_us
              << " p50_us=" << g_latency->latency_percentile(0.50)
              << " p90_us=" << g_latency->latency_percentile(0.90)
              << " p99_us=" << g_latency->latency_percentile(0.99)
              << " p999_us=" << g_latency->latency_percentile(0.999)
              << " max_us=" << g_latency->max_latency()
              << " rps=" << rps
              << " server_cpu_percent=" << server_cpu_percent
              << " client_cpu_percent=" << client_cpu_percent
              << " throughput_mb_s=" << throughput_mb_s
              << " requests=" << requests
              << " errors=" << g_error_count.get_value()
              << " server_cpu_samples=" << server_cpu_samples
              << " client_cpu_samples=" << client_cpu_samples << std::endl;
    return 0;
}

#else

#include <cstdio>
int main() {
    printf("This example requires brpc built with -DWITH_URMA=ON.\n");
    return 0;
}

#endif  // BRPC_WITH_URMA
