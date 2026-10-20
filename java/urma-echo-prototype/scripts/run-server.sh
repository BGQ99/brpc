#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
require_build
role=${1:?Usage: run-server.sh native|java --port=8000 --transport=tcp|urma}
shift
case "$role" in
    native) exec "$BUILD_DIR/echo_native_server" "$@" ;;
    java) exec "$JAVA_BIN" "-Djava.library.path=$BUILD_DIR" -cp "$BUILD_DIR/classes" prototype.echo.ServerMain "$@" ;;
    *) printf 'role must be native or java\n' >&2; exit 1 ;;
esac
