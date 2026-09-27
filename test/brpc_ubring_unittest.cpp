// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to You under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <gflags/gflags.h>
#include <string>
#include <thread>
#include "butil/macros.h"
#include "butil/sys_byteorder.h"
#include "brpc/socket.h"

#if BRPC_WITH_UBRING
#include "brpc/ubshm/common/common.h"
#include "brpc/ubshm/ub_endpoint.h"
#include "brpc/ubshm/shm/shm_def.h"
#include "brpc/ubshm/shm/shm_mgr.h"
#include "brpc/ubshm/ub_ring_manager.h"
#include "brpc/ubshm/ub_ring.h"
#include "brpc/ubshm/ubr_msg.h"
#include "brpc/ubshm/ubr_msg_v2_experimental.h"

namespace brpc {
namespace ubring {
DECLARE_int32(ub_disconnect_timeout_s);
DECLARE_int32(ub_connect_timeout_s);
DECLARE_int32(ub_hb_timer_interval_s);
DECLARE_int32(ub_event_queue_timer_interval_us);
DECLARE_int32(ub_flying_io_timeout_s);

extern bool g_skip_ub_init;
}  // namespace ubring
}  // namespace brpc

namespace {
template <size_t SlotSize, size_t PayloadOffset = 64>
void CheckIpcV2Candidate() {
    typedef brpc::ubring::experimental::IpcV2Slot<SlotSize, PayloadOffset> Slot;
    static_assert(std::is_standard_layout<Slot>::value, "Slot layout must be stable");
    static_assert(sizeof(Slot) == SlotSize, "Unexpected slot stride");
    static_assert(alignof(Slot) == 64, "Unexpected slot alignment");
    static_assert(offsetof(Slot, header) == 0, "Header must be first");
    static_assert(offsetof(Slot, payload) == PayloadOffset, "Unexpected payload offset");
    static_assert(sizeof(((Slot*)nullptr)->payload) == SlotSize - PayloadOffset,
                  "Unexpected payload capacity");

    uint32_t capacity = 99;
    EXPECT_FALSE(Slot::CalculateCapacity(0, capacity));
    EXPECT_EQ(0u, capacity);
    EXPECT_FALSE(Slot::CalculateCapacity(2 * SlotSize - 1, capacity));
    EXPECT_EQ(0u, capacity);
    EXPECT_TRUE(Slot::CalculateCapacity(2 * SlotSize, capacity));
    EXPECT_EQ(2u, capacity);
    EXPECT_TRUE(Slot::CalculateCapacity(3 * SlotSize - 1, capacity));
    EXPECT_EQ(2u, capacity);
    EXPECT_TRUE(Slot::CalculateCapacity(3 * SlotSize, capacity));
    EXPECT_EQ(3u, capacity);

    // Exercise narrowing boundaries only when size_t can represent the input.
    const uint64_t max_slots = std::numeric_limits<uint32_t>::max();
    const uint64_t oversized_bytes = (max_slots + 1) * SlotSize;
    if (std::numeric_limits<size_t>::max() >= oversized_bytes) {
        EXPECT_TRUE(Slot::CalculateCapacity(
            static_cast<size_t>(max_slots * SlotSize), capacity));
        EXPECT_EQ(std::numeric_limits<uint32_t>::max(), capacity);
        EXPECT_FALSE(Slot::CalculateCapacity(
            static_cast<size_t>(oversized_bytes), capacity));
        EXPECT_EQ(0u, capacity);
    }
    const size_t max_bytes = std::numeric_limits<size_t>::max();
    const bool fits = max_bytes / SlotSize <= max_slots;
    EXPECT_EQ(fits, Slot::CalculateCapacity(max_bytes, capacity));
}

TEST(IpcV2ExperimentalLayoutTest, candidate_capacity_boundaries) {
    CheckIpcV2Candidate<1024>();
    CheckIpcV2Candidate<4096>();
    CheckIpcV2Candidate<8192>();
    CheckIpcV2Candidate<1024, 16>();
    CheckIpcV2Candidate<4096, 16>();
    CheckIpcV2Candidate<8192, 16>();
    static_assert(std::is_same<
        brpc::ubring::experimental::IpcV2Slot<4096>,
        brpc::ubring::experimental::IpcV2Slot<4096, 64>>::value,
        "The default candidate must retain separated metadata");
}

template <size_t SlotSize, size_t PayloadOffset>
void CheckIpcV2SlotTransfer() {
    using namespace brpc::ubring::experimental;
    IpcV2Slot<SlotSize, PayloadOffset> slot;
    slot.Initialize();
    const std::string input(sizeof(slot.payload), 'x');
    // Distinct bytes across the split detect an incorrect partial-read cursor.
    std::string source = input;
    source[0] = 'a';
    source[source.size() / 2] = 'b';
    source.back() = 'z';
    std::string output(source.size() + 1, '?');
    uint32_t offset = 0;
    size_t copied = 99;
    bool batch_end = true;
    EXPECT_EQ(IPC_V2_SLOT_NOT_READY,
              slot.TryConsume(&output[0], output.size(), offset, copied, batch_end));
    EXPECT_EQ(0u, copied);
    EXPECT_FALSE(batch_end);
    EXPECT_EQ(std::string(output.size(), '?'), output);

    ASSERT_EQ(IPC_V2_SLOT_OK,
              slot.TryPublish(source.data(), source.size(), IPC_V2_SLOT_EOF));
    EXPECT_EQ(0u, slot.header.reserved);
    EXPECT_EQ(IPC_V2_SLOT_NOT_READY, slot.TryPublish("!", 1, 0));
    EXPECT_EQ(IPC_V2_SLOT_OK, slot.TryConsume(nullptr, 0, offset, copied, batch_end));
    EXPECT_EQ(0u, offset);
    EXPECT_EQ(0u, copied);
    EXPECT_FALSE(batch_end);
    const size_t split = source.size() / 2;
    ASSERT_EQ(IPC_V2_SLOT_OK,
              slot.TryConsume(&output[0], split, offset, copied, batch_end));
    EXPECT_EQ(split, copied);
    EXPECT_EQ(split, offset);
    EXPECT_FALSE(batch_end);
    EXPECT_EQ(IPC_V2_SLOT_READY, __atomic_load_n(&slot.header.state, __ATOMIC_ACQUIRE));
    EXPECT_EQ(IPC_V2_SLOT_NOT_READY, slot.TryPublish("!", 1, 0));
    ASSERT_EQ(IPC_V2_SLOT_OK,
              slot.TryConsume(&output[split], output.size() - split,
                              offset, copied, batch_end));
    EXPECT_EQ(source.size() - split, copied);
    EXPECT_EQ(0u, offset);
    EXPECT_TRUE(batch_end);
    EXPECT_EQ(source, output.substr(0, source.size()));
    EXPECT_EQ('?', output.back());
    EXPECT_EQ(IPC_V2_SLOT_EMPTY, __atomic_load_n(&slot.header.state, __ATOMIC_ACQUIRE));

    // Reuse after returning ownership, including a batch without EOF.
    ASSERT_EQ(IPC_V2_SLOT_OK, slot.TryPublish("r", 1, 0));
    ASSERT_EQ(IPC_V2_SLOT_OK,
              slot.TryConsume(&output[0], output.size(), offset, copied, batch_end));
    EXPECT_EQ(1u, copied);
    EXPECT_EQ('r', output[0]);
    EXPECT_FALSE(batch_end);
    EXPECT_EQ(0u, offset);
}

template <size_t SlotSize, size_t PayloadOffset>
void CheckIpcV2SlotRejection() {
    using namespace brpc::ubring::experimental;
    IpcV2Slot<SlotSize, PayloadOffset> slot;
    slot.Initialize();
    const std::string source(sizeof(slot.payload) + 1, 'x');
    EXPECT_EQ(IPC_V2_SLOT_INVALID, slot.TryPublish(nullptr, 1, 0));
    EXPECT_EQ(IPC_V2_SLOT_INVALID, slot.TryPublish(source.data(), 0, 0));
    EXPECT_EQ(IPC_V2_SLOT_INVALID, slot.TryPublish(source.data(), source.size(), 0));
    EXPECT_EQ(IPC_V2_SLOT_INVALID,
              slot.TryPublish(source.data(), std::numeric_limits<size_t>::max(), 0));
    EXPECT_EQ(IPC_V2_SLOT_INVALID, slot.TryPublish(source.data(), 1, 2));
    EXPECT_EQ(IPC_V2_SLOT_EMPTY, __atomic_load_n(&slot.header.state, __ATOMIC_ACQUIRE));
    EXPECT_EQ(0u, slot.header.payload_len);

    // Corruption is injected with no concurrent access, before publication.
    // Invalid reads must neither copy bytes nor return the slot to its producer.
    for (int invalid = 0; invalid < 7; ++invalid) {
        slot.Initialize();
        slot.header.payload_len = 2;
        slot.header.flags = 0;
        slot.header.reserved = 0;
        uint32_t offset = 0;
        switch (invalid) {
        case 0: slot.header.payload_len = 0; break;
        case 1: slot.header.payload_len = sizeof(slot.payload) + 1; break;
        case 2: slot.header.flags = 2; break;
        case 3: slot.header.reserved = 1; break;
        case 4: offset = 2; break;
        case 5: offset = std::numeric_limits<uint32_t>::max(); break;
        case 6: break;  // Null destination with non-zero size.
        }
        __atomic_store_n(&slot.header.state, IPC_V2_SLOT_READY, __ATOMIC_RELEASE);
        const uint32_t old_offset = offset;
        char output = '?';
        size_t copied = 99;
        bool batch_end = true;
        EXPECT_EQ(IPC_V2_SLOT_INVALID,
                  slot.TryConsume(invalid == 6 ? nullptr : &output, 1,
                                  offset, copied, batch_end));
        EXPECT_EQ('?', output);
        EXPECT_EQ(old_offset, offset);
        EXPECT_EQ(0u, copied);
        EXPECT_FALSE(batch_end);
        EXPECT_EQ(IPC_V2_SLOT_READY,
                  __atomic_load_n(&slot.header.state, __ATOMIC_ACQUIRE));
    }
    slot.Initialize();
    uint32_t offset = 1;
    size_t copied = 99;
    bool batch_end = true;
    char output = '?';
    EXPECT_EQ(IPC_V2_SLOT_INVALID,
              slot.TryConsume(&output, 1, offset, copied, batch_end));
    // Unknown state is corruption, not ordinary backpressure.
    __atomic_store_n(&slot.header.state, 3, __ATOMIC_RELEASE);
    EXPECT_EQ(IPC_V2_SLOT_INVALID, slot.TryPublish(source.data(), 1, 0));
    offset = 0;
    EXPECT_EQ(IPC_V2_SLOT_INVALID,
              slot.TryConsume(&output, 1, offset, copied, batch_end));
    EXPECT_EQ(3u, __atomic_load_n(&slot.header.state, __ATOMIC_ACQUIRE));
    EXPECT_EQ('?', output);
    EXPECT_EQ(0u, copied);
    EXPECT_FALSE(batch_end);
}

TEST(IpcV2ExperimentalSlotTest, publish_partial_consume_and_reuse) {
    CheckIpcV2SlotTransfer<1024, 16>();
    CheckIpcV2SlotTransfer<4096, 16>();
    CheckIpcV2SlotTransfer<8192, 16>();
    CheckIpcV2SlotTransfer<1024, 64>();
    CheckIpcV2SlotTransfer<4096, 64>();
    CheckIpcV2SlotTransfer<8192, 64>();
}

TEST(IpcV2ExperimentalSlotTest, reject_invalid_input_without_releasing_slot) {
    CheckIpcV2SlotRejection<1024, 16>();
    CheckIpcV2SlotRejection<4096, 16>();
    CheckIpcV2SlotRejection<8192, 16>();
    CheckIpcV2SlotRejection<1024, 64>();
    CheckIpcV2SlotRejection<4096, 64>();
    CheckIpcV2SlotRejection<8192, 64>();
}

template <size_t PayloadOffset>
void CheckIpcV2ConcurrentSlot() {
    using namespace brpc::ubring::experimental;
    IpcV2Slot<4096, PayloadOffset> slot;
    slot.Initialize();
    const uint32_t rounds = 2000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // Only cancellation uses this flag. No extra per-message synchronization
    // may hide a broken slot publication/reclamation protocol.
    std::atomic<bool> stop(false);
    std::string producer_error;
    std::string consumer_error;
    uint32_t published = 0;
    uint32_t consumed = 0;
    const auto make_message = [](uint32_t sequence, char* data, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            data[i] = static_cast<char>((sequence * 17 + i * 31) & 0xff);
        }
        memcpy(data, &sequence, sizeof(sequence));
    };
    const auto message_size = [&](uint32_t sequence) {
        return sequence % 8 == 0 ? sizeof(slot.payload)
                                : 16 + sequence % (sizeof(slot.payload) - 16);
    };
    // Thread creation publishes the completed initialization. The test thread
    // is the consumer; join protects the slot and all producer-owned locals.
    std::thread producer([&] {
        char input[sizeof(slot.payload)];
        for (uint32_t sequence = 0; sequence < rounds; ++sequence) {
            const size_t length = message_size(sequence);
            make_message(sequence, input, length);
            for (;;) {
                if (stop.load(std::memory_order_relaxed)) {
                    return;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    producer_error = "producer timed out";
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
                const auto result = slot.TryPublish(
                    input, length,
                    sequence % 2 ? static_cast<uint32_t>(IPC_V2_SLOT_EOF) : 0u);
                if (result == IPC_V2_SLOT_OK) {
                    ++published;
                    break;
                }
                if (result != IPC_V2_SLOT_NOT_READY) {
                    producer_error = "producer rejected a valid message";
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
            }
        }
    });
    // No assertions or early returns until producer.join(): failures must not
    // destroy storage while the producer can still access it.
    char output[sizeof(slot.payload)];
    char expected[sizeof(slot.payload)];
    for (uint32_t sequence = 0; sequence < rounds; ++sequence) {
        const size_t length = message_size(sequence);
        make_message(sequence, expected, length);
        size_t received = 0;
        uint32_t offset = 0;
        while (received < length && !stop.load(std::memory_order_relaxed)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                consumer_error = "consumer timed out";
                break;
            }
            const size_t chunk = std::min(size_t(37), length - received);
            size_t copied = 0;
            bool batch_end = false;
            const auto result = slot.TryConsume(
                output + received, chunk, offset, copied, batch_end);
            if (result == IPC_V2_SLOT_NOT_READY && received == 0 &&
                copied == 0 && !batch_end && offset == 0) {
                std::this_thread::yield();
                continue;
            }
            if (result != IPC_V2_SLOT_OK || copied != chunk ||
                offset != (received + chunk == length ? 0 : received + chunk) ||
                batch_end != (received + chunk == length && sequence % 2 != 0)) {
                consumer_error = "invalid partial-read result or premature reclamation";
                break;
            }
            received += copied;
            std::this_thread::yield();
        }
        if (!consumer_error.empty() || stop.load(std::memory_order_relaxed)) {
            break;
        }
        if (memcmp(output, expected, length) != 0) {
            consumer_error = "message corrupted, overwritten or out of order";
            break;
        }
        ++consumed;
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    EXPECT_TRUE(producer_error.empty()) << producer_error;
    EXPECT_TRUE(consumer_error.empty()) << consumer_error;
    EXPECT_EQ(rounds, published);
    EXPECT_EQ(rounds, consumed);
    EXPECT_EQ(IPC_V2_SLOT_EMPTY, __atomic_load_n(&slot.header.state, __ATOMIC_ACQUIRE));
}

TEST(IpcV2ExperimentalSlotTest, concurrent_publish_and_partial_consume) {
    CheckIpcV2ConcurrentSlot<16>();
    CheckIpcV2ConcurrentSlot<64>();
}

template <size_t PayloadOffset>
void CheckIpcV2RingWriteReadAndWrap() {
    using namespace brpc::ubring::experimental;
    typedef IpcV2Slot<1024, PayloadOffset> Slot;
    Slot slots[4] = {};
    IpcV2RingControl control = {};
    IpcV2TxView<Slot> tx(slots, &control, 4);
    IpcV2RxView<Slot> rx(slots, &control, 4);
    EXPECT_EQ(IPC_V2_SLOT_UNINITIALIZED,
              __atomic_load_n(&slots[0].header.state, __ATOMIC_ACQUIRE));
    ASSERT_TRUE(IpcV2RxView<Slot>::InitializeShared(slots, &control, 4));
    EXPECT_EQ(3u, __atomic_load_n(&control.tail, __ATOMIC_ACQUIRE));
    const size_t payload_size = sizeof(slots[0].payload);
    std::string source(payload_size + 19, '\0');
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<char>((i * 29 + 7) & 0xff);
    }
    struct iovec write_iov[4] = {
        {nullptr, 0},
        {&source[0], 7},
        {&source[7], payload_size - 3},
        {&source[payload_size + 4], 15},
    };
    size_t written = 99;
    ASSERT_EQ(IPC_V2_RING_OK, tx.TryWritev(write_iov, 4, written));
    EXPECT_EQ(source.size(), written);
    EXPECT_EQ(payload_size, slots[0].header.payload_len);
    EXPECT_EQ(0u, slots[0].header.flags);
    EXPECT_EQ(19u, slots[1].header.payload_len);
    EXPECT_EQ(static_cast<uint32_t>(IPC_V2_SLOT_EOF), slots[1].header.flags);
    EXPECT_EQ(IPC_V2_SLOT_EMPTY,
              __atomic_load_n(&slots[2].header.state, __ATOMIC_ACQUIRE));

    std::string output(source.size(), '?');
    struct iovec first_read = {&output[0], 31};
    size_t read = 99;
    bool batch_end = true;
    ASSERT_EQ(IPC_V2_RING_OK, rx.TryReadv(&first_read, 1, read, batch_end));
    EXPECT_EQ(31u, read);
    EXPECT_FALSE(batch_end);
    EXPECT_EQ(3u, __atomic_load_n(&control.tail, __ATOMIC_ACQUIRE));
    struct iovec remaining_iov[4] = {
        {nullptr, 0},
        {&output[31], 5},
        {&output[36], payload_size - 20},
        {&output[payload_size + 16], 3},
    };
    ASSERT_EQ(IPC_V2_RING_OK,
              rx.TryReadv(remaining_iov, 4, read, batch_end));
    EXPECT_EQ(source.size() - 31, read);
    EXPECT_TRUE(batch_end);
    EXPECT_EQ(source, output);
    EXPECT_EQ(1u, __atomic_load_n(&control.tail, __ATOMIC_ACQUIRE));

    // Four physical slots retain one empty sentinel, so four required slots
    // are too large even while the ring is empty.
    std::string oversized(payload_size * 4, 'x');
    struct iovec oversized_iov = {&oversized[0], oversized.size()};
    EXPECT_EQ(IPC_V2_RING_TOO_LARGE,
              tx.TryWritev(&oversized_iov, 1, written));
    EXPECT_EQ(0u, written);

    // Fill three one-slot batches, observe backpressure, consume one and write
    // through the end of the array to exercise cursor wrap-around.
    char values[4] = {'a', 'b', 'c', 'd'};
    for (int i = 0; i < 3; ++i) {
        struct iovec item = {&values[i], 1};
        ASSERT_EQ(IPC_V2_RING_OK, tx.TryWritev(&item, 1, written));
    }
    struct iovec fourth = {&values[3], 1};
    EXPECT_EQ(IPC_V2_RING_RETRY, tx.TryWritev(&fourth, 1, written));
    EXPECT_EQ(0u, written);
    char result[4] = {};
    struct iovec one = {&result[0], 1};
    ASSERT_EQ(IPC_V2_RING_OK, rx.TryReadv(&one, 1, read, batch_end));
    ASSERT_TRUE(batch_end);
    EXPECT_EQ(2u, __atomic_load_n(&control.tail, __ATOMIC_ACQUIRE));
    ASSERT_EQ(IPC_V2_RING_OK, tx.TryWritev(&fourth, 1, written));
    for (int i = 1; i < 4; ++i) {
        struct iovec destination = {&result[i], 1};
        ASSERT_EQ(IPC_V2_RING_OK,
                  rx.TryReadv(&destination, 1, read, batch_end));
        ASSERT_TRUE(batch_end);
    }
    EXPECT_EQ(0, memcmp(values, result, sizeof(values)));
    EXPECT_EQ(1u, __atomic_load_n(&control.tail, __ATOMIC_ACQUIRE));
}

TEST(IpcV2ExperimentalRingTest, writev_readv_boundaries_and_wrap) {
    CheckIpcV2RingWriteReadAndWrap<16>();
    CheckIpcV2RingWriteReadAndWrap<64>();
}

TEST(IpcV2ExperimentalRingTest, rejects_invalid_iovec_without_publication) {
    using namespace brpc::ubring::experimental;
    typedef IpcV2Slot<1024> Slot;
    Slot slots[2] = {};
    IpcV2RingControl control = {};
    EXPECT_FALSE(IpcV2RxView<Slot>::InitializeShared(nullptr, &control, 2));
    EXPECT_FALSE(IpcV2RxView<Slot>::InitializeShared(slots, nullptr, 2));
    EXPECT_FALSE(IpcV2RxView<Slot>::InitializeShared(slots, &control, 1));
    IpcV2TxView<Slot> tx(slots, &control, 2);
    IpcV2RxView<Slot> rx(slots, &control, 2);
    size_t bytes = 99;
    bool batch_end = true;
    char value = 'x';
    struct iovec one_byte = {&value, 1};
    EXPECT_EQ(IPC_V2_RING_INVALID, tx.TryWritev(&one_byte, 1, bytes));
    EXPECT_EQ(IPC_V2_RING_INVALID,
              rx.TryReadv(&one_byte, 1, bytes, batch_end));
    EXPECT_EQ(IPC_V2_SLOT_UNINITIALIZED,
              __atomic_load_n(&slots[0].header.state, __ATOMIC_ACQUIRE));

    ASSERT_TRUE(IpcV2RxView<Slot>::InitializeShared(slots, &control, 2));
    EXPECT_EQ(IPC_V2_RING_INVALID, tx.TryWritev(nullptr, 1, bytes));
    EXPECT_EQ(IPC_V2_RING_INVALID, tx.TryWritev(nullptr, -1, bytes));
    EXPECT_EQ(IPC_V2_RING_INVALID,
              rx.TryReadv(nullptr, 1, bytes, batch_end));
    struct iovec null_data = {nullptr, 1};
    EXPECT_EQ(IPC_V2_RING_INVALID, tx.TryWritev(&null_data, 1, bytes));
    struct iovec overflow[2] = {
        {reinterpret_cast<void*>(1), std::numeric_limits<size_t>::max()},
        {reinterpret_cast<void*>(1), 1},
    };
    EXPECT_EQ(IPC_V2_RING_INVALID, tx.TryWritev(overflow, 2, bytes));
    EXPECT_EQ(IPC_V2_RING_OK, tx.TryWritev(nullptr, 0, bytes));
    EXPECT_EQ(0u, bytes);
    EXPECT_EQ(IPC_V2_RING_OK, rx.TryReadv(nullptr, 0, bytes, batch_end));
    EXPECT_EQ(0u, bytes);
    EXPECT_FALSE(batch_end);
    EXPECT_EQ(IPC_V2_SLOT_EMPTY,
              __atomic_load_n(&slots[0].header.state, __ATOMIC_ACQUIRE));
}

template <size_t PayloadOffset>
void CheckIpcV2ConcurrentRing() {
    using namespace brpc::ubring::experimental;
    typedef IpcV2Slot<1024, PayloadOffset> Slot;
    Slot slots[8] = {};
    IpcV2RingControl control = {};
    ASSERT_TRUE(IpcV2RxView<Slot>::InitializeShared(slots, &control, 8));
    IpcV2TxView<Slot> tx(slots, &control, 8);
    IpcV2RxView<Slot> rx(slots, &control, 8);
    const uint32_t rounds = 1000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::atomic<bool> stop(false);
    std::string producer_error;
    std::string consumer_error;
    const size_t payload_size = sizeof(slots[0].payload);
    const auto message_size = [payload_size](uint32_t sequence) {
        return 1 + (sequence * 53) % (payload_size * 2);
    };
    const auto fill = [](uint32_t sequence, char* data, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            data[i] = static_cast<char>((sequence * 11 + i * 37) & 0xff);
        }
    };
    std::thread producer([&] {
        std::string input(payload_size * 2, '\0');
        for (uint32_t sequence = 0; sequence < rounds; ++sequence) {
            const size_t length = message_size(sequence);
            fill(sequence, &input[0], length);
            const size_t first = length < 3 ? length : 3;
            const size_t second = (length - first) / 2;
            struct iovec parts[4] = {
                {&input[0], first},
                {nullptr, 0},
                {&input[first], second},
                {&input[first + second], length - first - second},
            };
            for (;;) {
                size_t written = 0;
                const auto result = tx.TryWritev(parts, 4, written);
                if (result == IPC_V2_RING_OK && written == length) {
                    break;
                }
                if (result != IPC_V2_RING_RETRY) {
                    producer_error = "valid writev failed";
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
                if (stop.load(std::memory_order_relaxed) ||
                    std::chrono::steady_clock::now() >= deadline) {
                    producer_error = "producer timed out";
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
            }
        }
    });
    std::string output(payload_size * 2, '\0');
    std::string expected(payload_size * 2, '\0');
    for (uint32_t sequence = 0; sequence < rounds; ++sequence) {
        const size_t length = message_size(sequence);
        fill(sequence, &expected[0], length);
        size_t received = 0;
        bool ended = false;
        while (!ended && !stop.load(std::memory_order_relaxed)) {
            const size_t chunk = std::min(size_t(113), length - received);
            struct iovec destination = {&output[received], chunk};
            size_t read = 0;
            const auto result = rx.TryReadv(&destination, 1, read, ended);
            if (result == IPC_V2_RING_RETRY) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    consumer_error = "consumer timed out";
                    break;
                }
                std::this_thread::yield();
                continue;
            }
            if (result != IPC_V2_RING_OK || read == 0 || read > chunk) {
                consumer_error = "valid readv failed";
                break;
            }
            received += read;
            if (std::chrono::steady_clock::now() >= deadline) {
                consumer_error = "consumer timed out";
                break;
            }
        }
        if (!consumer_error.empty() || stop.load(std::memory_order_relaxed)) {
            break;
        }
        if (received != length || memcmp(output.data(), expected.data(), length) != 0) {
            consumer_error = "batch length, order or content mismatch";
            break;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    EXPECT_TRUE(producer_error.empty()) << producer_error;
    EXPECT_TRUE(consumer_error.empty()) << consumer_error;
    EXPECT_LT(__atomic_load_n(&control.tail, __ATOMIC_ACQUIRE), 8u);
}

TEST(IpcV2ExperimentalRingTest, concurrent_wraparound_writev_readv) {
    CheckIpcV2ConcurrentRing<16>();
    CheckIpcV2ConcurrentRing<64>();
}

struct __attribute__((packed)) HelloMessageLayout {
    uint16_t msg_len;
    uint16_t hello_ver;
    uint16_t impl_ver;
    uint64_t len;
    char shm_name[SHM_MAX_NAME_BUFF_LEN];
};
}

class HelloMessageTest : public ::testing::Test {
protected:
    void SetUp() override {
        memset(&msg, 0, sizeof(msg));
        buffer.resize(256, 0);
    }

    brpc::ubring::HelloMessage msg;
    std::string buffer;
};

TEST(HelloFormatExtensionTest, serialize_deserialize_roundtrip) {
    brpc::ubring::HelloFormatExtension extension = {
        brpc::ubring::HelloFormatExtension::WIRE_SIZE,
        brpc::ubring::UBR_DATA_FORMAT_LEGACY_64};
    char buffer[brpc::ubring::HelloFormatExtension::WIRE_SIZE] = {};

    extension.Serialize(buffer);

    brpc::ubring::HelloFormatExtension decoded = {};
    decoded.Deserialize(buffer);
    EXPECT_EQ(extension.extension_len, decoded.extension_len);
    EXPECT_EQ(extension.format_id, decoded.format_id);
}

TEST(HelloFormatExtensionTest, serialize_uses_network_byte_order) {
    brpc::ubring::HelloFormatExtension extension = {0x0102, 0x0304};
    char buffer[brpc::ubring::HelloFormatExtension::WIRE_SIZE] = {};
    const unsigned char expected[] = {0x01, 0x02, 0x03, 0x04};

    extension.Serialize(buffer);

    EXPECT_EQ(0, memcmp(expected, buffer, sizeof(expected)));
}

TEST(HelloFormatExtensionTest, deserialize_none_format) {
    const unsigned char buffer[] = {0x00, 0x04, 0x00, 0x00};
    brpc::ubring::HelloFormatExtension extension = {};

    extension.Deserialize(buffer);

    EXPECT_EQ(4, extension.extension_len);
    EXPECT_EQ(brpc::ubring::UBR_DATA_FORMAT_NONE, extension.format_id);
}

TEST(HelloFormatExtensionTest, deserialize_unknown_format) {
    const unsigned char buffer[] = {0x00, 0x04, 0x12, 0x34};
    brpc::ubring::HelloFormatExtension extension = {};

    extension.Deserialize(buffer);

    EXPECT_EQ(4, extension.extension_len);
    EXPECT_EQ(0x1234, extension.format_id);
}

TEST_F(HelloMessageTest, serialize_deserialize_roundtrip) {
    msg.msg_len = 64;
    msg.hello_ver = 2;
    msg.impl_ver = 1;
    msg.len = 4 * 1024 * 1024;
    memcpy(msg.shm_name, "UBRING_test_C", 14);

    msg.Serialize(&buffer[0]);

    brpc::ubring::HelloMessage decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.Deserialize(&buffer[0]);

    EXPECT_EQ(msg.msg_len, decoded.msg_len);
    EXPECT_EQ(msg.hello_ver, decoded.hello_ver);
    EXPECT_EQ(msg.impl_ver, decoded.impl_ver);
    EXPECT_EQ(msg.len, decoded.len);
    EXPECT_EQ(0, memcmp(msg.shm_name, decoded.shm_name, SHM_MAX_NAME_BUFF_LEN));
}

TEST_F(HelloMessageTest, serialize_uses_network_byte_order) {
    msg.msg_len = 0x0102;
    msg.hello_ver = 0x0304;
    msg.impl_ver = 0x0506;
    msg.len = 0x0102030405060708ULL;
    memset(msg.shm_name, 0, SHM_MAX_NAME_BUFF_LEN);

    msg.Serialize(&buffer[0]);

    HelloMessageLayout* raw = reinterpret_cast<HelloMessageLayout*>(&buffer[0]);
    EXPECT_EQ(butil::HostToNet16(0x0102), raw->msg_len);
    EXPECT_EQ(butil::HostToNet16(0x0304), raw->hello_ver);
    EXPECT_EQ(butil::HostToNet16(0x0506), raw->impl_ver);
    EXPECT_EQ(butil::HostToNet64(0x0102030405060708ULL), raw->len);
}

TEST_F(HelloMessageTest, large_len_value) {
    msg.msg_len = 64;
    msg.hello_ver = 2;
    msg.impl_ver = 1;
    msg.len = 0xFFFFFFFFFFFFFFFFULL;
    memset(msg.shm_name, 0, SHM_MAX_NAME_BUFF_LEN);

    msg.Serialize(&buffer[0]);

    brpc::ubring::HelloMessage decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.Deserialize(&buffer[0]);

    EXPECT_EQ(0xFFFFFFFFFFFFFFFFULL, decoded.len);
}

TEST_F(HelloMessageTest, full_shm_name) {
    memset(msg.shm_name, 'A', SHM_MAX_NAME_BUFF_LEN);
    msg.msg_len = 64;
    msg.hello_ver = 2;
    msg.impl_ver = 1;
    msg.len = 0;
    msg.Serialize(&buffer[0]);

    brpc::ubring::HelloMessage decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.Deserialize(&buffer[0]);

    EXPECT_EQ(0, memcmp(msg.shm_name, decoded.shm_name, SHM_MAX_NAME_BUFF_LEN));
}

TEST_F(HelloMessageTest, toString_contains_fields) {
    msg.msg_len = 64;
    msg.hello_ver = 2;
    msg.impl_ver = 1;
    msg.len = 4194304;
    memcpy(msg.shm_name, "UBRING_test", 12);

    std::string s = msg.toString();
    EXPECT_NE(std::string::npos, s.find("msg_len=64"));
    EXPECT_NE(std::string::npos, s.find("hello_ver=2"));
    EXPECT_NE(std::string::npos, s.find("impl_ver=1"));
    EXPECT_NE(std::string::npos, s.find("UBRING_test"));
}

TEST(UBRingConfigurationTest, time_flags_include_units_and_expected_defaults) {
    struct TimeFlagExpectation {
        const char* name;
        const char* suffix;
        const char* unit;
        const char* default_value;
    };
    const TimeFlagExpectation expected_flags[] = {
        {"ub_disconnect_timeout_s", "_s", "seconds", "5"},
        {"ub_connect_timeout_s", "_s", "seconds", "1"},
        {"ub_hb_timer_interval_s", "_s", "seconds", "5"},
        {"ub_event_queue_timer_interval_us", "_us", "microseconds", "100"},
        {"ub_flying_io_timeout_s", "_s", "seconds", "5"},
    };

    for (const auto& expected : expected_flags) {
        GFLAGS_NAMESPACE::CommandLineFlagInfo info;
        ASSERT_TRUE(GFLAGS_NAMESPACE::GetCommandLineFlagInfo(
            expected.name, &info)) << expected.name;
        const std::string flag_name(expected.name);
        const std::string suffix(expected.suffix);
        ASSERT_GE(flag_name.size(), suffix.size());
        EXPECT_EQ(flag_name.size() - suffix.size(), flag_name.rfind(suffix));
        EXPECT_NE(std::string::npos, info.description.find(expected.unit));
        EXPECT_EQ(std::string(expected.default_value), info.default_value);
    }

    EXPECT_EQ(5, brpc::ubring::FLAGS_ub_disconnect_timeout_s);
    EXPECT_EQ(1, brpc::ubring::FLAGS_ub_connect_timeout_s);
    EXPECT_EQ(5, brpc::ubring::FLAGS_ub_hb_timer_interval_s);
    EXPECT_EQ(100, brpc::ubring::FLAGS_ub_event_queue_timer_interval_us);
    EXPECT_EQ(5, brpc::ubring::FLAGS_ub_flying_io_timeout_s);
    EXPECT_EQ(100U * USEC_TO_NSEC,
              static_cast<uint32_t>(
                  brpc::ubring::FLAGS_ub_event_queue_timer_interval_us) *
                  USEC_TO_NSEC);
}

namespace brpc {
namespace ubring {
class UBShmEndpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        _saved_skip = g_skip_ub_init;
        g_skip_ub_init = false;
        ShmMgrInit();
        UBRingManager::UbrMgrInit();
        UBShmEndpoint::GlobalInitialize();

        brpc::SocketOptions options;
        ASSERT_EQ(0, brpc::Socket::Create(options, &_socket_id));
        brpc::SocketUniquePtr s;
        ASSERT_EQ(0, brpc::Socket::Address(_socket_id, &s));
        _socket = s.get();
        _ep = new UBShmEndpoint(_socket);
    }

    void TearDown() override {
        delete _ep;
        brpc::SocketUniquePtr s;
        if (brpc::Socket::Address(_socket_id, &s) == 0) {
            s->SetFailed();
        }
        UBShmEndpoint::GlobalRelease();
        UBRingManager::UbrMgrFini();
        ShmMgrFini();
        g_skip_ub_init = _saved_skip;
    }

    brpc::SocketId _socket_id = brpc::INVALID_SOCKET_ID;
    brpc::Socket* _socket = nullptr;
    UBShmEndpoint* _ep = nullptr;
    bool _saved_skip = false;
};
}  // namespace ubring
}  // namespace brpc

using brpc::ubring::UBShmEndpointTest;

TEST_F(UBShmEndpointTest, construct_initial_state) {
    ASSERT_NE(nullptr, _ep);
    EXPECT_EQ(brpc::ubring::UBR_DATA_FORMAT_NONE,
              _ep->_negotiated_data_format);
}

TEST_F(UBShmEndpointTest, reset_clears_negotiated_data_format) {
    _ep->_negotiated_data_format = brpc::ubring::UBR_DATA_FORMAT_LEGACY_64;

    _ep->Reset();

    EXPECT_EQ(brpc::ubring::UBR_DATA_FORMAT_NONE,
              _ep->_negotiated_data_format);
}

TEST_F(UBShmEndpointTest, allocate_client_resources_real_shm) {
    brpc::ubring::SHM local_trx_shm =
        {nullptr, 4 * 1024 * 1024, 0, {0}, (uint32_t)_socket->fd()};
    int ret = _ep->AllocateClientResources(&local_trx_shm, "UBRING_ut_client");
    EXPECT_EQ(0, ret);
}

TEST_F(UBShmEndpointTest, reset_cleans_up_resources) {
    brpc::ubring::SHM local_trx_shm =
        {nullptr, 4 * 1024 * 1024, 0, {0}, (uint32_t)_socket->fd()};
    _ep->AllocateClientResources(&local_trx_shm, "UBRING_ut_reset");
    _ep->Reset();
}

TEST_F(UBShmEndpointTest, reset_is_idempotent) {
    _ep->Reset();
    _ep->Reset();
}

// The receive paths (UbrTrxRecvBlockMode / StartReadv) read `msg_len' and
// `cur_index' out of a chunk header the remote peer writes into the ring, then
// copy `msg_len - cur_index' bytes from the 60-byte `payload.inner'. A peer
// that writes msg_len > 60, or cur_index > msg_len (which underflows the
// uint8_t subtraction), makes that copy over-read the payload into adjacent
// shared memory. IsRecvChunkHeaderValid is the guard both paths now apply.
TEST(UBRingRecvChunkHeaderTest, reject_out_of_range_len_and_index) {
    using brpc::ubring::UBRing;
    // Legitimate values a well-formed peer produces: full payload, partial
    // consume, and the fully-consumed boundary.
    EXPECT_TRUE(UBRing::IsRecvChunkHeaderValid(UBR_MSG_PAYLOAD_LEN, 0));
    EXPECT_TRUE(UBRing::IsRecvChunkHeaderValid(10, 5));
    EXPECT_TRUE(UBRing::IsRecvChunkHeaderValid(0, 0));
    EXPECT_TRUE(UBRing::IsRecvChunkHeaderValid(UBR_MSG_PAYLOAD_LEN,
                                               UBR_MSG_PAYLOAD_LEN));
    // msg_len past the payload capacity -> over-read source.
    EXPECT_FALSE(UBRing::IsRecvChunkHeaderValid(UBR_MSG_PAYLOAD_LEN + 1, 0));
    EXPECT_FALSE(UBRing::IsRecvChunkHeaderValid(255, 0));
    // cur_index past msg_len -> `msg_len - cur_index' underflows to a large
    // uint8_t.
    EXPECT_FALSE(UBRing::IsRecvChunkHeaderValid(0, 1));
    EXPECT_FALSE(UBRing::IsRecvChunkHeaderValid(10, 20));
}

// A crafted chunk laid out exactly like one in the ring: the guard rejects it
// so the recv loop never reaches the over-reading memcpy.
TEST(UBRingRecvChunkHeaderTest, crafted_chunk_is_rejected) {
    brpc::ubring::UbrMsgFormat chunk;
    memset(&chunk, 0xAB, sizeof(chunk));
    chunk.header[UBR_MSG_LEN_INDEX] = 255;  // peer claims 255 bytes in a 60-byte payload
    chunk.header[UBR_MSG_CUR_INDEX] = 0;
    EXPECT_FALSE(brpc::ubring::UBRing::IsRecvChunkHeaderValid(
        chunk.header[UBR_MSG_LEN_INDEX], chunk.header[UBR_MSG_CUR_INDEX]));

    chunk.header[UBR_MSG_LEN_INDEX] = UBR_MSG_PAYLOAD_LEN;
    chunk.header[UBR_MSG_CUR_INDEX] = 0;
    EXPECT_TRUE(brpc::ubring::UBRing::IsRecvChunkHeaderValid(
        chunk.header[UBR_MSG_LEN_INDEX], chunk.header[UBR_MSG_CUR_INDEX]));
}

#else

TEST(UbringDisabledTest, skip) {
    SUCCEED() << "BRPC_WITH_UBRING is not enabled, skip.";
}

#endif  // BRPC_WITH_UBRING
