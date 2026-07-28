#include "port.h"

#include "config.h"
#include "net_eth.h"

namespace {

// Максимальное время, которое байт ждёт попутчиков перед отправкой в сеть.
// Компромисс: слишком мало — множество мелких датаграмм и накладные расходы
// заголовков; слишком много — растёт задержка. При 500 мкс на 400 кбод в
// пакет собирается около 20 байт, то есть примерно один CRSF-фрейм.
constexpr uint32_t kTxAggregateMicros = 500;

// Партнёр считается живым, если данные приходили не позже этого срока.
constexpr uint32_t kPeerTimeoutMillis = 1000;

}  // namespace

BridgePort::BridgePort(uint8_t uartNum, int8_t txPin, int8_t rxPin,
                       uint16_t udpPort, const char* name)
    : serial_(uartNum),
      txPin_(txPin),
      rxPin_(rxPin),
      udpPort_(udpPort),
      name_(name),
      cfg_{115200, PortMode::TRANSPARENT, 150} {}

void BridgePort::begin(const PortConfig& cfg, const IPAddress& peer) {
  peer_ = peer;
  cfg_ = cfg;

  // Размер приёмного буфера задаётся до begin(): после открытия порта
  // драйвер его уже не переразмерит.
  serial_.setRxBufferSize(UART_RX_BUFFER_SIZE);
  serial_.begin(cfg_.baud, SERIAL_8N1, rxPin_, txPin_);

  netParser_.begin(&BridgePort::onCrsfFrameFromNetwork, this);
  pacer_.begin(cfg_.pps);

  // Слушаем свой порт. Обработчик вызывается из системного контекста сети,
  // поэтому внутри него только запись в UART и счётчики — ничего тяжёлого.
  if (udp_.listen(udpPort_)) {
    udp_.onPacket([this](AsyncUDPPacket packet) { onNetworkPacket(packet); });
    started_ = true;
    Serial.printf("[%s] %lu бод, режим %s", name_, cfg_.baud,
                  cfg_.mode == PortMode::CRSF ? "CRSF" : "прозрачный");
    if (cfg_.mode == PortMode::CRSF) {
      Serial.printf(" %u Гц", cfg_.pps);
    }
    Serial.printf(", TX=%d RX=%d, UDP :%u -> %s:%u\n", txPin_, rxPin_,
                  udpPort_, peer_.toString().c_str(), udpPort_);
  } else {
    Serial.printf("[%s] ОШИБКА: не удалось открыть UDP-порт %u\n", name_,
                  udpPort_);
  }
}

void BridgePort::applyConfig(const PortConfig& cfg) {
  const bool baudChanged = cfg.baud != cfg_.baud;
  const bool modeChanged = cfg.mode != cfg_.mode;
  cfg_ = cfg;

  if (baudChanged) {
    // Дожидаемся выхода того, что уже в аппаратной очереди: смена скорости
    // на полуотправленном байте порождает мусор на линии.
    serial_.flush();
    serial_.updateBaudRate(cfg_.baud);
    Serial.printf("[%s] скорость изменена на %lu бод\n", name_, cfg_.baud);
  }

  pacer_.setPps(cfg_.pps);

  if (modeChanged) {
    // Смена режима обнуляет незавершённую сборку и очередь: их содержимое
    // относится к прежней трактовке потока и в новой смысла не имеет.
    netParser_.reset();
    pacer_.reset();
    Serial.printf("[%s] режим изменён на %s\n", name_,
                  cfg_.mode == PortMode::CRSF ? "CRSF" : "прозрачный");
  }
}

void BridgePort::poll() {
  // --- UART -> сеть ---
  while (serial_.available() > 0) {
    const int b = serial_.read();
    if (b < 0) {
      break;
    }
    stats_.uartRxBytes++;

    if (txLen_ == 0) {
      txFirstByteMicros_ = micros();
    }
    txBuf_[txLen_++] = static_cast<uint8_t>(b);

    if (txLen_ >= sizeof(txBuf_)) {
      sendToNetwork(txBuf_, txLen_);
      txLen_ = 0;
    }
  }

  // Накопленное отправляем по таймауту, чтобы редкий поток не залипал в
  // буфере в ожидании, пока тот наполнится.
  if (txLen_ > 0 && (micros() - txFirstByteMicros_) >= kTxAggregateMicros) {
    sendToNetwork(txBuf_, txLen_);
    txLen_ = 0;
  }

  // --- сеть -> UART, режим CRSF ---
  if (cfg_.mode == PortMode::CRSF) {
    pacerTick();

    stats_.crsfFramesOk = netParser_.framesOk();
    stats_.crsfFramesBadCrc = netParser_.framesBadCrc();
    stats_.crsfFramesSent = pacer_.framesSent();
    stats_.crsfFramesDropped = pacer_.framesDropped();
    stats_.crsfStarved = pacer_.starved();
    stats_.crsfQueued = pacer_.queued();
  }
}

void BridgePort::sendToNetwork(const uint8_t* data, size_t len) {
  if (!started_ || !ethLinkUp()) {
    stats_.udpTxDropped++;
    return;
  }

  if (udp_.writeTo(data, len, peer_, udpPort_) == len) {
    stats_.udpTxPackets++;
  } else {
    stats_.udpTxDropped++;
  }
}

void BridgePort::onNetworkPacket(AsyncUDPPacket& packet) {
  const size_t len = packet.length();
  if (len == 0) {
    return;
  }

  stats_.udpRxPackets++;
  stats_.lastRxMillis = millis();

  if (cfg_.mode == PortMode::CRSF) {
    // Разбираем на фреймы и складываем в очередь. В UART здесь ничего не
    // пишем — этим занимается pacerTick() по своему таймеру.
    netParser_.feed(packet.data(), len);
    return;
  }

  // Прозрачный режим: пишем в UART только то, что действительно помещается
  // в буфер передачи. Блокирующая запись здесь недопустима — обработчик
  // выполняется в системном контексте сети, и задержка в нём тормозит стек.
  const size_t room = static_cast<size_t>(serial_.availableForWrite());
  const size_t toWrite = len < room ? len : room;

  if (toWrite > 0) {
    serial_.write(packet.data(), toWrite);
    stats_.uartTxBytes += toWrite;
  }
  if (toWrite < len) {
    // Переполнение означает, что UART не успевает за сетью: скорость порта
    // ниже реального потока данных.
    stats_.uartTxDropped += (len - toWrite);
  }
}

void BridgePort::onCrsfFrameFromNetwork(void* ctx, const uint8_t* frame,
                                        size_t len) {
  static_cast<BridgePort*>(ctx)->pacer_.push(frame, len);
}

void BridgePort::pacerTick() {
  uint8_t frame[CRSF_FRAME_MAX];
  const size_t len = pacer_.popDue(frame, sizeof(frame));
  if (len == 0) {
    return;
  }

  // Фрейм выдаём целиком или не выдаём вовсе: половина фрейма на линии —
  // это гарантированный сбой разбора у приёмника.
  if (static_cast<size_t>(serial_.availableForWrite()) < len) {
    stats_.uartTxDropped += len;
    return;
  }

  serial_.write(frame, len);
  stats_.uartTxBytes += len;
}

bool BridgePort::peerAlive() const {
  if (stats_.lastRxMillis == 0) {
    return false;
  }
  return (millis() - stats_.lastRxMillis) < kPeerTimeoutMillis;
}
