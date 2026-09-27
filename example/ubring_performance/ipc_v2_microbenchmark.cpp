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

#include <errno.h>
#include <inttypes.h>
#include <new>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <limits>
#include <string>
#include <vector>

#include "brpc/ubshm/ubr_msg_v2_experimental.h"

namespace {

struct Options {
    size_t slot_size = 4096;
    size_t payload_offset = 64;
    size_t message_size = 4096;
    size_t ring_bytes = 64 * 1024 * 1024;
    uint64_t warmup_iterations = 10000;
    uint64_t iterations = 100000;
    bool verify = false;
};

bool ParseUnsigned(const char* value, uint64_t& result) {
    if (value == nullptr || *value == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    result = static_cast<uint64_t>(parsed);
    return true;
}

bool ParseBool(const char* value, bool& result) {
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        result = true;
        return true;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        result = false;
        return true;
    }
    return false;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const char* argument = argv[i];
        if (strcmp(argument, "--help") == 0) {
            printf("Usage: %s [options]\n"
                   "  --slot_size=N              1024, 4096 or 8192\n"
                   "  --payload_offset=N         16 or 64\n"
                   "  --message_size=N           bytes per batch\n"
                   "  --ring_bytes=N             data bytes reserved for slots\n"
                   "  --warmup_iterations=N      excluded from statistics\n"
                   "  --iterations=N             measured batches\n"
                   "  --verify=true|false        verify measured payloads\n",
                   argv[0]);
            return false;
        }
        const char* equal = strchr(argument, '=');
        if (equal == nullptr || strncmp(argument, "--", 2) != 0) {
            fprintf(stderr, "Invalid argument: %s\n", argument);
            return false;
        }
        const std::string name(argument, equal);
        const char* value = equal + 1;
        uint64_t number = 0;
        if (name == "--slot_size" || name == "--payload_offset" ||
            name == "--message_size" || name == "--ring_bytes" ||
            name == "--warmup_iterations" || name == "--iterations") {
            if (!ParseUnsigned(value, number) || number > SIZE_MAX) {
                fprintf(stderr, "Invalid numeric value: %s\n", argument);
                return false;
            }
            if (name == "--slot_size") options.slot_size = number;
            if (name == "--payload_offset") options.payload_offset = number;
            if (name == "--message_size") options.message_size = number;
            if (name == "--ring_bytes") options.ring_bytes = number;
            if (name == "--warmup_iterations") options.warmup_iterations = number;
            if (name == "--iterations") options.iterations = number;
        } else if (name == "--verify") {
            if (!ParseBool(value, options.verify)) {
                fprintf(stderr, "Invalid boolean value: %s\n", argument);
                return false;
            }
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argument);
            return false;
        }
    }
    return true;
}

template <typename Slot>
uint8_t ExpectedByte(uint64_t sequence, size_t offset) {
    return static_cast<uint8_t>((sequence * 17 + offset * 31) & 0xff);
}

const char* FailureMessage(int code) {
    switch (code) {
    case 1: return "producer wrote a short batch";
    case 2: return "producer ring operation failed";
    case 3: return "consumer read an incomplete batch";
    case 4: return "payload verification failed";
    case 5: return "consumer ring operation failed";
    default: return "unknown benchmark failure";
    }
}

template <typename Slot>
int Run(const Options& options) {
    uint32_t capacity = 0;
    if (!Slot::CalculateCapacity(options.ring_bytes, capacity)) {
        fprintf(stderr, "ring_bytes does not produce a valid capacity\n");
        return 2;
    }
    if (options.message_size == 0 || options.warmup_iterations == 0 ||
        options.iterations == 0) {
        fprintf(stderr, "message_size, warmup_iterations and iterations "
                       "must be positive\n");
        return 2;
    }
    if (options.message_size >
        static_cast<size_t>(std::numeric_limits<uint32_t>::max()) *
            sizeof(Slot::payload)) {
        fprintf(stderr, "message_size is too large\n");
        return 2;
    }

    const size_t slot_bytes = static_cast<size_t>(capacity) * sizeof(Slot);
    void* storage = nullptr;
    if (posix_memalign(&storage, alignof(Slot), slot_bytes) != 0) {
        fprintf(stderr, "posix_memalign failed for %zu bytes\n", slot_bytes);
        return 2;
    }
    Slot* slots = static_cast<Slot*>(storage);
    for (uint32_t i = 0; i < capacity; ++i) {
        new (&slots[i]) Slot();
    }
    brpc::ubring::experimental::IpcV2RingControl control = {};
    if (!brpc::ubring::experimental::IpcV2RxView<Slot>::InitializeShared(
            slots, &control, capacity)) {
        fprintf(stderr, "shared ring initialization failed\n");
        for (uint32_t i = 0; i < capacity; ++i) {
            slots[i].~Slot();
        }
        free(storage);
        return 2;
    }

    brpc::ubring::experimental::IpcV2TxView<Slot> tx(
        slots, &control, capacity);
    brpc::ubring::experimental::IpcV2RxView<Slot> rx(
        slots, &control, capacity);
    std::vector<char> input(options.message_size, '\0');
    std::vector<char> output(options.message_size, '\0');
    std::atomic<int> ready(0);
    std::atomic<bool> start(false);
    std::atomic<bool> failed(false);
    std::atomic<int> failure_code(0);
    std::atomic<uint64_t> measured_bytes(0);
    std::chrono::steady_clock::time_point measured_start;
    std::chrono::steady_clock::time_point measured_end;
    const auto fail = [&](int code) {
        int expected = 0;
        failure_code.compare_exchange_strong(
            expected, code, std::memory_order_relaxed);
        failed.store(true, std::memory_order_release);
    };

    std::thread producer([&] {
        struct iovec source = {input.data(), input.size()};
        const uint64_t total = options.warmup_iterations + options.iterations;
        for (uint64_t sequence = 0; sequence < total; ++sequence) {
            for (size_t i = 0; i < input.size(); ++i) {
                input[i] = static_cast<char>(ExpectedByte<Slot>(sequence, i));
            }
            for (;;) {
                size_t written = 0;
                const auto result = tx.TryWritev(&source, 1, written);
                if (result == brpc::ubring::experimental::IPC_V2_RING_OK) {
                    if (written != input.size()) {
                        fail(1);
                    }
                    break;
                }
                if (result != brpc::ubring::experimental::IPC_V2_RING_RETRY) {
                    fail(2);
                    return;
                }
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }
            if (sequence + 1 == options.warmup_iterations) {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire) &&
                       !failed.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        }
    });

    std::thread consumer([&] {
        for (uint64_t sequence = 0;
             sequence < options.warmup_iterations + options.iterations;
             ++sequence) {
            size_t message_read = 0;
            for (;;) {
                struct iovec destination = {
                    output.data() + message_read, output.size() - message_read};
                size_t read = 0;
                bool batch_end = false;
                const auto result = rx.TryReadv(&destination, 1, read, batch_end);
                if (result == brpc::ubring::experimental::IPC_V2_RING_OK) {
                    if (read == 0 || read > output.size() - message_read) {
                        fail(3);
                        return;
                    }
                    message_read += read;
                    if (!batch_end) {
                        if (message_read == output.size()) {
                            fail(3);
                            return;
                        }
                        continue;
                    }
                    if (message_read != output.size()) {
                        fail(3);
                        return;
                    }
                    if (options.verify || sequence < options.warmup_iterations) {
                        for (size_t i = 0; i < output.size(); ++i) {
                            if (static_cast<uint8_t>(output[i]) !=
                                ExpectedByte<Slot>(sequence, i)) {
                                fail(4);
                                return;
                            }
                        }
                    }
                    if (sequence + 1 == options.warmup_iterations) {
                        ready.fetch_add(1, std::memory_order_release);
                        while (!start.load(std::memory_order_acquire) &&
                               !failed.load(std::memory_order_acquire)) {
                            std::this_thread::yield();
                        }
                        measured_start = std::chrono::steady_clock::now();
                    }
                    if (sequence >= options.warmup_iterations) {
                        measured_bytes.fetch_add(message_read,
                                                 std::memory_order_relaxed);
                    }
                    if (sequence + 1 ==
                        options.warmup_iterations + options.iterations) {
                        measured_end = std::chrono::steady_clock::now();
                    }
                    break;
                }
                if (result != brpc::ubring::experimental::IPC_V2_RING_RETRY) {
                    fail(5);
                    return;
                }
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }
        }
    });

    while (ready.load(std::memory_order_acquire) != 2 &&
           !failed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    if (failed.load(std::memory_order_acquire)) {
        fprintf(stderr, "benchmark failed: %s\n",
                FailureMessage(failure_code.load(std::memory_order_relaxed)));
        for (uint32_t i = 0; i < capacity; ++i) {
            slots[i].~Slot();
        }
        free(storage);
        return 1;
    }
    const double seconds = std::chrono::duration<double>(
        measured_end - measured_start).count();
    const uint64_t bytes = measured_bytes.load(std::memory_order_relaxed);
    printf("slot_size=%zu payload_offset=%zu payload_capacity=%zu "
           "capacity=%" PRIu32 " message_size=%zu iterations=%" PRIu64 "\n",
           sizeof(Slot), options.payload_offset, sizeof(Slot::payload), capacity,
           options.message_size, options.iterations);
    printf("seconds=%.6f bytes=%" PRIu64 " ops_per_sec=%.3f "
           "gib_per_sec=%.6f verify=%s\n",
           seconds, bytes, options.iterations / seconds,
           static_cast<double>(bytes) / seconds / (1024.0 * 1024.0 * 1024.0),
           options.verify ? "true" : "false");
    for (uint32_t i = 0; i < capacity; ++i) {
        slots[i].~Slot();
    }
    free(storage);
    return 0;
}

int Dispatch(const Options& options) {
    if (options.payload_offset == 16) {
        if (options.slot_size == 1024) {
            return Run<brpc::ubring::experimental::IpcV2Slot<1024, 16>>(options);
        }
        if (options.slot_size == 4096) {
            return Run<brpc::ubring::experimental::IpcV2Slot<4096, 16>>(options);
        }
        if (options.slot_size == 8192) {
            return Run<brpc::ubring::experimental::IpcV2Slot<8192, 16>>(options);
        }
    }
    if (options.payload_offset == 64) {
        if (options.slot_size == 1024) {
            return Run<brpc::ubring::experimental::IpcV2Slot<1024, 64>>(options);
        }
        if (options.slot_size == 4096) {
            return Run<brpc::ubring::experimental::IpcV2Slot<4096, 64>>(options);
        }
        if (options.slot_size == 8192) {
            return Run<brpc::ubring::experimental::IpcV2Slot<8192, 64>>(options);
        }
    }
    fprintf(stderr, "slot_size must be 1024, 4096 or 8192 and payload_offset 16 or 64\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        return argc > 1 && strcmp(argv[1], "--help") == 0 ? 0 : 2;
    }
    return Dispatch(options);
}
