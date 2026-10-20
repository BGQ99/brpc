#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
uname -srmo
grep -E '^(NAME|VERSION|VERSION_ID)=' /etc/os-release
"$JAVA_BIN" -version 2>&1
if command -v javac >/dev/null 2>&1; then javac -version 2>&1; fi
for tool in cmake c++ protoc; do
    if command -v "$tool" >/dev/null 2>&1; then "$tool" --version 2>&1 | head -n 1; fi
done
if [[ -f "$BUILD_DIR/build-id.txt" ]]; then
    printf 'PROTOTYPE_BUILD_ID='
    cat "$BUILD_DIR/build-id.txt"
fi
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    grep -E '^(BRPC_LIBRARY|BRPC_INCLUDE_DIR|Protobuf_LIBRARY_RELEASE|Protobuf_PROTOC_EXECUTABLE|ECHO_ENABLE_URMA):' "$BUILD_DIR/CMakeCache.txt" || true
fi
if [[ -f "$BUILD_DIR/libbrpc_echo_jni.so" ]]; then
    ldd "$BUILD_DIR/libbrpc_echo_jni.so"
fi
if [[ -n ${BRPC_ROOT:-} ]] && git -C "$BRPC_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$BRPC_ROOT" rev-parse HEAD
    git -C "$BRPC_ROOT" status --short
fi
printf 'Environment inventory only; no URMA data-path verification.\n'
