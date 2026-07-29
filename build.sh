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

# PlatformIO ставится по-разному: в PATH, в собственное виртуальное
# окружение (так делает расширение VS Code) или как модуль Python.
# Перебираем варианты, а не полагаемся на один.
if command -v pio >/dev/null 2>&1; then
  PIO="pio"
elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then
  PIO="$HOME/.platformio/penv/bin/pio"
elif [ -x "$HOME/.platformio/penv/Scripts/pio.exe" ]; then
  PIO="$HOME/.platformio/penv/Scripts/pio.exe"
elif python3 -c "import platformio" 2>/dev/null; then
  PIO="python3 -m platformio"
elif python -c "import platformio" 2>/dev/null; then
  PIO="python -m platformio"
else
  echo "ОШИБКА: PlatformIO не найден."
  echo
  echo "Установить:            pip install platformio"
  echo "Либо, если он есть в окружении VS Code, добавить в PATH:"
  echo "  export PATH=\"\$PATH:\$HOME/.platformio/penv/bin\""
  exit 1
fi

echo "PlatformIO: $PIO"

mkdir -p out

for ENV_NAME in $ENVS; do
  echo
  echo "=== $ENV_NAME ==="
  echo

  # Синтаксис страницы проверяется до сборки: ошибка в JavaScript не видна
  # компилятору C и проявилась бы только в браузере на плате.
  bash tools/check_web.sh

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

  # Адрес первого раздела приложения берётся из самой таблицы разделов,
  # а не задаётся здесь константой. Жёстко прописанное 0x10000 (значение
  # из таблицы Arduino) не совпадало с ota_0 по 0x20000: загрузчик не
  # находил прошивку по нужному адресу, писал "invalid magic byte" и
  # перебирал разделы наугад.
  APP_OFFSET=$(awk -F, '/^ota_0/ {gsub(/ /,"",$4); print $4}' partitions.csv)
  if [ -z "$APP_OFFSET" ]; then
    echo "ОШИБКА: в partitions.csv не найден раздел ota_0"
    exit 1
  fi
  echo "  раздел приложения: $APP_OFFSET"

  # Загрузчик всегда по 0x1000, таблица разделов по 0x8000 — эти адреса
  # зашиты в ПЗУ чипа и от таблицы не зависят. boot_app0 в ESP-IDF нет,
  # он специфичен для Arduino.
  $PIO pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 merge_bin \
    -o "$OUT" \
    --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x1000       "$BUILD/bootloader.bin" \
    0x8000       "$BUILD/partitions.bin" \
    "$APP_OFFSET" "$BUILD/firmware.bin"

  # Проверяем и загрузчик, и прошивку: магический байт 0xE9 должен стоять
  # в начале каждого образа ESP32. Раньше проверялся только загрузчик,
  # и прошивка по неверному адресу проверку проходила.
  BOOT_MAGIC=$(xxd -s 0x1000 -l 1 -p "$OUT" 2>/dev/null || echo "??")
  APP_MAGIC=$(xxd -s $((APP_OFFSET)) -l 1 -p "$OUT" 2>/dev/null || echo "??")

  if [ "$BOOT_MAGIC" != "e9" ]; then
    echo "  ОШИБКА: загрузчик не на месте (0x1000 = $BOOT_MAGIC)"
    exit 1
  fi
  if [ "$APP_MAGIC" != "e9" ]; then
    echo "  ОШИБКА: прошивка не на месте ($APP_OFFSET = $APP_MAGIC)"
    exit 1
  fi
  echo "  проверка: загрузчик и прошивка на своих адресах"
done

echo
echo "=== Готово ==="
ls -la out/
echo
echo "Заливать на ОБЕ платы один и тот же файл."
echo
if grep -qi microsoft /proc/version 2>/dev/null; then
  # В WSL порты Windows не видны: COM-порт существует только на стороне
  # хоста. Копируем файл туда и заливаем оттуда.
  echo "Ты в WSL — COM-порты отсюда не видны. Скопируй образ в Windows:"
  echo
  echo "  cp out/espbridge_MERGED.bin /mnt/c/путь/к/папке/с/esptool/"
  echo
  echo "и залей из командной строки Windows:"
  echo
  echo "  esptool.exe --chip esp32 --port COM15 --baud 921600 write-flash -z \\"
  echo "    --flash-mode dio --flash-freq 80m --flash-size 4MB \\"
  echo "    0x0 espbridge_MERGED.bin"
else
  echo "  esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 write-flash -z \\"
  echo "    --flash-mode dio --flash-freq 80m --flash-size 4MB \\"
  echo "    0x0 out/espbridge_MERGED.bin"
fi
echo
echo "После заливки первая плата поднимется на 192.168.4.1 как GROUND."
echo "Вторую настроить через веб-интерфейс: роль AIR, адрес 192.168.4.2."
echo "Две ненастроенные платы в одной сети одновременно включать нельзя —"
echo "у них будет одинаковый адрес."
echo
