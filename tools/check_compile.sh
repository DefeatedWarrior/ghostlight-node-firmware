#!/bin/bash
# Compile-only check of the firmware C sources (no build, no flash).
#
# Uses the ESP-IDF headers, generated sdkconfig.h and Xtensa GCC that ship
# with the Arduino-ESP32 core (3.3.x = ESP-IDF 5.5), and the same warning
# flags as an ESP-IDF build (-Wall -Werror=all -Wextra) at -O2, which is what
# CONFIG_COMPILER_OPTIMIZATION_PERF uses. It catches compile errors and the
# -O2-only warnings (stringop-truncation, maybe-uninitialized) before
# `idf.py build`. It does not link and does not replace a real build.
#
# Usage:  tools/check_compile.sh [file.c ...]     (default: all sources)
# Env:    ARDUINO_ESP32=~/Library/Arduino15/packages/esp32/tools

T=${ARDUINO_ESP32:-$HOME/Library/Arduino15/packages/esp32/tools}
L=$(ls -d "$T"/esp32s3-libs/* 2>/dev/null | sort -V | tail -1)
CC=$(ls "$T"/esp-x32/*/bin/xtensa-esp32s3-elf-gcc 2>/dev/null | sort -V | tail -1)
if [ -z "$L" ] || [ -z "$CC" ]; then
    echo "Arduino-ESP32 esp32s3-libs / xtensa toolchain not found under $T" >&2
    exit 2
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CFG=$(mktemp -d)
trap 'rm -rf "$CFG"' EXIT
# The Arduino sdkconfig.h plus the options this project sets that it lacks
cat > "$CFG/sdkconfig.h" <<CFGEOF
#pragma once
#include "$L/opi_opi/include/sdkconfig.h"
#define CONFIG_DMX_ISR_IN_IRAM 1
#define CONFIG_DMX_NVS_PARTITION_NAME "nvs"
#define CONFIG_RDM_DEVICE_UID_MAN_ID 0x7ff0
#define CONFIG_RDM_DEVICE_UID_DEV_ID 0xffffffff
#define CONFIG_RDM_DISCOVERY_TRANSACTION_SPACING 5800
#define CONFIG_RDM_MANUFACTURER_LABEL "ESP-NODE-2RDM"
#undef CONFIG_LWIP_MAX_SOCKETS
#define CONFIG_LWIP_MAX_SOCKETS 20
CFGEOF

cd "$ROOT" || exit 2
INC=""
for d in $(find components -type d -name include -not -path '*/examples/*' -not -path '*/esp_dmx/*'); do
    INC="$INC -I$ROOT/$d"
done
INC="$INC -I$ROOT/components/esp_dmx/src -I$ROOT/components/web_server"

# led_manager needs the led_strip managed component, which Arduino lacks
FILES=${*:-$(find components main -name '*.c' -not -path '*/examples/*' -not -path '*/led_manager/*' | sort)}
fail=0
for f in $FILES; do
    out=$(eval "\"$CC\" -c -o /dev/null -O2 -Wall -Werror=all -Wextra $(cat "$L/flags/c_flags") \
          $(cat "$L/flags/defines") -I\"$CFG\" -iprefix \"$L/include/\" $(cat "$L/flags/includes") \
          $INC \"$f\"" 2>&1)
    if [ $? -ne 0 ]; then echo "FAIL $f"; fail=1; else echo "ok   $f"; fi
    [ -n "$out" ] && echo "$out" | grep -E 'error|warning' | head -40
done
[ $fail -eq 0 ] && echo "All files compile cleanly."
exit $fail
