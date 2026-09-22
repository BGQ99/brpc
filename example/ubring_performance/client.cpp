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
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>
#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include "butil/fast_rand.h"
#include "butil/logging.h"
#include "brpc/server.h"
#include "brpc/channel.h"
#include "bthread/bthread.h"
#include "bvar/latency_recorder.h"
#include "bvar/variable.h"
#include "test.pb.h"

#ifdef BRPC_WITH_UBRING

DEFINE_int32(thread_num, 0, "How many threads are used");
DEFINE_int32(queue_depth, 1, "How many requests can be pending in the queue");
DEFINE_int32(expected_qps, 0, "The expected QPS");
DEFINE_int32(max_thread_num, 16, "The max number of threads are used");
DEFINE_int32(attachment_size, -1, "Attachment size is used (in Bytes)");
DEFINE_bool(echo_attachment, false, "Select whether attachment should be echo");
DEFINE_string(connection_type, "single", "Connection type of the channel");
DEFINE_string(protocol, "baidu_std", "Protocol type.");
DEFINE_string(servers, "0.0.0.0:8002+0.0.0.0:8002", "IP Address of servers");
DEFINE_bool(use_ubring, false, "Use UBRING or not");
DEFINE_int32(rpc_timeout_ms, 5000, "RPC call timeout");
DEFINE_int32(warmup_seconds, 5, "Warm-up time excluded from statistics");
DEFINE_int32(test_seconds, 20, "Test running time");
DEFINE_int32(test_iterations, 0, "Test iterations");
DEFINE_int32(dummy_port, 8001, "Dummy server port number");

std::vector<std::string> g_servers;
int rr_index = 0;
std::atomic<bool> g_stop(false);

butil::atomic<int64_t> g_token(10000);

struct TestStats {
    explicit TestStats(time_t window_size)
        : latency_recorder(window_size)
        , server_cpu_recorder(window_size)
        , client_cpu_recorder(window_size)
        , last_cpu_sample_us(0)
        , total_bytes(0)
        , total_count(0)
        , failed_count(0) {}

    bvar::LatencyRecorder latency_recorder;
    bvar::LatencyRecorder server_cpu_recorder;
    bvar::LatencyRecorder client_cpu_recorder;
    butil::atomic<uint64_t> last_cpu_sample_us;
    butil::atomic<uint64_t> total_bytes;
    butil::atomic<uint64_t> total_count;
    std::atomic<uint64_t> failed_count;
};

static void* GenerateToken(void* arg) {
    int64_t start_time = butil::monotonic_time_ns();
    int64_t accumulative_token = g_token.load(butil::memory_order_relaxed);
    while (!g_stop.load(std::memory_order_acquire)) {
        bthread_usleep(100000);
        int64_t now = butil::monotonic_time_ns();
        if (accumulative_token * 1000000000 / (now - start_time) < FLAGS_expected_qps) {
            int64_t delta = FLAGS_expected_qps * (now - start_time) /
                            1000000000 - accumulative_token;
            g_token.fetch_add(delta, butil::memory_order_relaxed);
            accumulative_token += delta;
        }
    }
    return nullptr;
}

class PerformanceTest {
public:
    PerformanceTest(int attachment_size, bool echo_attachment, TestStats* stats)
        : _addr(nullptr)
        , _channel(nullptr)
        , _attachment_ready(true)
        , _measurement_start_us(0)
        , _measurement_end_us(std::numeric_limits<uint64_t>::max())
        , _remaining_requests(FLAGS_test_iterations)
        , _inflight(0)
        , _stop(false)
        , _failed(false)
        , _stats(stats)
    {
        if (attachment_size > 0) {
            _addr = malloc(attachment_size);
            if (_addr == nullptr) {
                _attachment_ready = false;
                return;
            }
            butil::fast_rand_bytes(_addr, attachment_size);
            _attachment.append(_addr, attachment_size);
        }
        _echo_attachment = echo_attachment;
    }

    ~PerformanceTest() {
        if (_addr) {
            free(_addr);
        }
        delete _channel;
    }

    bool IsStop() const { return _stop.load(std::memory_order_acquire); }
    bool IsDrained() const {
        return _inflight.load(std::memory_order_acquire) == 0;
    }
    bool Failed() const { return _failed.load(std::memory_order_acquire); }
    void Stop() { _stop.store(true, std::memory_order_release); }
    void SetMeasurementInterval(uint64_t start_us, uint64_t end_us) {
        _measurement_start_us = start_us;
        _measurement_end_us = end_us;
    }

    int Init() {
        if (!_attachment_ready) {
            LOG(ERROR) << "Fail to allocate request attachment";
            return -1;
        }
        brpc::ChannelOptions options;
        options.socket_mode = FLAGS_use_ubring? brpc::SOCKET_MODE_UBRING : brpc::SOCKET_MODE_TCP;
        options.protocol = FLAGS_protocol;
        options.connection_type = FLAGS_connection_type;
        options.timeout_ms = FLAGS_rpc_timeout_ms;
        options.max_retry = 0;
        // TODO A bug exists when the connection_group parameter is used.
        // options.connection_group = std::to_string(reinterpret_cast<uintptr_t>(this));
        std::string server = g_servers[(rr_index++) % g_servers.size()];
        _channel = new brpc::Channel();
        if (_channel->Init(server.c_str(), &options) != 0) {
            LOG(ERROR) << "Fail to initialize channel";
            return -1;
        }
        
        // Add retry mechanism for RPC call
        int retry = 3;
        while (retry > 0) {
            brpc::Controller cntl;
            test::PerfTestResponse response;
            test::PerfTestRequest request;
            request.set_echo_attachment(_echo_attachment);
            test::PerfTestService_Stub stub(_channel);
            stub.Test(&cntl, &request, &response, nullptr);
            if (!cntl.Failed()) {
                return 0;
            }
            LOG(WARNING) << "RPC call failed, retrying... (" << retry
                         << " left): " << cntl.ErrorText();
            retry--;
            bthread_usleep(1000000); // 1s delay before retry
        }
        LOG(ERROR) << "RPC call failed after multiple retries";
        return -1;
    }

    struct RespClosure {
        brpc::Controller* cntl;
        test::PerfTestResponse* resp;
        PerformanceTest* test;
    };

    bool SendRequest() {
        if (IsStop()) {
            return false;
        }
        if (FLAGS_test_iterations > 0) {
            uint32_t remaining = _remaining_requests.load(std::memory_order_relaxed);
            while (remaining > 0 &&
                   !_remaining_requests.compare_exchange_weak(
                       remaining, remaining - 1,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {}
            if (remaining == 0) {
                Stop();
                return false;
            }
        } else if (butil::monotonic_time_us() >= _measurement_end_us) {
            Stop();
            return false;
        }
        if (FLAGS_expected_qps > 0) {
            while (g_token.load(butil::memory_order_relaxed) <= 0) {
                if (IsStop() || g_stop.load(std::memory_order_acquire)) {
                    return false;
                }
                bthread_usleep(10);
            }
            g_token.fetch_sub(1, butil::memory_order_relaxed);
        }
        RespClosure* closure = new RespClosure;
        test::PerfTestRequest request;
        closure->resp = new test::PerfTestResponse();
        closure->cntl = new brpc::Controller();
        request.set_echo_attachment(_echo_attachment);
        closure->cntl->request_attachment().append(_attachment);
        closure->test = this;
        google::protobuf::Closure* done = brpc::NewCallback(&HandleResponse, closure);
        test::PerfTestService_Stub stub(_channel);
        _inflight.fetch_add(1, std::memory_order_relaxed);
        stub.Test(closure->cntl, &request, closure->resp, done);
        return true;
    }

    static void HandleResponse(RespClosure* closure) {
        std::unique_ptr<RespClosure> closure_guard(closure);
        std::unique_ptr<brpc::Controller> cntl_guard(closure->cntl);
        std::unique_ptr<test::PerfTestResponse> response_guard(closure->resp);
        PerformanceTest* test = closure->test;
        bool send_next = false;
        if (closure->cntl->Failed()) {
            LOG(ERROR) << "RPC call failed: " << closure->cntl->ErrorText();
            test->_stats->failed_count.fetch_add(1, std::memory_order_relaxed);
            test->_failed.store(true, std::memory_order_release);
            test->Stop();
        } else {
            const uint64_t now = butil::monotonic_time_us();
            const bool in_measurement = FLAGS_test_iterations > 0 ||
                (now >= test->_measurement_start_us &&
                 now < test->_measurement_end_us);
            if (in_measurement) {
                test->_stats->latency_recorder << closure->cntl->latency_us();
                if (!closure->resp->cpu_usage().empty()) {
                    test->_stats->server_cpu_recorder <<
                        atof(closure->resp->cpu_usage().c_str()) * 100;
                }
                test->_stats->total_bytes.fetch_add(
                    closure->cntl->request_attachment().size(),
                    butil::memory_order_relaxed);
                test->_stats->total_count.fetch_add(
                    1, butil::memory_order_relaxed);

                uint64_t last = test->_stats->last_cpu_sample_us.load(
                    butil::memory_order_relaxed);
                if (now > last && now - last > 100000 &&
                    test->_stats->last_cpu_sample_us.exchange(
                        now, butil::memory_order_relaxed) == last) {
                    test->_stats->client_cpu_recorder <<
                        atof(bvar::Variable::describe_exposed(
                            "process_cpu_usage").c_str()) * 100;
                }
            }
            send_next = !test->IsStop();
        }

        if (send_next) {
            test->SendRequest();
        }
        test->_inflight.fetch_sub(1, std::memory_order_release);
    }

    static void* RunTest(void* arg) {
        PerformanceTest* test = (PerformanceTest*)arg;
        for (int i = 0; i < FLAGS_queue_depth; ++i) {
            if (!test->SendRequest()) {
                break;
            }
        }

        return nullptr;
    }

private:
    void* _addr;
    brpc::Channel* _channel;
    bool _attachment_ready;
    uint64_t _measurement_start_us;
    uint64_t _measurement_end_us;
    std::atomic<uint32_t> _remaining_requests;
    std::atomic<int> _inflight;
    std::atomic<bool> _stop;
    std::atomic<bool> _failed;
    TestStats* _stats;
    butil::IOBuf _attachment;
    bool _echo_attachment;
};

int Test(int thread_num, int attachment_size) {
    std::cout << "[Threads: " << thread_num
        << ", Depth: " << FLAGS_queue_depth
        << ", Attachment: " << attachment_size << "B"
        << ", UBRING: " << (FLAGS_use_ubring ? "yes" : "no")
        << ", Echo: " << (FLAGS_echo_attachment ? "yes]" : "no]")
        << std::endl;
    g_token.store(10000, butil::memory_order_relaxed);
    g_stop.store(false, std::memory_order_release);
    // This recorder is local to one Test() invocation and receives samples
    // only during the measurement interval, so retain all of its samples.
    TestStats stats(-1);
    std::vector<PerformanceTest*> tests;
    for (int k = 0; k < thread_num; ++k) {
        PerformanceTest* t = new PerformanceTest(
            attachment_size, FLAGS_echo_attachment, &stats);
        if (t->Init() < 0) {
            delete t;
            for (PerformanceTest* initialized_test : tests) {
                delete initialized_test;
            }
            return 1;
        }
        tests.push_back(t);
    }
    std::vector<bthread_t> tids(thread_num);
    bthread_t token_tid = INVALID_BTHREAD;
    bool token_started = false;
    bool failed = false;
    if (FLAGS_expected_qps > 0) {
        if (bthread_start_background(
                &token_tid, &BTHREAD_ATTR_NORMAL,
                GenerateToken, nullptr) != 0) {
            LOG(ERROR) << "Fail to start QPS token generator";
            failed = true;
        } else {
            token_started = true;
        }
    }
    const uint64_t run_start_us = butil::monotonic_time_us();
    const uint64_t measurement_start_us = FLAGS_test_iterations > 0
        ? run_start_us
        : run_start_us +
            static_cast<uint64_t>(FLAGS_warmup_seconds) * 1000000;
    const uint64_t measurement_end_us = FLAGS_test_iterations > 0
        ? std::numeric_limits<uint64_t>::max()
        : measurement_start_us +
            static_cast<uint64_t>(FLAGS_test_seconds) * 1000000;
    for (PerformanceTest* test : tests) {
        test->SetMeasurementInterval(
            measurement_start_us, measurement_end_us);
    }
    int started_tests = 0;
    for (; !failed && started_tests < thread_num; ++started_tests) {
        if (bthread_start_background(
                &tids[started_tests], &BTHREAD_ATTR_NORMAL,
                PerformanceTest::RunTest, tests[started_tests]) != 0) {
            LOG(ERROR) << "Fail to start benchmark worker " << started_tests;
            failed = true;
            break;
        }
    }
    if (failed) {
        for (PerformanceTest* test : tests) {
            test->Stop();
        }
    }
    for (int k = 0; k < started_tests; ++k) {
        bthread_join(tids[k], nullptr);
    }

    bool all_stopped = false;
    while (!all_stopped) {
        all_stopped = true;
        for (PerformanceTest* test : tests) {
            failed = failed || test->Failed();
            if (!test->IsStop()) {
                all_stopped = false;
            }
        }
        if (failed || (FLAGS_test_iterations == 0 &&
                       butil::monotonic_time_us() >= measurement_end_us)) {
            for (PerformanceTest* test : tests) {
                test->Stop();
            }
        }
        if (!all_stopped) {
            bthread_usleep(10000);
        }
    }

    for (PerformanceTest* test : tests) {
        while (!test->IsDrained()) {
            bthread_usleep(1000);
        }
        failed = failed || test->Failed();
    }

    g_stop.store(true, std::memory_order_release);
    if (token_started) {
        bthread_join(token_tid, nullptr);
    }

    const uint64_t measured_us = FLAGS_test_iterations > 0
        ? butil::monotonic_time_us() - measurement_start_us
        : static_cast<uint64_t>(FLAGS_test_seconds) * 1000000;
    const uint64_t total_bytes =
        stats.total_bytes.load(butil::memory_order_relaxed);
    const uint64_t total_cnt =
        stats.total_count.load(butil::memory_order_relaxed);
    double throughput = measured_us == 0
        ? 0 : total_bytes / 1.048576 / measured_us;
    double qps = measured_us == 0
        ? 0 : total_cnt * 1000000.0 / measured_us;
    if (FLAGS_test_iterations == 0) {
        std::cout << "Measured-Seconds: " << measured_us / 1000000.0
            << ", Completed: " << total_cnt
            << ", Failed: " << stats.failed_count.load(std::memory_order_relaxed)
            << ", Avg-Latency: " << stats.latency_recorder.latency()
            << ", 90th-Latency: " << stats.latency_recorder.latency_percentile(0.9)
            << ", 99th-Latency: " << stats.latency_recorder.latency_percentile(0.99)
            << ", 99.9th-Latency: " << stats.latency_recorder.latency_percentile(0.999)
            << ", Request-Throughput: " << throughput << "MB/s"
            << ", QPS: " << qps
            << ", Server CPU-utilization: " << stats.server_cpu_recorder.latency() << "%"
            << ", Client CPU-utilization: " << stats.client_cpu_recorder.latency() << "%"
            << std::endl;
    } else {
        std::cout << " Request-Throughput: " << throughput << "MB/s"
                  << std::endl;
    }
    for (PerformanceTest* test : tests) {
        delete test;
    }
    return failed || stats.failed_count.load(std::memory_order_relaxed) != 0
        ? 1 : 0;
}

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

    brpc::StartDummyServerAt(FLAGS_dummy_port);

    std::string::size_type pos1 = 0;
    std::string::size_type pos2 = FLAGS_servers.find('+');
    while (pos2 != std::string::npos) {
        g_servers.push_back(FLAGS_servers.substr(pos1, pos2 - pos1));
        pos1 = pos2 + 1;
        pos2 = FLAGS_servers.find('+', pos1);
    }
    g_servers.push_back(FLAGS_servers.substr(pos1));

    if (FLAGS_queue_depth <= 0 || FLAGS_max_thread_num <= 0 ||
        FLAGS_expected_qps < 0 || FLAGS_rpc_timeout_ms <= 0 ||
        FLAGS_warmup_seconds < 0 || FLAGS_test_seconds <= 0 ||
        FLAGS_test_iterations < 0) {
        LOG(ERROR) << "queue_depth and test_seconds must be positive, "
                   << "max_thread_num and rpc_timeout_ms must be positive, "
                   << "and expected_qps, warmup_seconds, and test_iterations "
                   << "must be non-negative";
        return 1;
    }

    int rc = 0;
    if (FLAGS_thread_num > 0 && FLAGS_attachment_size >= 0) {
        rc = Test(FLAGS_thread_num, FLAGS_attachment_size);
    } else if (FLAGS_thread_num <= 0 && FLAGS_attachment_size >= 0) {
        for (int i = 1; i <= FLAGS_max_thread_num; i *= 2) {
            if (Test(i, FLAGS_attachment_size) != 0) {
                rc = 1;
                break;
            }
        }
    } else if (FLAGS_thread_num > 0 && FLAGS_attachment_size < 0) {
        for (int i = 1; i <= 1024; i *= 4) {
            if (Test(FLAGS_thread_num, i) != 0) {
                rc = 1;
                break;
            }
        }
    } else {
        for (int j = 1; j <= 1024; j *= 4) {
            for (int i = 1; i <= FLAGS_max_thread_num; i *= 2) {
                if (Test(i, j) != 0) {
                    rc = 1;
                    break;
                }
            }
            if (rc != 0) {
                break;
            }
        }
    }

    return rc;
}

#else

int main(int argc, char* argv[]) {
    LOG(ERROR) << " brpc is not compiled with ubring. To enable it, "
               << "please refer to the ubring documentation";
    return 1;
}

#endif
