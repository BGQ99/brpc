package prototype.echo;

import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

final class Arguments {
    private final Map<String, String> values = new HashMap<>();
    Arguments(String[] args) {
        Set<String> allowed = new HashSet<>(Arrays.asList("server", "transport", "size", "concurrency",
                "warmup", "seconds", "timeout-ms", "capacity", "port", "device", "polling", "group", "workers"));
        for (String arg : args) {
            int equals = arg.indexOf('=');
            if (!arg.startsWith("--") || equals < 3) throw new IllegalArgumentException("use --key=value");
            String key = arg.substring(2, equals);
            if (!allowed.contains(key) || values.put(key, arg.substring(equals + 1)) != null) {
                throw new IllegalArgumentException("unknown or duplicate argument: " + key);
            }
        }
    }
    String text(String key, String fallback) { return values.containsKey(key) ? values.get(key) : fallback; }
    int number(String key, int fallback, int low, int high) {
        int value = Integer.parseInt(text(key, Integer.toString(fallback)));
        if (value < low || value > high) throw new IllegalArgumentException("invalid " + key);
        return value;
    }
    boolean polling() {
        String value = text("polling", "false");
        if (!value.equals("true") && !value.equals("false")) throw new IllegalArgumentException("invalid polling");
        return Boolean.parseBoolean(value);
    }
}
