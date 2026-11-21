#!/bin/bash
# Build (or flash, monitor, menuconfig...) the firmware for one board.
#
#   tools/build.sh list                         boards you can build
#   tools/build.sh <board>                      build it
#   tools/build.sh <board> -p PORT flash monitor
#   tools/build.sh <board> menuconfig           change features for that board
#   tools/build.sh all                          build every board
#
# Each board keeps its own build folder (build-<board>/) and its own settings
# (build-<board>/sdkconfig), made from sdkconfig.defaults plus
# boards/<board>/sdkconfig.defaults. Delete build-<board>/sdkconfig to go
# back to the defaults after changing those files.
#
# EXTRA_DEFAULTS="file1;file2" adds more defaults files on top, and
# BUILD_DIR=folder builds somewhere other than build-<board>/. CI uses both
# to build with every optional feature switched off:
#   BUILD_DIR=build-minimal EXTRA_DEFAULTS=tools/ci/minimal.defaults \
#       tools/build.sh esp32-devkitc

set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

boards() { ls -d boards/*/ 2>/dev/null | xargs -n1 basename; }

usage() {
    echo "usage: tools/build.sh <board|all|list> [idf.py arguments]" >&2
    echo "boards: $(boards | tr '\n' ' ')" >&2
    exit 2
}

[ $# -ge 1 ] || usage
BOARD=$1
shift

if [ "$BOARD" = "list" ]; then
    boards
    exit 0
fi

if [ "$BOARD" = "all" ]; then
    [ -z "$BUILD_DIR" ] || { echo "BUILD_DIR cannot be used with 'all'" >&2; exit 2; }
    for b in $(boards); do
        echo "=== $b"
        "$0" "$b" "$@"
    done
    exit 0
fi

DEFAULTS="boards/$BOARD/sdkconfig.defaults"
[ -f "$DEFAULTS" ] || { echo "unknown board '$BOARD'" >&2; usage; }

TARGET=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' "$DEFAULTS")
[ -n "$TARGET" ] || { echo "$DEFAULTS does not set CONFIG_IDF_TARGET" >&2; exit 2; }

if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    . "$ROOT/tools/idf_env.sh"
fi

# idf.py refuses to build when an IDF_TARGET left in the environment (by
# export.sh, or the CI image) differs from the board's chip
export IDF_TARGET="$TARGET"

ALL_DEFAULTS="sdkconfig.defaults;$DEFAULTS"
[ -n "$EXTRA_DEFAULTS" ] && ALL_DEFAULTS="$ALL_DEFAULTS;$EXTRA_DEFAULTS"

[ $# -ge 1 ] || set -- build
BUILD="${BUILD_DIR:-build-$BOARD}"
exec idf.py -B "$BUILD" -D IDF_TARGET="$TARGET" \
    -D SDKCONFIG="$ROOT/$BUILD/sdkconfig" \
    -D SDKCONFIG_DEFAULTS="$ALL_DEFAULTS" \
    "$@"
