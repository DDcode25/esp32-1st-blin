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

typedef struct {
    bridge_role_t role;
    char local_ip[IP_STR_LEN];
    char netmask[IP_STR_LEN];
    char gateway[IP_STR_LEN];
    char peer_ip[IP_STR_LEN];

    port_config_t port0;
    port_config_t port1;
} settings_t;

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
