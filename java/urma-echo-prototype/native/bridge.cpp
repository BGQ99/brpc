#include "prototype_echo_NativeBridge.h"
#include "core.h"

#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {
std::mutex registry_mutex;
bool configured_objects = false;
jlong next_handle = 1;
std::unordered_map<jlong, std::shared_ptr<echo_proto::ClientCore>> clients;
std::unordered_map<jlong, std::shared_ptr<echo_proto::ServerCore>> servers;

void Fail(JNIEnv* env, const char* text) {
    if (env->ExceptionCheck()) return;
    jclass type = env->FindClass("java/lang/IllegalStateException");
    if (type) { env->ThrowNew(type, text); env->DeleteLocalRef(type); }
}

template<class F, class R> R Guard(JNIEnv* env, R failure, F action) {
    try { return action(); }
    catch (const std::exception& e) { Fail(env, e.what()); }
    catch (...) { Fail(env, "unknown native exception"); }
    return failure;
}

std::string Text(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* p = env->GetStringUTFChars(value, nullptr);
    if (!p) throw std::runtime_error("cannot read Java string");
    std::string result;
    try { result.assign(p); }
    catch (...) { env->ReleaseStringUTFChars(value, p); throw; }
    env->ReleaseStringUTFChars(value, p);
    return result;
}

std::string Bytes(JNIEnv* env, jbyteArray value) {
    if (!value) throw std::invalid_argument("payload is null");
    jsize size = env->GetArrayLength(value);
    if (static_cast<size_t>(size) > echo_proto::kMaxPayload) throw std::invalid_argument("payload exceeds 16 MiB");
    std::string bytes(static_cast<size_t>(size), '\0');
    if (size) env->GetByteArrayRegion(value, 0, size, reinterpret_cast<jbyte*>(&bytes[0]));
    if (env->ExceptionCheck()) throw std::runtime_error("cannot copy Java payload");
    return bytes;
}

jbyteArray Array(JNIEnv* env, const std::string& bytes) {
    jbyteArray result = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (!result) throw std::runtime_error("cannot allocate Java payload");
    if (!bytes.empty()) env->SetByteArrayRegion(result, 0, static_cast<jsize>(bytes.size()),
                                              reinterpret_cast<const jbyte*>(bytes.data()));
    if (env->ExceptionCheck()) throw std::runtime_error("cannot deliver Java payload");
    return result;
}

void WaitMillis(jint value) {
    if (value < 0 || value > 1000) throw std::invalid_argument("wait_ms must be 0..1000");
}

template<class T> std::shared_ptr<T> Lookup(
        const std::unordered_map<jlong, std::shared_ptr<T>>& table, jlong handle) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto it = table.find(handle);
    if (it == table.end()) throw std::runtime_error("native handle is closed or invalid");
    return it->second;
}

template<class T> jlong Insert(std::unordered_map<jlong, std::shared_ptr<T>>& table,
                               std::shared_ptr<T> value) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    if (next_handle <= 0 || next_handle == std::numeric_limits<jlong>::max()) {
        throw std::overflow_error("native handle space exhausted");
    }
    jlong handle = next_handle++;
    table.emplace(handle, std::move(value));
    return handle;
}

template<class T> std::shared_ptr<T> Remove(
        std::unordered_map<jlong, std::shared_ptr<T>>& table, jlong handle) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto it = table.find(handle);
    if (it == table.end()) return {};
    auto value = it->second;
    table.erase(it);
    return value;
}

void MarkStarted() {
    std::lock_guard<std::mutex> lock(registry_mutex);
    configured_objects = true;
}
}

extern "C" {

JNIEXPORT void JNICALL Java_prototype_echo_NativeBridge_configure(
        JNIEnv* env, jclass, jstring device, jboolean polling) {
    Guard(env, false, [&] {
        std::lock_guard<std::mutex> lock(registry_mutex);
        if (configured_objects) throw std::runtime_error("configure must precede the first client/server creation");
        echo_proto::ConfigureUrma(Text(env, device), polling == JNI_TRUE);
        return true;
    });
}

JNIEXPORT jboolean JNICALL Java_prototype_echo_NativeBridge_hasUrma(JNIEnv*, jclass) {
    return echo_proto::HasUrma() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_prototype_echo_NativeBridge_buildId(JNIEnv* env, jclass) {
    return env->NewStringUTF(ECHO_BUILD_ID);
}

JNIEXPORT jlong JNICALL Java_prototype_echo_NativeBridge_createClient(
        JNIEnv* env, jclass, jstring address, jstring transport, jint timeout_ms, jint capacity) {
    return Guard(env, static_cast<jlong>(0), [&] {
        if (capacity < 1) throw std::invalid_argument("capacity must be positive");
        MarkStarted();
        return Insert(clients, echo_proto::ClientCore::Create(Text(env, address), Text(env, transport),
                                                             timeout_ms, static_cast<size_t>(capacity)));
    });
}

JNIEXPORT void JNICALL Java_prototype_echo_NativeBridge_submitClient(
        JNIEnv* env, jclass, jlong handle, jlong id, jbyteArray payload) {
    Guard(env, false, [&] {
        if (id <= 0) throw std::invalid_argument("call id must be positive");
        auto client = Lookup(clients, handle);
        std::string error;
        if (!client->Submit(static_cast<uint64_t>(id), Bytes(env, payload), &error)) {
            throw std::runtime_error(error);
        }
        return true;
    });
}

JNIEXPORT jobject JNICALL Java_prototype_echo_NativeBridge_takeClientCompletion(
        JNIEnv* env, jclass, jlong handle, jint wait_ms) {
    return Guard(env, static_cast<jobject>(nullptr), [&]() -> jobject {
        WaitMillis(wait_ms);
        echo_proto::Completion completion;
        if (!Lookup(clients, handle)->Take(&completion, wait_ms)) return nullptr;
        jclass type = env->FindClass("prototype/echo/NativeBridge$Completion");
        if (!type) throw std::runtime_error("Completion class unavailable");
        jmethodID ctor = env->GetMethodID(type, "<init>", "(JJ[BLjava/lang/String;)V");
        if (!ctor) throw std::runtime_error("Completion constructor unavailable");
        jbyteArray data = Array(env, completion.payload);
        jstring error = completion.error.empty() ? nullptr : env->NewStringUTF(completion.error.c_str());
        if (!completion.error.empty() && !error) throw std::runtime_error("cannot allocate RPC error");
        jobject event = env->NewObject(type, ctor, static_cast<jlong>(completion.id),
                                       static_cast<jlong>(completion.sequence), data, error);
        env->DeleteLocalRef(data);
        if (error) env->DeleteLocalRef(error);
        env->DeleteLocalRef(type);
        return event;
    });
}

JNIEXPORT void JNICALL Java_prototype_echo_NativeBridge_closeClient(
        JNIEnv* env, jclass, jlong handle) {
    Guard(env, false, [&] {
        auto client = Remove(clients, handle);
        if (client) client->Shutdown();
        return true;
    });
}

JNIEXPORT jlong JNICALL Java_prototype_echo_NativeBridge_startServer(
        JNIEnv* env, jclass, jint port, jstring transport, jint capacity) {
    return Guard(env, static_cast<jlong>(0), [&] {
        if (capacity < 1) throw std::invalid_argument("capacity must be positive");
        MarkStarted();
        auto server = std::make_shared<echo_proto::ServerCore>(true, static_cast<size_t>(capacity));
        server->Start(port, Text(env, transport));
        return Insert(servers, std::move(server));
    });
}

JNIEXPORT jobject JNICALL Java_prototype_echo_NativeBridge_takeServerRequest(
        JNIEnv* env, jclass, jlong handle, jint wait_ms) {
    return Guard(env, static_cast<jobject>(nullptr), [&]() -> jobject {
        WaitMillis(wait_ms);
        echo_proto::ServerRequest request;
        if (!Lookup(servers, handle)->Take(&request, wait_ms)) return nullptr;
        jclass type = env->FindClass("prototype/echo/NativeBridge$Request");
        if (!type) throw std::runtime_error("Request class unavailable");
        jmethodID ctor = env->GetMethodID(type, "<init>", "(JJ[B)V");
        if (!ctor) throw std::runtime_error("Request constructor unavailable");
        jbyteArray data = Array(env, request.payload);
        jobject event = env->NewObject(type, ctor, static_cast<jlong>(request.id),
                                       static_cast<jlong>(request.sequence), data);
        env->DeleteLocalRef(data);
        env->DeleteLocalRef(type);
        return event;
    });
}

JNIEXPORT jboolean JNICALL Java_prototype_echo_NativeBridge_completeServerRequest(
        JNIEnv* env, jclass, jlong handle, jlong id, jbyteArray payload, jstring error) {
    return Guard(env, static_cast<jboolean>(JNI_FALSE), [&]() -> jboolean {
        if (id <= 0) throw std::invalid_argument("request id must be positive");
        std::string failure = Text(env, error);
        std::string data = failure.empty() ? Bytes(env, payload) : std::string();
        return Lookup(servers, handle)->Complete(static_cast<uint64_t>(id), std::move(data), failure)
            ? JNI_TRUE : JNI_FALSE;
    });
}

JNIEXPORT void JNICALL Java_prototype_echo_NativeBridge_closeServer(
        JNIEnv* env, jclass, jlong handle) {
    Guard(env, false, [&] {
        auto server = Remove(servers, handle);
        if (server) server->Shutdown();
        return true;
    });
}
}
