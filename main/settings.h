#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "port.h"

// ============================================================================
// Настройки, переживающие перезагрузку.
//
// Хранятся в NVS. Роль модуля (ground/air) остаётся флагом сборки и здесь
// не меняется — от неё зависят значения по умолчанию, а не текущие.
// ============================================================================

#define IP_STR_LEN 16

// Роль влияет только на подпись в логе и интерфейсе. Поведение моста
// определяется адресами: кто на каком адресе и куда шлёт.
typedef enum {
    ROLE_GROUND = 0,
    ROLE_AIR = 1,
} bridge_role_t;

// Пины порта. Меняются только с перезагрузкой: переназначить UART на лету
// нельзя — драйвер уже установлен, и смена пинов оставила бы порт
// в неопределённом состоянии.
typedef struct {
    int8_t tx;
    int8_t rx;
    // Меняет TX и RX местами при инициализации. Перепутанные провода —
    // самая частая ошибка при подключении UART, и переключатель избавляет
    // от перепайки: достаточно поставить галочку и перезагрузить.
    bool swap;
} port_pins_t;

// UDP-порты. Обычно совпадают, но разведены отдельно: наземная станция
// может ждать данные на своём порту, отличном от того, где слушает плата.
typedef struct {
    uint16_t local;
    uint16_t remote;
} port_udp_t;

typedef struct {
    bridge_role_t role;
    char local_ip[IP_STR_LEN];
    char netmask[IP_STR_LEN];
    char gateway[IP_STR_LEN];
    char peer_ip[IP_STR_LEN];

    port_config_t port0;
    port_config_t port1;

    port_pins_t pins0;
    port_pins_t pins1;

    port_udp_t udp0;
    port_udp_t udp1;
} settings_t;

// Проверяет, годится ли пин для UART на WT32-ETH01.
// reason заполняется пояснением, если пин негоден или проблемный.
bool settings_pin_usable(int pin, bool for_tx, const char **reason);

// Читает настройки из NVS. Отсутствующие поля берутся из config.h —
// значений по умолчанию для роли, заданной при сборке.
void settings_load(settings_t *out);

// Сохраняет в NVS. Сетевые параметры применяются только после перезагрузки:
// менять адрес интерфейса на лету означало бы оборвать соединение, через
// которое пришла команда, до отправки ответа.
bool settings_save(const settings_t *s);

// Сбрасывает к значениям из config.h.
bool settings_reset(void);

// Проверяет, что строка — корректный адрес IPv4.
bool settings_valid_ip(const char *s);

const char *settings_role_name(bridge_role_t role);
