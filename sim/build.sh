#!/bin/bash
# Build everything sim/wokwi.toml needs into sim/build/:
#   PIDKiln.sim.flash.bin  - full flash image (bootloader, partitions, app built with -DPIDKILN_SIM,
#                            LittleFS image of data/ at the data partition's offset)
#   PIDKiln.ino.elf        - for stack traces / debugging
#   max31855.chip.wasm/.json - the thermocouple + kiln model chip
# Needs arduino-cli (1.3+) and wokwi-cli (0.20+) on PATH, or ARDUINO_CLI / WOKWI_CLI set. CI runs this too.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/sim/build"
WORK="${SIM_BUILD_WORK:-$OUT/work}"
ARDUINO_CLI="${ARDUINO_CLI:-arduino-cli}"
WOKWI_CLI="${WOKWI_CLI:-wokwi-cli}"
mkdir -p "$OUT" "$WORK"
cd "$ROOT"

echo "== Firmware (simulation build)"
"$ARDUINO_CLI" compile --profile wrover \
  --build-property "compiler.cpp.extra_flags=-DTEMPLATE_PLACEHOLDER='~' -DPIDKILN_SIM" \
  --warnings default --build-path "$WORK" --output-dir "$OUT" .

echo "== LittleFS data image"
PROPS=$("$ARDUINO_CLI" compile --profile wrover --show-properties .)
MKLITTLEFS=$(sed -n 's/^runtime\.tools\.mklittlefs\.path=//p' <<<"$PROPS")
read -r FS_OFFSET FS_SIZE < <(awk -F, '$1 ~ /^spiffs/ {gsub(/ /,"",$4); gsub(/ /,"",$5); print $4, $5}' "$WORK/partitions.csv")
test -x "$MKLITTLEFS/mklittlefs" && test -n "$FS_OFFSET" && test -n "$FS_SIZE"
"$MKLITTLEFS/mklittlefs" -c data -p 256 -b 4096 -s "$FS_SIZE" "$OUT/PIDKiln.littlefs.bin"

echo "== Flash image (merged app image + LittleFS at $FS_OFFSET)"
MERGED="$OUT/PIDKiln.ino.merged.bin"   # bootloader + partition table + boot_app0 + app, made by the ESP32 core
test -f "$MERGED" || { echo "missing $MERGED - the ESP32 core did not produce a merged image" >&2; exit 1; }
FLASH="$OUT/PIDKiln.sim.flash.bin"
head -c $(( 4 * 1024 * 1024 )) /dev/zero | tr '\0' '\377' > "$FLASH"   # erased 4 MB flash (0xFF, so NVS starts empty)
dd if="$MERGED" of="$FLASH" conv=notrunc status=none
dd if="$OUT/PIDKiln.littlefs.bin" of="$FLASH" bs=4096 seek=$(( FS_OFFSET / 4096 )) conv=notrunc status=none
test "$(stat -c %s "$FLASH")" = $(( 4 * 1024 * 1024 ))

echo "== Custom chip"
"$WOKWI_CLI" chip compile sim/chips/max31855.chip.c -o "$OUT/max31855.chip.wasm"
cp sim/chips/max31855.chip.json "$OUT/max31855.chip.json"

ls -l "$OUT"/*.bin "$OUT"/*.elf "$OUT"/*.wasm
