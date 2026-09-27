# UBRing IPC_V2 数据格式设计草案

> 状态：内部讨论稿，尚未形成兼容性承诺，也不应视为最终实现方案。
>
> 目标：为 Issue #3463 Phase 3 PR2 提供设计评审输入。在开始修改
> UBRing 数据路径之前，确认 IPC 专用 slot layout、初始化时序、
> direct-write 方式和共享内存发布语义。

## 1. 背景

UBRing 当前的 IPC 和 UBS 后端共用同一种数据格式：

- 每个 slot 固定为 64B；
- 其中 payload 为 60B，header 为 4B；
- 单个 slot 的有效长度保存在 8 位字段中；
- Producer 先把数据复制到 `UbrTx::local_msg_space`，再通过
  `Copy64Byte()` 发布到共享内存；
- IPC 和 UBS 使用相同的 `UbrTrxWritev()`、`StartReadv()` 和容量计算。

Phase 3 PR1 保持 64B Base Hello 不变，增加了独立的 4B format
negotiation，并定义了 `LEGACY_64`。PR2 计划新增 IPC 专用格式
`IPC_V2`，而 UBS 继续使用 `LEGACY_64`。

PR2 的目标不是修改 UBS，也不是重新设计 handshake；它使用 PR1
保存的 negotiated format，选择对应的数据队列实现。

## 2. 目标

PR2 需要做到：

1. 定义一个布局固定、可明确协商的 IPC 专用格式 `IPC_V2`；
2. IPC 使用明显大于 60B 的固定 payload；
3. IPC Producer 从 `iovec` 直接写入共享内存 slot，不再经过
   `local_msg_space -> Copy64Byte()`；
4. 使用明确的 release/acquire 语义发布和回收 slot；
5. IPC 和 IPC 节点协商成功后才能启用 `IPC_V2`；
6. UBS 始终保留现有 `LEGACY_64` 布局和数据路径；
7. 不兼容的 backend/format 安全 fallback TCP；
8. format 初始化失败时不得进入 UBRing 数据路径。

## 3. 非目标

本 PR 不计划：

- 修改 64B Base Hello 或 4B FormatExtension；
- 修改 `LEGACY_64` 的 64B slot、60B payload 或 `Copy64Byte()`；
- 优化 UBS direct-write；
- 顺带重构已有 timer、连接关闭或 fallback 清理机制；但本次初始化拆分
  引入的部分初始化状态，必须定义退出和资源释放方式；
- 引入可在运行时任意配置的 IPC slot size；
- 为跨不同 ABI、不同字节序的进程提供 IPC_V2 兼容性。

## 4. 当前实现与主要问题

### 4.1 当前 slot

当前 `UbrMsgFormat` 定义在 `src/brpc/ubshm/ubr_msg.h`：

```text
+------------------------------+------------------+
| payload: 60B                 | header: 4B       |
+------------------------------+------------------+
|                              | flag/len/cursor  |
+------------------------------+------------------+
```

`header[1]` 是 8 位长度，无法表达 4KiB 或 8KiB payload。

### 4.2 当前发送路径

```text
IOBuf
  -> iovec[]
  -> memcpy to UbrTx::local_msg_space (最多 60B)
  -> Copy64Byte()
  -> remote shared-memory slot
```

对于大消息，这会产生大量 60B 分片以及额外的中间复制。

### 4.3 当前接收路径

Consumer 通过共享 header 中的 `flag` 判断 slot 是否可读，复制 payload
后更新共享 `cursor`；整个 slot 消费完后将 `flag` 清为 `NONE` 并更新
tail。

### 4.4 当前初始化时序与 format negotiation 冲突

Server 当前在 format negotiation 之前执行 `UbrServerTrxInit()`。该函数
已经：

- 使用 `data_queue_len / 64` 计算 capacity；
- 按 `UbrMsgFormat` 清空 data queue；
- 写入 `tail = capacity - 1`；
- 启动 timer；
- 将 TX/RX 状态设为 `CONNECTED`。

因此 PR2 不能只在 send/recv 入口按 format 分支。必须先把“分配和映射
raw shared memory”与“按 negotiated format 初始化数据队列”拆开。

### 4.5 调用链与并发前提

以下为当前代码可确认的正常调用路径，函数位置以符号名为准：

```text
Socket::DoWrite
  -> UBShmTransport::CutFromIOBufList
  -> UBShmEndpoint::CutFromIOBufList
  -> UBRing::UbrTrxWritev

UBShmEndpoint::PollingModeInitialize 中的 poller 循环
  -> UBShmEndpoint::PollIn
  -> IOBuf::append_from_reader
  -> UBRing::ReadV -> UbrTrxReadv -> StartReadv
```

- `src/brpc/socket.h` 的 `Socket::Write` 说明及 `socket.cpp` 的
  `StartWrite` 队列保证同一 Socket 同一时刻只有一个写入者。
- `src/brpc/ubshm/ub_endpoint.cpp` 的 `PollerRegisterEvent` 按
  `_poller_sid` 选取 poller；轮询循环直接、顺序调用 `PollIn`。
  同一 poller 内按 sid 去重，但 group 由 `bthread_self_tag()` 决定。
- 因此 IPC_V2 建议依赖单 producer、单 consumer：同一时刻各只有一个
  活动的数据写入者和读取者，不要求始终使用同一个线程。直接并发调用
  同一个 UBRing 的底层发送或接收接口不在该约定内。
- 同一连接的注册、修改和移除是否始终落在同一 group，仍需验证；也不能
  据此认为 timer、PollOut、关闭线程不会并发访问其他状态。

### 4.6 EOF 与 RPC 边界

`Socket::DoWrite` 会合并多个写请求；endpoint 又按 iovec 数量上限组装
一次底层写入。`UbrTrxWritev` 的 EOF 标记该次写入批次的最后一个 slot，
不保证对应一个完整 RPC，也不是连接关闭标志。RPC 边界由上层协议解析。
IPC_V2 首版保留这一语义，不为 slot 增加 RPC ID。

## 5. Format negotiation 策略

建议新增：

```cpp
UBR_DATA_FORMAT_IPC_V2 = 2
```

建议的选择矩阵：

| Client backend | Server backend | Client proposal | Server selection |
| --- | --- | --- | --- |
| IPC | IPC | `IPC_V2` | `IPC_V2` |
| UBS | UBS | `LEGACY_64` | `LEGACY_64` |
| IPC | UBS | `IPC_V2` | `NONE` |
| UBS | IPC | `LEGACY_64` | `NONE` |

PR1 的 FormatExtension 一次只表达一个 format，不是候选格式列表。因此
`IPC_V2` 方案不假设 mixed backend 会退回 `LEGACY_64`；mixed backend
应 fallback TCP。

handshake 需要查询本地 backend。建议由 `shm_mgr` 提供只读接口，例如：

```cpp
SHM_TYPE GetShmType();
```

或提供更窄的判定接口。不能直接让 endpoint 读取 `shm_mgr.cpp` 的内部
全局变量。

## 6. Format 类型的代码分层

`UbrDataFormat` 当前定义在 `ub_endpoint.h`，但 PR2 中 `UBRing` 也需要
保存和读取它。`ub_ring.h` 不应反向依赖 endpoint。

建议将 format enum 移到独立头文件，例如：

```text
src/brpc/ubshm/ubr_data_format.h
```

由 `ub_endpoint.h` 和 `ub_ring.h` 同时包含。该移动只改变声明位置，不
改变 PR1 的 wire value。

## 7. IPC_V2 候选 slot layout

本节给出用于讨论的候选布局，不在评审完成前冻结具体大小。

```cpp
enum IpcV2SlotState : uint32_t {
    IPC_V2_SLOT_UNINITIALIZED = 0,
    IPC_V2_SLOT_EMPTY = 1,
    IPC_V2_SLOT_READY = 2,
};

enum IpcV2SlotFlags : uint32_t {
    IPC_V2_SLOT_EOF = 1u << 0,
};

struct IpcV2SlotHeader {
    uint32_t state;
    uint32_t payload_len;
    uint32_t flags;
    uint32_t reserved;
};
```

实验头文件使用相同状态值，并由双view实验验证初始化状态转换；这些值在
layout冻结前仍然不构成稳定共享内存ABI。

候选 slot：

```text
+----------------------+--------------------------+
| header: 16B          | payload: fixed N bytes   |
+----------------------+--------------------------+
| state/len/flags      |                          |
+----------------------+--------------------------+
```

要求：

- 新分配的零填充共享内存自然处于`UNINITIALIZED`，不能把尚未完成格式
  初始化的slot误认为可写；
- queue owner完成其他metadata初始化后，以release语义将state从
  `UNINITIALIZED`发布为`EMPTY`；
- `state` 自然对齐，并通过原子load/store访问；
- `payload_len` 使用 32 位，能够表达大 payload；
- `flags` 表达一次底层 write 批次的 EOF，含义见 4.6；
- `reserved` 当前必须写 0；接收方是否拒绝非零值需在冻结协议前确定。
  预留字段不自动提供兼容性：未来用途若改变必需的读取语义，仍须重新
  评估 format 兼容性；
- slot起始地址和stride至少按cache line对齐；
- 使用`static_assert`固定header大小、字段偏移、slot大小和对齐；
- IPC_V2是共享内存ABI，不能依赖编译器未约束的padding；
- 不能直接把整个C++对象当成不稳定布局传输。

### 7.1 Header在前还是在后

当前实验头文件仅实现 header 在前的两种候选，暂不实现尾部 header：

| 候选 | header | header 后 padding | payload 起始偏移 | 4KiB slot 的 payload |
| --- | ---: | ---: | ---: | ---: |
| 紧凑 | 16B | 0B | 16B | 4080B |
| 分隔（当前默认） | 16B | 48B | 64B | 4032B |

两种候选的 slot 起始地址均按 64B 对齐，总大小均可选 1/4/8KiB。
实验类型 `IpcV2Slot<SlotSize, PayloadOffset>` 的第二个参数选择 16 或 64，
默认 64。成员 `alignas` 约束 payload 偏移，布局测试检查六种组合的
大小、对齐、字段偏移和容量边界，不使用零长度 padding 数组。
64B 是实验对齐选择，不代表所有目标 CPU 的 cache line 大小。
是否保留 padding 仍需相同算法和负载下的 benchmark 决定；当前只有布局，
尚未启用 IPC_V2 数据路径，也未分配稳定 format ID。

候选A：header在前。

- 优点：实现和调试直观；
- 缺点：Consumer轮询state时会反复访问payload所在slot的首个cache line。

候选B：payload在前、header在尾部。

- 优点：接近当前布局，轮询不直接访问 payload 开头；
- 缺点：header 仍可能与 payload 尾部共享 cache line，并不自动消除
  争用；地址计算稍复杂，slot 总大小仍需严格固定。

该选择需要结合cache profile和benchmark决定，不能只依据代码美观。

### 7.2 Consumer读取cursor

建议不再把部分读取cursor存入共享slot。将其保存在Consumer本地：

```cpp
uint32_t ipc_v2_read_offset;
```

理由：

- slot发布后，metadata在归还前可以保持只读；
- Producer只负责`EMPTY -> READY`；
- Consumer只负责读取并执行`READY -> EMPTY`；
- 减少对共享header的多次写入。

必须验证一次`ReadV()`未消费完整slot时，本地cursor能够跨调用保存，并在
slot完全消费或连接reset时清零。

## 8. Slot size 与 payload size

`IPC_V2`的format id必须唯一决定完整布局，包括slot size、header大小和
payload大小。不能让两端通过未协商的gflag分别配置payload，否则同样的
`IPC_V2`会被解释成不同布局。

建议在合入前比较至少三组候选：

| 候选slot | 近似payload | 4MiB data queue中的slot数量 | 特点 |
| --- | ---: | ---: | --- |
| 1KiB | 约1008B | 约4096 | 小消息浪费较少，大消息仍有较多分片 |
| 4KiB | 约4080B | 约1024 | 与常见page和RPC块大小接近 |
| 8KiB | 约8176B | 约512 | 大消息分片更少，但环容量下降明显 |

表中 1/4/8KiB 指 slot 总大小；16B header 且无额外 padding 时，payload
分别为 1008/4080/8176B。从 shm 总长度计算实际 data queue 长度时，需扣除
控制区和对齐开销；沿用保留一个空位的 ring 规则时，可用 slot 数还要减一。
实验阶段允许编译期切换大小，但两端必须使用相同候选构建，不能把不同
布局以同一个 `IPC_V2` ID 混合运行。4KiB/8KiB 当前均未选定。

最终值需要在目标IPC机器上通过benchmark确定。数值一旦随`IPC_V2`进入
稳定版本，后续改变它应使用新的format id，而不是静默修改常量。

## 9. 初始化与激活时序

建议将当前流程区分为raw prepare、format prepare和runtime commit三个阶段。
这些阶段是实现约束，不要求额外引入大型状态机类。

### 9.1 Prepare raw shared memory

只执行：

- 分配local shm；
- 映射local shm；
- 在当前握手阶段已具备远端信息时映射 remote shm；Client 映射 Server
  shm 仍放在收到匹配的 format selection 之后；
- 设置共享内存控制区和data queue的基地址、长度；
- 保持TX/RX未连接；
- 不计算格式相关capacity和tail；
- 不允许进入send/recv数据路径。

### 9.2 Prepare negotiated format and runtime prerequisites

format确定后先执行布局初始化，例如：

```cpp
RETURN_CODE UBRing::InitializeDataFormat(UbrDataFormat format);
```

该步骤负责：

- 校验backend与format匹配；
- 根据format得到slot size和payload size；
- 计算rx/tx capacity；
- 校验capacity至少能维护一个有效ring；
- 初始化本地读写位置、metadata 和 cursor；
- 只初始化本端负责的slot state和tail，见9.5；
- 保持TX/RX未连接。

布局初始化成功后，再准备建立连接所必需、且创建过程可能失败的运行期
资源，例如：

```cpp
RETURN_CODE UBRing::PrepareRuntimeResources();
```

该步骤创建timer等资源，但不发布`CONNECTED`：

- 保持TX/RX未连接，timer callback必须在观察到未连接状态时安全退出；
- 准备函数返回成功不等于允许poller或数据路径访问。

函数名称只是职责示意；重点是格式布局初始化、可失败资源准备和最终commit
不能混成一个对外不可区分的操作。

将“布局准备成功”与“激活连接”区分开。所有可能失败的必要准备，应在
Server 回复成功 selection / Client 发送 ACK=1 之前完成。当前timer API在
创建timer后会立即允许callback运行，因此callback必须通过`trx_state`等
运行期状态门控，不能在format prepare期间访问尚未激活的数据路径。

不能把所有timer创建都推迟到ACK之后：timer创建可能失败，而ACK=1后双方
已经对成功建立UBRing作出承诺。若未来提供“先创建、后无失败地arm”的
timer API，也可以使用该两阶段API实现相同语义。失败时遵循第15节，
endpoint不保存成功协商结果。

### 9.3 Commit runtime

commit只执行不应失败的状态发布操作：

- Client在成功写出ACK=1后commit；
- Server在成功读取并校验ACK=1后commit；
- 设置底层TX/RX状态为`CONNECTED`；
- 在endpoint中保存negotiated format；
- 登记poller并允许transport进入UBRing数据路径；
- 成功后不再执行会清零slot或重置tail的初始化。

poller登记是异步操作，但READY slot会保留到Consumer读取，poller开始扫描后
仍能发现提前到达的数据，因此不需要为首次数据增加一次性通知协议。

### 9.4 移除Client对remote tail推导长度的依赖

当前Client映射Server shm时，通过共享tail反推remote shm长度。该tail又
由Server按Legacy capacity提前写入，这与延迟format初始化冲突。

Server Base Hello已经携带共享内存长度。建议Client直接使用收到的
`remote_msg.len`构造remote `SHM`，不要再用尚未初始化的tail反推长度。

这一改动需要单独测试非法长度、溢出、后端实际要求的映射长度/对齐约束
及错误 shm 名称；不能把当前分配配置直接视为所有后端的协议约束。

raw映射阶段只做与format无关的长度和后端约束检查。slot size、capacity及
`capacity - 1`等检查必须放在format prepare阶段，不能继续使用
`remote_len / UBR_MSG_LEN`这样的Legacy专用条件验证IPC_V2。

### 9.5 IPC_V2 初始化职责（建议）

以 Client -> Server 为例：数据队列位于 Server shm，Client 写入，Server
消费；Server 将回收位置写入 Client shm 的发送状态区。反方向对称。

| 内容 | 初始化方 | 完成时间 |
| --- | --- | --- |
| Server 接收队列 slot state | Server | 成功 selection 前 |
| Client 发送状态区的初始 tail | Server | 成功 selection 前 |
| Client 接收队列 slot state | Client | ACK=1 前 |
| Server 发送状态区的初始 tail | Client | ACK=1 前 |
| 各端本地读写位置、cursor、rx/tx capacity | 各端自身 | 各自承诺成功前 |

初始 tail 按其对应数据队列的容量计算，不假设两端 shm 大小相等。
这里的“owner”按字段职责划分，不等于只写自己映射的整块内存：Consumer
拥有并初始化自己的接收队列slot，但对应tail位于对端Producer的状态区，
因此由Consumer通过remote mapping初始化和更新。每端不再次清零对端队列。
当前
`ub_ring.cpp` 的 `PrewriteUbrTx` / `PrewriteUbrRx` 分别清零远端和本地
队列，IPC_V2 不能照搬两端重复清零的行为；Legacy 保持现有路径。

初始化每个slot时，先写metadata，再release-store `EMPTY`；完成全部slot后，
release-store初始tail。Producer先acquire-load初始tail，再访问slot state。
共享控制字段的其余初始化写入、发布屏障和timer访问也必须纳入审查。
握手字节的先后顺序不能替代目标平台所需的共享内存发布保证。

## 10. 建议的新握手与初始化流程

### 10.1 IPC_V2成功路径

```text
Client                               Server
  | prepare local raw shm              |
  |--- 64B Base Hello V3 ------------->|
  |                          prepare/map raw shm
  |<-- 64B Base Hello V3 --------------|
  |--- FormatExtension IPC_V2 -------->|
  |                    validate backend/format
  |                    prepare own RX slots + peer TX tail
  |                    create gated timers
  |                    complete all fallible preparation
  |<-- Selected IPC_V2 ----------------|
  | map Server raw shm                  |
  | prepare own RX slots + peer TX tail |
  | create gated timers                 |
  | complete all fallible preparation   |
  |--- ACK=1 -------------------------->|
  |                              publish CONNECTED
  |                              register poller
  | publish CONNECTED                   |
  | register poller                     |
```

“commit endpoint”表示设置 negotiated format 并允许 transport 进入 UBRing
数据路径。Client 成功发送 ACK 后可开始发送，因此 Server 必须能容纳
“数据已写入，但尚未处理 ACK”的情况；处理 ACK 后不得再清零队列。
Client 在 ACK=1 后同样不得再执行破坏性初始化。

timer可在format prepare中创建，但callback必须由未连接状态门控；poller和
业务数据访问等待commit。可回退的初始化失败走NONE/ACK=0；握手I/O失败或
数据路径启用后的失败按第15节关闭连接，不能笼统地保证所有失败都可切回
TCP。

### 10.2 UBS LEGACY_64路径

握手仍通过PR1选择`LEGACY_64`。数据队列初始化和send/recv最终调用现有
Legacy实现，保持：

- 64B slot；
- 60B payload；
- `local_msg_space`；
- `Copy64Byte()`；
- 原有发布和回收行为。

## 11. IPC_V2 direct-write

### 11.1 批次与返回值约定（建议）

首版沿用 `ub_ring.cpp::WritevHasEnoughSpace` 的整批空间预检原则，保留
一个空slot。shared tail是计算可用slot数量的权威来源；slot state负责数据
发布、归还和覆盖前校验。总字节数累加和向上取整计算必须检查溢出，不能
先窄化到32位再计算。

| 条件 | 对外行为 | 共享队列变化 |
| --- | --- | --- |
| 总字节数为 0 | 返回 0 | 不发布 slot，不生成 EOF |
| 所需 slot 数大于 capacity - 1 | 错误，`EMSGSIZE` | 不发布 slot |
| ring 可容纳整批，但当前空闲不足 | 重试；endpoint 转换为 `-1/EAGAIN` | 不发布 slot |
| 空间足够 | 成功返回整批字节数 | 按顺序发布，最后一片带 EOF |

这里的“整批预检”不代表原子发布整批数据：Consumer 仍可能在 Producer
尚未发布最后一片时开始读取。若中途发生连接错误或发现状态损坏，必须
终止连接，不能把已经发布的前缀当作未发送而重试整个批次。

### 11.2 Slot 写入

上层仍提供`iovec[]`。所谓direct-write是去掉UBRing内部中间slot，不是
完全零拷贝。

建议使用本地iovec cursor：

```cpp
struct IovecCursor {
    int index;
    size_t offset;
};
```

每个slot的Producer流程：

1. acquire-load tail并确认整批空间充足；
2. 以acquire语义确认每个目标slot为`EMPTY`；
3. 从一个或多个iovec直接`memcpy`到共享slot payload；
4. 写入`payload_len`；
5. 写入`flags`和reserved字段；
6. 以release语义执行`state = READY`；
7. 推进`write_pos`。

禁止先发布`READY`再填充payload，也不能在slot尚未变为`EMPTY`时覆盖它。

## 12. IPC_V2接收

每个slot的Consumer流程：

1. 以acquire语义读取`state`，`UNINITIALIZED`不是普通的暂无数据；
2. 只有`READY`才读取其他metadata和payload；
3. 校验 `0 < payload_len <= IPC_V2_PAYLOAD_SIZE`、flags 和本地 cursor，
   并在本地保存本次所需的 EOF 判断；
4. 从slot直接复制到一个或多个目标iovec；
5. 若调用者buffer不足，保存本地`ipc_v2_read_offset`；
6. 整个slot消费完成后，以release语义执行`state = EMPTY`；
7. 发布回收 tail，清零本地 cursor，再推进 `read_pos`，同步约定见 13.1；
8. EOF slot 完整消费后结束本次底层批次读取；如果目标 buffer 提前用完，
   返回已读字节并在下次继续，不能提前归还 slot。发布 EMPTY 后不再访问
   该 slot 的 payload 或 metadata，因为 Producer 已有权覆盖它。

没有数据时返回 `-1/EAGAIN`，不返回表示连接结束的 0。正常读取一部分
后遇到暂时无数据，返回已读字节数。零长度目标 buffer 不消费队列，也
不能将其无操作返回值当成实际连接 EOF 传给上层；调用边界需单独验证。

遇到非法长度或非法flags时，不能越界读取。建议将连接标记失败，而不是
把损坏slot继续当作正常数据消费。

## 13. Memory ordering

建议的最小发布关系：

```text
Producer                              Consumer
write payload/len/flags
release-store READY  ------------->  acquire-load READY
                                      read payload/len/flags

acquire-load EMPTY  <-------------  release-store EMPTY
overwrite reused slot

acquire-load tail   <-------------  release-store tail
observe reclaimed space             after releasing EMPTY
```

`volatile`不能替代这些同步关系。

可考虑使用编译器原子内建：

```cpp
__atomic_store_n(&header.state, IPC_V2_SLOT_READY, __ATOMIC_RELEASE);
__atomic_load_n(&header.state, __ATOMIC_ACQUIRE);
```

采用前必须确认：

- `state`自然对齐；
- 目标CPU上的32位原子操作lock-free；
- 项目支持的编译器对跨进程共享内存提供所需实现；
- UBS Legacy路径不使用这套新语义；
- 初始化和reset期间没有并发数据访问。

设计评审需要由熟悉目标ARM/IPC平台内存模型的同事重点确认这一节。

### 13.1 共享字段与空间回收（建议）

| 字段 | 写入者 | 读取者 | IPC_V2 约定 |
| --- | --- | --- | --- |
| payload、payload_len、flags、reserved | Producer | Consumer | 在 READY 前写完，归还前不变 |
| slot state | Producer / Consumer 交替 | 对端及可读检查 | release-store / acquire-load |
| tail | Consumer | Producer、可写检查 | 建议原子 release-store / acquire-load |
| 本地读取 cursor | Consumer | Consumer | 完整消费或 reset 时清零 |
| io_id | Producer | 可读检查 | 是否继续参与 IPC_V2 通知及同步方式待定 |

tail作为整批空间预检的权威来源：Consumer完成读取，先release-store
EMPTY，再release-store新tail；Producer acquire-load tail计算空闲量，
覆盖slot前再acquire-load检查EMPTY。tail不得提前宣告尚未归还的空间。
可写检查须使用同一回收规则；不能混用同一字段的普通访问和原子访问。
若空间预检与 slot 状态矛盾，按协议错误处理，不覆盖仍在使用的数据。

### 13.2 Poller 与通知约定

当前 `ub_endpoint.cpp` 的资源注册使用 `EPOLLIN`，poller 持续扫描并直接
执行 `PollIn`。`ub_ring.cpp::IsUbrTrxReadable` 仅在 `EPOLLET` 模式下用
`io_id` 去重和 `ep_eof_pos` 选择检查位置，不能认为所有读取都由 io_id 驱动。

IPC_V2可读检查必须通过acquire观察READY，且不能因过期通知信息一直
跳过READY slot。首版建议直接检查当前slot state，不让`io_id`参与正确性
判断；`io_id`最多作为后续可验证的性能提示。若保留共享`io_id`读写，也
必须定义原子访问和发布顺序，不能继续把普通`uint64_t`并发读写当作同步。

`ubshm_transport.cpp::WaitEpollOut` 注册 EPOLLOUT 并等待
butex，由 `PollOut` 检查空间后唤醒；新格式的预检与唤醒必须使用一致的
capacity/tail 规则。可写只表示有进展机会，不保证足够容纳下一整批。

poller持续扫描已登记endpoint，因此在ADD生效前发布的READY slot可以在
登记后被发现。仍需验证等待写空间的注册/取消竞争，以及同一连接是否可能
被登记到不同poller group并产生多个Consumer。slot的release/acquire只
证明数据发布关系，不能单独证明线程调度和对象生命周期安全。此处不改变
UBS的通知行为。

### 13.3 进程两端的本地状态

生产模型中Client和Server分别持有独立的UBRing对象，不能共享包含双方游标
的同一个C++对象。建议按方向保存两个本地view：

```cpp
struct IpcV2TxView {
    IpcV2Slot* remote_slots;
    UbrDataStatusQMsg* local_status;
    uint32_t capacity;
    uint32_t write_pos;
};

struct IpcV2RxView {
    IpcV2Slot* local_slots;
    UbrDataStatusQMsg* remote_status;
    uint32_t capacity;
    uint32_t read_pos;
    uint32_t read_offset;
};
```

字段名称仅表达所有权和地址方向，不要求直接采用该公开类型。Tx只拥有本地
`write_pos`，Rx只拥有本地`read_pos/read_offset`；两端只通过共享slots、
tail及既有控制区通信。实验和benchmark必须使用两个独立view，不能让
Producer和Consumer共享一个同时包含读写游标的ring对象。

## 14. 数据路径分派

建议在公共入口按format分派，而不是在Legacy循环内部散落大量条件：

```cpp
ssize_t UBRing::UbrTrxWritev(const iovec* iov, int iovcnt) {
    switch (_data_format) {
    case UBR_DATA_FORMAT_LEGACY_64:
        return UbrTrxWritevLegacy(iov, iovcnt);
    case UBR_DATA_FORMAT_IPC_V2:
        return UbrTrxWritevIpcV2(iov, iovcnt);
    default:
        return UBRING_ERR;
    }
}
```

同样需要分派：

- `UbrTrxReadv()`；
- `IsUbrTrxReadable()`；
- `IsUbrTrxWriteable()`；
- capacity/space计算；
- slot初始化；
- 部分读取实现。

Legacy函数可以从当前实现机械提取，提取提交中不改变其行为。IPC_V2使用
独立实现，避免两套slot语义互相污染。

## 15. 错误处理

### 15.1 协商错误

- 不支持的proposal：Server选择`NONE`；
- Client收到`NONE`或不同format：发送`ACK=0`并fallback TCP；
- 新旧Hello version：不交换extension，按PR1 fallback TCP。

### 15.2 初始化错误

- slot size或共享内存长度非法：初始化失败；
- capacity不足：初始化失败；
- remote shm映射失败：ACK=0或连接失败，遵循现有handshake边界；
- Server无法初始化所选format：不得向Client回复成功selection；
- Client无法初始化Server选择的format：发送`ACK=0`。

以上 fallback 以握手仍可正常交换规定帧、业务数据尚未启用为前提。
ACK 或其他握手 I/O 失败时关闭连接；ACK=1 后的失败不能再通过 ACK=0
反悔。拆分初始化后，需列出仅分配、已映射、布局就绪、已启用回调各状态
持有的资源，确保失败清理不访问未映射内存，也不遗漏本次新增的资源。

format prepare期间创建的timer在abort时必须停止并等待正在执行的callback
退出，之后才能释放mapping和manager slot。不能通过timer fd是否非零推断
callback已经结束。该实现应复用Phase 2提供的timer/cleanup所有权基础，
PR2只增加握手期prepared-resource abort适配，不重新实现一套timer系统。

### 15.3 数据损坏

- READY slot 的 `payload_len` 为 0 或超过固定上限；
- 未知state或flags；
- cursor超过payload长度；

这些情况应终止UBRing连接，不能继续读取越界数据，也不应静默切回TCP，
因为TCP与UBRing的数据流此时已经不是同一消费位置。

## 16. 建议的实现步骤

1. 先把实验ring拆成独立Tx/Rx view，并加入shared tail；当前共享读写游标的
   实验结果不能用于选择最终slot；
2. 用双view实验验证`UNINITIALIZED -> EMPTY -> READY`、tail及回绕；
3. 在同一算法下benchmark候选slot大小和padding，冻结唯一IPC_V2布局；
4. 移动共享format定义，新增稳定`IPC_V2`值和冻结后的layout；
5. 增加backend只读查询，完成IPC/UBS format选择测试；
6. 拆分raw shm prepare、format prepare与runtime commit，Legacy继续调用原有
   数据实现；
7. 让`UBRing`保存prepared format，并在公共入口按committed format分派；
8. 按确定的release/acquire和tail预检约定接入IPC_V2 writev/readv；
9. 接入可读/可写检查，验证poller、写等待和并发可见性；
10. 基于Phase 2清理能力完成握手期abort，并在IPC和UBS真实环境回归。

建议每一步使用独立commit，便于确认Legacy数据路径是否发生意外变化。

## 17. 测试计划

### 17.1 Layout UT

- header字段大小和offset；
- slot大小与对齐；
- state字段对齐；
- payload长度可表达大于255B；
- capacity计算不越界、不溢出。

### 17.2 Negotiation UT

- IPC + IPC选择`IPC_V2`；
- UBS + UBS选择`LEGACY_64`；
- mixed backend选择`NONE`；
- 未知format选择`NONE`；
- V2/V3不进入extension阶段。

### 17.3 IPC_V2数据路径UT

测试必须让Producer和Consumer持有两个独立view，只共享模拟shared-memory
slots和control字段，不能共享包含双方游标的同一个ring对象。

消息长度至少覆盖：

- 1B；
- `payload_size - 1`；
- `payload_size`；
- `payload_size + 1`；
- `2 * payload_size`；
- 多个iovec拼成一个slot；
- 一个iovec跨多个slot；
- 零长度iovec段；
- ring尾部回绕到slot 0；
- ring满返回`EAGAIN`；
- Consumer目标buffer小于当前slot，跨调用继续读取；
- 非法payload length被拒绝。

还需验证本次明确的接口约定：零字节 write 无发布；超过可用容量返回
EMSGSIZE；暂时空间不足时无部分发布；预检溢出被拒绝；EOF slot 的部分
读取保持 cursor；多个 RPC 合并或一个 RPC 跨批次时数据流仍完整。

### 17.4 并发与可见性压力测试

- Producer/Consumer持续复用少量slot；
- payload写入递增序号、固定pattern和checksum；
- 检查旧数据、半写数据、重复数据和乱序；
- 多种消息大小和iovec分段；
- 长时间运行并重复建立/关闭连接。

定向调度场景包括：EMPTY 与 tail 发布之间暂停、发布部分批次后暂停、
写等待注册期间 Consumer 归还空间、各端必要初始化失败、ACK 已发送但
Server 尚未处理时 Client 写入、关闭时仍有 poller/timer 访问。验证同一
连接不会因跨 group 注册获得两个并行 Consumer；实验结果不能代替调用
约束和同步关系审查。

另需覆盖零填充内存仍为`UNINITIALIZED`、format prepare后才能观察到
`EMPTY`、timer已创建但尚未commit时callback不访问数据路径，以及abort
等待callback退出后才释放共享内存。

TSAN可以辅助发现进程内问题，但不能单独证明跨进程共享内存的内存顺序。

### 17.5 真实环境

- 同机IPC：V3/V3协商`IPC_V2`并完成RPC；
- UBS环境：V3/V3协商`LEGACY_64`并完成RPC；
- 新旧版本安全fallback TCP；
- UBS调用路径仍经过Legacy和`Copy64Byte()`；
- IPC大消息能够跨slot传输；
- 连接关闭、异常退出和资源清理无回归。

### 17.6 Benchmark

至少比较：

- slot 总大小候选：1KiB、4KiB、8KiB，payload 扣除 header 和 padding；
- RPC payload：0B、64B、1KiB、4KiB、8KiB、64KiB及更大消息；
- 单线程延迟；
- 多线程吞吐；
- queue depth变化；
- CPU、cache miss、内存占用和NUMA影响。

以 LEGACY_64 为基线；不同大小候选保持相同算法、编译选项、总共享内存
预算和负载，记录实际 capacity。每组预热、重复测量并记录吞吐、P99 和
CPU；两端使用匹配构建。先通过正确性检查，再据结果冻结唯一布局。

## 18. 需要评审确认的问题

1. IPC_V2的固定slot size最终选择1KiB、4KiB还是8KiB？
2. header放在slot头部还是尾部？
3. Consumer cursor是否完全移到本地`UbrRx`？
4. 32位state的原子访问方式是否满足目标平台的跨进程要求？
5. 是否确认mixed IPC/UBS直接fallback，而不尝试`LEGACY_64`？
6. 是否确认shared tail作为空间预检的权威来源，slot state只承担发布、
   回收和覆盖前校验？
7. Phase 2合入后的最小握手期abort接口应放在`UBRing`还是更底层的trx资源
   管理层？
8. 是否确认timer在成功selection/ACK前创建、由未连接状态门控，并在commit
   时只发布不应失败的状态？
9. Client是否可以直接使用Server Base Hello中的`len`映射remote shm？
10. Legacy实现的机械提取是否应单独成为不改变行为的准备commit？
11. PR2是否需要拆成多个社区PR，还是保持一个PR、多个可独立review的commit？
12. 同一连接的ADD/MOD/REMOVE是否始终使用同一poller group？
13. IPC_V2是否完全不使用`io_id/ep_eof_pos`，还是仅把它们作为不影响正确性
    的性能提示？
14. 是否采用9.5的初始化职责，以及共享字段发布和部分初始化失败清理如何
    落实？
15. reserved非零值的接收策略，以及header对齐和原子访问实现如何固定？

## 19. 评审通过标准

在开始完整数据路径实现前，至少应确认：

- 同一个format id在所有节点上唯一决定相同layout；
- IPC_V2不会被UBS选择；
- Server不再在format确认前按Legacy初始化data queue；
- Producer发布READY前payload和metadata已经完成；
- Consumer观察READY后能够安全读取payload；
- Consumer归还EMPTY后Producer才能覆盖slot；
- partial read、ring wrap和queue full行为已经定义；
- 单 producer/consumer 的入口约束和 poller group 归属已经确认；
- tail、slot state 与可读/可写判断一致，通知不会遗漏已发布的数据；
- 初始化职责唯一，成功 selection/ACK 后不会再清零对端可能使用的队列；
- 部分初始化失败、握手 I/O 失败和建连后损坏的退出边界明确；
- Legacy路径可以保持现有行为并独立回归。
