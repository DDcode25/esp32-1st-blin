#!/usr/bin/env bash
# Проверяет страницу веб-интерфейса: извлекает её из main/web.c и прогоняет
# JavaScript через синтаксический разбор.
#
# Страница собирается из строковых литералов C, и ошибка экранирования там
# не видна ни компилятору, ни глазу: escape-последовательности раскрываются
# ещё на этапе компиляции, и в браузер уезжает битый скрипт. Проявляется
# это только на плате, в консоли браузера.
#
# Запускается автоматически из build.sh.

set -e

cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# В некоторых средах python3 указывает на заглушку Windows Store,
# поэтому берём первый работающий интерпретатор.
if python3 -c "" 2>/dev/null; then
  PY=python3
elif python -c "" 2>/dev/null; then
  PY=python
else
  echo "  проверка страницы пропущена: python не найден"
  exit 0
fi

$PY tools/extract_page.py main/web.c "$OUT/page.html" >/dev/null

if command -v node >/dev/null 2>&1; then
  if node --check "$OUT/page.html.js" 2>"$OUT/err"; then
    echo "  проверка страницы: синтаксис JavaScript корректен"
  else
    echo "  ОШИБКА в JavaScript веб-интерфейса:"
    head -5 "$OUT/err" | sed 's/^/    /'
    exit 1
  fi
else
  echo "  проверка страницы пропущена: node не установлен"
fi
