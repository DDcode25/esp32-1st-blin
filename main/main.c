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
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_eth.h"
#include "nvs_flash.h"
#include "port.h"

static const char *TAG = "main";

static port_t *s_port0;
static port_t *s_port1;

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
    ESP_LOGI(TAG, "роль %s, сборка %s %s", BRIDGE_ROLE_NAME, __DATE__,
             __TIME__);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    led_init();
    eth_bridge_start();

    // Оба порта стартуют в прозрачном режиме намеренно: до проверки на
    // железе безопаснее отдавать байты как есть, а CRSF включать осознанно.
    const port_config_t cfg0 = {
        .baud = 400000,
        .mode = PORT_MODE_TRANSPARENT,
        .pps = 150,
    };
    const port_config_t cfg1 = {
        .baud = 57600,
        .mode = PORT_MODE_TRANSPARENT,
        .pps = 150,
    };

    s_port0 = port_create(PORT0_UART_NUM, PORT0_TX_GPIO, PORT0_RX_GPIO,
                          PORT0_UDP_PORT, BRIDGE_PEER_IP, "port0", &cfg0);
    s_port1 = port_create(PORT1_UART_NUM, PORT1_TX_GPIO, PORT1_RX_GPIO,
                          PORT1_UDP_PORT, BRIDGE_PEER_IP, "port1", &cfg1);

    if (!s_port0 || !s_port1) {
        ESP_LOGE(TAG, "не удалось создать порты, мост не работает");
        return;
    }

    xTaskCreate(status_task, "status", 2048, NULL, 3, NULL);
    xTaskCreate(report_task, "report", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "инициализация завершена");
}
