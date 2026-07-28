#pragma once

#include <stdbool.h>

// Поднимает Ethernet со статическим адресом. Адреса берутся из сохранённых
// настроек, а не из макросов: пользователь может их менять.
//
// Возвращает управление сразу, не дожидаясь линка — кабель может быть
// воткнут позже, это не ошибка.
void eth_bridge_start(const char *local_ip, const char *netmask,
                      const char *gateway, const char *peer_ip);

// true, если физический линк установлен и адрес назначен.
bool eth_bridge_link_up(void);

// Скорость линка в Мбит/с, 0 если линка нет.
int eth_bridge_link_speed(void);
