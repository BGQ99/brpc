package prototype.echo;

/** Internal JNI boundary. Only Java-created platform threads enter these methods. */
final class NativeBridge {
    static { System.loadLibrary("brpc_echo_jni"); }
    private NativeBridge() {}

    static native void configure(String device, boolean polling);
    static native boolean hasUrma();
    static native String buildId();
    static native long createClient(String address, String transport, int timeoutMs, int capacity);
    static native void submitClient(long handle, long id, byte[] payload);
    static native Completion takeClientCompletion(long handle, int waitMs);
    static native void closeClient(long handle);
    static native long startServer(int port, String transport, int capacity);
    static native Request takeServerRequest(long handle, int waitMs);
    static native boolean completeServerRequest(long handle, long id, byte[] payload, String error);
    static native void closeServer(long handle);

    static final class Completion {
        final long id;
        final long sequence;
        final byte[] payload;
        final String error;
        Completion(long id, long sequence, byte[] payload, String error) {
            this.id = id; this.sequence = sequence; this.payload = payload; this.error = error;
        }
    }
    static final class Request {
        final long id;
        final long sequence;
        final byte[] payload;
        Request(long id, long sequence, byte[] payload) {
            this.id = id; this.sequence = sequence; this.payload = payload;
        }
    }
}
