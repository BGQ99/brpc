#include "core.h"

#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <gflags/gflags.h>
#include <brpc/controller.h>
#include <brpc/callback.h>
#include <brpc/closure_guard.h>

namespace echo_proto {
namespace {
brpc::SocketMode Mode(const std::string& transport) {
    if (transport == "tcp") return brpc::SOCKET_MODE_TCP;
    if (transport != "urma") throw std::invalid_argument("transport must be tcp or urma");
#if ECHO_ENABLE_URMA
    if (!HasUrma()) throw std::runtime_error("loaded libbrpc has no urma_device flag; check its URMA build");
    return brpc::SOCKET_MODE_URMA;
#else
    throw std::runtime_error("prototype built with ECHO_ENABLE_URMA=OFF");
#endif
}
}

bool HasUrma() {
    gflags::CommandLineFlagInfo info;
    return gflags::GetCommandLineFlagInfo("urma_device", &info);
}

void ConfigureUrma(const std::string& device, bool polling) {
    if (!HasUrma()) {
        if (!device.empty() || polling) throw std::runtime_error("libbrpc has no URMA flags");
        return;
    }
    if (!device.empty() && gflags::SetCommandLineOption("urma_device", device.c_str()).empty()) {
        throw std::runtime_error("cannot set urma_device");
    }
    if (gflags::SetCommandLineOption("urma_use_polling", polling ? "true" : "false").empty()) {
        throw std::runtime_error("cannot set urma_use_polling");
    }
}

struct ClientCore::Call {
    std::shared_ptr<ClientCore> owner;
    uint64_t id;
    brpc::Controller controller;
    benchmark::EchoRequest request;
    benchmark::EchoResponse response;
};

std::shared_ptr<ClientCore> ClientCore::Create(const std::string& address,
                                              const std::string& transport,
                                              int timeout_ms, size_t capacity) {
    if (address.empty() || timeout_ms < 1 || timeout_ms > 600000 || capacity < 1 || capacity > 65536) {
        throw std::invalid_argument("address, timeout_ms (1..600000), or capacity (1..65536) invalid");
    }
    auto client = std::shared_ptr<ClientCore>(new ClientCore(capacity));
    brpc::ChannelOptions options;
    options.protocol = "baidu_std";
    options.connection_type = "single";
    options.socket_mode = Mode(transport);
    options.timeout_ms = timeout_ms;
    options.connect_timeout_ms = timeout_ms;
    options.max_retry = 0;
    options.backup_request_ms = -1;
    if (client->channel_.Init(address.c_str(), &options) != 0) {
        throw std::runtime_error("bRPC Channel.Init failed");
    }
    return client;
}

bool ClientCore::Submit(uint64_t id, std::string payload, std::string* error) {
    if (id == 0 || payload.size() > kMaxPayload) {
        *error = "invalid id or payload larger than 16 MiB";
        return false;
    }
    std::unique_ptr<Call> call(new Call);
    call->owner = shared_from_this();
    call->id = id;
    call->request.set_sequence(id);
    *call->request.mutable_payload() = std::move(payload);
    // Allocate the closure before admission so allocation failure reserves nothing.
    std::unique_ptr<google::protobuf::Closure> done(brpc::NewCallback(&ClientCore::Finish, call.get()));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || reserved_.size() >= capacity_ || reserved_.count(id)) {
            *error = "client closed, capacity exhausted, or duplicate call id";
            return false;
        }
        reserved_.insert(id);
        ++active_;
    }
    Call* raw = call.release();
    // May finish inline. Do not touch raw after CallMethod returns.
    channel_.CallMethod(benchmark::EchoService::descriptor()->FindMethodByName("Echo"),
                        &raw->controller, &raw->request, &raw->response, done.release());
    return true;
}

void ClientCore::Finish(Call* raw) {
    std::unique_ptr<Call> call(raw);
    auto owner = call->owner;
    Completion event;
    event.id = call->id;
    event.sequence = call->response.sequence();
    if (call->controller.Failed()) {
        event.error = call->controller.ErrorText();
        if (event.error.empty()) event.error = "RPC failed";
    } else {
        event.payload = std::move(*call->response.mutable_payload());
    }
    {
        std::lock_guard<std::mutex> lock(owner->mutex_);
        if (!owner->closing_) {
            // Admission reserves one terminal-event slot per outstanding call.
            owner->completed_.push_back(std::move(event));
        } else {
            owner->reserved_.erase(call->id);
        }
        --owner->active_;
    }
    owner->ready_.notify_one();
    owner->drained_.notify_all();
}

bool ClientCore::Take(Completion* result, int wait_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                    [this] { return closing_ || !completed_.empty(); });
    if (completed_.empty()) return false;
    *result = std::move(completed_.front());
    completed_.pop_front();
    reserved_.erase(result->id);
    return true;
}

void ClientCore::Shutdown() {
    std::unique_lock<std::mutex> lock(mutex_);
    closing_ = true;
    ready_.notify_all();
    // Every call has a finite bRPC timeout. No controller is freed before done.
    drained_.wait(lock, [this] { return active_ == 0; });
    completed_.clear();
    reserved_.clear();
}

struct ServerCore::Context {
    uint64_t id;
    uint64_t sequence;
    std::string payload;  // Independent of protobuf lifetime, including shutdown.
    brpc::Controller* controller;
    benchmark::EchoResponse* response;
    google::protobuf::Closure* done;
};

class ServerCore::Service : public benchmark::EchoService {
public:
    explicit Service(ServerCore* owner) : owner_(owner) {}
    void Echo(google::protobuf::RpcController* controller,
              const benchmark::EchoRequest* request,
              benchmark::EchoResponse* response,
              google::protobuf::Closure* done) override {
        owner_->Receive(controller, request, response, done);
    }
private:
    ServerCore* owner_;
};

ServerCore::ServerCore(bool java_business, size_t capacity)
    : java_business_(java_business), capacity_(capacity), service_(new Service(this)) {
    if (capacity < 1 || capacity > 65536) throw std::invalid_argument("server capacity must be 1..65536");
}

ServerCore::~ServerCore() { Shutdown(); }

void ServerCore::Start(int port, const std::string& transport) {
    if (port < 1 || port > 65535) throw std::invalid_argument("port must be 1..65535");
    if (server_.AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        throw std::runtime_error("Server.AddService failed");
    }
    brpc::ServerOptions options;
    options.socket_mode = Mode(transport);
    if (server_.Start(port, &options) != 0) throw std::runtime_error("Server.Start failed");
    started_ = true;
}

void ServerCore::Receive(google::protobuf::RpcController* controller,
                          const benchmark::EchoRequest* request,
                          benchmark::EchoResponse* response,
                          google::protobuf::Closure* done) {
    auto* cntl = static_cast<brpc::Controller*>(controller);
    brpc::ClosureGuard guard(done);
    try {
    if (request->payload().size() > kMaxPayload) {
        cntl->SetFailed(EINVAL, "payload exceeds 16 MiB");
        return;
    }
    if (!java_business_) {
        response->set_sequence(request->sequence());
        response->set_payload(request->payload());
        return;
    }
    auto context = std::make_shared<Context>();
    context->sequence = request->sequence();
    context->payload = request->payload();
    context->controller = cntl;
    context->response = response;
    context->done = done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || contexts_.size() >= capacity_) {
            cntl->SetFailed(EBUSY, "Java server is closing or its admission capacity is exhausted");
            return;
        }
        if (next_id_ == 0 || next_id_ > static_cast<uint64_t>(INT64_MAX)) {
            cntl->SetFailed(EOVERFLOW, "server request id space exhausted");
            return;
        }
        context->id = next_id_++;
        contexts_.emplace(context->id, context);
        try { pending_.push_back(context); }
        catch (...) {
            contexts_.erase(context->id);
            throw;
        }
        guard.release();  // Context remains owned until Complete or Shutdown.
    }
    ready_.notify_one();
    } catch (const std::exception&) {
        cntl->SetFailed(ENOMEM, "cannot prepare Echo request context");
    }
}

bool ServerCore::Take(ServerRequest* result, int wait_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                    [this] { return closing_ || !pending_.empty(); });
    if (closing_ || pending_.empty()) return false;
    auto context = pending_.front();
    pending_.pop_front();
    result->id = context->id;
    result->sequence = context->sequence;
    result->payload = std::move(context->payload);
    return true;
}

void ServerCore::FinishContext(const std::shared_ptr<Context>& context,
                                std::string payload, const std::string& error) {
    brpc::ClosureGuard guard(context->done);
    if (!error.empty()) {
        context->controller->SetFailed(EIO, "%s", error.c_str());
        return;
    }
    context->response->set_sequence(context->sequence);
    *context->response->mutable_payload() = std::move(payload);
}

bool ServerCore::Complete(uint64_t id, std::string payload, const std::string& error) {
    if (payload.size() > kMaxPayload) throw std::invalid_argument("response exceeds 16 MiB");
    std::shared_ptr<Context> context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = contexts_.find(id);
        if (it == contexts_.end()) return false;  // Late/duplicate completion.
        context = it->second;
        contexts_.erase(it);  // The winner alone owns the right to call done.
    }
    FinishContext(context, std::move(payload), error);
    return true;
}

void ServerCore::Shutdown() {
    std::unordered_map<uint64_t, std::shared_ptr<Context>> abandoned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) return;
        closing_ = true;
        abandoned.swap(contexts_);
        pending_.clear();
    }
    ready_.notify_all();
    if (started_) server_.Stop(0);
    for (auto& item : abandoned) FinishContext(item.second, {}, "Java server closed before completion");
    if (started_) server_.Join();
}

}  // namespace echo_proto
