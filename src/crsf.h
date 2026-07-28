#pragma once

#include <Arduino.h>

// ============================================================================
// CRSF — разбор потока на фреймы.
//
// Формат фрейма:
//   [addr][len][type][payload...][crc8]
//    0     1    2     3..         len+1
//
// Поле len считает всё, что идёт после него самого: type + payload + crc.
// Полная длина фрейма в байтах = len + 2.
//
// CRC8 (полином 0xD5) считается по type + payload, то есть от байта 2
// включительно до последнего байта payload.
// ============================================================================

// Минимальная и максимальная длина поля len, которую считаем осмысленной.
// Ниже 2 фрейм не может нести даже type+crc, выше 62 не бывает по протоколу.
static constexpr uint8_t CRSF_LEN_MIN = 2;
static constexpr uint8_t CRSF_LEN_MAX = 62;

// Максимальный размер полного фрейма: addr + len + 62.
static constexpr size_t CRSF_FRAME_MAX = 64;

// Адреса устройств, встречающиеся в поле addr.
static constexpr uint8_t CRSF_ADDR_FLIGHT_CONTROLLER = 0xC8;
static constexpr uint8_t CRSF_ADDR_RADIO_TRANSMITTER = 0xEA;
static constexpr uint8_t CRSF_ADDR_CRSF_TRANSMITTER = 0xEE;
static constexpr uint8_t CRSF_ADDR_CRSF_RECEIVER = 0xEC;

// Типы фреймов, которые нас интересуют отдельно.
static constexpr uint8_t CRSF_TYPE_RC_CHANNELS = 0x16;
static constexpr uint8_t CRSF_TYPE_LINK_STATISTICS = 0x14;

// Считает CRC8 по правилам CRSF для указанного участка.
uint8_t crsfCrc8(const uint8_t* data, size_t len);

// Инкрементальный сборщик фреймов из байтового потока.
//
// Поток с UART приходит произвольными кусками, границы фреймов в нём не
// обозначены ничем, кроме самой структуры. Парсер накапливает байты и
// сообщает о каждом собранном фрейме с корректной контрольной суммой.
class CrsfParser {
 public:
  // Вызывается на каждый распознанный фрейм: указатель на полный фрейм
  // (включая addr и len) и его длина.
  using FrameHandler = void (*)(void* ctx, const uint8_t* frame, size_t len);

  void begin(FrameHandler handler, void* ctx);

  // Скармливает очередную порцию байт из потока.
  void feed(const uint8_t* data, size_t len);

  // Сбрасывает состояние сборки — например, при смене режима порта.
  void reset();

  uint32_t framesOk() const { return framesOk_; }
  uint32_t framesBadCrc() const { return framesBadCrc_; }
  uint32_t bytesDiscarded() const { return bytesDiscarded_; }

 private:
  void feedByte(uint8_t b);

  enum class State : uint8_t {
    WAIT_ADDR,
    WAIT_LEN,
    WAIT_BODY,
  };

  State state_ = State::WAIT_ADDR;
  uint8_t buf_[CRSF_FRAME_MAX];
  size_t pos_ = 0;
  uint8_t expectedLen_ = 0;

  FrameHandler handler_ = nullptr;
  void* ctx_ = nullptr;

  uint32_t framesOk_ = 0;
  uint32_t framesBadCrc_ = 0;
  uint32_t bytesDiscarded_ = 0;
};
