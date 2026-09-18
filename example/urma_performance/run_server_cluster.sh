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

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SERVER_BIN="${SCRIPT_DIR}/build/urma_performance_server"
PROCESS_NUM=8
BASE_PORT=8005
OUTPUT_DIR="${SCRIPT_DIR}/benchmark-results/server-cluster-$(date +%Y%m%d-%H%M%S)"
EXTRA_ARGS=()
pids=()

usage() {
    echo "Usage: $0 [options] [-- extra_server_flags...]"
    echo
    echo "Options:"
    echo "  --server PATH          Server binary (default: ${SERVER_BIN})"
    echo "  --process-num N        Number of server processes (default: ${PROCESS_NUM})"
    echo "  --base-port N          Port of the first server (default: ${BASE_PORT})"
    echo "  --output-dir PATH      Server log directory"
    echo "  -h, --help             Show this help"
}

require_positive_integer() {
    local name=$1
    local value=$2
    if [[ ! ${value} =~ ^[1-9][0-9]*$ ]]; then
        echo "ERROR: ${name} must be a positive integer: ${value}" >&2
        exit 2
    fi
}

while (($# > 0)); do
    case "$1" in
        --server)
            (($# >= 2)) || { echo "ERROR: --server requires a value" >&2; exit 2; }
            SERVER_BIN=$2
            shift 2
            ;;
        --process-num)
            (($# >= 2)) || { echo "ERROR: --process-num requires a value" >&2; exit 2; }
            PROCESS_NUM=$2
            shift 2
            ;;
        --base-port)
            (($# >= 2)) || { echo "ERROR: --base-port requires a value" >&2; exit 2; }
            BASE_PORT=$2
            shift 2
            ;;
        --output-dir)
            (($# >= 2)) || { echo "ERROR: --output-dir requires a value" >&2; exit 2; }
            OUTPUT_DIR=$2
            shift 2
            ;;
        --)
            shift
            EXTRA_ARGS=("$@")
            break
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

require_positive_integer "--process-num" "${PROCESS_NUM}"
require_positive_integer "--base-port" "${BASE_PORT}"
if ((BASE_PORT + PROCESS_NUM - 1 > 65535)); then
    echo "ERROR: server port range exceeds 65535" >&2
    exit 2
fi
for arg in "${EXTRA_ARGS[@]}"; do
    case "${arg}" in
        --port|--port=*)
            echo "ERROR: runner-managed server flag must not appear after --: ${arg}" >&2
            exit 2
            ;;
    esac
done
if [[ ! -x ${SERVER_BIN} ]]; then
    echo "ERROR: server is not executable: ${SERVER_BIN}" >&2
    exit 2
fi
mkdir -p "${OUTPUT_DIR}" || exit 2

stop_servers() {
    trap - INT TERM
    local pid
    for pid in "${pids[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${pids[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
}

handle_signal() {
    stop_servers
    exit 130
}

trap handle_signal INT TERM

for ((process_index = 1; process_index <= PROCESS_NUM; ++process_index)); do
    port=$((BASE_PORT + process_index - 1))
    log_file="${OUTPUT_DIR}/server_process-${process_index}_port-${port}.log"
    "${SERVER_BIN}" "--port=${port}" "${EXTRA_ARGS[@]}" > "${log_file}" 2>&1 &
    pids+=("$!")
    echo "Started server process=${process_index} pid=${pids[process_index - 1]} port=${port} log=${log_file}"
done

echo "All ${PROCESS_NUM} server processes are running on ports ${BASE_PORT}-$((BASE_PORT + PROCESS_NUM - 1))"
echo "Press Ctrl-C to stop all server processes"

while true; do
    for process_index in "${!pids[@]}"; do
        pid=${pids[process_index]}
        if ! kill -0 "${pid}" 2>/dev/null; then
            status=0
            wait "${pid}" || status=$?
            port=$((BASE_PORT + process_index))
            echo "ERROR: server process=$((process_index + 1)) pid=${pid} port=${port} exited with status=${status}; inspect ${OUTPUT_DIR}/server_process-$((process_index + 1))_port-${port}.log" >&2
            stop_servers
            exit 1
        fi
    done
    sleep 1
done
