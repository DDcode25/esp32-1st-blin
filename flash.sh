#!/usr/bin/env bash
# ============================================================================
# ESPBridge — сборка и прошивка одной командой
#
#   ./flash.sh ground COM15      наземный модуль, 192.168.4.1
#   ./flash.sh air    COM15      бортовой модуль, 192.168.4.2
#   ./flash.sh test   COM15      тест Ethernet, без UART
#
# В Linux/WSL порт указывается как /dev/ttyUSB0.
# Если порт не указан — скрипт попробует найти плату сам.
#
# Собирает прошивку, склеивает единый образ и заливает его по адресу 0x0.
# Разбираться с адресами разделов не нужно — они внутри образа.
# ============================================================================

set -e

ROLE="$1"
PORT="$2"

case "$ROLE" in
  ground) ENV_NAME="ground";   DESC="НАЗЕМНЫЙ модуль (192.168.4.1)" ;;
  air)    ENV_NAME="air";      DESC="БОРТОВОЙ модуль (192.168.4.2)" ;;
  test)   ENV_NAME="test_eth"; DESC="ТЕСТ Ethernet (без UART)" ;;
  *)
    echo "Использование: $0 {ground|air|test} [порт]"
    echo
    echo "  ground   наземный модуль, IP 192.168.4.1"
    echo "  air      бортовой модуль, IP 192.168.4.2"
    echo "  test     минимальный тест Ethernet"
    echo
    echo "Примеры:"
    echo "  $0 ground COM15"
    echo "  $0 air /dev/ttyUSB0"
    echo "  $0 test              (порт определится автоматически)"
    exit 1
    ;;
esac

# PlatformIO может быть недоступен как команда — тогда идём через модуль.
if command -v pio >/dev/null 2>&1; then
  PIO="pio"
else
  PIO="python3 -m platformio"
fi

echo
echo "=== ESPBridge: $DESC ==="
echo

echo "[1/3] Сборка..."
$PIO run -e "$ENV_NAME"

BUILD=".pio/build/$ENV_NAME"
BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

if [ ! -f "$BOOT_APP0" ]; then
  echo "ОШИБКА: не найден boot_app0.bin по пути:"
  echo "  $BOOT_APP0"
  echo "Обычно он появляется после первой сборки. Проверь путь вручную."
  exit 1
fi

echo
echo "[2/3] Сборка единого образа..."
# Единый образ вместо четырёх файлов: заливается одной командой по 0x0,
# перепутать адреса разделов невозможно.
$PIO pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 merge_bin \
  -o "$BUILD/MERGED.bin" \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x1000  "$BUILD/bootloader.bin" \
  0x8000  "$BUILD/partitions.bin" \
  0xe000  "$BOOT_APP0" \
  0x10000 "$BUILD/firmware.bin"

echo
echo "[3/3] Прошивка..."

PORT_ARG=""
if [ -n "$PORT" ]; then
  PORT_ARG="--port $PORT"
  echo "Порт: $PORT"
else
  echo "Порт не указан — ищу плату автоматически."
fi

echo
echo "Если плата не входит в режим прошивки — замкни IO0 на GND,"
echo "перезапусти питание и повтори."
echo

$PIO pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 $PORT_ARG \
  --baud 921600 write_flash -z \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 "$BUILD/MERGED.bin"

echo
echo "=== Готово ==="
echo "Сними перемычку IO0-GND и перезапусти питание."
echo "Открой монитор на 115200:  $PIO device monitor -b 115200"
echo
