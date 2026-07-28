#pragma once

#include <Arduino.h>
#include <AsyncUDP.h>
#include <HardwareSerial.h>
#include <IPAddress.h>

#include "crsf.h"
#include "crsf_pacer.h"

// Профиль порта. Определяет скорость UART и то, как байты попадают в сеть.
//
// TRANSPARENT — байт-в-байт, без разбора содержимого. Подходит для MAVLink
// и любого потокового протокола: потеря пакета означает потерю сообщения,
// но темп никто не гарантирует и не обязан.
//
// CRSF — режим с разбором фреймов. Пока ведёт себя как прозрачный, разбор
// добавляется на следующем этапе. Заведён отдельным значением сейчас, чтобы
// веб-интерфейс и хранилище настроек уже знали о нём.
enum class PortMode : uint8_t {
  TRANSPARENT = 0,
  CRSF = 1,
};

struct PortConfig {
  uint32_t baud;
  PortMode mode;
  // Частота выдачи фреймов в UART в режиме CRSF. В прозрачном режиме
  // не используется.
  uint16_t pps;
};

// Счётчики для диагностики. Без них отладка моста превращается в гадание:
// непонятно, молчит ли источник, теряется ли пакет в сети, или занят UART.
struct PortStats {
  uint32_t uartRxBytes;
  uint32_t uartTxBytes;
  uint32_t udpTxPackets;
  uint32_t udpRxPackets;
  uint32_t udpTxDropped;   // не ушло в сеть (нет линка / нет партнёра)
  uint32_t uartTxDropped;  // не влезло в буфер передачи UART
  uint32_t lastRxMillis;   // когда последний раз пришло из сети

  // Счётчики режима CRSF. В прозрачном режиме остаются нулевыми.
  uint32_t crsfFramesOk;      // собрано корректных фреймов с UART
  uint32_t crsfFramesBadCrc;  // отброшено по несовпадению CRC
  uint32_t crsfFramesSent;    // выдано в UART по таймеру
  uint32_t crsfFramesDropped; // выброшено из-за переполнения очереди
  uint32_t crsfStarved;       // очередь пуста в момент выдачи
  uint32_t crsfQueued;        // сколько сейчас в очереди
};

// Один порт данных: UART <-> UDP.
//
// Направления обслуживаются независимо. Приём из сети асинхронный, приём с
// UART — опросом из loop(). Опрос выбран сознательно: прерывания по каждому
// байту на 400 кбод дают недопустимую нагрузку, а UART имеет аппаратный FIFO,
// которого хватает при вызове poll() достаточно часто.
class BridgePort {
 public:
  BridgePort(uint8_t uartNum, int8_t txPin, int8_t rxPin, uint16_t udpPort,
             const char* name);

  // Инициализирует UART и открывает UDP-сокет. Вызывать после подъёма
  // Ethernet: сокет привязывается к уже существующему интерфейсу.
  void begin(const PortConfig& cfg, const IPAddress& peer);

  // Обслуживает направление UART -> сеть. Вызывать как можно чаще.
  void poll();

  // Меняет скорость и режим на лету, без перезагрузки.
  void applyConfig(const PortConfig& cfg);

  const PortConfig& config() const { return cfg_; }
  const PortStats& stats() const { return stats_; }
  const char* name() const { return name_; }

  // true, если из сети недавно приходили данные — признак живого партнёра.
  bool peerAlive() const;

 private:
  void sendToNetwork(const uint8_t* data, size_t len);
  void onNetworkPacket(AsyncUDPPacket& packet);

  // Обслуживает выдачу фреймов из очереди в режиме CRSF.
  void pacerTick();

  // Статический мостик к методу: парсер принимает обычный указатель
  // на функцию, а не std::function — ради предсказуемости в горячем пути.
  static void onCrsfFrameFromNetwork(void* ctx, const uint8_t* frame,
                                     size_t len);

  HardwareSerial serial_;
  AsyncUDP udp_;

  // Разбор потока, пришедшего ИЗ СЕТИ, на фреймы перед постановкой в очередь.
  // Направление UART -> сеть разбора не требует: там байты уходят как есть.
  CrsfParser netParser_;
  CrsfPacer pacer_;

  const int8_t txPin_;
  const int8_t rxPin_;
  const uint16_t udpPort_;
  const char* name_;

  PortConfig cfg_;
  IPAddress peer_;
  bool started_ = false;

  PortStats stats_ = {};

  uint8_t txBuf_[512];
  size_t txLen_ = 0;
  uint32_t txFirstByteMicros_ = 0;
};
