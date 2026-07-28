#include "port.h"

#include <string.h>
#include <sys/socket.h>

#include "config.h"
#include "crsf.h"
#include "crsf_pacer.h"
#include "debug.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net_eth.h"

struct port_s {
    uart_port_t uart_num;
    int tx_gpio;
    int rx_gpio;
    uint16_t udp_port;
    const char *name;

    int sock;
    struct sockaddr_in peer_addr;

    port_config_t cfg;
    SemaphoreHandle_t cfg_lock;

    port_stats_t stats;

    crsf_parser_t net_parser;
    crsf_pacer_t pacer;

    // Будит задачу выдачи в режиме CRSF. Аппаратный таймер вместо
    // vTaskDelay: тик FreeRTOS даже при 1000 Гц даёт для 150 Гц округление
    // до 143 или 166 Гц, что для CRSF неприемлемо.
    esp_timer_handle_t pace_timer;
    TaskHandle_t pace_task;
};

static const char *TAG = "port";

// --- вспомогательное ---------------------------------------------------------

static port_mode_t port_mode(port_t *p)
{
    xSemaphoreTake(p->cfg_lock, portMAX_DELAY);
    const port_mode_t m = p->cfg.mode;
    xSemaphoreGive(p->cfg_lock);
    return m;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// --- UART -> сеть ------------------------------------------------------------

static void uart_to_net_task(void *arg)
{
    port_t *p = (port_t *)arg;
    uint8_t buf[UDP_PACKET_MAX];

    while (1) {
        // Читаем с таймаутом, равным окну агрегации: драйвер вернёт данные
        // либо по заполнению буфера, либо по истечении окна. Так редкий
        // поток не залипает в ожидании, пока буфер наполнится, а частый
        // не порождает по датаграмме на байт.
        const int n = uart_read_bytes(p->uart_num, buf, sizeof(buf),
                                      pdMS_TO_TICKS(1));
        if (n <= 0) {
            continue;
        }

        p->stats.uart_rx_bytes += n;
        debug_hex(p->name, "UART->сеть", buf, n);

        if (!eth_bridge_link_up()) {
            p->stats.udp_tx_dropped++;
            continue;
        }

        const int sent = sendto(p->sock, buf, n, 0,
                                (struct sockaddr *)&p->peer_addr,
                                sizeof(p->peer_addr));
        if (sent == n) {
            p->stats.udp_tx_packets++;
        } else {
            p->stats.udp_tx_dropped++;
        }
    }
}

// --- сеть -> UART ------------------------------------------------------------

// Вызывается парсером на каждый собранный фрейм в режиме CRSF.
static void on_crsf_frame(void *ctx, const uint8_t *frame, size_t len)
{
    port_t *p = (port_t *)ctx;
    crsf_pacer_push(&p->pacer, frame, len);
}

static void net_to_uart_task(void *arg)
{
    port_t *p = (port_t *)arg;
    uint8_t buf[UDP_PACKET_MAX];

    while (1) {
        const int n = recv(p->sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        p->stats.udp_rx_packets++;
        p->stats.last_rx_ms = now_ms();
        debug_hex(p->name, "сеть->UART", buf, n);

        if (port_mode(p) == PORT_MODE_CRSF) {
            // Разбираем на фреймы и кладём в очередь. В UART здесь ничего
            // не пишем — этим занимается задача выдачи по своему таймеру.
            crsf_parser_feed(&p->net_parser, buf, n);
            p->stats.crsf_frames_ok = p->net_parser.frames_ok;
            p->stats.crsf_frames_bad_crc = p->net_parser.frames_bad_crc;
            continue;
        }

        const int written = uart_write_bytes(p->uart_num, buf, n);
        if (written > 0) {
            p->stats.uart_tx_bytes += written;
        }
        if (written < n) {
            p->stats.uart_tx_dropped += (n - written);
        }
    }
}

// --- выдача по таймеру (режим CRSF) ------------------------------------------

static void pace_timer_cb(void *arg)
{
    port_t *p = (port_t *)arg;
    // Из контекста таймера только будим задачу: запись в UART может
    // блокироваться, а обработчик esp_timer выполняется в задаче с высоким
    // приоритетом, задерживать которую нельзя.
    if (p->pace_task) {
        xTaskNotifyGive(p->pace_task);
    }
}

static void pace_task_fn(void *arg)
{
    port_t *p = (port_t *)arg;
    uint8_t frame[CRSF_FRAME_MAX];

    // Измеряет реальные интервалы выдачи. Разброс между минимумом и
    // максимумом — это джиттер, ради устранения которого делался
    // выравниватель темпа. Включается командой "dbg timing".
    debug_timing_t timing;
    debug_timing_init(&timing, "выдача CRSF");

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        debug_timing_tick(&timing);

        if (port_mode(p) != PORT_MODE_CRSF) {
            continue;
        }

        const size_t len = crsf_pacer_pop(&p->pacer, frame, sizeof(frame));
        if (len == 0) {
            p->stats.crsf_starved = p->pacer.starved;
            continue;
        }

        // Фрейм выдаём целиком: половина фрейма на линии — гарантированный
        // сбой разбора у приёмника.
        const int written = uart_write_bytes(p->uart_num, frame, len);
        if (written > 0) {
            p->stats.uart_tx_bytes += written;
        }

        p->stats.crsf_frames_sent = p->pacer.sent;
        p->stats.crsf_frames_dropped = p->pacer.dropped;
        p->stats.crsf_queued = crsf_pacer_queued(&p->pacer);
    }
}

// --- создание и настройка ----------------------------------------------------

static bool setup_uart(port_t *p, uint32_t baud)
{
    const uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_driver_install(p->uart_num, UART_RX_BUFFER_SIZE,
                            UART_TX_BUFFER_SIZE, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] не удалось установить драйвер UART", p->name);
        return false;
    }
    if (uart_param_config(p->uart_num, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] неверные параметры UART", p->name);
        return false;
    }
    if (uart_set_pin(p->uart_num, p->tx_gpio, p->rx_gpio, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] не удалось назначить пины UART", p->name);
        return false;
    }
    return true;
}

static bool setup_socket(port_t *p, const char *peer_ip)
{
    p->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (p->sock < 0) {
        ESP_LOGE(TAG, "[%s] не удалось создать сокет", p->name);
        return false;
    }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(p->udp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(p->sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "[%s] не удалось занять порт %u", p->name, p->udp_port);
        close(p->sock);
        p->sock = -1;
        return false;
    }

    // Таймаут на приём, чтобы задача не висела в recv() навсегда и могла
    // реагировать на смену конфигурации.
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(p->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&p->peer_addr, 0, sizeof(p->peer_addr));
    p->peer_addr.sin_family = AF_INET;
    p->peer_addr.sin_port = htons(p->udp_port);
    p->peer_addr.sin_addr.s_addr = inet_addr(peer_ip);

    return true;
}

port_t *port_create(uart_port_t uart_num, int tx_gpio, int rx_gpio,
                    uint16_t udp_port, const char *peer_ip, const char *name,
                    const port_config_t *cfg)
{
    port_t *p = calloc(1, sizeof(port_t));
    if (!p) {
        return NULL;
    }

    p->uart_num = uart_num;
    p->tx_gpio = tx_gpio;
    p->rx_gpio = rx_gpio;
    p->udp_port = udp_port;
    p->name = name;
    p->cfg = *cfg;
    p->sock = -1;

    p->cfg_lock = xSemaphoreCreateMutex();
    if (!p->cfg_lock) {
        free(p);
        return NULL;
    }

    crsf_parser_init(&p->net_parser, on_crsf_frame, p);
    if (!crsf_pacer_init(&p->pacer, cfg->pps)) {
        ESP_LOGE(TAG, "[%s] не удалось создать выравниватель темпа", name);
        free(p);
        return NULL;
    }

    if (!setup_uart(p, cfg->baud) || !setup_socket(p, peer_ip)) {
        free(p);
        return NULL;
    }

    // Приём с UART приоритетнее приёма из сети: пропущенные байты на линии
    // не восстановить, а сетевой пакет подождёт в буфере стека.
    xTaskCreate(uart_to_net_task, "uart2net", 4096, p, 12, NULL);
    xTaskCreate(net_to_uart_task, "net2uart", 4096, p, 10, NULL);
    xTaskCreate(pace_task_fn, "pacer", 3072, p, 13, &p->pace_task);

    const esp_timer_create_args_t timer_args = {
        .callback = pace_timer_cb,
        .arg = p,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "crsf_pace",
    };
    if (esp_timer_create(&timer_args, &p->pace_timer) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] не удалось создать таймер выдачи", name);
        free(p);
        return NULL;
    }

    if (cfg->mode == PORT_MODE_CRSF) {
        esp_timer_start_periodic(p->pace_timer, 1000000UL / cfg->pps);
    }

    ESP_LOGI(TAG, "[%s] %lu бод, режим %s%s, TX=%d RX=%d, UDP :%u -> %s:%u",
             name, (unsigned long)cfg->baud,
             cfg->mode == PORT_MODE_CRSF ? "CRSF" : "прозрачный",
             cfg->mode == PORT_MODE_CRSF ? "" : "",
             tx_gpio, rx_gpio, udp_port, peer_ip, udp_port);

    return p;
}

void port_apply_config(port_t *p, const port_config_t *cfg)
{
    xSemaphoreTake(p->cfg_lock, portMAX_DELAY);
    const port_config_t old = p->cfg;
    p->cfg = *cfg;
    xSemaphoreGive(p->cfg_lock);

    if (cfg->baud != old.baud) {
        uart_wait_tx_done(p->uart_num, pdMS_TO_TICKS(100));
        uart_set_baudrate(p->uart_num, cfg->baud);
        ESP_LOGI(TAG, "[%s] скорость изменена на %lu бод", p->name,
                 (unsigned long)cfg->baud);
    }

    crsf_pacer_set_pps(&p->pacer, cfg->pps);

    if (cfg->mode != old.mode || cfg->pps != old.pps) {
        esp_timer_stop(p->pace_timer);

        if (cfg->mode != old.mode) {
            // Смена режима обнуляет незавершённую сборку и очередь: их
            // содержимое относится к прежней трактовке потока.
            crsf_parser_reset(&p->net_parser);
            crsf_pacer_reset(&p->pacer);
            ESP_LOGI(TAG, "[%s] режим изменён на %s", p->name,
                     cfg->mode == PORT_MODE_CRSF ? "CRSF" : "прозрачный");
        }

        if (cfg->mode == PORT_MODE_CRSF) {
            esp_timer_start_periodic(p->pace_timer, 1000000UL / p->pacer.pps);
        }
    }
}

void port_get_stats(port_t *p, port_stats_t *out)
{
    *out = p->stats;
    out->crsf_queued = crsf_pacer_queued(&p->pacer);
}

void port_get_config(port_t *p, port_config_t *out)
{
    xSemaphoreTake(p->cfg_lock, portMAX_DELAY);
    *out = p->cfg;
    xSemaphoreGive(p->cfg_lock);
}

const char *port_name(port_t *p)
{
    return p->name;
}

bool port_peer_alive(port_t *p)
{
    if (p->stats.last_rx_ms == 0) {
        return false;
    }
    return (now_ms() - p->stats.last_rx_ms) < PEER_TIMEOUT_MS;
}
