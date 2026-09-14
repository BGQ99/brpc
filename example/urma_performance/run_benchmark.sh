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
REPEATS=3
OUTPUT_DIR="${SCRIPT_DIR}/benchmark-results/$(date +%Y%m%d-%H%M%S)"
PAYLOAD_SIZES=(16 64 256 1024 4096 8192 1048576 8388608)
THREAD_NUMS=(1 4 8)
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
    echo "  --payload-sizes LIST   Comma-separated byte sizes"
    echo "                         (default: ${PAYLOAD_SIZES[*]})"
    echo "  --thread-nums LIST     Comma-separated client concurrency values"
    echo "                         (default: ${THREAD_NUMS[*]})"
    echo "  --output-dir PATH      Log and CSV output directory"
    echo "  -h, --help             Show this help"
}

require_positive_integer_list() {
    local name=$1
    local value=$2
    if [[ ! ${value} =~ ^[1-9][0-9]*(,[1-9][0-9]*)*$ ]]; then
        echo "ERROR: ${name} must be a comma-separated list of positive integers: ${value}" >&2
        exit 2
    fi
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
        --payload-sizes)
            (($# >= 2)) || { echo "ERROR: --payload-sizes requires a value" >&2; exit 2; }
            require_positive_integer_list "--payload-sizes" "$2"
            IFS=',' read -r -a PAYLOAD_SIZES <<< "$2"
            shift 2
            ;;
        --thread-nums)
            (($# >= 2)) || { echo "ERROR: --thread-nums requires a value" >&2; exit 2; }
            require_positive_integer_list "--thread-nums" "$2"
            IFS=',' read -r -a THREAD_NUMS <<< "$2"
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

for arg in "${EXTRA_ARGS[@]}"; do
    case "${arg}" in
        --server|--server=*|--test_seconds|--test_seconds=*|\
        --attachment_size|--attachment_size=*|--thread_num|--thread_num=*|\
        --use_urma|--use_urma=*|--urma_use_polling|--urma_use_polling=*)
            echo "ERROR: runner-managed client flag must not appear after --: ${arg}" >&2
            exit 2
            ;;
    esac
done

if [[ ! -x ${CLIENT_BIN} ]]; then
    echo "ERROR: client is not executable: ${CLIENT_BIN}" >&2
    exit 2
fi

mkdir -p "${OUTPUT_DIR}" || exit 2

MODE_NAMES=(tcp urma_event urma_poll)
MODE_SERVERS=("${TCP_SERVER}" "${URMA_EVENT_SERVER}" "${URMA_POLL_SERVER}")
MODE_USE_URMA=(false true true)
MODE_POLLING=(false false true)
failures=0

for mode_index in "${!MODE_NAMES[@]}"; do
    mode=${MODE_NAMES[mode_index]}
    if [[ ${MODE} != all && ${mode} != "${MODE}" ]]; then
        continue
    fi
    server=${MODE_SERVERS[mode_index]}
    use_urma=${MODE_USE_URMA[mode_index]}
    polling=${MODE_POLLING[mode_index]}
    csv_file="${OUTPUT_DIR}/results_${mode}.csv"
    summary_file="${OUTPUT_DIR}/summary_${mode}.csv"
    echo "run,mode,server,transport,polling,io_size_byte,client_process_num,qps,avg_latency_us,min_latency_us,p50_latency_us,p90_latency_us,p99_latency_us,p999_latency_us,max_latency_us,server_cpu_percent,client_cpu_percent,throughput_mb_s,requests,errors,server_cpu_samples,client_cpu_samples,status" > "${csv_file}"

    for payload in "${PAYLOAD_SIZES[@]}"; do
        for thread_num in "${THREAD_NUMS[@]}"; do
            for ((run = 1; run <= REPEATS; ++run)); do
                log_file="${OUTPUT_DIR}/${mode}_payload-${payload}_threads-${thread_num}_run-${run}.log"
                command=(
                    "${CLIENT_BIN}"
                    "--server=${server}"
                    "--test_seconds=${TEST_SECONDS}"
                    "--attachment_size=${payload}"
                    "--thread_num=${thread_num}"
                    "--use_urma=${use_urma}"
                    "--urma_use_polling=${polling}"
                    "${EXTRA_ARGS[@]}"
                )

                echo "Running mode=${mode} server=${server} payload=${payload} threads=${thread_num} run=${run}/${REPEATS}"
                "${command[@]}" 2>&1 | tee "${log_file}"
                client_status=${PIPESTATUS[0]}

                result_count=$(grep -c '^RESULT ' "${log_file}" || true)
                if ((client_status != 0 || result_count != 1)); then
                    echo "ERROR: mode=${mode} server=${server} payload=${payload} threads=${thread_num} run=${run} status=${client_status} RESULT_lines=${result_count}" >&2
                    ((failures += 1))
                    continue
                fi

                result_row=$(grep '^RESULT ' "${log_file}" | awk -v run="${run}" -v mode="${mode}" -v server="${server}" '
                    BEGIN { OFS = "," }
                    {
                        delete value
                        for (i = 2; i <= NF; ++i) {
                            split($i, field, "=")
                            value[field[1]] = field[2]
                        }
                        status = (value["errors"] == 0 &&
                                  value["server_cpu_samples"] > 0 &&
                                  value["client_cpu_samples"] > 0) ? "ok" : "failed"
                        print run, mode, server, value["transport"], value["polling"],
                              value["payload"], value["thread_num"], value["rps"],
                              value["avg_us"], value["min_us"], value["p50_us"],
                              value["p90_us"], value["p99_us"], value["p999_us"],
                              value["max_us"], value["server_cpu_percent"],
                              value["client_cpu_percent"], value["throughput_mb_s"],
                              value["requests"], value["errors"],
                              value["server_cpu_samples"], value["client_cpu_samples"],
                              status
                    }
                ')
                echo "${result_row}" >> "${csv_file}"
                if [[ ${result_row##*,} != ok ]]; then
                    echo "ERROR: invalid metrics for mode=${mode} payload=${payload} threads=${thread_num} run=${run}; check errors and CPU sample counts" >&2
                    ((failures += 1))
                fi
            done
        done
    done

    awk -F, -v expected_repeats="${REPEATS}" '
        BEGIN {
            OFS = ","
            print "io_size_byte", "client_process_num", "protocol", "qps", \
                  "avg_latency_us", "p99_latency_us", "server_cpu_percent", \
                  "client_cpu_percent", "throughput_mb_s", "successful_repeats"
        }
        NR == 1 || $23 != "ok" { next }
        {
            key = $6 SUBSEP $7
            if (!(key in seen)) {
                seen[key] = 1
                order[++group_count] = key
                payload[key] = $6
                threads[key] = $7
                protocol[key] = $2
            }
            count[key]++
            qps[key, count[key]] = $8
            avg[key, count[key]] = $9
            p99[key, count[key]] = $13
            server_cpu[key, count[key]] = $16
            client_cpu[key, count[key]] = $17
            throughput[key, count[key]] = $18
        }
        function median(metric, key, n, values, i, j, tmp) {
            delete values
            for (i = 1; i <= n; ++i) {
                values[i] = metric[key, i] + 0
            }
            for (i = 2; i <= n; ++i) {
                tmp = values[i]
                j = i - 1
                while (j >= 1 && values[j] > tmp) {
                    values[j + 1] = values[j]
                    --j
                }
                values[j + 1] = tmp
            }
            if (n % 2 == 1) {
                return values[(n + 1) / 2]
            }
            return (values[n / 2] + values[n / 2 + 1]) / 2
        }
        END {
            for (i = 1; i <= group_count; ++i) {
                key = order[i]
                n = count[key]
                if (n != expected_repeats) {
                    continue
                }
                printf "%s,%s,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d\n", \
                       payload[key], threads[key], protocol[key], \
                       median(qps, key, n), median(avg, key, n), \
                       median(p99, key, n), median(server_cpu, key, n), \
                       median(client_cpu, key, n), median(throughput, key, n), n
            }
        }
    ' "${csv_file}" > "${summary_file}"

    echo "Raw results: ${csv_file}"
    echo "Median summary: ${summary_file}"
done

if ((failures > 0)); then
    echo "Completed with ${failures} failed run(s); inspect logs in ${OUTPUT_DIR}" >&2
    exit 1
fi

tcp_summary="${OUTPUT_DIR}/summary_tcp.csv"
event_summary="${OUTPUT_DIR}/summary_urma_event.csv"
poll_summary="${OUTPUT_DIR}/summary_urma_poll.csv"
comparison_file="${OUTPUT_DIR}/comparison.csv"
if [[ -f ${tcp_summary} && -f ${event_summary} && -f ${poll_summary} ]]; then
    awk -F, -v tcp_file="${tcp_summary}" '
        BEGIN {
            OFS = ","
            print "io_size(Byte)", "client_process_num", \
                  "TCP QPS(sum)", "TCP avg_latency_us", "TCP p99_latency_us", \
                  "TCP server_cpu_percent", "TCP client_cpu_percent", "TCP throughput_MB/s", \
                  "URMA Event QPS(sum)", "URMA Event avg_latency_us", "URMA Event p99_latency_us", \
                  "URMA Event server_cpu_percent", "URMA Event client_cpu_percent", "URMA Event throughput_MB/s", \
                  "URMA Poll QPS(sum)", "URMA Poll avg_latency_us", "URMA Poll p99_latency_us", \
                  "URMA Poll server_cpu_percent", "URMA Poll client_cpu_percent", "URMA Poll throughput_MB/s"
        }
        FNR == 1 { next }
        {
            key = $1 SUBSEP $2
            mode = $3
            data[key, mode] = $4 OFS $5 OFS $6 OFS $7 OFS $8 OFS $9
            if (FILENAME == tcp_file) {
                order[++row_count] = key
                payload[key] = $1
                threads[key] = $2
            }
        }
        END {
            for (i = 1; i <= row_count; ++i) {
                key = order[i]
                print payload[key], threads[key], data[key, "tcp"], \
                      data[key, "urma_event"], data[key, "urma_poll"]
            }
        }
    ' "${tcp_summary}" "${event_summary}" "${poll_summary}" > "${comparison_file}"
    echo "Protocol comparison: ${comparison_file}"
fi
echo "All benchmark runs completed successfully"
