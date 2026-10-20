#!/usr/bin/env python3
"""Compare one matched closed-loop run pair; never assert URMA path verification."""
import argparse
import json
import math
from pathlib import Path


MATCH_FIELDS = (
    "schema", "build_id", "transport_requested", "load_model", "percentiles", "polling",
    "size", "concurrency", "capacity", "timeout_ms", "warmup_seconds", "requested_seconds",
)


def load(path):
    with Path(path).open(encoding="utf-8") as stream:
        record = json.load(stream)
    validate(record)
    return record


def validate(record):
    if not isinstance(record, dict) or record.get("schema") != 1:
        raise ValueError("not a schema=1 result")
    build_id = record.get("build_id", "")
    if len(build_id) != 64 or any(c not in "0123456789abcdef" for c in build_id):
        raise ValueError("missing or malformed build_id")
    if record.get("group") not in ("N", "C", "S", "J"):
        raise ValueError("invalid group")
    expected_client = "cpp" if record["group"] in ("N", "S") else "java"
    if record.get("client") != expected_client:
        raise ValueError("group/client mismatch")
    if record.get("transport_requested") not in ("tcp", "urma"):
        raise ValueError("unknown requested transport")
    if record.get("path_verified") is not False:
        raise ValueError("raw prototype results must retain path_verified=false; keep independent evidence separately")
    if record.get("load_model") != "closed_loop":
        raise ValueError("this reporter supports closed_loop only")
    if record.get("percentiles") != "log_histogram_upper_bound":
        raise ValueError("unknown percentile estimator")
    if type(record.get("polling")) is not bool:
        raise ValueError("polling must be boolean")
    for field in ("size", "concurrency", "capacity", "timeout_ms", "warmup_seconds",
                  "requested_seconds", "elapsed_ns", "success", "failures"):
        value = record.get(field)
        if type(value) is not int or value < 0:
            raise ValueError("invalid " + field)
    if min(record["concurrency"], record["capacity"], record["timeout_ms"],
           record["requested_seconds"], record["elapsed_ns"]) <= 0:
        raise ValueError("zero concurrency/capacity/timeout/duration")
    if record["capacity"] < record["concurrency"]:
        raise ValueError("capacity below concurrency")
    expected_qps = record["success"] * 1e9 / record["elapsed_ns"]
    qps = record.get("qps")
    if type(qps) not in (int, float) or not math.isfinite(qps) or not math.isclose(qps, expected_qps, rel_tol=1e-8, abs_tol=1e-8):
        raise ValueError("qps inconsistent with success/elapsed_ns")
    percentiles = [record.get(key) for key in ("p50_ns", "p95_ns", "p99_ns")]
    if record["success"]:
        if any(type(value) is not int or value < 1 for value in percentiles) or percentiles != sorted(percentiles):
            raise ValueError("invalid percentile values/order")
    elif percentiles != [None, None, None]:
        raise ValueError("zero-success run must have null percentiles")
    cpu = record.get("client_cpu_ns")
    if cpu is not None and (type(cpu) is not int or cpu < 0):
        raise ValueError("invalid CPU time")


def compare(baseline, candidate):
    validate(baseline)
    validate(candidate)
    if baseline["group"] != "N" or candidate["group"] not in ("C", "S", "J"):
        raise ValueError("compare requires baseline N and candidate C, S, or J")
    differences = [field for field in MATCH_FIELDS if baseline[field] != candidate[field]]
    if differences:
        raise ValueError("unmatched runs: " + ", ".join(differences))
    if not baseline["success"] or not candidate["success"]:
        raise ValueError("cannot estimate overhead with zero successful RPCs")
    output = {
        "baseline": "N", "candidate": candidate["group"],
        "transport_requested": baseline["transport_requested"],
        "transport_path_verified": False,
        "business_acceptance_evaluated": False,
        "load_model": "closed_loop",
        "clean_runs": baseline["failures"] == 0 and candidate["failures"] == 0,
        "observed_qps_change_pct": (candidate["qps"] / baseline["qps"] - 1) * 100,
        "interpretation": "Descriptive closed-loop differences only; not fixed-arrival-load latency or SLO-constrained maximum-throughput loss.",
        "warnings": [
            "Requested transport is not data-path proof; attach real-library/device/handshake/traffic evidence separately.",
            "Repeat each point and report run-to-run spread; this is one pair only.",
            "Server CPU, GC, memory stability and fixed-arrival-load results are not measured by this client report.",
        ],
    }
    for key in ("p50_ns", "p95_ns", "p99_ns"):
        output[key + "_delta"] = candidate[key] - baseline[key]
        output[key + "_increase_pct"] = (candidate[key] / baseline[key] - 1) * 100
    for name, record in (("baseline", baseline), ("candidate", candidate)):
        output[name + "_error_rate"] = record["failures"] / (record["success"] + record["failures"])
        output[name + "_client_cpu_ns_per_success"] = (
            record["client_cpu_ns"] / record["success"] if record["client_cpu_ns"] is not None else None
        )
    b_cpu, c_cpu = output["baseline_client_cpu_ns_per_success"], output["candidate_client_cpu_ns_per_success"]
    output["client_cpu_cost_increase_pct"] = (c_cpu / b_cpu - 1) * 100 if b_cpu and c_cpu is not None else None
    if not output["clean_runs"]:
        output["warnings"].append("Runs contain failures; successful-call latency alone hides failed requests.")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    args = parser.parse_args()
    try:
        output = compare(load(args.baseline), load(args.candidate))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, "COMPARE=FAIL " + str(error) + "\n")
    print(json.dumps(output, ensure_ascii=False, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
