package prototype.echo;

import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** The dispatcher enters JNI itself; native bthreads never invoke the service. */
public final class EchoServer implements AutoCloseable {
    private final long handle;
    private final Thread dispatcher;
    private final ThreadPoolExecutor workers;
    private final EchoService service;
    private final AtomicBoolean closed = new AtomicBoolean();
    private volatile Throwable dispatchFailure;

    public EchoServer(int port, String transport, int workerCount, int capacity, EchoService service) {
        if (workerCount < 1 || workerCount > 256 || capacity < 1 || service == null) {
            throw new IllegalArgumentException("invalid worker count/capacity/service");
        }
        this.service = service;
        workers = new ThreadPoolExecutor(workerCount, workerCount, 0L, TimeUnit.MILLISECONDS,
                new ArrayBlockingQueue<Runnable>(capacity), task -> {
                    Thread thread = new Thread(task, "echo-business");
                    thread.setDaemon(true);
                    return thread;
                });
        long serverHandle = 0;
        try {
            serverHandle = NativeBridge.startServer(port, transport, capacity);
            handle = serverHandle;
            dispatcher = new Thread(this::dispatch, "echo-dispatch");
            dispatcher.setDaemon(true);
            dispatcher.start();
        } catch (Throwable error) {
            if (serverHandle != 0) {
                try { NativeBridge.closeServer(serverHandle); }
                catch (Throwable closeError) { error.addSuppressed(closeError); }
            }
            try { workers.shutdownNow(); }
            catch (Throwable closeError) { error.addSuppressed(closeError); }
            throw error;
        }
    }

    private void dispatch() {
        try {
            while (!closed.get()) {
                NativeBridge.Request request = NativeBridge.takeServerRequest(handle, 100);
                if (request == null) continue;
                try { workers.execute(() -> execute(request)); }
                catch (RejectedExecutionException full) { finish(request.id, null, "Java business queue full"); }
            }
        } catch (Throwable error) {
            if (!closed.get()) {
                dispatchFailure = error;
                // Claim and fail all still-native contexts; do not strand them.
                try { close(); } catch (Throwable closeError) { error.addSuppressed(closeError); }
            }
        }
    }

    private void execute(NativeBridge.Request request) {
        byte[] response;
        try {
            response = service.echo(request.payload);
            if (response == null) throw new IllegalStateException("EchoService returned null");
            if (response.length > 16 * 1024 * 1024) throw new IllegalStateException("response exceeds 16 MiB");
        } catch (Throwable error) {
            finish(request.id, null, "Java EchoService failed: " + error.getClass().getSimpleName());
            return;
        }
        finish(request.id, response, null);
    }

    private void finish(long id, byte[] response, String error) {
        try {
            boolean completed = NativeBridge.completeServerRequest(handle, id, response, error);
            if (!completed && !closed.get()) throw new IllegalStateException("duplicate/unknown server completion " + id);
        } catch (Throwable failure) {
            if (!closed.get()) {
                dispatchFailure = failure;
                // This request may still be in the native registry: shutdown fails it.
                try { close(); } catch (Throwable closeError) { failure.addSuppressed(closeError); }
            }
        }
    }

    public void checkHealthy() {
        if (dispatchFailure != null) throw new IllegalStateException("server dispatcher failed", dispatchFailure);
    }

    @Override public void close() {
        if (!closed.compareAndSet(false, true)) return;
        try { NativeBridge.closeServer(handle); }
        finally {
            workers.shutdownNow();
            if (Thread.currentThread() != dispatcher) {
                boolean interrupted = false;
                while (dispatcher.isAlive()) {
                    try { dispatcher.join(); }
                    catch (InterruptedException e) { interrupted = true; }
                }
                if (interrupted) Thread.currentThread().interrupt();
            }
        }
    }
}
