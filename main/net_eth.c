#include "net_eth.h"

#include <string.h>

#include "config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "eth";

static volatile bool s_link_up = false;
static esp_netif_t *s_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;
static char s_peer_ip[16];

// Обработчик событий драйвера Ethernet. Вызывается из системного цикла
// событий, поэтому внутри только запись флага и логирование.
static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "кабель подключён, линк есть");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        ESP_LOGW(TAG, "линк потерян");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "интерфейс запущен");
        break;
    case ETHERNET_EVENT_STOP:
        s_link_up = false;
        ESP_LOGI(TAG, "интерфейс остановлен");
        break;
    default:
        break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id,
                           void *data)
{
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    const esp_netif_ip_info_t *ip = &event->ip_info;

    s_link_up = true;

    ESP_LOGI(TAG, "адрес получен: " IPSTR ", маска " IPSTR,
             IP2STR(&ip->ip), IP2STR(&ip->netmask));
    ESP_LOGI(TAG, "партнёр ожидается на %s", s_peer_ip);
}

// Подаёт питание на PHY. На WT32-ETH01 LAN8720A запитан через транзистор,
// управляемый GPIO16, и до его включения не отвечает по MDIO — линк не
// появится вообще, а в логе будет тишина после запуска интерфейса.
static void phy_power_on(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << ETH_PHY_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(ETH_PHY_POWER_GPIO, 1));

    // PHY нужно время на выход из сброса до первого обращения по MDIO.
    vTaskDelay(pdMS_TO_TICKS(10));
}

void eth_bridge_start(const char *local_ip, const char *netmask,
                      const char *gateway, const char *peer_ip)
{
    strlcpy(s_peer_ip, peer_ip, sizeof(s_peer_ip));

    ESP_LOGI(TAG, "роль %s, адрес %s", BRIDGE_ROLE_NAME, local_ip);

    phy_power_on();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Базовая конфигурация Ethernet-интерфейса, но без DHCP-клиента:
    // в прямом соединении двух плат адрес выдавать некому.
    esp_netif_inherent_config_t netif_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    netif_cfg.flags &= ~ESP_NETIF_DHCP_CLIENT;
    netif_cfg.ip_info = NULL;

    esp_netif_config_t cfg = {
        .base = &netif_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&cfg);
    assert(s_netif);

    // Статический адрес назначаем до старта драйвера, чтобы он применился
    // сразу при поднятии интерфейса.
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr = esp_ip4addr_aton(local_ip);
    ip_info.netmask.addr = esp_ip4addr_aton(netmask);
    ip_info.gw.addr = esp_ip4addr_aton(gateway);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip_info));

    // MAC: интерфейс RMII, тактирование приходит извне на GPIO0.
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac_cfg.interface = EMAC_DATA_INTERFACE_RMII;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    // Тактовые 50 МГц приходят на GPIO0 от внешнего осциллятора платы.
    // Отсюда же берётся неудобство с входом в режим прошивки: GPIO0 занят.
    emac_cfg.clock_config.rmii.clock_gpio = 0;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    assert(mac);

    // PHY: используется generic-драйвер IEEE 802.3. В ESP-IDF 6 драйверы
    // конкретных микросхем вынесены из ядра в отдельные компоненты, но
    // LAN8720A соответствует стандарту, и всё нужное — определение линка,
    // скорости и дуплекса — работает через стандартные регистры.
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    assert(phy);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &s_eth_handle));

    ESP_ERROR_CHECK(
        esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               got_ip_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    ESP_LOGI(TAG, "ожидание линка, воткни кабель");
}

bool eth_bridge_link_up(void)
{
    return s_link_up;
}

int eth_bridge_link_speed(void)
{
    if (!s_link_up || !s_eth_handle) {
        return 0;
    }

    eth_speed_t speed = ETH_SPEED_10M;
    if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &speed) != ESP_OK) {
        return 0;
    }
    return speed == ETH_SPEED_100M ? 100 : 10;
}
