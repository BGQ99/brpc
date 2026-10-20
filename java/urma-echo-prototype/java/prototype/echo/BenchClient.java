package prototype.echo;

import java.lang.management.ManagementFactory;
import java.util.Arrays;
import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Closed-loop driver only. Fixed-arrival-rate tests are not implemented here. */
public final class BenchClient {
    private static final class Finished {
        final long began;
        final byte[] response;
        final Throwable error;
        Finished(long began, byte[] response, Throwable error) {
            this.began = began; this.response = response; this.error = error;
        }
    }
    private static final class Stage {
        long success, failures, elapsedNs, cpuNs = -1;
        final Histogram latency = new Histogram();
    }
    private static long cpuNanos() {
        java.lang.management.OperatingSystemMXBean bean = ManagementFactory.getOperatingSystemMXBean();
        if (bean instanceof com.sun.management.OperatingSystemMXBean) {
            return ((com.sun.management.OperatingSystemMXBean) bean).getProcessCpuTime();
        }
        return -1;
    }
    private static void submit(EchoClient client, byte[] payload, ArrayBlockingQueue<Finished> queue,
                               AtomicReference<Throwable> overflow) {
        long began = System.nanoTime();
        client.echoAsync(payload).whenComplete((response, error) -> {
            if (!queue.offer(new Finished(began, response, error))) {
                overflow.compareAndSet(null, new IllegalStateException("benchmark completion queue overflow"));
            }
        });
    }
    private static Stage run(EchoClient client, byte[] payload, int concurrency, int seconds) throws Exception {
        Stage stats = new Stage();
        ArrayBlockingQueue<Finished> queue = new ArrayBlockingQueue<>(concurrency);
        AtomicReference<Throwable> overflow = new AtomicReference<>();
        long start = System.nanoTime();
        long deadline = start + TimeUnit.SECONDS.toNanos(seconds);
        long cpuStart = cpuNanos();
        int inFlight = 0;
        for (int i = 0; i < concurrency; i++) { submit(client, payload, queue, overflow); inFlight++; }
        while (inFlight > 0) {
            if (overflow.get() != null) throw new IllegalStateException("completion delivery failed", overflow.get());
            Finished result = queue.poll(100, TimeUnit.MILLISECONDS);
            if (result == null) continue;
            boolean valid = result.error == null && Arrays.equals(result.response, payload);
            long ended = System.nanoTime();  // Includes delivery and payload validation.
            if (valid) { stats.success++; stats.latency.add(ended - result.began); }
            else stats.failures++;
            inFlight--;
            if (System.nanoTime() < deadline) { submit(client, payload, queue, overflow); inFlight++; }
        }
        stats.elapsedNs = System.nanoTime() - start;
        long cpuEnd = cpuNanos();
        if (cpuStart >= 0 && cpuEnd >= cpuStart) stats.cpuNs = cpuEnd - cpuStart;
        return stats;
    }
    private static String number(long value) { return value < 0 ? "null" : Long.toString(value); }

    public static void main(String[] args) throws Exception {
        Arguments options = new Arguments(args);
        int size = options.number("size", 1024, 0, 16 * 1024 * 1024);
        int concurrency = options.number("concurrency", 1, 1, 65536);
        int capacity = options.number("capacity", concurrency, concurrency, 65536);
        int timeout = options.number("timeout-ms", 5000, 1, 600000);
        int warmup = options.number("warmup", 1, 0, 3600);
        int seconds = options.number("seconds", 5, 1, 86400);
        String transport = options.text("transport", "tcp");
        String group = options.text("group", "C");
        if (!group.equals("C") && !group.equals("J")) throw new IllegalArgumentException("Java client group must be C or J");
        NativeBridge.configure(options.text("device", ""), options.polling());
        byte[] payload = new byte[size];
        for (int i = 0; i < size; i++) payload[i] = (byte) (i * 31 + 7);
        Stage stats;
        try (EchoClient client = new EchoClient(options.text("server", "127.0.0.1:8000"), transport, timeout, capacity)) {
            byte[] response = client.echoAsync(payload).get(timeout + 2000L, TimeUnit.MILLISECONDS);
            if (!Arrays.equals(response, payload)) throw new IllegalStateException("warm-up payload mismatch");
            if (warmup > 0 && run(client, payload, concurrency, warmup).failures != 0) {
                throw new IllegalStateException("warm-up had failures");
            }
            stats = run(client, payload, concurrency, seconds);
        }
        System.out.printf(Locale.ROOT,
                "{\"schema\":1,\"build_id\":\"%s\",\"group\":\"%s\",\"client\":\"java\",\"transport_requested\":\"%s\","
                + "\"path_verified\":false,\"load_model\":\"closed_loop\",\"percentiles\":\"log_histogram_upper_bound\","
                + "\"polling\":%s,\"size\":%d,\"concurrency\":%d,\"capacity\":%d,\"timeout_ms\":%d,\"warmup_seconds\":%d,"
                + "\"requested_seconds\":%d,\"elapsed_ns\":%d,\"success\":%d,\"failures\":%d,\"qps\":%.12g,"
                + "\"p50_ns\":%s,\"p95_ns\":%s,\"p99_ns\":%s,\"client_cpu_ns\":%s}%n",
                NativeBridge.buildId(), group, transport, Boolean.toString(options.polling()),
                size, concurrency, capacity, timeout, warmup, seconds, stats.elapsedNs,
                stats.success, stats.failures, stats.success * 1e9 / stats.elapsedNs,
                number(stats.latency.percentile(50)), number(stats.latency.percentile(95)),
                number(stats.latency.percentile(99)), number(stats.cpuNs));
        if (stats.failures != 0 || stats.success == 0) System.exit(1);
    }
}
