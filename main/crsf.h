#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// CRSF — разбор потока на фреймы.
//
// Формат фрейма:
//   [addr][len][type][payload...][crc8]
//    0     1    2     3..         len+1
//
// Поле len считает всё, что идёт после него самого: type + payload + crc.
// Полная длина фрейма в байтах = len + 2.
//
// CRC8 (полином 0xD5) считается по type + payload — от байта 2 включительно
// до последнего байта payload.
// ============================================================================

#define CRSF_LEN_MIN    2
#define CRSF_LEN_MAX    62
#define CRSF_FRAME_MAX  64

#define CRSF_ADDR_FLIGHT_CONTROLLER  0xC8
#define CRSF_ADDR_RADIO_TRANSMITTER  0xEA

#define CRSF_TYPE_RC_CHANNELS        0x16
#define CRSF_TYPE_LINK_STATISTICS    0x14

uint8_t crsf_crc8(const uint8_t *data, size_t len);

// Вызывается на каждый собранный фрейм с корректной контрольной суммой.
typedef void (*crsf_frame_cb_t)(void *ctx, const uint8_t *frame, size_t len);

// Инкрементальный сборщик фреймов из байтового потока.
//
// Поток приходит произвольными кусками, границы фреймов в нём не обозначены
// ничем, кроме самой структуры.
typedef struct {
    enum {
        CRSF_STATE_WAIT_ADDR = 0,
        CRSF_STATE_WAIT_LEN,
        CRSF_STATE_WAIT_BODY,
    } state;

    uint8_t buf[CRSF_FRAME_MAX];
    size_t pos;
    uint8_t expected_len;

    crsf_frame_cb_t cb;
    void *ctx;

    uint32_t frames_ok;
    uint32_t frames_bad_crc;
    uint32_t bytes_discarded;
} crsf_parser_t;

void crsf_parser_init(crsf_parser_t *p, crsf_frame_cb_t cb, void *ctx);
void crsf_parser_reset(crsf_parser_t *p);
void crsf_parser_feed(crsf_parser_t *p, const uint8_t *data, size_t len);
