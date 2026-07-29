#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";
static const char *NS = "espbridge";

const char *settings_role_name(bridge_role_t role)
{
    return role == ROLE_AIR ? "AIR" : "GROUND";
}

// Пригодность GPIO под UART на WT32-ETH01.
//
// Часть выводов занята интерфейсом RMII к LAN8720A, часть работает только
// на вход, часть участвует в загрузке. Проверка не запрещает сомнительные
// варианты — только предупреждает: разводка плат встречается разная,
// и запрет мешал бы там, где на практике всё работает.
bool settings_pin_usable(int pin, bool for_tx, const char **reason)
{
    static const char *ok = NULL;
    *reason = ok;

    switch (pin) {
    // Заняты интерфейсом RMII — трогать нельзя, Ethernet перестанет работать.
    case 0:  *reason = "занят: тактирование RMII 50 МГц"; return false;
    case 18: *reason = "занят: MDIO интерфейса Ethernet"; return false;
    case 19: case 21: case 22: case 25: case 26: case 27:
        *reason = "занят: линии данных RMII"; return false;
    case 23: *reason = "занят: MDC интерфейса Ethernet"; return false;
    case 16: *reason = "занят: питание PHY"; return false;

    // Порт прошивки и отладочного лога.
    case 1: case 3:
        *reason = "занят: UART0, прошивка и лог"; return false;

    // Только вход — под TX не годятся.
    case 35: case 36: case 39:
        if (for_tx) {
            *reason = "только вход, TX невозможен";
            return false;
        }
        *reason = "только вход, без внутренней подтяжки";
        return true;

    // Работают, но с оговорками при загрузке.
    case 12:
        *reason = "плата не загрузится, если при старте высокий уровень";
        return true;
    case 15:
        *reason = "при высоком уровне на старте выводится загрузочный лог";
        return true;
    case 14:
        *reason = "выдаёт PWM в момент загрузки";
        return true;
    case 2:
        *reason = "мешает входу в режим прошивки при высоком уровне";
        return true;
    case 5: case 17:
        *reason = "на линии светодиод — нагрузка искажает фронты сигнала";
        return true;

    // Чистые.
    case 4: case 32: case 33:
        return true;

    default:
        *reason = "нет на этой плате или не выведен наружу";
        return false;
    }
}

bool settings_valid_ip(const char *s)
{
    if (!s || !*s) {
        return false;
    }

    unsigned a, b, c, d;
    char extra;
    // Лишний %c ловит мусор в хвосте: "1.2.3.4abc" иначе прошло бы.
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) {
        return false;
    }
    return a < 256 && b < 256 && c < 256 && d < 256;
}

static void load_str(nvs_handle_t nvs, const char *key, char *out,
                     size_t cap, const char *fallback)
{
    size_t len = cap;
    if (nvs_get_str(nvs, key, out, &len) != ESP_OK) {
        strlcpy(out, fallback, cap);
    }
}

void settings_load(settings_t *out)
{
    memset(out, 0, sizeof(*out));

    // Значения по умолчанию для платы, которую ещё не настраивали.
    out->role = BRIDGE_DEFAULT_ROLE;
    strlcpy(out->local_ip, BRIDGE_DEFAULT_IP, IP_STR_LEN);
    strlcpy(out->netmask, BRIDGE_NETMASK, IP_STR_LEN);
    strlcpy(out->gateway, BRIDGE_DEFAULT_GATEWAY, IP_STR_LEN);
    strlcpy(out->peer_ip, BRIDGE_DEFAULT_PEER, IP_STR_LEN);

    out->port0.baud = 400000;
    out->port0.mode = PORT_MODE_TRANSPARENT;
    out->port0.pps = 150;

    out->port1.baud = 57600;
    out->port1.mode = PORT_MODE_TRANSPARENT;
    out->port1.pps = 150;

    out->pins0.tx = PORT0_TX_GPIO;
    out->pins0.rx = PORT0_RX_GPIO;
    out->pins1.tx = PORT1_TX_GPIO;
    out->pins1.rx = PORT1_RX_GPIO;

    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "сохранённых настроек нет, взяты значения по умолчанию");
        return;
    }

    uint8_t role;
    if (nvs_get_u8(nvs, "role", &role) == ESP_OK) {
        out->role = (role == ROLE_AIR) ? ROLE_AIR : ROLE_GROUND;
    }

    load_str(nvs, "ip", out->local_ip, IP_STR_LEN, out->local_ip);
    load_str(nvs, "mask", out->netmask, IP_STR_LEN, out->netmask);
    load_str(nvs, "gw", out->gateway, IP_STR_LEN, out->gateway);
    load_str(nvs, "peer", out->peer_ip, IP_STR_LEN, out->peer_ip);

    uint32_t u32;
    uint8_t u8;
    uint16_t u16;


    if (nvs_get_u32(nvs, "p0baud", &u32) == ESP_OK) out->port0.baud = u32;
    if (nvs_get_u8(nvs, "p0mode", &u8) == ESP_OK) out->port0.mode = u8;
    if (nvs_get_u16(nvs, "p0pps", &u16) == ESP_OK) out->port0.pps = u16;

    if (nvs_get_u32(nvs, "p1baud", &u32) == ESP_OK) out->port1.baud = u32;
    if (nvs_get_u8(nvs, "p1mode", &u8) == ESP_OK) out->port1.mode = u8;
    if (nvs_get_u16(nvs, "p1pps", &u16) == ESP_OK) out->port1.pps = u16;

    int8_t i8;
    if (nvs_get_i8(nvs, "p0tx", &i8) == ESP_OK) out->pins0.tx = i8;
    if (nvs_get_i8(nvs, "p0rx", &i8) == ESP_OK) out->pins0.rx = i8;
    if (nvs_get_i8(nvs, "p1tx", &i8) == ESP_OK) out->pins1.tx = i8;
    if (nvs_get_i8(nvs, "p1rx", &i8) == ESP_OK) out->pins1.rx = i8;

    nvs_close(nvs);

    ESP_LOGI(TAG, "роль %s, %s -> %s", settings_role_name(out->role),
             out->local_ip, out->peer_ip);
    ESP_LOGI(TAG, "порт 0: TX=GPIO%d RX=GPIO%d, порт 1: TX=GPIO%d RX=GPIO%d",
             out->pins0.tx, out->pins0.rx, out->pins1.tx, out->pins1.rx);
}

bool settings_save(const settings_t *s)
{
    // Некорректный адрес, сохранённый в NVS, сделает плату недоступной по
    // сети после перезагрузки, и вернуть её можно будет только по проводу.
    if (!settings_valid_ip(s->local_ip) || !settings_valid_ip(s->netmask) ||
        !settings_valid_ip(s->gateway) || !settings_valid_ip(s->peer_ip)) {
        ESP_LOGE(TAG, "отказ: неверный формат адреса");
        return false;
    }

    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }

    nvs_set_u8(nvs, "role", (uint8_t)s->role);
    nvs_set_str(nvs, "ip", s->local_ip);
    nvs_set_str(nvs, "mask", s->netmask);
    nvs_set_str(nvs, "gw", s->gateway);
    nvs_set_str(nvs, "peer", s->peer_ip);

    nvs_set_u32(nvs, "p0baud", s->port0.baud);
    nvs_set_u8(nvs, "p0mode", (uint8_t)s->port0.mode);
    nvs_set_u16(nvs, "p0pps", s->port0.pps);

    nvs_set_u32(nvs, "p1baud", s->port1.baud);
    nvs_set_u8(nvs, "p1mode", (uint8_t)s->port1.mode);
    nvs_set_u16(nvs, "p1pps", s->port1.pps);

    nvs_set_i8(nvs, "p0tx", s->pins0.tx);
    nvs_set_i8(nvs, "p0rx", s->pins0.rx);
    nvs_set_i8(nvs, "p1tx", s->pins1.tx);
    nvs_set_i8(nvs, "p1rx", s->pins1.rx);

    const esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "настройки сохранены");
        return true;
    }
    return false;
}

bool settings_reset(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }

    // Стираем только свои ключи: флаги отладки лежат в том же пространстве
    // имён и к сетевым настройкам отношения не имеют.
    const char *keys[] = {"role", "ip", "mask", "gw", "peer",
                          "p0baud", "p0mode", "p0pps",
                          "p1baud", "p1mode", "p1pps",
                          "p0tx", "p0rx", "p1tx", "p1rx"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        nvs_erase_key(nvs, keys[i]);
    }

    const esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGW(TAG, "настройки сброшены к значениям по умолчанию");
    return err == ESP_OK;
}
