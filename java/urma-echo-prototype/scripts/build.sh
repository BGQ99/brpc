#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
[[ $(uname -s) == Linux ]] || { printf 'Linux build required\n' >&2; exit 1; }
command -v cmake >/dev/null || { printf 'cmake not found\n' >&2; exit 1; }
if [[ -z ${JAVA_HOME:-} ]]; then
    command -v javac >/dev/null || { printf 'JDK javac not found\n' >&2; exit 1; }
    JAVA_HOME=$(dirname -- "$(dirname -- "$(readlink -f -- "$(command -v javac)")")")
    export JAVA_HOME
fi
[[ -x "$JAVA_HOME/bin/javac" && -f "$JAVA_HOME/include/jni.h" ]] || {
    printf 'JAVA_HOME must identify an existing development JDK\n' >&2; exit 1;
}
export PATH="$JAVA_HOME/bin:$PATH"
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON "$@"
cmake --build "$BUILD_DIR" --parallel "${ECHO_BUILD_JOBS:-4}"
printf 'BUILD=PASS (compilation only; no TCP/URMA runtime test has run)\n'
