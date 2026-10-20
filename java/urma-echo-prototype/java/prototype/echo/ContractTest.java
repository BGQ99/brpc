package prototype.echo;

import java.net.ServerSocket;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Integration checks: run only after a real native build, using loopback TCP. */
public final class ContractTest {
    private static int port() throws Exception {
        try (ServerSocket socket = new ServerSocket(0)) { return socket.getLocalPort(); }
    }
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    private static void failed(CompletableFuture<?> future) throws Exception {
        try { future.get(3, TimeUnit.SECONDS); throw new AssertionError("expected a failed RPC"); }
        catch (ExecutionException expected) { /* Expected terminal RPC failure. */ }
    }
    public static void main(String[] args) throws Exception {
        NativeBridge.configure("", false);
        byte[] binary = new byte[] {0, 1, -1, 0, 127, -128};
        int echoPort = port();
        try (EchoServer server = new EchoServer(echoPort, "tcp", 2, 16, data -> data);
             EchoClient client = new EchoClient("127.0.0.1:" + echoPort, "tcp", 1000, 8)) {
            List<CompletableFuture<byte[]>> calls = new ArrayList<>();
            for (int i = 0; i < 8; i++) calls.add(client.echoAsync(binary));
            for (CompletableFuture<byte[]> call : calls) {
                require(Arrays.equals(binary, call.get(3, TimeUnit.SECONDS)), "binary payload mismatch");
            }
            require(client.echoAsync(new byte[0]).get(3, TimeUnit.SECONDS).length == 0, "empty payload mismatch");
            server.checkHealthy();
        }
        System.out.println("CHECK binary_concurrent_and_empty=PASS");

        // Reservation lasts until Take, even if the RPC already completed.
        int duplicatePort = port();
        try (EchoServer server = new EchoServer(duplicatePort, "tcp", 1, 4, data -> data)) {
            long handle = NativeBridge.createClient("127.0.0.1:" + duplicatePort, "tcp", 1000, 2);
            try {
                NativeBridge.submitClient(handle, 7, binary);
                boolean rejected = false;
                try { NativeBridge.submitClient(handle, 7, binary); }
                catch (IllegalStateException expected) { rejected = true; }
                require(rejected, "duplicate admission was not rejected");
                NativeBridge.submitClient(handle, 8, binary);
                rejected = false;
                try { NativeBridge.submitClient(handle, 9, binary); }
                catch (IllegalStateException expected) { rejected = true; }
                require(rejected, "capacity admission was not rejected");
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                java.util.Set<Long> received = new java.util.HashSet<>();
                while (received.size() < 2 && System.nanoTime() < deadline) {
                    NativeBridge.Completion event = NativeBridge.takeClientCompletion(handle, 100);
                    if (event == null) continue;
                    require(event.error == null && (event.id == 7 || event.id == 8), "original call failed");
                    require(received.add(event.id), "duplicate completion");
                }
                require(received.size() == 2, "accepted call was lost");
            } finally { NativeBridge.closeClient(handle); }
        }
        System.out.println("CHECK duplicate_and_capacity=PASS");

        int exceptionPort = port();
        try (EchoServer server = new EchoServer(exceptionPort, "tcp", 1, 4, data -> {
                 throw new Exception("test business failure");
             });
             EchoClient client = new EchoClient("127.0.0.1:" + exceptionPort, "tcp", 1000, 4)) {
            failed(client.echoAsync(binary));
            server.checkHealthy();
        }
        System.out.println("CHECK java_exception=PASS");

        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        int timeoutPort = port();
        try (EchoServer server = new EchoServer(timeoutPort, "tcp", 1, 4, data -> {
                 entered.countDown(); release.await(); return data;
             });
             EchoClient client = new EchoClient("127.0.0.1:" + timeoutPort, "tcp", 300, 4)) {
            CompletableFuture<byte[]> timed = client.echoAsync(binary);
            require(entered.await(2, TimeUnit.SECONDS), "service was not entered");
            failed(timed);
            release.countDown();
            require(Arrays.equals(binary, client.echoAsync(binary).get(3, TimeUnit.SECONDS)), "late completion broke next call");
            server.checkHealthy();
        } finally { release.countDown(); }
        System.out.println("CHECK timeout_late_response_and_reuse=PASS");

        CountDownLatch closingEntered = new CountDownLatch(1);
        CountDownLatch closingRelease = new CountDownLatch(1);
        int closePort = port();
        EchoServer closingServer = new EchoServer(closePort, "tcp", 1, 4, data -> {
            closingEntered.countDown(); closingRelease.await(); return data;
        });
        try (EchoClient client = new EchoClient("127.0.0.1:" + closePort, "tcp", 1000, 4)) {
            CompletableFuture<byte[]> call = client.echoAsync(binary);
            require(closingEntered.await(2, TimeUnit.SECONDS), "close test service not entered");
            closingServer.close();
            failed(call);
        } finally { closingRelease.countDown(); closingServer.close(); }
        System.out.println("CHECK server_close_in_flight=PASS");

        CountDownLatch clientEntered = new CountDownLatch(1);
        CountDownLatch clientRelease = new CountDownLatch(1);
        int clientClosePort = port();
        try (EchoServer server = new EchoServer(clientClosePort, "tcp", 1, 4, data -> {
                 clientEntered.countDown(); clientRelease.await(); return data;
             });
             EchoClient client = new EchoClient("127.0.0.1:" + clientClosePort, "tcp", 5000, 4)) {
            CompletableFuture<byte[]> call = client.echoAsync(binary);
            require(clientEntered.await(2, TimeUnit.SECONDS), "client close service not entered");
            AtomicReference<Throwable> closeFailure = new AtomicReference<>();
            Thread closer = new Thread(() -> {
                try { client.close(); }
                catch (Throwable error) { closeFailure.set(error); }
            }, "test-client-close");
            closer.setDaemon(true);
            closer.start();
            try {
                try {
                    call.get(3, TimeUnit.SECONDS);
                    throw new AssertionError("client close did not fail its in-flight RPC");
                } catch (ExecutionException expected) {
                    require(expected.getCause() instanceof IllegalStateException &&
                            "client closed".equals(expected.getCause().getMessage()),
                            "in-flight RPC failed for a reason other than client close");
                }
            } finally { clientRelease.countDown(); }
            closer.join(7000);
            require(!closer.isAlive(), "client close did not finish");
            if (closeFailure.get() != null) throw new AssertionError("client close failed", closeFailure.get());
            try (EchoClient next = new EchoClient("127.0.0.1:" + clientClosePort, "tcp", 1000, 4)) {
                require(Arrays.equals(binary, next.echoAsync(binary).get(3, TimeUnit.SECONDS)),
                        "server unusable after in-flight client close");
            }
            server.checkHealthy();
        } finally { clientRelease.countDown(); }
        System.out.println("CHECK client_close_in_flight=PASS");

        EchoClient closed = new EchoClient("127.0.0.1:1", "tcp", 100, 1);
        closed.close();
        closed.close();
        boolean rejected = false;
        try { closed.echoAsync(binary); } catch (IllegalStateException expected) { rejected = true; }
        require(rejected, "closed client accepted request");
        System.out.println("CHECK closed_client=PASS");
        System.out.println("CONTRACT_TEST=PASS transport=tcp (not URMA validation)");
    }
}
