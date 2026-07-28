#!/usr/bin/env bash
# ============================================================================
# ESPBridge — сборка образа для прошивки
#
#   ./build.sh ground     наземный модуль, 192.168.4.1
#   ./build.sh air        бортовой модуль, 192.168.4.2
#   ./build.sh test       тест Ethernet, без UART
#   ./build.sh all        все три
#
# Результат — файл *_MERGED.bin в папке out/. Его и надо заливать,
# по адресу 0x0.
#
# --------------------------------------------------------------------------
# ВАЖНО: заливать нужно именно *_MERGED.bin, а НЕ firmware.bin.
#
# `pio run` создаёт firmware.bin — только прошивку, без загрузчика и
# таблицы разделов. Если залить её по адресу 0x0, плата не найдёт заголовок
# и будет бесконечно перезагружаться с сообщением:
#
#     invalid header: 0x...
#     rst:0x10 (RTCWDT_RTC_RESET)
#
# Этот скрипт склеивает загрузчик, таблицу разделов и прошивку в один файл,
# где всё лежит по своим адресам.
# ============================================================================

set -e

ROLE="${1:-all}"

case "$ROLE" in
  ground) ENVS="ground" ;;
  air)    ENVS="air" ;;
  test)   ENVS="test_eth" ;;
  all)    ENVS="ground air test_eth" ;;
  *)
    echo "Использование: $0 {ground|air|test|all}"
    echo
    echo "  ground   наземный модуль, IP 192.168.4.1"
    echo "  air      бортовой модуль, IP 192.168.4.2"
    echo "  test     минимальный тест Ethernet"
    echo "  all      все три (по умолчанию)"
    exit 1
    ;;
esac

if command -v pio >/dev/null 2>&1; then
  PIO="pio"
else
  PIO="python3 -m platformio"
fi

BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

mkdir -p out

for ENV_NAME in $ENVS; do
  echo
  echo "=== $ENV_NAME ==="
  echo

  $PIO run -e "$ENV_NAME"

  BUILD=".pio/build/$ENV_NAME"

  if [ ! -f "$BOOT_APP0" ]; then
    echo "ОШИБКА: не найден boot_app0.bin:"
    echo "  $BOOT_APP0"
    exit 1
  fi

  OUT="out/${ENV_NAME}_MERGED.bin"

  $PIO pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 merge_bin \
    -o "$OUT" \
    --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x1000  "$BUILD/bootloader.bin" \
    0x8000  "$BUILD/partitions.bin" \
    0xe000  "$BOOT_APP0" \
    0x10000 "$BUILD/firmware.bin"

  # Проверяем, что образ действительно собрался: по смещению 0x1000 должен
  # стоять магический байт 0xE9 — признак корректного образа ESP32.
  MAGIC=$(xxd -s 0x1000 -l 1 -p "$OUT" 2>/dev/null || echo "??")
  if [ "$MAGIC" = "e9" ]; then
    echo "  проверка: заголовок на месте (0xE9)"
  else
    echo "  ВНИМАНИЕ: неожиданный заголовок ($MAGIC), образ может быть битым"
  fi
done

echo
echo "=== Готово ==="
ls -la out/
echo
echo "Заливать так (порт подставить свой):"
echo
echo "  esptool.exe --chip esp32 --port COM15 --baud 921600 write-flash -z \\"
echo "    --flash-mode dio --flash-freq 80m --flash-size 4MB \\"
echo "    0x0 ground_MERGED.bin"
echo
echo "Файлы для копирования в Windows лежат в:  $(pwd)/out/"
echo
