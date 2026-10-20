import copy
import importlib.util
from pathlib import Path
import unittest

MODULE = Path(__file__).resolve().parents[1] / "tools" / "compare.py"
SPEC = importlib.util.spec_from_file_location("comparison", MODULE)
comparison = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(comparison)


def result(group="N"):
    return dict(schema=1, build_id="a" * 64, group=group,
                client="cpp" if group in ("N", "S") else "java",
                transport_requested="urma", path_verified=False,
                load_model="closed_loop", percentiles="log_histogram_upper_bound", polling=False,
                size=1024, concurrency=8, capacity=8, timeout_ms=1000, warmup_seconds=60,
                requested_seconds=180, elapsed_ns=180_000_000_000, success=180_000,
                failures=0, qps=1000.0, p50_ns=10_000, p95_ns=20_000, p99_ns=30_000,
                client_cpu_ns=90_000_000_000)


class ComparisonTests(unittest.TestCase):
    def test_positive_and_negative_changes(self):
        native, java = result(), result("J")
        java["p99_ns"] = 36_000
        java["p50_ns"] = 9_000
        java["client_cpu_ns"] = 135_000_000_000
        output = comparison.compare(native, java)
        self.assertAlmostEqual(output["p99_ns_increase_pct"], 20)
        self.assertAlmostEqual(output["p50_ns_increase_pct"], -10)
        self.assertAlmostEqual(output["client_cpu_cost_increase_pct"], 50)
        self.assertFalse(output["transport_path_verified"])
        self.assertFalse(output["business_acceptance_evaluated"])

    def test_unmatched_runs_rejected(self):
        for field, value in (("size", 64), ("concurrency", 1), ("build_id", "b" * 64),
                             ("transport_requested", "tcp"), ("polling", True), ("timeout_ms", 5000)):
            with self.subTest(field=field):
                java = result("C")
                java[field] = value
                with self.assertRaises(ValueError): comparison.compare(result(), java)

    def test_failures_are_not_silently_cleaned(self):
        java = result("J")
        java["failures"] = 200
        output = comparison.compare(result(), java)
        self.assertFalse(output["clean_runs"])
        self.assertGreater(output["candidate_error_rate"], 0)
        self.assertTrue(any("failures" in warning for warning in output["warnings"]))

    def test_zero_success_rejected(self):
        java = result("J")
        java.update(success=0, failures=10, qps=0.0, p50_ns=None, p95_ns=None, p99_ns=None)
        with self.assertRaises(ValueError): comparison.compare(result(), java)

    def test_forged_raw_path_claim_rejected(self):
        java = result("J")
        java["path_verified"] = True
        with self.assertRaises(ValueError): comparison.compare(result(), java)

    def test_malformed_qps_and_percentiles_rejected(self):
        for change in (dict(qps=2000), dict(qps=float("nan")), dict(p99_ns=1), dict(client_cpu_ns=-1)):
            with self.subTest(change=change):
                java = result("J")
                java.update(change)
                with self.assertRaises(ValueError): comparison.compare(result(), java)

    def test_missing_cpu_is_not_zero_cost(self):
        java = result("J")
        java["client_cpu_ns"] = None
        output = comparison.compare(result(), java)
        self.assertIsNone(output["candidate_client_cpu_ns_per_success"])
        self.assertIsNone(output["client_cpu_cost_increase_pct"])

    def test_baseline_role_required(self):
        with self.assertRaises(ValueError): comparison.compare(result("S"), result("J"))

    def test_inputs_not_modified(self):
        native, java = result(), result("C")
        original = copy.deepcopy((native, java))
        comparison.compare(native, java)
        self.assertEqual((native, java), original)


if __name__ == "__main__":
    unittest.main()
