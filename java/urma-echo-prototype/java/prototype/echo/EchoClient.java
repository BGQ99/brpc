package prototype.echo;

import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/** Async client: the native bRPC callback never calls Java. */
public final class EchoClient implements AutoCloseable {
    private final long handle;
    private final Thread completionThread;
    private final Map<Long, CompletableFuture<byte[]>> pending = new ConcurrentHashMap<>();
    private final AtomicLong ids = new AtomicLong(1);
    private final Object lifecycle = new Object();
    private volatile boolean closed;
    private volatile Throwable completionFailure;

    public EchoClient(String address, String transport, int timeoutMs, int capacity) {
        handle = NativeBridge.createClient(address, transport, timeoutMs, capacity);
        try {
            completionThread = new Thread(this::completions, "echo-completions");
            completionThread.setDaemon(true);
            completionThread.start();
        } catch (Throwable error) {
            try { NativeBridge.closeClient(handle); }
            catch (Throwable closeError) { error.addSuppressed(closeError); }
            throw error;
        }
    }

    /** Payload is copied during this call. Future.cancel is local cancellation only. */
    public CompletableFuture<byte[]> echoAsync(byte[] payload) {
        if (payload == null || payload.length > 16 * 1024 * 1024) {
            throw new IllegalArgumentException("payload must be non-null and at most 16 MiB");
        }
        synchronized (lifecycle) {
            if (closed || completionFailure != null) throw new IllegalStateException("client unavailable", completionFailure);
            long id = ids.getAndIncrement();
            if (id <= 0) throw new IllegalStateException("call id space exhausted");
            CompletableFuture<byte[]> future = new CompletableFuture<>();
            pending.put(id, future);
            try { NativeBridge.submitClient(handle, id, payload); }
            catch (Throwable error) {
                pending.remove(id);
                future.completeExceptionally(error);
            }
            return future;
        }
    }

    private void completions() {
        try {
            while (!closed) {
                NativeBridge.Completion event = NativeBridge.takeClientCompletion(handle, 100);
                if (event == null) continue;
                CompletableFuture<byte[]> future = pending.remove(event.id);
                if (future == null) throw new IllegalStateException("unknown/duplicate completion " + event.id);
                if (event.error != null) future.completeExceptionally(new IllegalStateException(event.error));
                else if (event.sequence != event.id) future.completeExceptionally(new IllegalStateException("sequence mismatch"));
                else future.complete(event.payload);
            }
        } catch (Throwable error) {
            synchronized (lifecycle) {
                if (!closed) {
                    completionFailure = error;
                    failPending(error);
                }
            }
        }
    }

    private void failPending(Throwable error) {
        for (Map.Entry<Long, CompletableFuture<byte[]>> entry : pending.entrySet()) {
            if (pending.remove(entry.getKey(), entry.getValue())) entry.getValue().completeExceptionally(error);
        }
    }

    @Override public void close() {
        synchronized (lifecycle) {
            if (closed) return;
            closed = true;
        }
        // Removal and terminal completion are atomic with respect to the event loop.
        failPending(new IllegalStateException("client closed"));
        try { NativeBridge.closeClient(handle); }
        finally {
            if (Thread.currentThread() != completionThread) {
                boolean interrupted = false;
                while (completionThread.isAlive()) {
                    try { completionThread.join(); }
                    catch (InterruptedException e) { interrupted = true; }
                }
                if (interrupted) Thread.currentThread().interrupt();
            }
        }
    }
}
