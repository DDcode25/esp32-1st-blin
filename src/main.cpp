// ============================================================================
// ESPBridge — прозрачный мост UART <-> Ethernet
//
// Два модуля WT32-ETH01, соединённые кабелем напрямую, пробрасывают два
// независимых потока UART через IP: CRSF и MAVLink.
//
// Роль (GROUND/AIR) задаётся флагом сборки, от неё зависят адреса.
// Сборка: pio run -e ground   /   pio run -e air
// ============================================================================

#include <Arduino.h>
#include <ArduinoOTA.h>

#include "config.h"
#include "net_eth.h"
#include "port.h"
#include "web.h"

namespace {

BridgePort g_port0(PORT0_UART_NUM, PORT0_TX_PIN, PORT0_RX_PIN, PORT0_UDP_PORT,
                   "port0");
BridgePort g_port1(PORT1_UART_NUM, PORT1_TX_PIN, PORT1_RX_PIN, PORT1_UDP_PORT,
                   "port1");

uint32_t g_lastLedMillis = 0;
bool g_ledState = false;

// Во время записи во flash мост не обслуживается: приём данных мешает
// записи, а прерванная прошивка оставляет плату неработоспособной.
bool g_otaInProgress = false;

void setupOta() {
  ArduinoOTA.setHostname(BRIDGE_HOSTNAME);

  // Без пароля любой в сети может залить свою прошивку. Пока это прямой
  // кабель между двумя платами — риск невелик, но мост рассчитан и на работу
  // через общую сеть, где открытая OTA недопустима.
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    g_otaInProgress = true;
    Serial.println("[ota] начало прошивки, мост остановлен");
  });
  ArduinoOTA.onEnd([]() {
    g_otaInProgress = false;
    Serial.println("\n[ota] прошивка завершена, перезагрузка");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    Serial.printf("[ota] %u%%\r", (done * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    g_otaInProgress = false;
    Serial.printf("[ota] ОШИБКА %u: ", error);
    switch (error) {
      case OTA_AUTH_ERROR:    Serial.println("неверный пароль"); break;
      case OTA_BEGIN_ERROR:   Serial.println("не удалось начать"); break;
      case OTA_CONNECT_ERROR: Serial.println("обрыв соединения"); break;
      case OTA_RECEIVE_ERROR: Serial.println("ошибка приёма"); break;
      case OTA_END_ERROR:     Serial.println("не удалось завершить"); break;
      default:                Serial.println("неизвестная"); break;
    }
  });

  ArduinoOTA.begin();
  Serial.printf("[ota] готов на %s, порт 3232\n",
                BRIDGE_LOCAL_IP.toString().c_str());
}

// Индикация: линка нет — редкое мигание, линк есть но партнёр молчит —
// частое, оба конца обмениваются данными — горит ровно.
void updateStatusLed() {
  const bool link = ethLinkUp();
  const bool traffic = g_port0.peerAlive() || g_port1.peerAlive();

  if (link && traffic) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    return;
  }

  const uint32_t period = link ? 150 : 800;
  const uint32_t now = millis();
  if (now - g_lastLedMillis >= period) {
    g_lastLedMillis = now;
    g_ledState = !g_ledState;
    digitalWrite(STATUS_LED_PIN, g_ledState ? HIGH : LOW);
  }
}

}  // namespace

void setup() {
  Serial.begin(DEBUG_SERIAL_BAUD);
  delay(200);

  Serial.println();
  Serial.println("=== ESPBridge ===");
  Serial.printf("роль: %s, сборка %s %s\n", BRIDGE_ROLE_NAME, __DATE__,
                __TIME__);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  ethBegin();

  // Настройки читаются из NVS: то, что задано через веб-интерфейс, переживает
  // перезагрузку. По умолчанию порт 0 — под CRSF, порт 1 — под MAVLink.
  //
  // Порт 0 стартует в прозрачном режиме намеренно: до проверки на железе
  // безопаснее отдавать байты как есть, а CRSF включать осознанно из
  // веб-интерфейса.
  const PortConfig cfg0 = webLoadPortConfig(0, 400000, PortMode::TRANSPARENT);
  const PortConfig cfg1 = webLoadPortConfig(1, 57600, PortMode::TRANSPARENT);

  g_port0.begin(cfg0, BRIDGE_PEER_IP);
  g_port1.begin(cfg1, BRIDGE_PEER_IP);

  webBegin(&g_port0, &g_port1);
  setupOta();

  Serial.println("[main] инициализация завершена");
}

void loop() {
  ArduinoOTA.handle();

  if (g_otaInProgress) {
    // Идёт запись во flash. Всё остальное отключено: обслуживание портов
    // отнимает время у записи, а прерванная прошивка означает неработающую
    // плату до перепрошивки по проводу.
    return;
  }

  // Порты опрашиваются первыми и без задержек: задержка здесь напрямую
  // становится задержкой моста.
  g_port0.poll();
  g_port1.poll();

  ethLoop();
  webLoop();

  updateStatusLed();
}
