#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
require_build
if test_output=$(
    cd -- "$BUILD_DIR" || exit 1
    ctest --verbose --output-on-failure 2>&1
); then
    printf '%s\n' "$test_output"
else
    test_status=$?
    printf '%s\n' "$test_output" >&2
    exit "$test_status"
fi
# CTest 3.16 can return success with no tests. Require both programs' markers.
if ! grep -Eq '^[0-9]+: HISTOGRAM_TEST=PASS$' <<< "$test_output" ||
   ! grep -Eq '^[0-9]+: CONTRACT_TEST=PASS transport=tcp \(not URMA validation\)$' <<< "$test_output"; then
    printf 'TCP_CONTRACT=FAIL required tests did not both report success\n' >&2
    exit 1
fi
printf 'TCP_CONTRACT=PASS (not URMA verification)\n'
