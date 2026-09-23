#!/usr/bin/env bash

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "${script_dir}/../.." && pwd)

server_bin=${SERVER_BIN:-"${repo_root}/bazel-bin/example/ubring_performance_server"}
client_bin=${CLIENT_BIN:-"${repo_root}/bazel-bin/example/ubring_performance_client"}
server_host=${SERVER_HOST:-127.0.0.1}
server_port=${SERVER_PORT:-8002}
dummy_port=${DUMMY_PORT:-8001}
thread_num=${THREAD_NUM:-1}
queue_depth=${QUEUE_DEPTH:-1}
attachment_size=${ATTACHMENT_SIZE:-4096}
echo_attachment=${ECHO_ATTACHMENT:-true}
warmup_seconds=${WARMUP_SECONDS:-5}
test_seconds=${TEST_SECONDS:-20}
expected_qps=${EXPECTED_QPS:-0}
startup_seconds=${STARTUP_SECONDS:-2}
shutdown_seconds=${SHUTDOWN_SECONDS:-10}
label=${LABEL:-legacy64}
result_root=${RESULT_ROOT:-/tmp/brpc-ubring-benchmark}
server_cpuset=${SERVER_CPUSET:-}
client_cpuset=${CLIENT_CPUSET:-}

timestamp=$(date +%Y%m%d-%H%M%S)
result_dir="${result_root}/${label}-${timestamp}"
mkdir -p "${result_dir}"

if [[ ! -x "${server_bin}" ]]; then
    echo "Server binary is not executable: ${server_bin}" >&2
    exit 2
fi
if [[ ! -x "${client_bin}" ]]; then
    echo "Client binary is not executable: ${client_bin}" >&2
    exit 2
fi
if [[ -n "${server_cpuset}" || -n "${client_cpuset}" ]] &&
   ! command -v taskset >/dev/null 2>&1; then
    echo "taskset is required when SERVER_CPUSET or CLIENT_CPUSET is set" >&2
    exit 2
fi
if [[ ! "${shutdown_seconds}" =~ ^[1-9][0-9]*$ ]]; then
    echo "SHUTDOWN_SECONDS must be a positive integer" >&2
    exit 2
fi

run_with_cpuset() {
    local cpuset=$1
    shift
    if [[ -n "${cpuset}" ]]; then
        taskset -c "${cpuset}" "$@"
    else
        "$@"
    fi
}

server_pid=
cleanup() {
    if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
        kill -INT "${server_pid}" 2>/dev/null || true
        local checks=$((shutdown_seconds * 10))
        local i
        for ((i = 0; i < checks; ++i)); do
            if ! kill -0 "${server_pid}" 2>/dev/null; then
                wait "${server_pid}" 2>/dev/null || true
                return
            fi
            sleep 0.1
        done
        echo "Server did not exit within ${shutdown_seconds}s; killing it" >&2
        kill -KILL "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

{
    echo "timestamp=${timestamp}"
    echo "label=${label}"
    echo "server_bin=${server_bin}"
    echo "client_bin=${client_bin}"
    echo "server=${server_host}:${server_port}"
    echo "thread_num=${thread_num}"
    echo "queue_depth=${queue_depth}"
    echo "attachment_size=${attachment_size}"
    echo "echo_attachment=${echo_attachment}"
    echo "warmup_seconds=${warmup_seconds}"
    echo "test_seconds=${test_seconds}"
    echo "expected_qps=${expected_qps}"
    echo "shutdown_seconds=${shutdown_seconds}"
    echo "server_cpuset=${server_cpuset}"
    echo "client_cpuset=${client_cpuset}"
    echo "git_head=$(git -C "${repo_root}" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "git_status_begin"
    git -C "${repo_root}" status --short 2>/dev/null || true
    echo "git_status_end"
    uname -a
    if command -v lscpu >/dev/null 2>&1; then
        lscpu
    fi
} >"${result_dir}/metadata.txt"

server_command=("${server_bin}" \
    --port="${server_port}" \
    --use_ubring=true \
    --ub_shm_type=1 \
    --ub_trace_verbose=true)
if [[ -n "${server_cpuset}" ]]; then
    server_command=(taskset -c "${server_cpuset}" "${server_command[@]}")
fi
"${server_command[@]}" >"${result_dir}/server.log" 2>&1 &
server_pid=$!

sleep "${startup_seconds}"
if ! kill -0 "${server_pid}" 2>/dev/null; then
    echo "Server exited during startup. See ${result_dir}/server.log" >&2
    exit 1
fi

set +e
run_with_cpuset "${client_cpuset}" "${client_bin}" \
    --servers="${server_host}:${server_port}" \
    --dummy_port="${dummy_port}" \
    --thread_num="${thread_num}" \
    --queue_depth="${queue_depth}" \
    --attachment_size="${attachment_size}" \
    --echo_attachment="${echo_attachment}" \
    --warmup_seconds="${warmup_seconds}" \
    --test_seconds="${test_seconds}" \
    --test_iterations=0 \
    --expected_qps="${expected_qps}" \
    --connection_type=single \
    --use_ubring=true \
    --ub_shm_type=1 \
    --ub_trace_verbose=true \
    >"${result_dir}/client.log" 2>&1
client_rc=$?
set -e

cleanup
server_pid=

if (( client_rc != 0 )); then
    echo "Client failed with exit code ${client_rc}. See ${result_dir}/client.log" >&2
    exit "${client_rc}"
fi

if ! grep -q "Client handshake ends (use ubring)" "${result_dir}/client.log"; then
    echo "No successful UBRing client handshake was found; result rejected." >&2
    exit 1
fi
if grep -q "handshake ends (use tcp)" "${result_dir}/client.log" \
        "${result_dir}/server.log"; then
    echo "TCP fallback was detected; result rejected." >&2
    exit 1
fi
if ! grep -q "Failed: 0" "${result_dir}/client.log"; then
    echo "The client did not report a clean measurement; result rejected." >&2
    exit 1
fi

grep "Measured-Seconds:" "${result_dir}/client.log" | tail -n 1 \
    | tee "${result_dir}/summary.txt"
echo "Logs and metadata: ${result_dir}"
