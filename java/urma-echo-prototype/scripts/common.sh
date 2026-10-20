#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${ECHO_BUILD_DIR:-"$ROOT/build"}
if [[ -n ${JAVA_HOME:-} ]]; then
    JAVA_BIN="$JAVA_HOME/bin/java"
else
    JAVA_BIN=java
fi
require_build() {
    [[ -f "$BUILD_DIR/libbrpc_echo_jni.so" && -d "$BUILD_DIR/classes" ]] || {
        printf 'Run scripts/build.sh first. Missing prototype build in %s\n' "$BUILD_DIR" >&2
        exit 1
    }
}
