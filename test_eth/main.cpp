// ============================================================================
// ESPBridge — МИНИМАЛЬНЫЙ ТЕСТ ETHERNET
//
// Ничего, кроме подъёма LAN8720 и веб-страницы. Ни UART, ни CRSF, ни OTA.
//
// Проверяет ровно одну гипотезу: правильно ли выбраны параметры PHY для
// WT32-ETH01 и поднимается ли линк. Если эта прошивка работает — основной
// проект имеет под собой основание. Если нет — дальше идти незачем.
//
// Адрес платы: 192.168.4.1 (одинаковый для любой платы, тест одиночный).
// ============================================================================

#include <Arduino.h>
#include <ETH.h>
#include <WebServer.h>
#include <WiFi.h>

// Параметры PHY для WT32-ETH01. Продиктованы разводкой платы.
#define PHY_ADDR      1
#define PHY_POWER     16
#define PHY_MDC       23
#define PHY_MDIO      18
#define PHY_TYPE      ETH_PHY_LAN8720
#define PHY_CLK_MODE  ETH_CLOCK_GPIO0_IN

#define LED_PIN       2

static const IPAddress kLocalIp(192, 168, 4, 1);
static const IPAddress kGateway(192, 168, 4, 1);
static const IPAddress kNetmask(255, 255, 255, 0);

static WebServer server(80);
static volatile bool g_gotIp = false;
static uint32_t g_bootMillis = 0;

static void onNetEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("[eth] START — интерфейс инициализирован");
      ETH.setHostname("espbridge-test");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("[eth] CONNECTED — кабель подключён, есть линк");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      g_gotIp = true;
      Serial.printf("[eth] GOT_IP — адрес %s, %d Мбит/с, %s\n",
                    ETH.localIP().toString().c_str(), ETH.linkSpeed(),
                    ETH.fullDuplex() ? "full-duplex" : "half-duplex");
      Serial.printf("[eth] MAC %s\n", ETH.macAddress().c_str());
      Serial.println();
      Serial.println("  >>> ОТКРОЙ В БРАУЗЕРЕ:  http://192.168.4.1/");
      Serial.println();
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      g_gotIp = false;
      Serial.println("[eth] DISCONNECTED — линк пропал");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      g_gotIp = false;
      Serial.println("[eth] STOP");
      break;
    default:
      break;
  }
}

static void handleRoot() {
  const uint32_t up = (millis() - g_bootMillis) / 1000;

  String html = F(
      "<!doctype html><html lang=ru><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>ESPBridge - тест Ethernet</title><style>"
      "body{font-family:system-ui,sans-serif;background:#f4f6f8;color:#111;"
      "margin:0;padding:24px}"
      ".c{max-width:520px;margin:0 auto;background:#fff;border-radius:8px;"
      "padding:24px;box-shadow:0 1px 3px rgba(0,0,0,.1)}"
      "h1{margin:0 0 4px;font-size:20px}"
      ".ok{color:#0a0;font-weight:600;font-size:18px}"
      "table{width:100%;border-collapse:collapse;margin-top:16px;font-size:14px}"
      "td{padding:6px 0;border-bottom:1px solid #eee}"
      "td:last-child{text-align:right;font-variant-numeric:tabular-nums}"
      "</style></head><body><div class=c>"
      "<h1>ESPBridge</h1><div>тест Ethernet</div>"
      "<p class=ok>Ethernet работает</p><table>");

  html += "<tr><td>IP-адрес</td><td>" + ETH.localIP().toString() + "</td></tr>";
  html += "<tr><td>MAC</td><td>" + ETH.macAddress() + "</td></tr>";
  html += "<tr><td>Скорость линка</td><td>" + String(ETH.linkSpeed()) +
          " Мбит/с</td></tr>";
  html += "<tr><td>Режим</td><td>" +
          String(ETH.fullDuplex() ? "full-duplex" : "half-duplex") +
          "</td></tr>";
  html += "<tr><td>Время работы</td><td>" + String(up) + " c</td></tr>";
  html += "<tr><td>Свободно RAM</td><td>" + String(ESP.getFreeHeap()) +
          " Б</td></tr>";
  html += F("</table><p style='color:#555;font-size:13px;margin-top:16px'>"
            "Эта страница открылась — значит PHY поднялся, адрес назначен "
            "и сетевой стек работает. Можно переходить к основной прошивке."
            "</p></div></body></html>");

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  g_bootMillis = millis();

  Serial.println();
  Serial.println("========================================");
  Serial.println("  ESPBridge — тест Ethernet");
  Serial.printf("  сборка %s %s\n", __DATE__, __TIME__);
  Serial.println("========================================");
  Serial.printf("[chip] %s rev%d, %d МГц, флеш %u МБ\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getCpuFreqMHz(),
                ESP.getFlashChipSize() / (1024 * 1024));

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  WiFi.onEvent(onNetEvent);
  WiFi.mode(WIFI_OFF);

  Serial.printf("[eth] запуск PHY: addr=%d power=GPIO%d mdc=GPIO%d "
                "mdio=GPIO%d\n",
                PHY_ADDR, PHY_POWER, PHY_MDC, PHY_MDIO);

  if (!ETH.begin(PHY_ADDR, PHY_POWER, PHY_MDC, PHY_MDIO, PHY_TYPE,
                 PHY_CLK_MODE)) {
    Serial.println("[eth] !!! ОШИБКА: ETH.begin() вернул false");
    Serial.println("[eth] PHY не отвечает. Проверь питание 5В.");
  }

  if (!ETH.config(kLocalIp, kGateway, kNetmask)) {
    Serial.println("[eth] !!! ОШИБКА: не удалось задать статический адрес");
  } else {
    Serial.println("[eth] статический адрес задан: 192.168.4.1");
  }

  server.on("/", handleRoot);
  server.begin();
  Serial.println("[web] сервер запущен на порту 80");
  Serial.println();
  Serial.println("Жду линк. Воткни кабель в плату и в компьютер.");
  Serial.println("На компьютере задай адрес 192.168.4.100, маска 255.255.255.0");
  Serial.println();
}

void loop() {
  server.handleClient();

  // Индикация: линк есть — горит ровно, нет — мигает.
  static uint32_t lastBlink = 0;
  static bool ledOn = false;
  if (g_gotIp) {
    digitalWrite(LED_PIN, HIGH);
  } else if (millis() - lastBlink >= 500) {
    lastBlink = millis();
    ledOn = !ledOn;
    digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
  }

  // Раз в 5 секунд сообщаем состояние — чтобы в мониторе было видно, что
  // плата жива, даже когда линка нет.
  static uint32_t lastReport = 0;
  if (millis() - lastReport >= 5000) {
    lastReport = millis();
    if (g_gotIp) {
      Serial.printf("[status] линк есть, %s, uptime %lu c\n",
                    ETH.localIP().toString().c_str(), millis() / 1000);
    } else {
      Serial.printf("[status] линка нет, uptime %lu c\n", millis() / 1000);
    }
  }
}
