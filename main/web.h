#pragma once

#include "port.h"
#include "settings.h"

// Поднимает веб-интерфейс. Вызывать после подъёма Ethernet.
void web_start(port_t *port0, port_t *port1, settings_t *settings);
