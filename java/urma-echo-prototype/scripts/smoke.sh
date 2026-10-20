#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
require_build
mkdir -p -- "$ROOT/results"
logs=$(mktemp -d "$ROOT/results/tcp-smoke.XXXXXX")
port=${ECHO_TEST_PORT:-18971}
server_pid=
active_role=
cleanup() {
    local forced=0
    if [[ -n "$server_pid" ]]; then
        kill -TERM "$server_pid" 2>/dev/null || true
        # Only reap the child created by this script; never enumerate/kill other services.
        for ((i=0; i<50; ++i)); do
            if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
            local child_state
            child_state=$(ps -o stat= -p "$server_pid" 2>/dev/null || true)
            if [[ "$child_state" == *Z* ]]; then break; fi
            sleep 0.1
        done
        local child_state
        child_state=$(ps -o stat= -p "$server_pid" 2>/dev/null || true)
        if kill -0 "$server_pid" 2>/dev/null && [[ "$child_state" != *Z* ]]; then
            printf 'Test child failed to stop normally; terminating PID %s\n' "$server_pid" >&2
            kill -KILL "$server_pid" 2>/dev/null || true
            forced=1
        fi
        wait "$server_pid" 2>/dev/null || true
        server_pid=
        if ! grep -q 'SERVER_CLOSED=PASS' "$logs/$active_role-server.log"; then
            printf 'Server did not report a clean close; inspect %s\n' "$logs/$active_role-server.log" >&2
            forced=1
        fi
    fi
    return "$forced"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
start_server() {
    local role=$1
    active_role=$role
    bash "$ROOT/scripts/run-server.sh" "$role" "--port=$port" --transport=tcp --capacity=32 >"$logs/$role-server.log" 2>&1 &
    server_pid=$!
    local ready=false
    for ((i=0; i<100; ++i)); do
        kill -0 "$server_pid" 2>/dev/null || { cat "$logs/$role-server.log" >&2; return 1; }
        if grep -q 'SERVER_READY' "$logs/$role-server.log"; then ready=true; break; fi
        sleep 0.1
    done
    [[ "$ready" == true ]] || { cat "$logs/$role-server.log" >&2; return 1; }
}
client() {
    local role=$1 group=$2
    timeout 20s bash "$ROOT/scripts/run-client.sh" "$role" "--group=$group" "--server=127.0.0.1:$port" \
        --transport=tcp --size=1024 --concurrency=8 --warmup=0 --seconds=1 --timeout-ms=5000 \
        >"$logs/$group.jsonl" 2>"$logs/$group.stderr"
}
start_server native
client native N
client java C
cleanup
start_server java
client native S
client java J
cleanup
printf 'TCP_FOUR_GROUP_SMOKE=PASS logs=%s\n' "$logs"
printf 'No URMA data path or performance acceptance has been verified.\n'
