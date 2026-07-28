#include "debug.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "dbg";

static uint32_t s_flags = 0;

// Ограничение частоты дампа. На 400 кбод пакеты идут каждые 500 мкс, и
// печать каждого превратила бы лог в узкое место, исказив ровно то, что
// мы пытаемся измерить.
#define HEX_MIN_INTERVAL_MS  200
#define HEX_MAX_BYTES        32

void debug_init(void)
{
    // Флаги переживают перезагрузку: включив отладку, не хочется терять её
    // при каждом сбросе платы во время поиска проблемы.
    nvs_handle_t nvs;
    if (nvs_open("espbridge", NVS_READONLY, &nvs) == ESP_OK) {
        uint32_t stored = 0;
        if (nvs_get_u32(nvs, "dbg", &stored) == ESP_OK) {
            s_flags = stored;
        }
        nvs_close(nvs);
    }

    if (s_flags) {
        ESP_LOGW(TAG, "отладка включена, флаги 0x%02lx", (unsigned long)s_flags);
    }
}

bool debug_enabled(debug_flag_t flag)
{
    return (s_flags & flag) != 0;
}

void debug_set(uint32_t flags)
{
    s_flags = flags;

    nvs_handle_t nvs;
    if (nvs_open("espbridge", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, "dbg", flags);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGW(TAG, "флаги отладки: 0x%02lx", (unsigned long)flags);
}

uint32_t debug_get(void)
{
    return s_flags;
}

void debug_hex(const char *tag, const char *dir, const uint8_t *data,
               size_t len)
{
    if (!debug_enabled(DBG_HEX)) {
        return;
    }

    // Счётчик на каждый тег отдельно, чтобы порты не глушили друг друга.
    static uint32_t last_ms[4];
    static const char *last_tag[4];
    int slot = -1;

    for (int i = 0; i < 4; i++) {
        if (last_tag[i] == tag) {
            slot = i;
            break;
        }
        if (last_tag[i] == NULL) {
            last_tag[i] = tag;
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
    }

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - last_ms[slot] < HEX_MIN_INTERVAL_MS) {
        return;
    }
    last_ms[slot] = now;

    const size_t show = len < HEX_MAX_BYTES ? len : HEX_MAX_BYTES;
    char hex[HEX_MAX_BYTES * 3 + 1];
    size_t pos = 0;

    for (size_t i = 0; i < show; i++) {
        pos += snprintf(&hex[pos], sizeof(hex) - pos, "%02x ", data[i]);
    }
    hex[pos > 0 ? pos - 1 : 0] = '\0';

    if (show < len) {
        ESP_LOGI(TAG, "%s %s %u байт: %s ... (+%u)", tag, dir, (unsigned)len,
                 hex, (unsigned)(len - show));
    } else {
        ESP_LOGI(TAG, "%s %s %u байт: %s", tag, dir, (unsigned)len, hex);
    }
}

void debug_timing_init(debug_timing_t *t, const char *name)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->min_us = UINT32_MAX;
}

void debug_timing_tick(debug_timing_t *t)
{
    if (!debug_enabled(DBG_TIMING)) {
        return;
    }

    const uint32_t now = (uint32_t)esp_timer_get_time();

    if (t->last_us != 0) {
        const uint32_t dt = now - t->last_us;
        t->sum_us += dt;
        t->count++;
        if (dt < t->min_us) {
            t->min_us = dt;
        }
        if (dt > t->max_us) {
            t->max_us = dt;
        }
    }
    t->last_us = now;

    const uint32_t now_ms = now / 1000;
    if (now_ms - t->last_report_ms < 1000) {
        return;
    }
    t->last_report_ms = now_ms;

    if (t->count > 0) {
        const uint32_t avg = t->sum_us / t->count;
        // Разброс между минимумом и максимумом — это и есть джиттер.
        // Для CRSF на 150 Гц период 6666 мкс; разброс больше пары сотен
        // микросекунд означает, что выдача неровная.
        ESP_LOGI(TAG, "%s: %lu/с, период %lu мкс (мин %lu, макс %lu), джиттер %lu",
                 t->name, (unsigned long)t->count, (unsigned long)avg,
                 (unsigned long)t->min_us, (unsigned long)t->max_us,
                 (unsigned long)(t->max_us - t->min_us));
    }

    t->count = 0;
    t->sum_us = 0;
    t->min_us = UINT32_MAX;
    t->max_us = 0;
}
