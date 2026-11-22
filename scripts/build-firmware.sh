#!/usr/bin/env bash
# Build release firmware for the Waveshare ESP32-S3-Touch-LCD-2.1.
#
# Fetches Waveshare's demo package (display/touch drivers + LVGL 8.3), applies
# the LVGL settings Claude Dash needs, compiles, and writes to dist/:
#   claude-dash-<version>-full.bin    flash at 0x0    (first install; erases Wi-Fi/pairing)
#   claude-dash-<version>-update.bin  flash at 0xe000 (USB update; keeps Wi-Fi/pairing,
#                                      resets the boot slot so it works after wireless updates)
#   claude-dash-<version>-app.bin     app image the Mac bridge serves for wireless updates
#   SHA256SUMS
#
# Needs: arduino-cli, curl, unzip. Usage: VERSION=v1.2.3 scripts/build-firmware.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${VERSION:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}"
WORK="${WORK:-$ROOT/build}"
DIST="$ROOT/dist"
CORE_VERSION="3.3.13"
ARDUINOJSON_VERSION="7.4.3"
WAVESHARE_ZIP="https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1/ESP32-S3-Touch-LCD-2.1-Demo.zip"
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc"
DRIVERS="Display_ST7701 TCA9554PWR I2C_Driver Touch_CST820 LVGL_Driver"

export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS="https://espressif.github.io/arduino-esp32/package_esp32_index.json"
# Keep libraries isolated from whatever is in the user's own Arduino folder.
export ARDUINO_DIRECTORIES_USER="$WORK/arduino-user"

echo "==> Claude Dash firmware $VERSION"
mkdir -p "$WORK" "$DIST" "$ARDUINO_DIRECTORIES_USER/libraries"

echo "==> Toolchain: esp32 core $CORE_VERSION, ArduinoJson $ARDUINOJSON_VERSION"
arduino-cli core update-index >/dev/null
arduino-cli core install "esp32:esp32@$CORE_VERSION" >/dev/null
arduino-cli lib install "ArduinoJson@$ARDUINOJSON_VERSION" >/dev/null

echo "==> Waveshare demo package"
[ -f "$WORK/waveshare-demo.zip" ] || curl -fsSL -o "$WORK/waveshare-demo.zip" "$WAVESHARE_ZIP"
rm -rf "$WORK/waveshare-demo"
unzip -q "$WORK/waveshare-demo.zip" -d "$WORK/waveshare-demo"
DEMO="$WORK/waveshare-demo/Arduino"

echo "==> LVGL 8.3 with Claude Dash settings"
rm -rf "$ARDUINO_DIRECTORIES_USER/libraries/lvgl"
cp -R "$DEMO/libraries/lvgl" "$ARDUINO_DIRECTORIES_USER/libraries/lvgl"
CONF="$ARDUINO_DIRECTORIES_USER/libraries/lvgl/src/lv_conf.h"
for opt in LV_FONT_MONTSERRAT_20 LV_FONT_MONTSERRAT_28 LV_FONT_MONTSERRAT_40 LV_USE_QRCODE; do
  sed -i.bak "s/#define $opt 0/#define $opt 1/" "$CONF"
  grep -q "#define $opt 1" "$CONF" || { echo "could not enable $opt in lv_conf.h" >&2; exit 1; }
done

echo "==> Sketch + Waveshare drivers"
SKETCH="$WORK/ClaudeDash"
rm -rf "$SKETCH" && mkdir -p "$SKETCH"
for f in $(git -C "$ROOT" ls-files firmware/ClaudeDash); do cp "$ROOT/$f" "$SKETCH/"; done
for d in $DRIVERS; do cp "$DEMO/examples/LVGL_Arduino/$d".* "$SKETCH/"; done

echo "==> Compile"
OUT="$WORK/out"
rm -rf "$OUT"
arduino-cli compile --fqbn "$FQBN" --output-dir "$OUT" \
  --build-property "compiler.cpp.extra_flags=-DDASH_VERSION=\"$VERSION\"" "$SKETCH" | grep -E "Sketch uses|Global variables"

echo "==> Merge images"
DATA_DIR="$(arduino-cli config get directories.data)"
ESPTOOL="$(find "$DATA_DIR/packages/esp32/tools/esptool_py" -type f -name esptool -perm -u+x | head -1)"
BOOT_APP0="$DATA_DIR/packages/esp32/hardware/esp32/$CORE_VERSION/tools/partitions/boot_app0.bin"
FULL="$DIST/claude-dash-$VERSION-full.bin"
APP="$DIST/claude-dash-$VERSION-app.bin"
rm -f "$DIST"/claude-dash-*.bin "$DIST/SHA256SUMS"
"$ESPTOOL" --chip esp32s3 merge-bin -o "$FULL" \
  --flash-mode keep --flash-freq keep --flash-size keep \
  0x0 "$OUT/ClaudeDash.ino.bootloader.bin" \
  0x8000 "$OUT/ClaudeDash.ino.partitions.bin" \
  0xe000 "$BOOT_APP0" \
  0x10000 "$OUT/ClaudeDash.ino.bin" >/dev/null
UPDATE="$DIST/claude-dash-$VERSION-update.bin"
"$ESPTOOL" --chip esp32s3 merge-bin -o "$UPDATE" --target-offset 0xe000 \
  --flash-mode keep --flash-freq keep --flash-size keep \
  0xe000 "$BOOT_APP0" \
  0x10000 "$OUT/ClaudeDash.ino.bin" >/dev/null
cp "$OUT/ClaudeDash.ino.bin" "$APP"
(cd "$DIST" && shasum -a 256 claude-dash-*.bin > SHA256SUMS)

echo "==> Done"
ls -la "$DIST"
