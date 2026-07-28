#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crsf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ============================================================================
// Выравниватель темпа выдачи CRSF-фреймов.
//
// Задача. Из сети фреймы приходят неравномерно: UDP доставляет их пачками,
// с джиттером, иногда с потерями. Полётный контроллер ждёт равномерного
// потока — при срыве темпа Betaflight/INAV поднимают failsafe.
//
// Решение. Принятые фреймы складываются в очередь, а выдача идёт отдельной
// задачей с фиксированным периодом. Сетевой джиттер гасится глубиной
// очереди ценой небольшой добавочной задержки.
//
// Очередь заполняется из задачи приёма UDP и опустошается из задачи выдачи,
// поэтому доступ к ней защищён мьютексом.
// ============================================================================

#define PACER_QUEUE_DEPTH 8

typedef struct {
    uint8_t data[CRSF_FRAME_MAX];
    uint8_t len;
} pacer_slot_t;

typedef struct {
    pacer_slot_t queue[PACER_QUEUE_DEPTH];
    size_t head;
    size_t tail;
    size_t count;

    SemaphoreHandle_t lock;

    uint16_t pps;

    uint32_t dropped;
    uint32_t sent;
    uint32_t starved;
} crsf_pacer_t;

bool crsf_pacer_init(crsf_pacer_t *p, uint16_t pps);
void crsf_pacer_set_pps(crsf_pacer_t *p, uint16_t pps);
void crsf_pacer_reset(crsf_pacer_t *p);

// Кладёт фрейм в очередь. При переполнении выбрасывается САМЫЙ СТАРЫЙ
// фрейм: для управления свежая команда ценнее устаревшей.
void crsf_pacer_push(crsf_pacer_t *p, const uint8_t *frame, size_t len);

// Забирает очередной фрейм. Возвращает 0, если очередь пуста.
// Ритм задаёт вызывающая сторона — эта функция его не ждёт.
size_t crsf_pacer_pop(crsf_pacer_t *p, uint8_t *out, size_t out_size);

// Период выдачи в тиках FreeRTOS, для vTaskDelayUntil.
uint32_t crsf_pacer_period_ticks(const crsf_pacer_t *p);

size_t crsf_pacer_queued(crsf_pacer_t *p);
