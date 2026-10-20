# Java 接入 C++ bRPC/URMA：基线与额外开销验证实施方案

版本：v0.2  
日期：2026-10-20  
状态：本文保留 v0.2 设计基线；当前已有待编译及运行验证的原型源码。实际已实现能力、测试结果及缺口见 README.md 和 VALIDATION.md，不能将本文全部要求视为已完成。

## 1. 已确定的目标和交付顺序

业务代码使用 Java，底层通过 C++ bRPC 使用 URMA。本轮明确测量：

> 相同 bRPC/URMA 通信条件下，Java/JNI 接入相对纯 C++ 实现增加多少延迟、吞吐损失和 CPU 成本？

先完成方案 1 的原型与测试，再携带设计、运行记录和结果请同事评估。方案 2（Java RPC 框架直接经 JNI 接 URMA）保留为后续路线，本轮不开发，也不宣称方案 1 的性能优于方案 2。

本轮交付五项：固定 Echo 协议、纯 C++ 基线、Java/JNI 客户端与服务端桥接、可重复运行的测试脚本、包含损耗和限制的报告。环境检查和 RPC 跑通只是前置条件，不能替代损耗测量。

## 2. 第一版明确采用的设计

| 决定 | 第一版实现 |
|---|---|
| RPC/传输 | 复用 C++ bRPC 和既有 URMA 实现，使用 baidu_std |
| Java 数据接口 | 固定 byte[] 有效载荷，Java 业务只做 Echo |
| Protobuf | 各组统一由 C++ 构造、解析和序列化 |
| 调用模型 | native 异步 RPC，Java 通过 Future 获得结果 |
| 线程边界 | Java 平台线程通过 JNI 主动领取请求/完成事件，native bthread 只操作 native 数据 |
| 缓冲区 | Java/native 边界显式复制；native 数据保留到异步引用结束 |
| 队列 | 请求/完成队列有界，使用等待与通知，无固定 sleep 拉取 |
| 功能范围 | 固定 Echo、并发、超时、异常、断连与受控关闭 |
| 优化策略 | 首版不做零拷贝、批量 JNI、通用方法注册或调度器修改 |
| 比较范围 | 四组均用真实 URMA；本轮不测 URMA 相对 TCP 的收益 |

这是具体桥接实现的基线，不是 JNI 理论最低成本，也不是完整 Java SDK 的最终性能。

### 2.1 消息与业务

所有程序使用同一份协议文件，建议如下：

```protobuf
syntax = "proto2";
package benchmark;
option cc_generic_services = true;

message EchoRequest {
    required uint64 sequence = 1;
    required bytes payload = 2;
}

message EchoResponse {
    required uint64 sequence = 1;
    required bytes payload = 2;
}

service EchoService {
    rpc Echo(EchoRequest) returns (EchoResponse);
}
```

Java 端接收和返回 byte[]。C++ bRPC 服务端保留 sequence 等协议元信息，桥接只将业务 payload 交给 Java；客户端校验返回的序号、有效载荷长度与内容。C++ Echo 与 Java Echo 都只原样返回相同有效载荷，不增加其他业务。

请求数据在测量区间外生成；相同测试点复用不可变的输入数据。JNI 提交返回前完成向 native 的复制，因此异步发送不依赖 Java 数组地址保持不变。

### 2.2 复用同一份 C++ 通信代码

纯 C++ 客户端和 JNI 客户端复用同一个 C++ EchoClient 核心，统一 Channel 配置、请求构造、Protobuf 编解码和异步调用。服务端复用相同生成的 EchoService，通过实现选择执行 C++ Echo，或把 payload 转交 Java Echo。

因此第一轮不需要让 Java 同时承担 Protobuf 编解码，也不引入不同 RPC 框架作为对照。若未来采用 Java Protobuf、通用对象 API 或 brpc-java，需要另外验证，不能把本轮结果直接套用。

### 2.3 客户端路径

```text
Java echoAsync(payload)
  → JNI 复制 payload，建立 native 调用上下文
  → 共用 C++ EchoClient 构造 Protobuf 并提交异步 bRPC
  → URMA 发出请求、接收响应
  → bRPC 完成回调将 native 结果入队
  → Java 完成线程经 JNI 领取结果，复制响应到 Java
  → 完成 Future，调用方校验序号与 payload
```

完成线程只负责交付结果。用户回调不能长期占用它；测试程序的响应校验与统计方式需保持有界并明确记录。队列等待计入端到端延迟。

### 2.4 服务端路径

```text
URMA 请求 → C++ bRPC 解析 Protobuf
  → 保存 controller/request/response/done 的调用关系，请求入队
  → Java 分发线程通过 JNI 领取 payload
  → Java 业务线程池执行 Echo
  → JNI 复制返回数据并提交响应
  → C++ 填充 EchoResponse 并调用 done
  → bRPC/URMA 发送响应
```

服务端 CallMethod 入队后异步返回，直到 Java 交付结果或 native 端完成错误处理时才结束该调用。上下文必须覆盖异步生命周期，done 只能执行一次。

Java 客户端和 Java 服务端均由 JVM 启动，通过 System.loadLibrary 加载桥接库。第一版不在 native 进程内创建另一个 JVM，也不在普通 bthread 栈上执行 JNI 回调。

### 2.5 桥接接口与所有权

以下是待实现的接口职责，不是已有 API：

| 接口职责 | 线程和数据行为 |
|---|---|
| 创建客户端/启动服务端 | Java 调用 JNI，创建 native 对象并返回受管理句柄 |
| submitClient | 接收调用 ID、sequence、byte[] 和超时；复制数据后提交异步 RPC |
| takeClientCompletion | Java 完成线程进入 JNI，等待通知并领取一个结果；支持关闭唤醒 |
| takeServerRequest | Java 分发线程进入 JNI，等待通知并领取一个请求及调用 ID |
| completeServerRequest | Java 处理结果或异常交给 JNI，填写响应或设置错误并完成调用 |
| closeClient/closeServer | 停止入场、唤醒等待线程、收敛在途调用并释放资源 |

句柄及调用 ID 通过注册表和代数校验管理，不直接让 Java 操作裸指针。Java Future 表由 Java 管理，native 上下文表由 C++ 管理；native bthread 不保存或跨线程使用 JNIEnv。队列项持有 native 上下文的有效引用，不能依赖栈上对象。

在途请求上限与完成容量在接收请求前关联预留。资源不足时拒绝新请求并返回可识别错误，不允许静默丢弃完成事件或无界增长。

## 3. 损耗的准确含义

本轮报告应命名为：

> 固定字节消息接口下，Java 业务接入 C++ bRPC/URMA 的额外开销。

包括 Java/native 数据复制、JNI、Java 与 native 线程交接、队列等待、Future 管理、Java Echo 执行和 JVM 运行时影响；各组共用 C++ Protobuf 和 bRPC 通信路径。

不包括 Java Protobuf 编解码、Spring、服务发现、业务对象映射等通用 Java SDK 能力。不把端到端差值全部归因于 JNI 函数调用，也不假定该实现达到性能上限。

## 4. 四组对照与结论范围

四组均使用相同硬件、协议、消息、连接数和 bRPC/URMA 参数：

| 组别 | 客户端 | 服务端业务 | 结论用途 |
|---|---|---|---|
| N | 纯 C++，共用 EchoClient | C++ Echo | native 基线 |
| C | Java/JNI，共用 EchoClient | C++ Echo | 客户端接入损耗 |
| S | 纯 C++，共用 EchoClient | Java/JNI Echo | 服务端接入损耗 |
| J | Java/JNI，共用 EchoClient | Java/JNI Echo | Java 两端的完整接入损耗 |

执行顺序固定为 N → C → S → J。四组都属于本轮计划；可以先形成 N/C 阶段结果，但只有测完 J 才能报告 Java 两端结果。C、S 用于定位，不能将其 P99 增幅或百分比直接相加预测 J。

单机两进程可做功能冒烟；正式跨节点结论需要两个可达的 URMA 节点。只有一个节点时，结果必须标注实际拓扑，跨节点工作保持待完成。

## 5. 正确性通过后才进入性能测试

| 检查 | 必须满足的行为 |
|---|---|
| 响应正确 | 序号、长度和内容匹配，未知服务/非法数据有明确错误 |
| 响应与超时竞争 | 一个 Java 调用只交付一个终态 |
| 迟到完成 | Future 已结束后，仍保留并最终回收底层需要的资源 |
| Java 业务异常 | 转换为明确 RPC 失败，不能遗留调用上下文 |
| 队列耗尽 | 入场控制生效，无丢失完成事件、无无界积压 |
| 断连与重连 | 相关请求以明确结果结束，新连接能继续工作 |
| 关闭 | 停止新请求、唤醒等待线程并收敛在途请求，无重复 done 或悬空访问 |

调用终态与资源释放分开管理；超时或取消不保证远端业务没有执行。第一轮关闭自动重试和备份请求，不提供业务恰好执行一次保证。

native 错误只在可恢复范围内映射为 Java 错误；native 崩溃仍可能终止 JVM。本轮采用显式关闭，不以 GC 触发释放作为正确性前提。

## 6. 固定实验配置与执行步骤

### 6.1 版本与真实数据路径

1. 确认服务器 JDK、编译器、CMake、Protobuf、bRPC commit、URMA SDK/provider/驱动及设备权限，记录实际版本。
2. 固定 Release 构建及 native 依赖；JNI 动态库与其链接对象的构建方式保持一致。
3. 在本次核实的 bRPC 版本中，使用 WITH_URMA=ON、URMA_USE_MOCK=OFF 排除 mock，并核对实际链接。版本变化时重新检查开关含义。
4. N 组先跑通，并保留设备、握手状态及与本次流量相关的 URMA 收发证据。
5. 每组确认数据路径。初始化日志、use_urma=true 或 RPC 成功都不足以单独证明实际使用 URMA。发生 mock/TCP 回退的运行不计入 URMA 对照结果。

### 6.2 首轮运行参数

以下作为本轮实验配置；如因设备或实现限制需要调整，四组同步调整并记录原因。

| 参数 | 配置 |
|---|---|
| payload | 64 B、1 KiB、8 KiB、64 KiB、1 MiB |
| 每客户端在途请求 | 1、8、32 |
| 连接 | 每组固定相同数量，测量前完成建链和首个 RPC |
| 协议/业务 | baidu_std，相同 Protobuf Echo |
| 压缩/重试/备份请求 | 关闭 |
| 日志 | 统一等级，关闭逐请求日志 |
| 预热 | 至少 60 秒；观察 JIT、吞吐、延迟和 GC，未稳定则延长 |
| 正式测量 | 每次 180 秒，每点至少重复 3 次 |
| JNI 批量 | 首版每次领取一个事件；批量优化另测 |

先执行 1 KiB/并发 1 和 64 KiB/并发 32 两个代表点，检查正确性、计时和路径证据，再扩展完整矩阵。

固定 Java 业务线程数、分发线程数、bRPC/URMA 线程配置与进程 CPU 预算，并记录各组总线程数。控制 NUMA、CPU 频率、其他节点负载、JVM 堆和 GC 配置。初始四组轮次完成后交替重复运行，降低顺序和环境波动的影响。

### 6.3 两种负载测试分开报告

**固定负载测延迟：**在所有组均可稳定承受的相同提供负载下比较 P50/P95/P99、实际完成 QPS 和错误率。延迟从业务提交前开始，到响应数据可用并完成校验结束，包括本地队列和交付开销。计时只在客户端完成，不跨机器相减时钟。

**递增负载测吞吐：**使用并发 1/8/32 等档位获得吞吐、延迟和错误率曲线。若最高档尚未到拐点，不称为最大吞吐；需要扩展负载时单独记录。最大稳定 QPS 的比较需采用相同延迟与错误率约束。

在途请求并发矩阵属于闭环测试，不能冒充固定到达率。固定提供负载测试需记录目标/实际发起 QPS、积压、失败和超时；若工具尚不支持，标为未完成，不能用闭环结果替代。

### 6.4 资源与分析

客户端、服务端分别记录 CPU 秒、RSS、线程数、队列长度、在途请求、GC 暂停和 native 资源变化。区分 JVM 堆、直接内存、native 与注册内存；RSS 变化不能单独证明泄漏。

性能主测记录端到端结果。定位队列、复制和 JNI 开销时另做开启诊断采样的运行，说明采样开销，不能用高频逐请求日志作为正式性能结果。

## 7. 损耗计算和报告格式

相同稳定提供负载、相同延迟统计量下：

```text
延迟增幅 = (Java 接入延迟 - N 组延迟) / N 组延迟 × 100%
```

相同延迟/错误率约束下，若已测出最大稳定吞吐：

```text
吞吐损失 = (N 组 QPS - Java 接入 QPS) / N 组 QPS × 100%
```

CPU 成本：

```text
每个成功 RPC 的 CPU 成本 = 测量区间进程 CPU 秒 / 成功 RPC 数
```

错误请求消耗的 CPU 不从区间中剔除，同时报告错误率。CPU 分客户端/服务端报告，合计另列。延迟同时列绝对差值和百分比，负差值也如实保留，并结合重复运行波动解释。

| 场景/负载模型 | 组别 | payload/并发/目标 QPS | 发起/完成 QPS | P50/P95/P99 | 错误/超时率 | 两端 CPU | 内存/GC | URMA 证据 |
|---|---|---|---|---|---|---|---|---|
| 待执行 | N | 待填 | 待填 | 待填 | 待填 | 待填 | 待填 | 待填 |
| 待执行 | C | 待填 | 待填 | 待填 | 待填 | 待填 | 待填 | 待填 |
| 待执行 | S | 待填 | 待填 | 待填 | 待填 | 待填 | 待填 | 待填 |
| 待执行 | J | 待填 | 待填 | 待填 | 待填 | 待填 | 待填 | 待填 |

报告必须回答：

1. J 相对 N 的 P50/P99 增加多少微秒、多少百分比？
2. 相同负载下客户端、服务端及合计 CPU 成本增加多少？
3. 吞吐曲线是否提前出现拐点，是否伴随排队、GC 或错误？
4. C/S 显示哪一端的接入成本更突出？哪些定位结果已经有证据？
5. 正确性、关闭、长时间资源变化是否有未解决问题？
6. 下一步优先优化复制、JNI 频率还是调度，或者重新评估接入路线？

业务 P99、吞吐、错误率和 CPU 接受阈值仍需需求方给出；它们不阻止先完成描述性测量，但阈值缺失时不宣布损耗可接受或不可接受。

## 8. 开发工作包与完成标准

下列文件/模块为拟新增内容，目前不存在对应原型交付：

| 工作包 | 拟实现内容 | 完成标准 |
|---|---|---|
| 协议与 C++ 核心 | echo.proto、共用 EchoClient、C++ Echo 服务、native 压测入口 | N 组运行正确，具备路径证据和结果输出 |
| JNI 基础 | 动态库加载、句柄注册、调用上下文、队列和关闭 | 生命周期检查通过，线程与数据所有权明确 |
| Java 客户端 | submit/Future 表、完成线程、客户端压测入口 | C 组运行和测量通过 |
| Java 服务端 | native 服务适配、请求分发、业务线程池、响应提交 | S 组运行和测量通过 |
| 完整链路 | Java 客户端连接 Java 业务服务端 | J 组通过正确性检查并完成同口径测量 |
| 测试与报告 | 统一参数脚本、原始数据、汇总表、损耗计算及限制 | 四组结果可复现，失败和未完成项完整披露 |

实施顺序：环境/版本确认 → N → JNI 基础与 C → S → J → 完整对照测量 → 结果分析 → 同事评估。

N/C 可先产出客户端阶段报告；完整报告以四组完成为准。报告提交时附本文、具体实现差异、版本与运行参数、原始结果、URMA 路径证据和未验证项，避免只发送一个损耗百分比。

## 9. 范围限制与后续决策

- 本轮测的是选定桥接实现的成本，不能证明所有 Java/JNI 实现都具有相同损耗。
- Protobuf 在 C++ 完成，不能直接外推到 Java 对象/Java Protobuf SDK。
- 固定 Echo 不能替代真实业务负载；单机结果不能替代跨节点结论。
- 本轮不实现 Spring、注册中心、多协议、完整 SDK 和零拷贝。
- 本轮不修改 bthread 调度器，不将切换线程模型或批量优化混入同一基线。
- 方案 2 没有实测结果，不做性能排名。若方案 1 的成本主要来自复制和队列，先评估定向优化；若业务约束要求 Java 框架主导 RPC，再考虑方案 2 原型。

先测方案 1 再给同事评估是本次采用的顺序。测量期间发现接口、数据路径或线程模型必须变化时，记录变更并重新审查比较口径，不能沿用旧数据而不说明。

## 10. 已核实依据与待确认环境

源码依据来自只读检查，固定快照如下；服务器实际版本仍需核对：

- Apache bRPC：b4e149f8190a7d32b03580b98e8a642706fc1eac。
- raptium/brpc-java：e4597d85bbdbded806763b2be0e8250eeeecdbe3。

C++ bRPC 已有 URMA Transport/Endpoint 与示例；该快照的 java/ 只有 TBD，不能假定有现成官方 JNI SDK。URMA 使用 TCP 控制面、支持真实库/mock 选择并有回退路径；当前实现面向 Linux、baidu_std 并限制 SSL 等配置。

bRPC 文档提示普通 bthread 栈不适合执行 JNI，JNIEnv 也只对当前线程有效。本版用 Java 平台线程主动进入 JNI；usercode_in_pthread、SerializedRequest/Response、BaiduMasterService 等替代入口不作为本轮实现。

参考：

- [C++ bRPC 仓库快照](https://github.com/apache/brpc/tree/b4e149f8190a7d32b03580b98e8a642706fc1eac)
- [URMA 构建、控制面与限制](https://github.com/apache/brpc/blob/b4e149f8190a7d32b03580b98e8a642706fc1eac/docs/cn/urma.md)
- [bRPC pthread 模式](https://github.com/apache/brpc/blob/b4e149f8190a7d32b03580b98e8a642706fc1eac/docs/cn/server.md)
- [JNI 线程规范](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/invocation.html)
- [brpc-java 的 Netty 通道接口](https://github.com/raptium/brpc-java/blob/e4597d85bbdbded806763b2be0e8250eeeecdbe3/brpc-java-communication/src/main/java/com/baidu/brpc/client/channel/BrpcChannel.java)

现有服务器信息为 openEuler 24.03 LTS SP4/aarch64，用户确认有 URMA。JDK、编译工具、SDK/provider/驱动、源码位置、设备权限及第二节点尚未完整盘点。本方案和本次文档更新均不构成实现、构建或性能验证完成。
