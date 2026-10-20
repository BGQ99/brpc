#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
require_build
role=${1:?Usage: run-client.sh native|java --group=N|C|S|J --server=IP:PORT}
shift
case "$role" in
    native) exec "$BUILD_DIR/echo_native_client" "$@" ;;
    java) exec "$JAVA_BIN" "-Djava.library.path=$BUILD_DIR" -cp "$BUILD_DIR/classes" prototype.echo.BenchClient "$@" ;;
    *) printf 'role must be native or java\n' >&2; exit 1 ;;
esac
