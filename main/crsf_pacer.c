#include "crsf_pacer.h"

#include <string.h>

// Разумные границы частоты выдачи. Ниже 25 Гц управление уже неприемлемо,
// выше 250 Гц не работает ни одна из распространённых связок.
#define PPS_MIN 25
#define PPS_MAX 250

bool crsf_pacer_init(crsf_pacer_t *p, uint16_t pps)
{
    memset(p, 0, sizeof(*p));

    p->lock = xSemaphoreCreateMutex();
    if (!p->lock) {
        return false;
    }

    crsf_pacer_set_pps(p, pps);
    return true;
}

void crsf_pacer_set_pps(crsf_pacer_t *p, uint16_t pps)
{
    if (pps < PPS_MIN) {
        pps = PPS_MIN;
    } else if (pps > PPS_MAX) {
        pps = PPS_MAX;
    }
    p->pps = pps;
}

uint32_t crsf_pacer_period_ticks(const crsf_pacer_t *p)
{
    // Период в тиках FreeRTOS. При CONFIG_FREERTOS_HZ=1000 тик равен 1 мс,
    // и для 150 Гц получается 6 тиков — то есть 166 Гц вместо 150.
    //
    // Задача выдачи использует не это, а esp_timer с микросекундной
    // точностью; функция оставлена для случаев, где хватает грубого
    // приближения.
    const uint32_t period_ms = 1000u / p->pps;
    const uint32_t ticks = pdMS_TO_TICKS(period_ms);
    return ticks > 0 ? ticks : 1;
}

void crsf_pacer_reset(crsf_pacer_t *p)
{
    xSemaphoreTake(p->lock, portMAX_DELAY);
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    xSemaphoreGive(p->lock);
}

void crsf_pacer_push(crsf_pacer_t *p, const uint8_t *frame, size_t len)
{
    if (len == 0 || len > CRSF_FRAME_MAX) {
        return;
    }

    xSemaphoreTake(p->lock, portMAX_DELAY);

    if (p->count == PACER_QUEUE_DEPTH) {
        // Очередь заполнена. Выбрасываем самый старый фрейм, а не приходящий:
        // для управления свежая команда ценнее устаревшей. Потеря позиции
        // стика полусекундной давности безвредна, потеря текущей — нет.
        p->head = (p->head + 1) % PACER_QUEUE_DEPTH;
        p->count--;
        p->dropped++;
    }

    pacer_slot_t *slot = &p->queue[p->tail];
    memcpy(slot->data, frame, len);
    slot->len = (uint8_t)len;

    p->tail = (p->tail + 1) % PACER_QUEUE_DEPTH;
    p->count++;

    xSemaphoreGive(p->lock);
}

size_t crsf_pacer_pop(crsf_pacer_t *p, uint8_t *out, size_t out_size)
{
    size_t len = 0;

    xSemaphoreTake(p->lock, portMAX_DELAY);

    if (p->count == 0) {
        // Очередь пуста — данные из сети не поспевают за темпом выдачи.
        // Частые срабатывания означают, что PPS выставлен выше реальной
        // частоты пакетов от источника.
        p->starved++;
        xSemaphoreGive(p->lock);
        return 0;
    }

    const pacer_slot_t *slot = &p->queue[p->head];
    if (slot->len <= out_size) {
        memcpy(out, slot->data, slot->len);
        len = slot->len;
        p->sent++;
    } else {
        // Не помещается в буфер вызывающего — пропускаем, иначе фрейм
        // заблокирует очередь навсегда.
        p->dropped++;
    }

    p->head = (p->head + 1) % PACER_QUEUE_DEPTH;
    p->count--;

    xSemaphoreGive(p->lock);
    return len;
}

size_t crsf_pacer_queued(crsf_pacer_t *p)
{
    xSemaphoreTake(p->lock, portMAX_DELAY);
    const size_t n = p->count;
    xSemaphoreGive(p->lock);
    return n;
}
