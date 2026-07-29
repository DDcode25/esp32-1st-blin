#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"

// Режим работы порта.
//
// TRANSPARENT — байты идут через мост как есть, без разбора содержимого.
// Подходит для MAVLink и любого потокового протокола.
//
// CRSF — поток разбирается на фреймы, фреймы выдаются в UART собственным
// таймером с заданной частотой. Без этого прямая подача в полётный
// контроллер вызывает failsafe: UDP доставляет пакеты пачками, а приёмник
// ждёт равномерного потока.
typedef enum {
    PORT_MODE_TRANSPARENT = 0,
    PORT_MODE_CRSF = 1,
} port_mode_t;

typedef struct {
    uint32_t baud;
    port_mode_t mode;
    uint16_t pps;  // частота выдачи в режиме CRSF, 25-250 Гц
} port_config_t;

// Счётчики для диагностики. Без них отладка моста превращается в гадание:
// непонятно, молчит ли источник, теряется ли пакет в сети, или занят UART.
typedef struct {
    uint32_t uart_rx_bytes;
    uint32_t uart_tx_bytes;
    uint32_t udp_tx_packets;
    uint32_t udp_rx_packets;
    uint32_t udp_tx_dropped;
    uint32_t uart_tx_dropped;
    uint32_t last_rx_ms;

    // Режим CRSF. В прозрачном остаются нулевыми.
    uint32_t crsf_frames_ok;
    uint32_t crsf_frames_bad_crc;
    uint32_t crsf_frames_sent;
    uint32_t crsf_frames_dropped;
    uint32_t crsf_starved;
    uint32_t crsf_queued;
} port_stats_t;

// Один порт данных: UART <-> UDP.
//
// Каждый порт обслуживается двумя задачами FreeRTOS — по одной на
// направление. Это позволяет задать им разный приоритет: приём с UART в
// режиме CRSF критичнее по времени, чем приём из сети.
typedef struct port_s port_t;

// Глубина очереди выдачи — для вывода в консоли.
#define PACER_QUEUE_DEPTH_PUBLIC 8

// Создаёт и запускает порт. Вызывать после подъёма Ethernet.
port_t *port_create(uart_port_t uart_num, int tx_gpio, int rx_gpio,
                    uint16_t local_port, uint16_t remote_port,
                    const char *peer_ip, const char *name,
                    const port_config_t *cfg);

// Меняет конфигурацию на лету, без перезапуска задач.
void port_apply_config(port_t *port, const port_config_t *cfg);

void port_get_stats(port_t *port, port_stats_t *out);
void port_get_config(port_t *port, port_config_t *out);
const char *port_name(port_t *port);

// true, если из сети недавно приходили данные — признак живого партнёра.
bool port_peer_alive(port_t *port);

// --- Терминал ---------------------------------------------------------------
//
// Кольцевой буфер последних принятых байт, чтобы веб-страница могла показать
// проходящий через порт трафик. Заполняется всегда — на работу моста не
// влияет, только на диагностику.

#define TERM_BUFFER_SIZE 1024

// Источник байта в буфере: пришёл с UART или из сети.
typedef enum {
    TERM_SRC_UART = 0,
    TERM_SRC_NET = 1,
} term_src_t;

// Отправляет данные с веб-страницы. to_uart — выдать в UART подключённому
// устройству, to_net — отправить партнёру по мосту. Можно и то и другое.
void port_terminal_send(port_t *port, const uint8_t *data, size_t len,
                        bool to_uart, bool to_net);

// Читает накопленное начиная с позиции from. Возвращает число скопированных
// байт и обновляет from до текущей позиции — так страница забирает только
// новое, не запрашивая всё заново.
size_t port_terminal_read(port_t *port, uint32_t *from, uint8_t *out,
                          size_t out_cap, term_src_t *src_out);
