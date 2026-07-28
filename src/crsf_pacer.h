#pragma once

#include <Arduino.h>

#include "crsf.h"

// ============================================================================
// Выравниватель темпа выдачи CRSF-фреймов.
//
// Задача. Из сети фреймы приходят неравномерно: UDP доставляет их пачками,
// с джиттером, иногда с потерями. Полётный контроллер ждёт равномерного
// потока — при срыве темпа Betaflight/INAV поднимают failsafe.
//
// Решение. Принятые фреймы складываются в очередь, а выдача в UART идёт
// собственным таймером с заданной частотой (PPS). Сетевой джиттер гасится
// глубиной очереди, ценой небольшой добавочной задержки.
//
// Компромисс очевиден и управляется параметром PPS: выше частота — меньше
// задержка, но меньше запас на джиттер.
// ============================================================================

class CrsfPacer {
 public:
  // Глубина очереди. Больше запас — устойчивее к джиттеру, но выше задержка
  // в худшем случае. Восьми фреймов при 150 Гц хватает примерно на 53 мс
  // сетевой паузы — этого достаточно для кабельного соединения с запасом.
  static constexpr size_t QUEUE_DEPTH = 8;

  void begin(uint16_t pps);

  // Меняет частоту выдачи на лету.
  void setPps(uint16_t pps);
  uint16_t pps() const { return pps_; }

  // Кладёт фрейм в очередь. Вызывается из обработчика сетевого пакета.
  void push(const uint8_t* frame, size_t len);

  // Забирает очередной фрейм, если подошло его время. Возвращает 0, если
  // выдавать пока рано или очередь пуста.
  // Вызывать часто — точность выдачи ограничена частотой вызова.
  size_t popDue(uint8_t* out, size_t outSize);

  // Сбрасывает очередь и таймер — при смене режима или потере связи.
  void reset();

  size_t queued() const { return count_; }
  uint32_t framesDropped() const { return dropped_; }
  uint32_t framesSent() const { return sent_; }
  uint32_t starved() const { return starved_; }

 private:
  struct Slot {
    uint8_t data[CRSF_FRAME_MAX];
    uint8_t len;
  };

  Slot queue_[QUEUE_DEPTH];
  size_t head_ = 0;   // откуда читаем
  size_t tail_ = 0;   // куда пишем
  size_t count_ = 0;

  uint16_t pps_ = 150;
  uint32_t intervalMicros_ = 6666;
  uint32_t nextDueMicros_ = 0;
  bool running_ = false;

  uint32_t dropped_ = 0;
  uint32_t sent_ = 0;
  uint32_t starved_ = 0;
};
