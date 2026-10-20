"""Exercise test.sh with a stub CTest; these are not JNI/TCP integration tests."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("bash"),
                     "requires Linux and Bash; no real JNI/TCP tests are run here")
class TestRunnerTests(unittest.TestCase):
    def run_runner(self, output, status=0):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="echo-test-runner-") as directory:
            temporary = Path(directory)
            build = temporary / "build"
            (build / "classes").mkdir(parents=True)
            (build / "libbrpc_echo_jni.so").touch()
            binary = temporary / "bin"
            binary.mkdir()
            ctest = binary / "ctest"
            ctest.write_text("#!/usr/bin/env bash\n"
                             "printf '%s\\n' \"$ECHO_STUB_OUTPUT\"\n"
                             "exit \"$ECHO_STUB_STATUS\"\n", encoding="utf-8")
            ctest.chmod(0o755)
            environment = dict(os.environ, ECHO_BUILD_DIR=str(build),
                               ECHO_STUB_OUTPUT=output, ECHO_STUB_STATUS=str(status),
                               PATH=str(binary) + os.pathsep + os.environ.get("PATH", ""))
            return subprocess.run(["bash", str(root / "scripts/test.sh")],
                                  env=environment, capture_output=True, text=True, timeout=10)

    def assert_rejected(self, output, status=0):
        result = self.run_runner(output, status)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("TCP_CONTRACT=PASS", result.stdout + result.stderr)

    def test_zero_tests_are_rejected(self):
        self.assert_rejected("No tests were found!!!")

    def test_missing_java_test_is_rejected(self):
        self.assert_rejected("1: HISTOGRAM_TEST=PASS")

    def test_missing_native_test_is_rejected(self):
        self.assert_rejected("2: CONTRACT_TEST=PASS transport=tcp (not URMA validation)")

    def test_disabled_tests_are_rejected(self):
        self.assert_rejected("1/2 Test #1: native_histogram .... Not Run (Disabled)\n"
                             "2/2 Test #2: java_tcp_contract .. Not Run (Disabled)")

    def test_nonzero_status_is_rejected_even_with_markers(self):
        self.assert_rejected("1: HISTOGRAM_TEST=PASS\n"
                             "2: CONTRACT_TEST=PASS transport=tcp (not URMA validation)", 8)

    def test_both_markers_and_zero_status_are_required(self):
        result = self.run_runner("1: HISTOGRAM_TEST=PASS\n"
                                 "2: CONTRACT_TEST=PASS transport=tcp (not URMA validation)")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TCP_CONTRACT=PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
