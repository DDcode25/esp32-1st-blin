#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Отладочный вывод.
//
// Уровни включаются независимо и на лету — командой в UART0 или из кода.
// Дамп трафика на 400 кбод способен сам стать узким местом, поэтому по
// умолчанию всё выключено, а частота вывода ограничена.
// ============================================================================

typedef enum {
    DBG_TRAFFIC   = 1 << 0,  // размеры пакетов в обе стороны
    DBG_HEX       = 1 << 1,  // содержимое пакетов в hex
    DBG_CRSF      = 1 << 2,  // разобранные фреймы CRSF: тип, длина, CRC
    DBG_PACER     = 1 << 3,  // очередь выдачи: глубина, переполнения, простои
    DBG_NET       = 1 << 4,  // события сокетов и ошибки отправки
    DBG_TIMING    = 1 << 5,  // интервалы между пакетами, джиттер
} debug_flag_t;

void debug_init(void);

bool debug_enabled(debug_flag_t flag);
void debug_set(uint32_t flags);
uint32_t debug_get(void);

// Печатает буфер в hex с ограничением: не чаще, чем раз в интервал,
// и не длиннее заданного числа байт. Без этого лог захлебнётся.
void debug_hex(const char *tag, const char *dir, const uint8_t *data,
               size_t len);

// Измеряет интервалы между вызовами и раз в секунду печатает статистику:
// среднее, минимум, максимум. Нужно для оценки джиттера выдачи CRSF —
// того самого, ради чего делался выравниватель темпа.
typedef struct {
    const char *name;
    uint32_t last_us;
    uint32_t count;
    uint32_t sum_us;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t last_report_ms;
} debug_timing_t;

void debug_timing_init(debug_timing_t *t, const char *name);
void debug_timing_tick(debug_timing_t *t);
