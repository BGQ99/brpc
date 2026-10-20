package prototype.echo;

/** Bounded-memory histogram; matches native/statistics.h. */
final class Histogram {
    private final long[] buckets = new long[56 * 64];
    private long count;
    void add(long nanos) {
        nanos = Math.max(1L, Math.min(nanos, (1L << 55) - 1));
        int power = 63 - Long.numberOfLeadingZeros(nanos);
        long base = 1L << power;
        int fraction = (int) ((nanos - base) * 64 / base);
        buckets[power * 64 + fraction]++;
        count++;
    }
    long percentile(int percent) {
        if (count == 0) return -1;
        long rank = (count * percent + 99) / 100;
        long seen = 0;
        for (int i = 0; i < buckets.length; i++) {
            seen += buckets[i];
            if (seen >= rank) {
                long base = 1L << (i / 64);
                return base + ((i % 64 + 1) * base + 63) / 64 - 1;
            }
        }
        throw new AssertionError("histogram count mismatch");
    }
}
