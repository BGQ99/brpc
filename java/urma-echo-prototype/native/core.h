#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <brpc/channel.h>
#include <brpc/server.h>
#include "echo.pb.h"

namespace echo_proto {

constexpr size_t kMaxPayload = 16 * 1024 * 1024;

struct Completion {
    uint64_t id = 0;
    uint64_t sequence = 0;
    std::string payload;
    std::string error;
};

// Shared by the pure C++ benchmark and the JNI client. No JNI in this core.
class ClientCore : public std::enable_shared_from_this<ClientCore> {
public:
    static std::shared_ptr<ClientCore> Create(const std::string& address,
                                             const std::string& transport,
                                             int timeout_ms, size_t capacity);
    bool Submit(uint64_t id, std::string payload, std::string* error);
    bool Take(Completion* result, int wait_ms);
    void Shutdown();
private:
    struct Call;
    explicit ClientCore(size_t capacity) : capacity_(capacity) {}
    static void Finish(Call* call);
    brpc::Channel channel_;
    std::mutex mutex_;
    std::condition_variable ready_, drained_;
    std::deque<Completion> completed_;
    std::unordered_set<uint64_t> reserved_;
    size_t active_ = 0;
    const size_t capacity_;
    bool closing_ = false;
};

struct ServerRequest {
    uint64_t id = 0;
    uint64_t sequence = 0;
    std::string payload;
};

class ServerCore {
public:
    ServerCore(bool java_business, size_t capacity);
    ~ServerCore();
    void Start(int port, const std::string& transport);
    bool Take(ServerRequest* result, int wait_ms);
    bool Complete(uint64_t id, std::string payload, const std::string& error);
    void Shutdown();
private:
    struct Context;
    class Service;
    void Receive(google::protobuf::RpcController* controller,
                 const benchmark::EchoRequest* request,
                 benchmark::EchoResponse* response,
                 google::protobuf::Closure* done);
    static void FinishContext(const std::shared_ptr<Context>& context,
                              std::string payload, const std::string& error);
    const bool java_business_;
    const size_t capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::unordered_map<uint64_t, std::shared_ptr<Context>> contexts_;
    std::deque<std::shared_ptr<Context>> pending_;
    uint64_t next_id_ = 1;
    bool closing_ = false;
    bool started_ = false;
    // Destruction order matters: Server is destroyed before its service.
    std::unique_ptr<Service> service_;
    brpc::Server server_;
};

void ConfigureUrma(const std::string& device, bool polling);
bool HasUrma();

}  // namespace echo_proto
