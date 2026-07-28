#pragma once

#include "port.h"

// Читает сохранённую конфигурацию порта из NVS.
// index: 0 или 1. Если ничего не сохранено — возвращает переданные значения
// по умолчанию. Вызывать до webBegin().
PortConfig webLoadPortConfig(int index, uint32_t defaultBaud,
                             PortMode defaultMode);

// Поднимает веб-интерфейс настройки. Вызывать после ethBegin().
void webBegin(BridgePort* port0, BridgePort* port1);

// Обслуживает HTTP-клиентов. Вызывать из loop().
void webLoop();
