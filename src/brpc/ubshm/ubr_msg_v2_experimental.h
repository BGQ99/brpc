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

#ifndef BRPC_UBSHM_UBR_MSG_V2_EXPERIMENTAL_H
#define BRPC_UBSHM_UBR_MSG_V2_EXPERIMENTAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/uio.h>
#include <limits>
#include <type_traits>

namespace brpc {
namespace ubring {
namespace experimental {

// Benchmark candidates only: no negotiated format ID or stable shared-memory
// ABI is assigned to these types. The 64-byte metadata region is an experimental
// choice, not a claim about the cache-line size of every supported CPU.
enum IpcV2SlotState : uint32_t {
    IPC_V2_SLOT_UNINITIALIZED = 0,
    IPC_V2_SLOT_EMPTY = 1,
    IPC_V2_SLOT_READY = 2,
};

enum IpcV2SlotFlags : uint32_t {
    // End of a write batch, not necessarily the end of an RPC or connection.
    IPC_V2_SLOT_EOF = 1u << 0,
};

enum IpcV2SlotResult {
    IPC_V2_SLOT_OK,
    IPC_V2_SLOT_NOT_READY,
    IPC_V2_SLOT_INVALID,
};

enum IpcV2RingResult {
    IPC_V2_RING_OK,
    IPC_V2_RING_RETRY,
    IPC_V2_RING_INVALID,
    IPC_V2_RING_TOO_LARGE,
};

struct IpcV2SlotHeader {
    // Concurrent access requires atomic operations; plain uint32_t defines
    // storage only. The experimental helpers are not used by the data path.
    uint32_t state;
    uint32_t payload_len;
    uint32_t flags;
    uint32_t reserved;  // Must be zero when publishing a slot.
};

struct IpcV2RingControl {
    // Consumer publishes the last reclaimed slot. Producer uses it to preserve
    // one empty sentinel and preflight a complete write batch.
    uint32_t tail;
};

static_assert(std::is_standard_layout<IpcV2SlotHeader>::value,
              "Header must have a predictable layout");
static_assert(sizeof(IpcV2SlotHeader) == 16, "Header must occupy 16 bytes");
static_assert(offsetof(IpcV2SlotHeader, state) == 0, "Unexpected state offset");
static_assert(offsetof(IpcV2SlotHeader, payload_len) == 4, "Unexpected length offset");
static_assert(offsetof(IpcV2SlotHeader, flags) == 8, "Unexpected flags offset");
static_assert(offsetof(IpcV2SlotHeader, reserved) == 12, "Unexpected reserved offset");
static_assert(std::is_standard_layout<IpcV2RingControl>::value,
              "Ring control must have a predictable layout");
static_assert(sizeof(IpcV2RingControl) == 4,
              "Ring control must contain only the shared tail");
static_assert(offsetof(IpcV2RingControl, tail) == 0, "Unexpected tail offset");

// PayloadOffset selects compact (16B) or separated (64B) metadata. Keep the
// existing separated candidate as the default; neither is a stable wire ABI.
template <size_t SlotSize, size_t PayloadOffset = 64>
struct alignas(64) IpcV2Slot {
    static_assert(SlotSize == 1024 || SlotSize == 4096 || SlotSize == 8192,
                  "Only 1/4/8 KiB benchmark candidates are supported");
    static_assert(PayloadOffset == 16 || PayloadOffset == 64,
                  "Only compact and separated metadata are supported");
    IpcV2SlotHeader header;
    // Member alignment supplies exactly 0 or 48 bytes after the header.
    // Layout tests assert the offset and stride for every candidate, without
    // relying on non-standard zero-length padding arrays.
    alignas(PayloadOffset) uint8_t payload[SlotSize - PayloadOffset];

    static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr),
                  "IPC slot state requires lock-free 32-bit atomics");

    // Experimental single-producer/single-consumer operations only. The caller
    // must provide correctly aligned storage and initialize it before sharing.
    // Ring initialization publishes every slot as EMPTY and then publishes the
    // initial tail. The peer must not access this slot before observing that
    // format initialization through the handshake and the shared tail.
    // Never copy/reset a live slot. Source/destination buffers and the local
    // read cursor must not overlap the slot. These helpers do not synchronize
    // multiple producers, multiple consumers, teardown or object lifetime.
    void Initialize() {
        header.payload_len = 0;
        header.flags = 0;
        header.reserved = 0;
        __atomic_store_n(&header.state, IPC_V2_SLOT_EMPTY, __ATOMIC_RELEASE);
    }

    IpcV2SlotResult TryPublish(const void* source, size_t length, uint32_t flags) {
        if (source == nullptr || length == 0 || length > sizeof(payload) ||
            (flags & ~static_cast<uint32_t>(IPC_V2_SLOT_EOF)) != 0) {
            return IPC_V2_SLOT_INVALID;
        }
        const uint32_t state = __atomic_load_n(&header.state, __ATOMIC_ACQUIRE);
        if (state == IPC_V2_SLOT_READY) {
            return IPC_V2_SLOT_NOT_READY;
        }
        if (state != IPC_V2_SLOT_EMPTY) {
            return IPC_V2_SLOT_INVALID;
        }
        memcpy(payload, source, length);
        header.payload_len = static_cast<uint32_t>(length);
        header.flags = flags;
        header.reserved = 0;
        __atomic_store_n(&header.state, IPC_V2_SLOT_READY, __ATOMIC_RELEASE);
        return IPC_V2_SLOT_OK;
    }

    // offset belongs exclusively to the consumer and starts at zero for each
    // slot. Partial reads retain READY. Completion resets offset and returns
    // EMPTY. batch_end is true only when an EOF-marked slot is fully consumed;
    // it never means connection EOF. A zero-sized read is a successful no-op.
    // On NOT_READY/INVALID, copied is zero, batch_end is false and offset and
    // the slot are unchanged. The caller must handle INVALID, not retry it as
    // ordinary backpressure.
    IpcV2SlotResult TryConsume(void* destination, size_t size, uint32_t& offset,
                             size_t& copied, bool& batch_end) {
        copied = 0;
        batch_end = false;
        if (size == 0) {
            return IPC_V2_SLOT_OK;
        }
        if (destination == nullptr) {
            return IPC_V2_SLOT_INVALID;
        }
        const uint32_t state = __atomic_load_n(&header.state, __ATOMIC_ACQUIRE);
        if (state == IPC_V2_SLOT_EMPTY) {
            return offset == 0 ? IPC_V2_SLOT_NOT_READY : IPC_V2_SLOT_INVALID;
        }
        if (state != IPC_V2_SLOT_READY) {
            return IPC_V2_SLOT_INVALID;
        }
        const uint32_t length = header.payload_len;
        const uint32_t flags = header.flags;
        if (length == 0 || length > sizeof(payload) || offset >= length ||
            (flags & ~static_cast<uint32_t>(IPC_V2_SLOT_EOF)) != 0 ||
            header.reserved != 0) {
            return IPC_V2_SLOT_INVALID;
        }
        const size_t remaining = length - offset;
        copied = size < remaining ? size : remaining;
        memcpy(destination, payload + offset, copied);
        offset += static_cast<uint32_t>(copied);
        if (offset == length) {
            batch_end = (flags & IPC_V2_SLOT_EOF) != 0;
            offset = 0;
            // Do not access metadata or payload after returning ownership.
            __atomic_store_n(&header.state, IPC_V2_SLOT_EMPTY, __ATOMIC_RELEASE);
        }
        return IPC_V2_SLOT_OK;
    }

    // Input is data-region length, after subtracting control data and alignment.
    // Discard a trailing partial slot. Reserve one slot to distinguish full
    // from empty, so at least two physical slots are required.
    static bool CalculateCapacity(size_t data_bytes, uint32_t& capacity) {
        capacity = 0;
        const size_t slots = data_bytes / SlotSize;
        if (slots < 2 || slots > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        capacity = static_cast<uint32_t>(slots);
        return true;
    }
};

inline bool ValidateIpcV2Iov(const struct iovec* iov, int iovcnt,
                             size_t& total) {
    total = 0;
    if (iovcnt < 0 || (iovcnt > 0 && iov == nullptr)) {
        return false;
    }
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len != 0 && iov[i].iov_base == nullptr) {
            return false;
        }
        if (iov[i].iov_len > std::numeric_limits<size_t>::max() - total) {
            return false;
        }
        total += iov[i].iov_len;
    }
    return true;
}

// Experimental producer-side view of one SPSC direction. Only slots and tail
// are shared with the consumer; write_pos belongs to this process. This class
// deliberately has no notification or lifetime management.
template <typename Slot>
class IpcV2TxView {
public:
    IpcV2TxView(Slot* slots, IpcV2RingControl* control, uint32_t capacity)
        : _slots(slots)
        , _control(control)
        , _capacity(capacity)
        , _write_pos(0) {}

    IpcV2RingResult TryWritev(const struct iovec* iov, int iovcnt,
                              size_t& written) {
        written = 0;
        size_t total = 0;
        if (!ValidateIpcV2Iov(iov, iovcnt, total)) {
            return IPC_V2_RING_INVALID;
        }
        if (total == 0) {
            return IPC_V2_RING_OK;
        }
        if (_slots == nullptr || _control == nullptr || _capacity < 2) {
            return IPC_V2_RING_INVALID;
        }
        const size_t payload_size = sizeof(_slots[0].payload);
        const size_t required = total / payload_size + (total % payload_size != 0);
        // Preserve the Legacy rule that one physical slot remains unused.
        if (required >= _capacity) {
            return IPC_V2_RING_TOO_LARGE;
        }

        // Zero-filled shared memory is UNINITIALIZED, not writable. The shared
        // tail is published only after the Consumer initializes every slot.
        const uint32_t current_state =
            __atomic_load_n(&_slots[_write_pos].header.state, __ATOMIC_ACQUIRE);
        if (current_state == IPC_V2_SLOT_UNINITIALIZED ||
            (current_state != IPC_V2_SLOT_EMPTY &&
             current_state != IPC_V2_SLOT_READY)) {
            return IPC_V2_RING_INVALID;
        }
        const uint32_t tail =
            __atomic_load_n(&_control->tail, __ATOMIC_ACQUIRE);
        if (tail >= _capacity) {
            return IPC_V2_RING_INVALID;
        }
        const uint32_t available = _write_pos > tail
            ? tail + _capacity - _write_pos : tail - _write_pos;
        if (available < required) {
            return IPC_V2_RING_RETRY;
        }

        // tail is authoritative for capacity. Slot state independently guards
        // ownership; disagreement means corruption rather than backpressure.
        for (size_t i = 0; i < required; ++i) {
            const uint32_t pos = static_cast<uint32_t>(
                (static_cast<uint64_t>(_write_pos) + i) % _capacity);
            const uint32_t state =
                __atomic_load_n(&_slots[pos].header.state, __ATOMIC_ACQUIRE);
            if (state != IPC_V2_SLOT_EMPTY) {
                return IPC_V2_RING_INVALID;
            }
        }

        int iov_index = 0;
        size_t iov_offset = 0;
        size_t remaining = total;
        while (remaining != 0) {
            Slot& slot = _slots[_write_pos];
            const size_t slot_length =
                remaining < payload_size ? remaining : payload_size;
            size_t slot_offset = 0;
            while (slot_offset < slot_length) {
                while (iov_index < iovcnt && iov[iov_index].iov_len == 0) {
                    ++iov_index;
                }
                const size_t available = iov[iov_index].iov_len - iov_offset;
                const size_t copy_length =
                    available < slot_length - slot_offset
                        ? available : slot_length - slot_offset;
                memcpy(slot.payload + slot_offset,
                       static_cast<const uint8_t*>(iov[iov_index].iov_base) + iov_offset,
                       copy_length);
                slot_offset += copy_length;
                iov_offset += copy_length;
                if (iov_offset == iov[iov_index].iov_len) {
                    ++iov_index;
                    iov_offset = 0;
                }
            }
            slot.header.payload_len = static_cast<uint32_t>(slot_length);
            slot.header.flags = remaining == slot_length
                ? static_cast<uint32_t>(IPC_V2_SLOT_EOF) : 0u;
            slot.header.reserved = 0;
            __atomic_store_n(&slot.header.state, IPC_V2_SLOT_READY, __ATOMIC_RELEASE);
            _write_pos = (_write_pos + 1) % _capacity;
            remaining -= slot_length;
        }
        written = total;
        return IPC_V2_RING_OK;
    }

private:
    IpcV2TxView(const IpcV2TxView&) = delete;
    IpcV2TxView& operator=(const IpcV2TxView&) = delete;

    Slot* _slots;
    IpcV2RingControl* _control;
    uint32_t _capacity;
    uint32_t _write_pos;
};

// Experimental consumer-side view of one SPSC direction. read_pos and the
// partial-read offset are process-local. The Consumer owns slot initialization
// and publishes reclaimed space through the Producer's shared tail.
template <typename Slot>
class IpcV2RxView {
public:
    IpcV2RxView(Slot* slots, IpcV2RingControl* control, uint32_t capacity)
        : _slots(slots)
        , _control(control)
        , _capacity(capacity)
        , _read_pos(0)
        , _read_offset(0) {}

    static bool InitializeShared(Slot* slots, IpcV2RingControl* control,
                                 uint32_t capacity) {
        if (slots == nullptr || control == nullptr || capacity < 2) {
            return false;
        }
        for (uint32_t i = 0; i < capacity; ++i) {
            slots[i].Initialize();
        }
        __atomic_store_n(&control->tail, capacity - 1, __ATOMIC_RELEASE);
        return true;
    }

    IpcV2RingResult TryReadv(const struct iovec* iov, int iovcnt,
                             size_t& read, bool& batch_end) {
        read = 0;
        batch_end = false;
        size_t destination_size = 0;
        if (!ValidateIpcV2Iov(iov, iovcnt, destination_size)) {
            return IPC_V2_RING_INVALID;
        }
        if (destination_size == 0) {
            return IPC_V2_RING_OK;
        }
        if (_slots == nullptr || _control == nullptr || _capacity < 2) {
            return IPC_V2_RING_INVALID;
        }

        int iov_index = 0;
        size_t iov_offset = 0;
        while (read < destination_size) {
            while (iov_index < iovcnt && iov[iov_index].iov_len == 0) {
                ++iov_index;
            }
            size_t copied = 0;
            bool slot_batch_end = false;
            const IpcV2SlotResult result = _slots[_read_pos].TryConsume(
                static_cast<uint8_t*>(iov[iov_index].iov_base) + iov_offset,
                iov[iov_index].iov_len - iov_offset, _read_offset,
                copied, slot_batch_end);
            if (result == IPC_V2_SLOT_NOT_READY) {
                return read == 0 ? IPC_V2_RING_RETRY : IPC_V2_RING_OK;
            }
            if (result != IPC_V2_SLOT_OK || copied == 0) {
                return IPC_V2_RING_INVALID;
            }
            read += copied;
            iov_offset += copied;
            if (iov_offset == iov[iov_index].iov_len) {
                ++iov_index;
                iov_offset = 0;
            }
            if (_read_offset == 0) {
                // TryConsume released EMPTY before returning. Publish tail only
                // after that slot is completely reclaimed.
                __atomic_store_n(&_control->tail, _read_pos, __ATOMIC_RELEASE);
                _read_pos = (_read_pos + 1) % _capacity;
                if (slot_batch_end) {
                    batch_end = true;
                    return IPC_V2_RING_OK;
                }
            }
        }
        return IPC_V2_RING_OK;
    }

private:
    IpcV2RxView(const IpcV2RxView&) = delete;
    IpcV2RxView& operator=(const IpcV2RxView&) = delete;

    Slot* _slots;
    IpcV2RingControl* _control;
    uint32_t _capacity;
    uint32_t _read_pos;
    uint32_t _read_offset;
};

}  // namespace experimental
}  // namespace ubring
}  // namespace brpc

#endif  // BRPC_UBSHM_UBR_MSG_V2_EXPERIMENTAL_H
