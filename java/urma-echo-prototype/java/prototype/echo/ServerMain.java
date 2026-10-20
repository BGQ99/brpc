package prototype.echo;

import java.util.concurrent.atomic.AtomicBoolean;

public final class ServerMain {
    public static void main(String[] args) throws Exception {
        Arguments options = new Arguments(args);
        int port = options.number("port", 8000, 1, 65535);
        String transport = options.text("transport", "tcp");
        int workerCount = options.number("workers", 4, 1, 256);
        int capacity = options.number("capacity", 128, 1, 65536);
        NativeBridge.configure(options.text("device", ""), options.polling());
        EchoServer server = new EchoServer(port, transport, workerCount, capacity, payload -> payload);
        AtomicBoolean stopping = new AtomicBoolean();
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            stopping.set(true);
            server.close();
            System.out.println("SERVER_CLOSED=PASS");
        }, "echo-shutdown"));
        System.out.println("SERVER_READY java port=" + port + " transport_requested=" + transport
                + " path_verified=false workers=" + workerCount + " capacity=" + capacity);
        try {
            while (!stopping.get()) { server.checkHealthy(); Thread.sleep(100); }
        } finally { server.close(); }
    }
}
