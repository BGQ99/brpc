# Java / JNI / C++ bRPC / URMA Echo 原型

对应《基线与额外开销验证实施方案》v0.2。业务接口使用 Java byte[]，C++ 统一处理 Protobuf 与 RPC；客户端是异步 JNI 提交/Java 主动领取完成事件，服务端是 Java 主动领取请求并执行 Java Echo。native bthread 不调用 JVM。

**当前是待在目标服务器编译和验证的源代码，不是已经通过运行验证的 SDK。** 打包前已执行的检查详见 VALIDATION.md。没有在此处测得 Java 损耗或证明真实 URMA 数据路径。

## 1. 交付内容

| 路径 | 内容 |
|---|---|
| proto/echo.proto | 同一份 Echo 请求、响应、服务定义 |
| native/core.* | N/C/S/J 共用的 bRPC 客户端与服务端核心 |
| native/bridge.cpp | JNI、受管理句柄、事件领取与响应提交 |
| native/native_client.cpp、native_server.cpp | 纯 C++ 对照入口 |
| java/prototype/echo | Java 异步客户端、Java 服务端、EchoService、测量及契约测试 |
| CMakeLists.txt | 只编译本原型，复用已有 libbrpc.so |
| scripts | 构建、运行、TCP 测试与环境记录 |
| tools/compare.py | 比较一对匹配的闭环运行结果 |
| tests/test_compare.py | 结果计算和比较口径的单元测试 |
| tests/test_runner.py | Linux/Bash 下使用替身 CTest 验证脚本不会误报 PASS，不执行真实 JNI/TCP |

不修改 bRPC 或既有 Benchmark，不自动下载/安装依赖，不调整系统代理、驱动、证书或软件源。运行产生的文件放在原型自己的 build/、results/ 中。仓库内位置为 java/urma-echo-prototype/，其构建入口独立于仓库根目录；不会自动加入 bRPC 主工程构建。

## 2. 依赖及已有 bRPC 的选择

目标平台：Linux，当前验证目标为 openEuler 24.03 LTS SP4/aarch64。需要已有 JDK 8+（javac、jni.h）、CMake 3.16+、C++ 编译器、Protobuf 开发库及匹配 protoc、gflags，以及 ABI 匹配的 bRPC 头文件和共享库。较新的 Protobuf 需要其兼容的 C++ 标准和依赖；以服务器实际构建结果为准。

第一版只支持复用共享 libbrpc.so。若只有 libbrpc.a，构建会停止；静态库是否 PIC、额外依赖和 JNI 动态链接方式需要另行确认，不会自动重建 bRPC。启动时动态库的所有依赖也必须可由加载器找到。

BRPC_ROOT 可以是已有源码/构建根目录或安装前缀。会优先查找常见 output/include、output/lib、include、lib、build/src 等位置；没有命中时显式传入实际头文件和共享库。不要填写示例中的占位路径。

```bash
# 在仓库根目录进入原型；填写服务器上已有 bRPC 的实际位置。
cd java/urma-echo-prototype
export BRPC_ROOT="/实际位置/brpc"
bash scripts/build.sh
```

如果自动查找失败：

```bash
bash scripts/build.sh \
  -DBRPC_INCLUDE_DIR="/实际位置/include" \
  -DBRPC_LIBRARY="/实际位置/libbrpc.so"
```

若已有 bRPC 还需要额外生成头文件目录，可通过 -DBRPC_EXTRA_INCLUDE_DIRS="/实际位置/生成目录" 提供。不能混用不同 bRPC/Protobuf 版本的头文件和库。检查构建输出中的 Existing bRPC library/headers，以及构建后的依赖记录。

build.sh 只在其子进程环境中推导未设置的 JAVA_HOME，不改 shell 启动文件。BUILD=PASS 仅表示此次编译命令完成，不表示测试通过。

需要显式确认首轮功能工作时，可以使用 --transport=tcp。它不要求真实 URMA 通路，但不等于已有 bRPC 的其他运行依赖可以缺失。若要以 TCP-only 原型构建，可加 -DECHO_ENABLE_URMA=OFF。

## 3. 先运行本机 TCP 契约测试和四组冒烟

构建成功后：

```bash
bash scripts/test.sh
bash scripts/smoke.sh
```

test.sh 包含 C++ 直方图检查及带 -Xcheck:jni 的 Java/TCP 契约测试，覆盖二进制/空消息、并发、容量/重复 ID、Java 业务异常、超时后的迟到响应和复用、在途服务端关闭、在途客户端关闭及服务端继续可用、客户端关闭后拒绝请求。脚本同时检查 CTest 退出码及两项测试各自的成功标记；零测试、缺少测试、禁用测试或测试失败均不能据此输出 TCP_CONTRACT=PASS。该检查兼容 CMake 3.16，不依赖较新版本的 --no-tests=error。

smoke.sh 自动启动并关闭它自己的本机测试子进程，分别跑 N/C/S/J，默认端口 18971，可用 ECHO_TEST_PORT 更换。旧结果保留，每次创建新的 results/tcp-smoke.* 目录。正常关闭失败时会终止自己的测试子进程，并返回失败，不会据此输出 PASS。

全部成功时分别输出 TCP_CONTRACT=PASS 和 TCP_FOUR_GROUP_SMOKE=PASS。它们均不代表 URMA 验证，也不能代替所有关闭/断连/资源压力场景。

有 Python 3 时，可在原型目录运行 `python3 -B -m unittest discover -s tests -v`：比较工具测试不需要 native 构建；脚本回归用例仅在 Linux/Bash 下运行，使用替身 CTest。其他平台会明确跳过这些脚本用例，不能把跳过或替身测试通过当作实际 TCP 契约通过。

## 4. 手工查看 Java 两端的业务链路

终端 A：

```bash
bash scripts/run-server.sh java --port=8000 --transport=tcp --workers=4 --capacity=128
```

终端 B：

```bash
bash scripts/run-client.sh java --group=J --server=127.0.0.1:8000 \
  --transport=tcp --size=1024 --concurrency=1 --warmup=0 --seconds=1
```

响应使用原样二进制 Echo，不拼接字符串前缀。程序检查 sequence、长度和内容；结果为一行 JSON，success 大于 0、failures 为 0 且客户端退出码为 0 才表示这次运行通过。

SERVER_READY 只表示服务已启动；path_verified=false 会始终保留。Ctrl-C 可关闭手工启动的服务端。

## 5. URMA 前置检查和四组运行

先确认两台机器可通过 TCP 控制面及实际 URMA 数据面通信，且 bRPC 使用真实 SDK/provider/驱动组合。在已有 bRPC 构建中排除 mock；所参考提交有 WITH_URMA=ON、URMA_USE_MOCK=OFF 等选项，实际版本需要重新核对。

原型的 ECHO_ENABLE_URMA=ON、发现 urma_device 标志或 ldd 中有 liburma，都不能单独证明没有 mock 或 TCP 回退。必须保留匹配测试时段的协商状态、真实设备收发/完成证据，以及已有 bRPC 的实际库选择信息。该原型不提供自动数据路径证明，报告也不会自动将 path_verified 改成 true。

四组角色：

| 组别 | 服务端角色 | 客户端角色 |
|---|---|---|
| N | native | native |
| C | native | java |
| S | java | native |
| J | java | java |

建议在两节点上运行。下例中替换服务端地址和实际设备名；不同节点设备名可以不同。--device 可以省略，届时采用已有 bRPC 的默认设备选择，但应记录实际选中的设备。

N/C 使用同一个 native 服务端：

```bash
# 服务端节点
bash scripts/run-server.sh native --port=8000 --transport=urma --device=实际设备名

# 客户端节点，分别运行并保存结果
mkdir -p results
bash scripts/run-client.sh native --group=N --server=服务端地址:8000 \
  --transport=urma --device=实际设备名 --size=1024 --concurrency=1 \
  --warmup=60 --seconds=180 > results/N-1k-c1-r1.json

bash scripts/run-client.sh java --group=C --server=服务端地址:8000 \
  --transport=urma --device=实际设备名 --size=1024 --concurrency=1 \
  --warmup=60 --seconds=180 > results/C-1k-c1-r1.json
```

停止 native 服务端后启动 Java 服务端：

```bash
# 服务端节点
bash scripts/run-server.sh java --port=8000 --transport=urma --device=实际设备名 \
  --workers=4 --capacity=128

# 客户端节点
bash scripts/run-client.sh native --group=S --server=服务端地址:8000 \
  --transport=urma --device=实际设备名 --size=1024 --concurrency=1 \
  --warmup=60 --seconds=180 > results/S-1k-c1-r1.json

bash scripts/run-client.sh java --group=J --server=服务端地址:8000 \
  --transport=urma --device=实际设备名 --size=1024 --concurrency=1 \
  --warmup=60 --seconds=180 > results/J-1k-c1-r1.json
```

先测 1 KiB/并发 1、64 KiB/并发 32。通过后扩展 64 B、1 KiB、8 KiB、64 KiB、1 MiB，配合并发 1/8/32，每点至少重复 3 次。改变结果文件名保存各轮，避免覆盖。容量参数必须大于等于客户端并发，server capacity 覆盖整机总在途请求。

--polling=false 是默认事件模式，--polling=true 使用已有 bRPC 的 URMA 轮询模式。四组同一比较点保持相同模式。超时默认 5000 ms，可用 --timeout-ms 修改；固定单连接，关闭自动重试、备份请求和压缩。

同一客户端节点用同一构建产物跑四组。JSON build_id 覆盖原型源码、bRPC 共享库摘要和部分构建配置；它不覆盖所有传递依赖、硬件状态或服务端配置，仍需另存环境记录和服务端角色。

## 6. 数据口径及比较

当前入口支持闭环并发负载。每次请求计时从提交前开始，到结果交付并完成内容校验结束；Java 计入 Future 与基准程序完成队列。计时在客户端完成，不跨机减时钟。每阶段尾部在途请求会被排空，elapsed_ns 和 QPS 包含排空时间。

P50/P95/P99 是成功请求延迟的对数直方图桶上界，使用每个 2 的幂区间 64 个细分桶，桶相对宽度至多约 1.5625%；并非精确分位数。Java/C++ 使用同一算法和固定内存大小，错误/超时单独计数。

客户端 CPU 是测量区间进程 CPU 时间，包括该进程其他线程及错误请求消耗；不代表服务端或整条链路总 CPU。服务端 CPU、GC、内存/注册资源和线程应另行采集并对齐区间。预热时间到期不自动证明 JIT/GC 已稳定。

```bash
python3 tools/compare.py --baseline results/N-1k-c1-r1.json \
  --candidate results/J-1k-c1-r1.json
```

工具要求构建标识、参数和负载模型匹配，保留正负差值、失败计数及缺失指标。它输出的是一对闭环结果的描述性差值，不自动宣称固定负载延迟损耗、满足 SLO 的最大吞吐损失、真实 URMA 路径或业务验收通过。

**尚未实现的测量：固定到达率负载、按同一延迟/错误率阈值寻找最大稳定吞吐、自动跨轮统计、自动服务端 CPU/GC/资源区间采集。** 原型源码交付或闭环曲线不表示实施方案的全部验证已完成。

## 7. 生命周期和复制边界

- 客户端进入 JNI 时把 byte[] 复制到 native 字符串，再移动给 C++ Protobuf payload；异步 Controller/请求/响应由调用上下文持有到 bRPC done。
- native 完成队列持有结果；Java 领取时分配 byte[] 并复制。未来可优化，但本轮必须记录它的成本。
- Java 服务端额外将 C++ Protobuf payload 复制到独立上下文，领取时复制到 Java；Java 返回时再复制到 native 并移动到响应。独立上下文避免关闭与交付并发时依赖已释放的 Protobuf 对象。
- registry 使用不重复的单调句柄与调用 ID，阻止关闭后的访问；客户端和服务端数据表分别管理。它不是 public 原生指针 API。
- 超时由 bRPC 产生终态；不能据此立即回收远端 Java 服务端上下文，迟到完成或服务端关闭仍需处理它。
- 客户端显式关闭先使 Java Future 失败，再等待有限超时的 native 请求结束并释放。服务端关闭会让仍在途的请求失败，正常 Echo worker 会响应中断；任意 Java 业务忽略中断时，不承诺强制终止其代码。
- Java 线程创建、setDaemon 或 start 失败时会回滚已创建的 native 对象；服务端也会关闭已创建的业务线程池。清理失败作为 suppressed 异常附在原始启动异常上，避免覆盖最初的失败原因。
- Future.cancel 只取消本地 Future，不承诺中止网络访问或远端执行。客户端非 Async 的 CompletableFuture 回调可能在完成线程运行，应保持短小或使用带执行器的 Async 回调。
- 不做 JNI critical array 长期固定、DirectByteBuffer 零拷贝、native bthread → JVM 回调或 usercode_in_pthread 模式切换。

## 8. 实际部署前需核对

构建后可以本地保存环境记录，无需发送服务器路径：

```bash
mkdir -p results
bash scripts/collect-env.sh > results/environment.txt
```

检查实际 libbrpc、Protobuf、gflags 及依赖来源。命中同名共享库不保证 ABI、URMA SDK 或 provider 兼容；ldd 有 not found 时先解决依赖。若只有静态 bRPC 或需要新安装依赖，应另行确认，不修改原有 Benchmark 来迁就原型。

最初接口分析参考 Apache bRPC b4e149f8190a7d32b03580b98e8a642706fc1eac。本次入库起点为 BGQ99/brpc 的 8252e8ec54da4db3b836d348151d757ba7fab647；已静态核对该提交的 ChannelOptions/ServerOptions 具有 socket_mode 字段，尚未进行完整编译兼容性验证。服务器实际构建版本仍需记录。此工程没有下载、内嵌或编译 bRPC/URMA SDK。新建的是独立原型和最小 C++ 对照入口，用来保证相同协议和业务，而不是重建原有 Benchmark。

上游依据：

- [bRPC URMA 实现与构建说明](https://github.com/apache/brpc/blob/b4e149f8190a7d32b03580b98e8a642706fc1eac/docs/cn/urma.md)
- [bRPC pthread 模式说明](https://github.com/apache/brpc/blob/b4e149f8190a7d32b03580b98e8a642706fc1eac/docs/cn/server.md)
- [JNI 线程规范](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/invocation.html)
