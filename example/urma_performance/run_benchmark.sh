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
URMA_POLL_SERVERS=()
MODE="all"
TEST_SECONDS=30
REPEATS=3
CLIENT_PROCESS_NUM=1
OUTPUT_DIR="${SCRIPT_DIR}/benchmark-results/$(date +%Y%m%d-%H%M%S)"
PAYLOAD_SIZES=(16 64 256 1024 4096 8192 102400 204800 1048576 8388608)
THREAD_NUMS=(1 4 8)
QUEUE_DEPTHS=(1)
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
    echo "  --urma-poll-servers LIST"
    echo "                         Comma-separated URMA Poll servers; one per client process"
    echo "  --mode MODE            tcp, urma_event, urma_poll, or all (default: ${MODE})"
    echo "  --test-seconds N       Duration of each run (default: ${TEST_SECONDS})"
    echo "  --repeats N            Repetitions per configuration (default: ${REPEATS})"
    echo "  --client-process-num N Concurrent client processes per run"
    echo "                         (default: ${CLIENT_PROCESS_NUM})"
    echo "  --payload-sizes LIST   Comma-separated byte sizes"
    echo "                         (default: ${PAYLOAD_SIZES[*]})"
    echo "  --thread-nums LIST     Comma-separated client concurrency values"
    echo "                         (default: ${THREAD_NUMS[*]})"
    echo "  --queue-depths LIST    Comma-separated per-thread queue depths"
    echo "                         (default: ${QUEUE_DEPTHS[*]})"
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
        --urma-poll-servers)
            (($# >= 2)) || { echo "ERROR: --urma-poll-servers requires a value" >&2; exit 2; }
            if [[ ! $2 =~ ^[^,]+(,[^,]+)*$ ]]; then
                echo "ERROR: --urma-poll-servers must be a comma-separated list of non-empty addresses: $2" >&2
                exit 2
            fi
            IFS=',' read -r -a URMA_POLL_SERVERS <<< "$2"
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
        --client-process-num)
            (($# >= 2)) || { echo "ERROR: --client-process-num requires a value" >&2; exit 2; }
            CLIENT_PROCESS_NUM=$2
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
        --queue-depths)
            (($# >= 2)) || { echo "ERROR: --queue-depths requires a value" >&2; exit 2; }
            require_positive_integer_list "--queue-depths" "$2"
            IFS=',' read -r -a QUEUE_DEPTHS <<< "$2"
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
require_positive_integer "--client-process-num" "${CLIENT_PROCESS_NUM}"
if ((${#URMA_POLL_SERVERS[@]} > 0)); then
    if ((${#URMA_POLL_SERVERS[@]} != CLIENT_PROCESS_NUM)); then
        echo "ERROR: --urma-poll-servers must contain exactly ${CLIENT_PROCESS_NUM} addresses, one per client process" >&2
        exit 2
    fi
    for server in "${URMA_POLL_SERVERS[@]}"; do
        if [[ -z ${server} || ${server} == *,* ]]; then
            echo "ERROR: --urma-poll-servers contains an empty or invalid address" >&2
            exit 2
        fi
    done
fi
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
        --queue_depth|--queue_depth=*|\
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
pids=()

terminate_active_clients() {
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
    exit 130
}

trap terminate_active_clients INT TERM

for mode_index in "${!MODE_NAMES[@]}"; do
    mode=${MODE_NAMES[mode_index]}
    if [[ ${MODE} != all && ${mode} != "${MODE}" ]]; then
        continue
    fi
    server=${MODE_SERVERS[mode_index]}
    mode_servers=("${server}")
    if [[ ${mode} == urma_poll && ${#URMA_POLL_SERVERS[@]} -gt 0 ]]; then
        mode_servers=("${URMA_POLL_SERVERS[@]}")
        printf -v server '%s+' "${mode_servers[@]}"
        server=${server%+}
    fi
    use_urma=${MODE_USE_URMA[mode_index]}
    polling=${MODE_POLLING[mode_index]}
    csv_file="${OUTPUT_DIR}/results_${mode}.csv"
    summary_file="${OUTPUT_DIR}/summary_${mode}.csv"
    echo "run,mode,server,transport,polling,io_size_byte,client_process_num,client_thread_num,queue_depth,qps,avg_latency_us,min_latency_us,p50_latency_us,p90_latency_us,p99_latency_us,p999_latency_us,max_latency_us,server_cpu_percent,client_cpu_percent,throughput_mb_s,requests,errors,server_cpu_samples,client_cpu_samples,status" > "${csv_file}"

    for payload in "${PAYLOAD_SIZES[@]}"; do
        for thread_num in "${THREAD_NUMS[@]}"; do
            for queue_depth in "${QUEUE_DEPTHS[@]}"; do
                for ((run = 1; run <= REPEATS; ++run)); do
                    echo "Running mode=${mode} server=${server} payload=${payload} processes=${CLIENT_PROCESS_NUM} threads_per_process=${thread_num} queue_depth=${queue_depth} run=${run}/${REPEATS}"
                    pids=()
                    process_logs=()
                    for ((process_index = 1; process_index <= CLIENT_PROCESS_NUM; ++process_index)); do
                        process_server=${mode_servers[0]}
                        if ((${#mode_servers[@]} > 1)); then
                            process_server=${mode_servers[process_index - 1]}
                        fi
                        command=(
                            "${CLIENT_BIN}"
                            "--server=${process_server}"
                            "--test_seconds=${TEST_SECONDS}"
                            "--attachment_size=${payload}"
                            "--thread_num=${thread_num}"
                            "--queue_depth=${queue_depth}"
                            "--use_urma=${use_urma}"
                            "--urma_use_polling=${polling}"
                            "${EXTRA_ARGS[@]}"
                        )
                        log_file="${OUTPUT_DIR}/${mode}_payload-${payload}_processes-${CLIENT_PROCESS_NUM}_threads-${thread_num}_queue-depth-${queue_depth}_run-${run}_process-${process_index}.log"
                        process_logs+=("${log_file}")
                        "${command[@]}" > "${log_file}" 2>&1 &
                        pids+=("$!")
                    done

                    process_failed=0
                    result_lines=()
                    for process_index in "${!pids[@]}"; do
                        client_status=0
                        wait "${pids[process_index]}" || client_status=$?
                        log_file=${process_logs[process_index]}
                        result_count=$(grep -c '^RESULT ' "${log_file}" || true)
                        if ((client_status != 0 || result_count != 1)); then
                            process_server=${mode_servers[0]}
                            if ((${#mode_servers[@]} > 1)); then
                                process_server=${mode_servers[process_index]}
                            fi
                            echo "ERROR: mode=${mode} server=${process_server} payload=${payload} processes=${CLIENT_PROCESS_NUM} threads_per_process=${thread_num} queue_depth=${queue_depth} run=${run} process=$((process_index + 1)) status=${client_status} RESULT_lines=${result_count}; inspect ${log_file}" >&2
                            process_failed=1
                            continue
                        fi
                        result_lines+=("$(grep '^RESULT ' "${log_file}")")
                    done
                    pids=()
                    if ((process_failed != 0)); then
                        ((failures += 1))
                        continue
                    fi

                    result_row=$(printf '%s\n' "${result_lines[@]}" | awk \
                        -v run="${run}" \
                        -v mode="${mode}" \
                        -v server="${server}" \
                        -v client_process_num="${CLIENT_PROCESS_NUM}" \
                        -v expected_thread_num="${thread_num}" \
                        -v queue_depth="${queue_depth}" '
                    BEGIN { OFS = "," }
                    {
                        delete value
                        for (i = 2; i <= NF; ++i) {
                            split($i, field, "=")
                            value[field[1]] = field[2]
                        }
                        if (record_count == 0) {
                            transport = value["transport"]
                            polling = value["polling"]
                            payload = value["payload"]
                            client_thread_num = value["thread_num"]
                            min_latency = value["min_us"] + 0
                            max_latency = value["max_us"] + 0
                        } else if (transport != value["transport"] ||
                                   polling != value["polling"] ||
                                   payload != value["payload"] ||
                                   client_thread_num != value["thread_num"]) {
                            inconsistent = 1
                        }
                        requests = value["requests"] + 0
                        server_samples = value["server_cpu_samples"] + 0
                        total_qps += value["rps"] + 0
                        total_throughput += value["throughput_mb_s"] + 0
                        total_requests += requests
                        total_errors += value["errors"] + 0
                        weighted_latency += (value["avg_us"] + 0) * requests
                        percentile_50 += value["p50_us"] + 0
                        percentile_90 += value["p90_us"] + 0
                        percentile_99 += value["p99_us"] + 0
                        percentile_999 += value["p999_us"] + 0
                        if ((value["min_us"] + 0) < min_latency) {
                            min_latency = value["min_us"] + 0
                        }
                        if ((value["max_us"] + 0) > max_latency) {
                            max_latency = value["max_us"] + 0
                        }
                        weighted_server_cpu += (value["server_cpu_percent"] + 0) * server_samples
                        total_server_samples += server_samples
                        total_client_cpu += value["client_cpu_percent"] + 0
                        total_client_samples += value["client_cpu_samples"] + 0
                        ++record_count
                    }
                    END {
                        avg_latency = total_requests > 0 ? weighted_latency / total_requests : 0
                        avg_server_cpu = total_server_samples > 0 ? weighted_server_cpu / total_server_samples : 0
                        avg_client_cpu = record_count > 0 ? total_client_cpu / record_count : 0
                        status = (record_count == client_process_num &&
                                  !inconsistent &&
                                  client_thread_num == expected_thread_num &&
                                  total_errors == 0 &&
                                  total_server_samples > 0 &&
                                  total_client_samples > 0) ? "ok" : "failed"
                        print run, mode, server, transport, polling,
                              payload, client_process_num, client_thread_num,
                              queue_depth, total_qps, avg_latency, min_latency,
                              percentile_50 / record_count,
                              percentile_90 / record_count,
                              percentile_99 / record_count,
                              percentile_999 / record_count,
                              max_latency, avg_server_cpu, avg_client_cpu,
                              total_throughput, total_requests, total_errors,
                              total_server_samples, total_client_samples, status
                    }
                    ')
                    echo "${result_row}" >> "${csv_file}"
                    if [[ ${result_row##*,} != ok ]]; then
                        echo "ERROR: invalid aggregated metrics for mode=${mode} payload=${payload} processes=${CLIENT_PROCESS_NUM} threads_per_process=${thread_num} queue_depth=${queue_depth} run=${run}; check errors and CPU sample counts" >&2
                        ((failures += 1))
                    else
                        echo "Aggregated result: ${result_row}"
                    fi
                done
            done
        done
    done

    awk -F, -v expected_repeats="${REPEATS}" '
        BEGIN {
            OFS = ","
            print "io_size_byte", "client_process_num", "client_thread_num", \
                  "queue_depth", "protocol", "qps", \
                  "avg_latency_us", "p99_latency_us", "server_cpu_percent", \
                  "client_cpu_percent", "throughput_mb_s", "successful_repeats"
        }
        NR == 1 || $25 != "ok" { next }
        {
            key = $6 SUBSEP $7 SUBSEP $8 SUBSEP $9
            if (!(key in seen)) {
                seen[key] = 1
                order[++group_count] = key
                payload[key] = $6
                processes[key] = $7
                threads[key] = $8
                queue_depth[key] = $9
                protocol[key] = $2
            }
            count[key]++
            qps[key, count[key]] = $10
            avg[key, count[key]] = $11
            p99[key, count[key]] = $15
            server_cpu[key, count[key]] = $18
            client_cpu[key, count[key]] = $19
            throughput[key, count[key]] = $20
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
                printf "%s,%s,%s,%s,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d\n", \
                       payload[key], processes[key], threads[key], queue_depth[key], \
                       protocol[key], \
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
            print "io_size(Byte)", "client_process_num", "client_thread_num", \
                  "queue_depth", \
                  "TCP QPS(sum)", "TCP avg_latency_us", "TCP p99_latency_us", \
                  "TCP server_cpu_percent", "TCP client_cpu_percent", "TCP throughput_MB/s", \
                  "URMA Event QPS(sum)", "URMA Event avg_latency_us", "URMA Event p99_latency_us", \
                  "URMA Event server_cpu_percent", "URMA Event client_cpu_percent", "URMA Event throughput_MB/s", \
                  "URMA Poll QPS(sum)", "URMA Poll avg_latency_us", "URMA Poll p99_latency_us", \
                  "URMA Poll server_cpu_percent", "URMA Poll client_cpu_percent", "URMA Poll throughput_MB/s"
        }
        FNR == 1 { next }
        {
            key = $1 SUBSEP $2 SUBSEP $3 SUBSEP $4
            mode = $5
            data[key, mode] = $6 OFS $7 OFS $8 OFS $9 OFS $10 OFS $11
            if (FILENAME == tcp_file) {
                order[++row_count] = key
                payload[key] = $1
                processes[key] = $2
                threads[key] = $3
                queue_depth[key] = $4
            }
        }
        END {
            for (i = 1; i <= row_count; ++i) {
                key = order[i]
                print payload[key], processes[key], threads[key], queue_depth[key], \
                      data[key, "tcp"], \
                      data[key, "urma_event"], data[key, "urma_poll"]
            }
        }
    ' "${tcp_summary}" "${event_summary}" "${poll_summary}" > "${comparison_file}"
    echo "Protocol comparison: ${comparison_file}"
fi
echo "All benchmark runs completed successfully"
