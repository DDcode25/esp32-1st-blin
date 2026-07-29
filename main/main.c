// ============================================================================
// ESPBridge — прозрачный мост UART <-> Ethernet
//
// Два модуля WT32-ETH01, соединённые по IP, пробрасывают два независимых
// потока UART: канал управления CRSF и телеметрию MAVLink.
//
// Роль (GROUND/AIR) задаётся флагом сборки, от неё зависят адреса.
// Сборка:  pio run -e ground   /   pio run -e air
// ============================================================================

#include "config.h"
#include "console.h"
#include "debug.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_eth.h"
#include "nvs_flash.h"
#include "ota.h"
#include "port.h"
#include "settings.h"
#include "web.h"

static const char *TAG = "main";

static port_t *s_port0;
static port_t *s_port1;
static settings_t s_settings;

static void led_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(STATUS_LED_GPIO, 0);
}

// Индикация: линка нет — редкое мигание, линк есть но партнёр молчит —
// частое, идёт обмен — горит ровно.
static void status_task(void *arg)
{
    bool led = false;

    while (1) {
        const bool link = eth_bridge_link_up();
        const bool traffic =
            port_peer_alive(s_port0) || port_peer_alive(s_port1);

        if (link && traffic) {
            gpio_set_level(STATUS_LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        led = !led;
        gpio_set_level(STATUS_LED_GPIO, led);
        vTaskDelay(pdMS_TO_TICKS(link ? 150 : 800));
    }
}

// Раз в пять секунд печатает состояние. По этим строкам видно, жив ли мост,
// даже когда данные не идут.
static void report_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (ota_in_progress()) {
            continue;
        }

        if (!eth_bridge_link_up()) {
            ESP_LOGI(TAG, "линка нет");
            continue;
        }

        port_stats_t s0, s1;
        port_get_stats(s_port0, &s0);
        port_get_stats(s_port1, &s1);

        ESP_LOGI(TAG,
                 "port0: UART rx=%lu tx=%lu | UDP tx=%lu rx=%lu | партнёр %s",
                 (unsigned long)s0.uart_rx_bytes,
                 (unsigned long)s0.uart_tx_bytes,
                 (unsigned long)s0.udp_tx_packets,
                 (unsigned long)s0.udp_rx_packets,
                 port_peer_alive(s_port0) ? "на связи" : "молчит");
        ESP_LOGI(TAG,
                 "port1: UART rx=%lu tx=%lu | UDP tx=%lu rx=%lu | партнёр %s",
                 (unsigned long)s1.uart_rx_bytes,
                 (unsigned long)s1.uart_tx_bytes,
                 (unsigned long)s1.udp_tx_packets,
                 (unsigned long)s1.udp_rx_packets,
                 port_peer_alive(s_port1) ? "на связи" : "молчит");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESPBridge ===");
    ESP_LOGI(TAG, "сборка %s %s", __DATE__, __TIME__);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    debug_init();
    led_init();

    settings_load(&s_settings);

    eth_bridge_start(s_settings.local_ip, s_settings.netmask,
                     s_settings.gateway, s_settings.peer_ip);

    // Своп применяется здесь, а не внутри порта: порту незачем знать
    // о том, что провода перепутаны — он получает уже верные пины.
    const int p0_tx = s_settings.pins0.swap ? s_settings.pins0.rx
                                            : s_settings.pins0.tx;
    const int p0_rx = s_settings.pins0.swap ? s_settings.pins0.tx
                                            : s_settings.pins0.rx;
    const int p1_tx = s_settings.pins1.swap ? s_settings.pins1.rx
                                            : s_settings.pins1.tx;
    const int p1_rx = s_settings.pins1.swap ? s_settings.pins1.tx
                                            : s_settings.pins1.rx;

    s_port0 = port_create(PORT0_UART_NUM, p0_tx,
                          p0_rx, s_settings.udp0.local,
                          s_settings.udp0.remote, s_settings.peer_ip,
                          "port0", &s_settings.port0);
    s_port1 = port_create(PORT1_UART_NUM, p1_tx,
                          p1_rx, s_settings.udp1.local,
                          s_settings.udp1.remote, s_settings.peer_ip,
                          "port1", &s_settings.port1);

    if (!s_port0 || !s_port1) {
        ESP_LOGE(TAG, "не удалось создать порты, мост не работает");
        return;
    }

    ota_start();
    web_start(s_port0, s_port1, &s_settings);
    console_start(s_port0, s_port1);

    xTaskCreate(status_task, "status", 2048, NULL, 3, NULL);
    xTaskCreate(report_task, "report", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "инициализация завершена");
}
