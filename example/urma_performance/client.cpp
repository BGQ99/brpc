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

#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

#include <gflags/gflags.h>

#include "butil/atomicops.h"
#include "butil/fast_rand.h"
#include "butil/logging.h"
#include "butil/time.h"
#include "brpc/channel.h"
#include "brpc/controller.h"
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

bvar::LatencyRecorder* g_latency = nullptr;
bvar::Adder<int64_t> g_error_count("client_error_count");
butil::atomic<int64_t> g_latency_sum(0);
butil::atomic<int64_t> g_success_count(0);
butil::atomic<int64_t> g_min_latency_us(
    std::numeric_limits<int64_t>::max());
butil::atomic<bool> g_stop(false);

namespace brpc {
namespace urma {
DECLARE_bool(urma_use_polling);
}  // namespace urma
}  // namespace brpc

struct WorkerArgs {
    test::PerfTestService_Stub* stub;
    int64_t deadline_us;
};

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

static void* worker(void* arg) {
    WorkerArgs* args = static_cast<WorkerArgs*>(arg);
    int qps = FLAGS_expected_qps;
    while (!brpc::IsAskedToQuit() &&
           !g_stop.load(butil::memory_order_relaxed) &&
           butil::monotonic_time_us() < args->deadline_us) {
        butil::FastRandSeed seed;
        butil::init_fast_rand_seed(&seed);
        std::vector<brpc::Controller> cntls(FLAGS_queue_depth);
        std::vector<test::PerfTestRequest> reqs(FLAGS_queue_depth);
        std::vector<test::PerfTestResponse> resps(FLAGS_queue_depth);
        std::vector<brpc::CallId> ids(FLAGS_queue_depth);
        int issued = 0;
        for (; issued < FLAGS_queue_depth; ++issued) {
            if (brpc::IsAskedToQuit() ||
                g_stop.load(butil::memory_order_relaxed) ||
                butil::monotonic_time_us() >= args->deadline_us) {
                break;
            }
            cntls[issued].set_log_id(butil::fast_rand(&seed) & 0x7fffffff);
            reqs[issued].set_echo_attachment(FLAGS_echo_attachment);
            if (FLAGS_attachment_size >= 0) {
                cntls[issued].request_attachment().resize(
                    FLAGS_attachment_size, 'a');
            }
            ids[issued] = cntls[issued].call_id();
            args->stub->Test(&cntls[issued], &reqs[issued], &resps[issued],
                             brpc::DoNothing());
        }
        for (int i = 0; i < issued; ++i) {
            brpc::Join(ids[i]);
            if (cntls[i].Failed()) {
                g_error_count << 1;
                LOG_EVERY_SECOND(WARNING)
                    << "RPC failed: " << cntls[i].ErrorText();
            } else {
                record_latency(cntls[i].latency_us());
            }
        }
        if (qps > 0 && issued > 0) {
            const int64_t sleep_us = issued * 1000000LL / qps;
            const int64_t remaining_us =
                args->deadline_us - butil::monotonic_time_us();
            if (remaining_us > 0) {
                bthread_usleep(static_cast<uint64_t>(
                    std::min(sleep_us, remaining_us)));
            }
        }
    }
    return nullptr;
}

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
    brpc::ChannelOptions options;
    options.socket_mode = FLAGS_use_urma ? brpc::SOCKET_MODE_URMA
                                          : brpc::SOCKET_MODE_TCP;
    options.connect_timeout_ms = FLAGS_rpc_timeout_ms;
    options.timeout_ms = FLAGS_rpc_timeout_ms;
    options.max_retry = 0;
    brpc::Channel channel;
    if (channel.Init(FLAGS_server.c_str(), &options) != 0) {
        LOG(ERROR) << "Fail to init channel to " << FLAGS_server;
        return -1;
    }
    test::PerfTestService_Stub stub(&channel);

    // Complete one RPC before starting all workers. This makes handshake and
    // data-path failures visible instead of looking like a hung benchmark.
    brpc::Controller warmup_cntl;
    warmup_cntl.set_timeout_ms(FLAGS_rpc_timeout_ms);
    test::PerfTestRequest warmup_req;
    test::PerfTestResponse warmup_resp;
    warmup_req.set_echo_attachment(false);
    stub.Test(&warmup_cntl, &warmup_req, &warmup_resp, nullptr);
    if (warmup_cntl.Failed()) {
        LOG(ERROR) << "Warm-up RPC failed after timeout_ms="
                   << FLAGS_rpc_timeout_ms << ": "
                   << warmup_cntl.ErrorText();
        return -1;
    }
    LOG(INFO) << "Warm-up RPC to " << FLAGS_server
              << " succeeded, latency=" << warmup_cntl.latency_us() << "us";

    int thread_num = FLAGS_thread_num;
    if (thread_num == 0) {
        thread_num = FLAGS_max_thread_num;
    }
    if (thread_num <= 0 || FLAGS_queue_depth <= 0) {
        LOG(ERROR) << "thread_num and queue_depth must be positive";
        return -1;
    }
    bvar::LatencyRecorder latency("client", stats_window_seconds);
    g_latency = &latency;
    const int64_t start_us = butil::monotonic_time_us();
    WorkerArgs worker_args = {&stub,
                              start_us + FLAGS_test_seconds * 1000000LL};
    std::vector<bthread_t> tids(thread_num);
    for (int i = 0; i < thread_num; ++i) {
        bthread_start_background(&tids[i], nullptr, worker, &worker_args);
    }
    LOG(INFO) << "URMA performance client started (server=" << FLAGS_server
              << ", use_urma=" << FLAGS_use_urma
              << ", threads=" << thread_num
              << ", rpc_timeout_ms=" << FLAGS_rpc_timeout_ms << ")";
    while (!brpc::IsAskedToQuit() &&
           butil::monotonic_time_us() < worker_args.deadline_us) {
        const int64_t remaining_us =
            worker_args.deadline_us - butil::monotonic_time_us();
        if (remaining_us > 0) {
            bthread_usleep(static_cast<uint64_t>(
                std::min<int64_t>(remaining_us, 1000000)));
        }
        LOG(INFO) << "rps=" << g_latency->qps(1)
                  << " avg=" << g_latency->latency(1) << "us"
                  << " errors=" << g_error_count.get_value();
    }
    g_stop.store(true, butil::memory_order_relaxed);
    for (int i = 0; i < thread_num; ++i) {
        bthread_join(tids[i], nullptr);
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
              << " avg_us=" << avg_us
              << " min_us=" << min_us
              << " p50_us=" << g_latency->latency_percentile(0.50)
              << " p90_us=" << g_latency->latency_percentile(0.90)
              << " p99_us=" << g_latency->latency_percentile(0.99)
              << " p999_us=" << g_latency->latency_percentile(0.999)
              << " max_us=" << g_latency->max_latency()
              << " rps=" << rps
              << " requests=" << requests
              << " errors=" << g_error_count.get_value() << std::endl;
    return 0;
}

#else

#include <cstdio>
int main() {
    printf("This example requires brpc built with -DWITH_URMA=ON.\n");
    return 0;
}

#endif  // BRPC_WITH_URMA
