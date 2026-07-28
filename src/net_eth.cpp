#include "net_eth.h"

#include <ETH.h>
#include <WiFi.h>

#include "config.h"

namespace {

// Состояние линка меняется в контексте обработчика событий сети, поэтому
// volatile: читается из loop(), пишется из другого контекста.
volatile bool g_linkUp = false;
bool g_lastReportedLink = false;

void onNetworkEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      // Имя хоста задаётся только после старта интерфейса — раньше его
      // просто некуда записать.
      ETH.setHostname(BRIDGE_HOSTNAME);
      Serial.println("[eth] интерфейс запущен");
      break;

    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("[eth] кабель подключён, линк есть");
      break;

    case ARDUINO_EVENT_ETH_GOT_IP:
      g_linkUp = true;
      break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
      g_linkUp = false;
      Serial.println("[eth] линк потерян");
      break;

    case ARDUINO_EVENT_ETH_STOP:
      g_linkUp = false;
      Serial.println("[eth] интерфейс остановлен");
      break;

    default:
      break;
  }
}

}  // namespace

void ethBegin() {
  WiFi.onEvent(onNetworkEvent);

  // Wi-Fi выключен намеренно и полностью: транспорт только Ethernet.
  // Радиомодуль в эфире на дроне — лишний источник помех и потребления.
  WiFi.mode(WIFI_OFF);
  WiFi.persistent(false);

  Serial.printf("[eth] роль %s, адрес %s\n", BRIDGE_ROLE_NAME,
                BRIDGE_LOCAL_IP.toString().c_str());

  // Порядок важен: ETH.begin() поднимает питание PHY через указанный пин,
  // без этого LAN8720A не отвечает по MDIO и линк не появится вообще.
  const bool started = ETH.begin(ETH_PHY_ADDR_LAN8720, ETH_PHY_POWER_PIN,
                                 ETH_PHY_MDC_PIN, ETH_PHY_MDIO_PIN,
                                 ETH_PHY_LAN8720, ETH_CLK_MODE_LAN8720);
  if (!started) {
    Serial.println("[eth] ОШИБКА: не удалось запустить интерфейс");
    return;
  }

  // Статическая конфигурация: в прямом соединении двух модулей DHCP-сервера
  // нет, адрес назначать некому.
  if (!ETH.config(BRIDGE_LOCAL_IP, BRIDGE_GATEWAY, BRIDGE_NETMASK,
                  BRIDGE_DNS)) {
    Serial.println("[eth] ОШИБКА: не удалось применить статический адрес");
  }
}

bool ethLinkUp() {
  return g_linkUp;
}

void ethLoop() {
  const bool now = g_linkUp;
  if (now == g_lastReportedLink) {
    return;
  }
  g_lastReportedLink = now;

  if (now) {
    Serial.printf("[eth] готов: IP %s, %d Мбит/с, %s\n",
                  ETH.localIP().toString().c_str(), ETH.linkSpeed(),
                  ETH.fullDuplex() ? "full-duplex" : "half-duplex");
    Serial.printf("[eth] партнёр ожидается на %s\n",
                  BRIDGE_PEER_IP.toString().c_str());
  }
}
