# 先做这一步

这是一份待在目标 Linux 服务器上编译和验证的源代码。原型位于仓库的 java/urma-echo-prototype/，使用独立构建入口，第一步只构建新增原型。BRPC_ROOT 指向要复用的已有 bRPC 头文件和共享库。

服务器需要已有 JDK（javac/jni.h）、CMake、C++、Protobuf/protoc、gflags，以及匹配的 bRPC 头文件和 libbrpc.so。此前的 JDK 软件源 504 问题没有被本工程解决；不要把源码交付当作环境已经就绪。

```bash
# 从本仓库根目录开始
cd java/urma-echo-prototype
export BRPC_ROOT="/服务器上已有brpc的实际位置"
bash scripts/build.sh
```

不清楚路径时，不要原样执行占位路径。可以先检查系统是否已安装 bRPC，或参考 README 的显式 BRPC_INCLUDE_DIR/BRPC_LIBRARY 配置。只有静态库时脚本会停下，不重建或修改原有工程。

构建出现 BUILD=PASS 后，再执行：

```bash
bash scripts/test.sh
bash scripts/smoke.sh
```

这两个测试只使用本机 TCP。它们通过后，按 README 选择两节点真实 URMA 运行；不要直接把闭环输出称为已确认的 URMA 损耗结论。

第一轮反馈只需给出阶段和结果：

- BUILD=PASS，或构建卡在依赖查找/Java 编译/C++ 编译/链接哪一步。
- TCP_CONTRACT=PASS 或失败检查名称。
- TCP_FOUR_GROUP_SMOKE=PASS 或失败组别 N/C/S/J。

完整错误信息可保留在服务器本地。若反馈少量错误，请保留错误类型和符号名，地址/账号/路径可以脱敏。
