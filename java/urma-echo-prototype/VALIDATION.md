# 本次交付的验证状态

日期：2026-10-20。源代码交付，不包含编译产物或真实性能数据。

## 本轮审阅修复

- EchoClient/EchoServer：将 Java 线程创建、配置和启动纳入 native 对象失败回滚，保留原始启动异常。
- scripts/test.sh：要求 CTest 正常退出且直方图、Java 契约测试都输出各自成功标记，避免零测试误报 PASS。
- ContractTest：新增客户端存在在途请求时关闭，检查 Future 的关闭错误、关闭线程结束及后续客户端仍可使用服务端。
- tests/test_runner.py：新增 6 项替身 CTest 的脚本回归用例，仅在 Linux/Bash 下执行；这些不是 JNI/TCP 测试。

以上修改已做静态复核，不表示 Java/native 代码已编译，或启动异常注入、真实关闭竞争已验证。

## 已实际执行

- `python -m unittest discover -s tests -v`：9 项结果比较测试通过。
- 本轮 `python -B -m unittest discover -s tests -v`：9 项结果比较测试通过；6 项脚本回归用例在本地 Windows 明确跳过，尚未执行通过。
- 全部交付文本可按 UTF-8 解码，未发现二进制夹带；Shell 源码为 LF 换行。
- NativeBridge 声明与 C++ JNI 导出符号名集合一致，共 11 项。这不是 ABI 或运行验证。
- 人工检查调用上下文、异步 done、注册表删除、关闭唤醒、消息复制和结果口径。
- 最初源代码交付时，原型目录不在 Git 仓库中。本目录由该交付复制并调整仓库内操作说明；这不是 Git 提交、上传或服务器验证的证明。
- 本次拟入库起点为 BGQ99/brpc 的 8252e8ec54da4db3b836d348151d757ba7fab647；仅静态核对了 socket_mode 等所用接口，未实际编译。

## 未通过环境门槛，未计为验证成功

- 本地 Windows 环境找不到 JDK 和 C++/CMake 工具，未执行 javac 或 native 编译。
- 本地 WSL 查询被拒绝（E_ACCESSDENIED）。
- 尝试 Git Bash 的 `bash -n scripts/smoke.sh` 时，Bash 初始化被系统访问控制拒绝；未完成 Shell 语法执行检查。

未安装依赖、修改系统配置或请求绕过访问控制。以上限制不代表源码已被证明能编译。

## 目标服务器上仍需执行

1. `bash scripts/build.sh`：实际 javac、protoc、C++ 编译及链接。
2. `bash scripts/test.sh`：C++ 直方图与 Java/TCP JNI 契约测试。
3. `bash scripts/smoke.sh`：本机 TCP N/C/S/J 四组及关闭检查。
4. 两节点 URMA 功能与真实数据路径证明，检查 mock/TCP 回退。
5. 各组闭环测量、重复运行、环境/服务端资源记录和损耗分析。

## 尚未实现的测量能力

- 固定到达率负载和其对应的延迟比较。
- 按统一 SLO 自动搜索最大稳定吞吐。
- 自动跨轮统计和自动对齐的服务端 CPU/GC/资源采集。

当前代码仅提供最小原型、闭环测量入口和比较工具。不能把本次交付称为完整方案验证完成，也不能把文中的 PASS 示例当作实际服务器结果。
