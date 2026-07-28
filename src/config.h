#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// ============================================================================
// ESPBridge — конфигурация железа и сети
//
// Плата WT32-ETH01. Почти все GPIO заняты интерфейсом RMII к LAN8720A,
// поэтому распиновка портов данных выбрана из того немногого, что выведено
// наружу и при этом двунаправленно.
// ============================================================================

// --- Ethernet / LAN8720A ----------------------------------------------------
// Эти значения продиктованы разводкой самой платы, менять их нельзя.
// Тактирование 50 МГц приходит на GPIO0 от внешнего осциллятора — по этой
// причине GPIO0 недоступен как обычный пин, а вход в режим прошивки требует
// ручного замыкания.
#define ETH_PHY_ADDR_LAN8720   1
#define ETH_PHY_POWER_PIN      16   // включение питания PHY
#define ETH_PHY_MDC_PIN        23
#define ETH_PHY_MDIO_PIN       18
#define ETH_CLK_MODE_LAN8720   ETH_CLOCK_GPIO0_IN

// --- Порты данных -----------------------------------------------------------
// UART1 — CRSF. Пины 32/33 без strapping-функций и без участия в загрузке,
// то есть самые «чистые» из доступных. CRSF чувствителен к таймингу, поэтому
// лучшие пины отданы именно ему.
#define PORT0_UART_NUM         1
#define PORT0_TX_PIN           32
#define PORT0_RX_PIN           33

// UART2 — MAVLink. Пин 15 при старте подтянут и влияет на вывод boot-лога,
// для MAVLink это безразлично, для CRSF было бы риском.
#define PORT1_UART_NUM         2
#define PORT1_TX_PIN           14
#define PORT1_RX_PIN           15

// UART0 (TXD0/RXD0) сознательно не используется под данные — это порт
// прошивки и отладочного лога. Отлаживать мост без лога крайне тяжело.
#define DEBUG_SERIAL_BAUD      115200

// GPIO2 выведен наружу и свободен — индикация состояния линка и трафика.
#define STATUS_LED_PIN         2

// --- Сеть -------------------------------------------------------------------
// Прямое соединение двух модулей кабелем, DHCP-сервера в такой топологии нет,
// поэтому адреса статические. Роль задаётся флагом сборки в platformio.ini.

#if defined(BRIDGE_ROLE_GROUND)
  #define BRIDGE_ROLE_NAME     "GROUND"
  #define BRIDGE_LOCAL_IP      IPAddress(192, 168, 4, 1)
  #define BRIDGE_PEER_IP       IPAddress(192, 168, 4, 2)
#elif defined(BRIDGE_ROLE_AIR)
  #define BRIDGE_ROLE_NAME     "AIR"
  #define BRIDGE_LOCAL_IP      IPAddress(192, 168, 4, 2)
  #define BRIDGE_PEER_IP       IPAddress(192, 168, 4, 1)
#else
  #error "Не задана роль модуля. Собирайте окружение 'ground' или 'air'."
#endif

#define BRIDGE_NETMASK         IPAddress(255, 255, 255, 0)
// Шлюза в прямом соединении нет. Указываем собственный адрес, чтобы стек
// не пытался маршрутизировать наружу.
#define BRIDGE_GATEWAY         BRIDGE_LOCAL_IP
#define BRIDGE_DNS             IPAddress(0, 0, 0, 0)

// UDP-порты: каждый порт данных получает свою пару, чтобы потоки CRSF и
// MAVLink не смешивались и могли иметь разную политику доставки.
#define PORT0_UDP_PORT         14551
#define PORT1_UDP_PORT         14552

// Порт веб-интерфейса настройки.
#define WEB_SERVER_PORT        80

// Имя устройства в сети (OTA, mDNS).
#define BRIDGE_HOSTNAME        "espbridge-" BRIDGE_ROLE_NAME

// Пароль прошивки по сети. Должен совпадать с --auth в platformio.ini.
// Без него любой в сети может залить в модуль свою прошивку.
#ifndef OTA_PASSWORD
  #define OTA_PASSWORD         "espbridge"
#endif

// --- Буферы -----------------------------------------------------------------
// Размер UDP-датаграммы. CRSF-фрейм максимум 64 байта, MAVLink v2 — 280.
// С запасом на агрегацию нескольких фреймов в один пакет.
#define UDP_PACKET_MAX         512

// Кольцевой буфер приёма с UART до отправки в сеть.
#define UART_RX_BUFFER_SIZE    2048
