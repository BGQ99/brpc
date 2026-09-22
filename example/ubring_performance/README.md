# UBRing performance benchmark

This directory contains a client/server benchmark for comparing UBRing data
paths. Treat benchmark changes and scripts as local test infrastructure until
they have been reviewed separately from a production change.

## What is measured

The client runs three phases:

1. Warm-up: establish UBRing and warm caches. Completed RPCs are not recorded.
2. Measurement: record RPC completions, latency, request attachment bytes, and
   sampled client/server CPU usage for exactly `test_seconds`.
3. Drain: stop issuing new RPCs and wait for every outstanding callback before
   releasing channels and test objects. Drain completions are not recorded.

The measurement uses completion time: an RPC is recorded when its callback
finishes inside the measurement interval. A failed RPC stops the run, drains
the remaining callbacks, and makes the client exit with a non-zero status.

`attachment_size` is the request attachment size. Reported throughput counts
request attachment bytes only. Set `echo_attachment=true` to exercise the same
payload size in both request and response directions; the reported byte rate
does not double to include the echoed response.

## Build

Build both binaries from the repository root:

```bash
bazel build --config=ubring \
  //example:ubring_performance_server \
  //example:ubring_performance_client
```

The runner defaults to the resulting paths under `bazel-bin/example/`.
Override them with `SERVER_BIN` and `CLIENT_BIN` when testing another build.

## Short validation run

Run this before reserving a machine for formal measurements:

```bash
THREAD_NUM=1 \
QUEUE_DEPTH=1 \
ATTACHMENT_SIZE=4096 \
WARMUP_SECONDS=2 \
TEST_SECONDS=5 \
LABEL=legacy64-smoke \
example/ubring_performance/run_benchmark.sh
```

The runner enables POSIX IPC UBRing on both processes, saves logs and machine
metadata, and rejects the result if the client exits unsuccessfully, no UBRing
handshake is observed, TCP fallback is observed, or the measurement reports a
failed RPC. Results default to `/tmp/brpc-ubring-benchmark`. On exit, the
runner gives the server `SHUTDOWN_SECONDS` (10 seconds by default) to stop
after `SIGINT`, then kills only that server child process if it is still alive.

Do not treat an RPC success by itself as proof that IPC was used: fallback TCP
can also complete the RPC. Keep the handshake checks enabled.

## Formal comparison

Prepare every candidate first, then run the baseline and candidates during the
same exclusive machine reservation. Run only one case at a time so the client
and server do not compete with another benchmark.

Use the same machine, binaries' build mode, total shared-memory budget, CPU
placement, message sizes, concurrency, warm-up, and measurement duration for
all candidates. Suggested initial matrix:

| Attachment | Threads | Queue depth | Echo attachment |
| ---: | ---: | ---: | --- |
| 64 B | 1 | 1 | true |
| 1 KiB | 1 | 1 | true |
| 4 KiB | 1 | 1 | true |
| 8 KiB | 1 | 1 | true |
| 64 KiB | 1 | 1 | true |
| selected sizes | 4, 8, 16 | 8 or 16 | true |

Repeat each case at least five times. Alternate candidate order between rounds
to reduce time and temperature bias. Compare throughput, QPS, average latency,
P99/P99.9 latency, client CPU, and server CPU. Keep every run's metadata and
raw logs, including rejected runs.

Optional CPU placement uses Linux `taskset`, for example:

```bash
SERVER_CPUSET=0-3 \
CLIENT_CPUSET=4-7 \
THREAD_NUM=4 \
QUEUE_DEPTH=8 \
ATTACHMENT_SIZE=8192 \
WARMUP_SECONDS=10 \
TEST_SECONDS=60 \
LABEL=legacy64-8k-t4-q8 \
RESULT_ROOT=/path/to/results \
example/ubring_performance/run_benchmark.sh
```

For IPC_V2 experiments, a format ID must still describe one fixed layout in a
given build. Use separate, matching client/server builds for 4 KiB and 8 KiB
candidates, and identify the build and Git commit through `LABEL` and metadata.
