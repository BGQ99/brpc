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
CLIENT_BIN="${SCRIPT_DIR}/build/urma_performance_client"
TCP_SERVER="127.0.0.1:8003"
URMA_EVENT_SERVER="127.0.0.1:8004"
URMA_POLL_SERVER="127.0.0.1:8005"
MODE="all"
TEST_SECONDS=30
REPEATS=1
OUTPUT_DIR="${SCRIPT_DIR}/benchmark-results/$(date +%Y%m%d-%H%M%S)"
PAYLOAD_SIZES=(64 1024 2048 4096 8192 16384 1048576 8388608)
EXTRA_ARGS=()

usage() {
    echo "Usage: $0 [options] [-- extra_client_flags...]"
    echo
    echo "Options:"
    echo "  --client PATH          Client binary (default: ${CLIENT_BIN})"
    echo "  --tcp-server ADDRESS   TCP server (default: ${TCP_SERVER})"
    echo "  --urma-event-server ADDRESS"
    echo "                         URMA Event server (default: ${URMA_EVENT_SERVER})"
    echo "  --urma-poll-server ADDRESS"
    echo "                         URMA Poll server (default: ${URMA_POLL_SERVER})"
    echo "  --mode MODE            tcp, urma_event, urma_poll, or all (default: ${MODE})"
    echo "  --test-seconds N       Duration of each run (default: ${TEST_SECONDS})"
    echo "  --repeats N            Repetitions per configuration (default: ${REPEATS})"
    echo "  --output-dir PATH      Log and CSV output directory"
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
        --client)
            (($# >= 2)) || { echo "ERROR: --client requires a value" >&2; exit 2; }
            CLIENT_BIN=$2
            shift 2
            ;;
        --tcp-server)
            (($# >= 2)) || { echo "ERROR: --tcp-server requires a value" >&2; exit 2; }
            TCP_SERVER=$2
            shift 2
            ;;
        --urma-event-server)
            (($# >= 2)) || { echo "ERROR: --urma-event-server requires a value" >&2; exit 2; }
            URMA_EVENT_SERVER=$2
            shift 2
            ;;
        --urma-poll-server)
            (($# >= 2)) || { echo "ERROR: --urma-poll-server requires a value" >&2; exit 2; }
            URMA_POLL_SERVER=$2
            shift 2
            ;;
        --mode)
            (($# >= 2)) || { echo "ERROR: --mode requires a value" >&2; exit 2; }
            MODE=$2
            shift 2
            ;;
        --test-seconds)
            (($# >= 2)) || { echo "ERROR: --test-seconds requires a value" >&2; exit 2; }
            TEST_SECONDS=$2
            shift 2
            ;;
        --repeats)
            (($# >= 2)) || { echo "ERROR: --repeats requires a value" >&2; exit 2; }
            REPEATS=$2
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

require_positive_integer "--test-seconds" "${TEST_SECONDS}"
require_positive_integer "--repeats" "${REPEATS}"
case "${MODE}" in
    tcp|urma_event|urma_poll|all) ;;
    *)
        echo "ERROR: --mode must be tcp, urma_event, urma_poll, or all: ${MODE}" >&2
        exit 2
        ;;
esac

if [[ ! -x ${CLIENT_BIN} ]]; then
    echo "ERROR: client is not executable: ${CLIENT_BIN}" >&2
    exit 2
fi

mkdir -p "${OUTPUT_DIR}" || exit 2
CSV_FILE="${OUTPUT_DIR}/results.csv"
echo "run,mode,server,transport,polling,payload,avg_us,min_us,p50_us,p90_us,p99_us,p999_us,max_us,rps,requests,errors" > "${CSV_FILE}"

MODE_NAMES=(tcp urma_event urma_poll)
MODE_SERVERS=("${TCP_SERVER}" "${URMA_EVENT_SERVER}" "${URMA_POLL_SERVER}")
MODE_USE_URMA=(false true true)
MODE_POLLING=(false false true)
failures=0

for payload in "${PAYLOAD_SIZES[@]}"; do
    for mode_index in "${!MODE_NAMES[@]}"; do
        mode=${MODE_NAMES[mode_index]}
        if [[ ${MODE} != all && ${mode} != "${MODE}" ]]; then
            continue
        fi
        server=${MODE_SERVERS[mode_index]}
        use_urma=${MODE_USE_URMA[mode_index]}
        polling=${MODE_POLLING[mode_index]}
        for ((run = 1; run <= REPEATS; ++run)); do
            log_file="${OUTPUT_DIR}/${mode}_payload-${payload}_run-${run}.log"
            command=(
                "${CLIENT_BIN}"
                "--server=${server}"
                "--test_seconds=${TEST_SECONDS}"
                "--attachment_size=${payload}"
                "--use_urma=${use_urma}"
                "--urma_use_polling=${polling}"
                "${EXTRA_ARGS[@]}"
            )

            echo "Running mode=${mode} server=${server} payload=${payload} run=${run}/${REPEATS}"
            "${command[@]}" 2>&1 | tee "${log_file}"
            client_status=${PIPESTATUS[0]}

            result_count=$(grep -c '^RESULT ' "${log_file}" || true)
            if ((client_status != 0 || result_count != 1)); then
                echo "ERROR: mode=${mode} server=${server} payload=${payload} run=${run} status=${client_status} RESULT_lines=${result_count}" >&2
                ((failures += 1))
                continue
            fi

            grep '^RESULT ' "${log_file}" | awk -v run="${run}" -v mode="${mode}" -v server="${server}" '
                BEGIN { OFS = "," }
                {
                    delete value
                    for (i = 2; i <= NF; ++i) {
                        split($i, field, "=")
                        value[field[1]] = field[2]
                    }
                    print run, mode, server, value["transport"], value["polling"],
                          value["payload"], value["avg_us"], value["min_us"],
                          value["p50_us"], value["p90_us"], value["p99_us"],
                          value["p999_us"], value["max_us"], value["rps"],
                          value["requests"], value["errors"]
                }
            ' >> "${CSV_FILE}"
        done
    done
done

echo "Results: ${CSV_FILE}"
if ((failures > 0)); then
    echo "Completed with ${failures} failed run(s); inspect logs in ${OUTPUT_DIR}" >&2
    exit 1
fi
echo "All benchmark runs completed successfully"
