#include "crsf_pacer.h"

namespace {

// Разумные границы частоты выдачи. Ниже 25 Гц управление уже неприемлемо,
// выше 250 Гц не работает ни одна из распространённых связок.
constexpr uint16_t kPpsMin = 25;
constexpr uint16_t kPpsMax = 250;

// Если выдача отстала больше чем на столько интервалов, темп не догоняем,
// а перезапускаем от текущего момента. Иначе после долгой паузы пойдёт
// шквал фреймов подряд, что для приёмника хуже, чем ровная пауза.
constexpr uint32_t kMaxCatchupIntervals = 3;

}  // namespace

void CrsfPacer::begin(uint16_t pps) {
  reset();
  setPps(pps);
}

void CrsfPacer::setPps(uint16_t pps) {
  if (pps < kPpsMin) {
    pps = kPpsMin;
  } else if (pps > kPpsMax) {
    pps = kPpsMax;
  }
  pps_ = pps;
  intervalMicros_ = 1000000UL / pps_;
}

void CrsfPacer::reset() {
  head_ = 0;
  tail_ = 0;
  count_ = 0;
  running_ = false;
}

void CrsfPacer::push(const uint8_t* frame, size_t len) {
  if (len == 0 || len > CRSF_FRAME_MAX) {
    return;
  }

  if (count_ == QUEUE_DEPTH) {
    // Очередь заполнена. Выбрасываем самый старый фрейм, а не приходящий:
    // для управления свежая команда ценнее устаревшей. Потеря позиции стика
    // полусекундной давности безвредна, потеря текущей — нет.
    head_ = (head_ + 1) % QUEUE_DEPTH;
    count_--;
    dropped_++;
  }

  Slot& slot = queue_[tail_];
  memcpy(slot.data, frame, len);
  slot.len = static_cast<uint8_t>(len);

  tail_ = (tail_ + 1) % QUEUE_DEPTH;
  count_++;
}

size_t CrsfPacer::popDue(uint8_t* out, size_t outSize) {
  if (count_ == 0) {
    // Очередь пуста — данные из сети не поспевают за темпом выдачи.
    // Считаем это отдельно: частые срабатывания означают, что PPS выставлен
    // выше, чем реальная частота пакетов от источника.
    if (running_) {
      const uint32_t now = micros();
      if (static_cast<int32_t>(now - nextDueMicros_) >= 0) {
        starved_++;
        nextDueMicros_ = now + intervalMicros_;
      }
    }
    return 0;
  }

  const uint32_t now = micros();

  if (!running_) {
    // Первый фрейм после паузы выдаём немедленно и от него отсчитываем темп.
    running_ = true;
    nextDueMicros_ = now;
  } else if (static_cast<int32_t>(now - nextDueMicros_) < 0) {
    // Время ещё не подошло. Сравнение через знаковую разность корректно
    // переживает переполнение micros() каждые ~71 минуту.
    return 0;
  }

  const Slot& slot = queue_[head_];
  const size_t len = slot.len;
  if (len > outSize) {
    // Не помещается в буфер вызывающего — фрейм пропускаем, иначе он
    // заблокирует очередь навсегда.
    head_ = (head_ + 1) % QUEUE_DEPTH;
    count_--;
    dropped_++;
    return 0;
  }

  memcpy(out, slot.data, len);
  head_ = (head_ + 1) % QUEUE_DEPTH;
  count_--;
  sent_++;

  // Следующий момент отсчитываем от запланированного, а не от текущего:
  // иначе задержки вызова накапливаются и частота уплывает вниз.
  nextDueMicros_ += intervalMicros_;

  // Но если отстали слишком сильно, догонять не пытаемся.
  if (static_cast<int32_t>(now - nextDueMicros_) >
      static_cast<int32_t>(kMaxCatchupIntervals * intervalMicros_)) {
    nextDueMicros_ = now + intervalMicros_;
  }

  return len;
}
