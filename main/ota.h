#pragma once

#include <stdbool.h>

// Запускает приём прошивки по сети. Вызывать после подъёма Ethernet.
//
// Совместимо с espota.py, который использует PlatformIO:
//   pio run -e ground-ota -t upload
//
// Пароль задаётся в config.h (OTA_PASSWORD) и должен совпадать с --auth
// в platformio.ini.
void ota_start(void);

// true, пока идёт запись прошивки. На это время мост приостанавливается:
// обслуживание портов отнимает время у записи во flash, а прерванная
// прошивка означает откат на старый раздел и потерю времени.
bool ota_in_progress(void);
