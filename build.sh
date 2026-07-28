#!/usr/bin/env bash
# ============================================================================
# ESPBridge — сборка образа, готового к заливке
#
#   ./build.sh
#
# Результат — out/espbridge_MERGED.bin. Заливать по адресу 0x0.
#
# Прошивка ОДНА для обеих плат: роль и адреса задаются через веб-интерфейс
# и хранятся в NVS. Перепутать образы при заливке невозможно.
#
# --------------------------------------------------------------------------
# ВАЖНО: заливать нужно именно *_MERGED.bin, а НЕ firmware.bin.
#
# `pio run` создаёт firmware.bin — только прошивку, без загрузчика и таблицы
# разделов. Если залить её по адресу 0x0, плата не найдёт заголовок и уйдёт
# в бесконечную перезагрузку:
#
#     invalid header: 0x...
#     rst:0x10 (RTCWDT_RTC_RESET)
#
# Этот скрипт склеивает всё в один файл, где каждая часть лежит по своему
# адресу.
# ============================================================================

set -e

ENVS="espbridge"

if command -v pio >/dev/null 2>&1; then
  PIO="pio"
else
  PIO="python3 -m platformio"
fi

mkdir -p out

for ENV_NAME in $ENVS; do
  echo
  echo "=== $ENV_NAME ==="
  echo

  $PIO run -e "$ENV_NAME"

  BUILD=".pio/build/$ENV_NAME"
  OUT="out/${ENV_NAME}_MERGED.bin"

  for f in bootloader.bin partitions.bin firmware.bin; do
    if [ ! -f "$BUILD/$f" ]; then
      echo "ОШИБКА: не найден $BUILD/$f"
      echo "Попробуй полную пересборку:  rm -rf .pio/build/$ENV_NAME"
      exit 1
    fi
  done

  # Смещения для ESP-IDF: загрузчик 0x1000, таблица разделов 0x8000,
  # прошивка 0x10000. У ESP-IDF нет boot_app0 — он специфичен для Arduino.
  $PIO pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 merge_bin \
    -o "$OUT" \
    --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x1000  "$BUILD/bootloader.bin" \
    0x8000  "$BUILD/partitions.bin" \
    0x10000 "$BUILD/firmware.bin"

  # Проверяем, что образ собрался: по смещению 0x1000 должен стоять
  # магический байт 0xE9 — признак корректного образа ESP32.
  MAGIC=$(xxd -s 0x1000 -l 1 -p "$OUT" 2>/dev/null || echo "??")
  if [ "$MAGIC" = "e9" ]; then
    echo "  проверка: заголовок на месте (0xE9)"
  else
    echo "  ВНИМАНИЕ: неожиданный заголовок ($MAGIC), образ может быть битым"
    exit 1
  fi
done

echo
echo "=== Готово ==="
ls -la out/
echo
echo "Заливать на ОБЕ платы одинаково (порт подставить свой):"
echo
echo "  esptool --chip esp32 --port COM15 --baud 921600 write-flash -z \\"
echo "    --flash-mode dio --flash-freq 80m --flash-size 4MB \\"
echo "    0x0 out/espbridge_MERGED.bin"
echo
echo "После заливки первая плата поднимется на 192.168.4.1 как GROUND."
echo "Вторую настроить через веб-интерфейс: роль AIR, адрес 192.168.4.2."
echo "Две ненастроенные платы в одной сети одновременно включать нельзя —"
echo "у них будет одинаковый адрес."
echo
