#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_rom_md5.h"

static const char *TAG = "ota";

// Команды протокола espota.
#define OTA_CMD_FLASH   0
#define OTA_CMD_SPIFFS  100
#define OTA_CMD_AUTH    200

#define OTA_UDP_PORT    3232
#define OTA_BUF_SIZE    1460  // один сегмент TCP, чтобы не дробить приём

static volatile bool s_in_progress = false;

bool ota_in_progress(void)
{
    return s_in_progress;
}

// Считает MD5 и возвращает его шестнадцатеричной строкой.
// Протокол espota оперирует именно строками, а не сырыми байтами.
static void md5_hex(const char *input, size_t len, char out[33])
{
    // Реализация MD5 из ПЗУ чипа: без внешних зависимостей и без выделения
    // памяти. В IDF 6 mbedtls-версия стала приватной и напрямую недоступна.
    uint8_t digest[16];
    md5_context_t ctx;

    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, input, (uint32_t)len);
    esp_rom_md5_final(digest, &ctx);

    for (int i = 0; i < 16; i++) {
        sprintf(&out[i * 2], "%02x", digest[i]);
    }
    out[32] = '\0';
}

// Принимает прошивку по TCP и пишет в неактивный раздел.
// Возвращает true, если образ записан и проверен целиком.
static bool receive_firmware(const char *host_ip, uint16_t host_port,
                             size_t image_size)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "нет свободного раздела для прошивки");
        return false;
    }

    ESP_LOGI(TAG, "запись в раздел %s (%lu КБ доступно, образ %u КБ)",
             target->label, (unsigned long)(target->size / 1024),
             (unsigned)(image_size / 1024));

    if (image_size > target->size) {
        ESP_LOGE(TAG, "образ не помещается в раздел");
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "не удалось создать TCP-сокет");
        return false;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(host_port),
        .sin_addr.s_addr = inet_addr(host_ip),
    };

    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "не удалось подключиться к %s:%u", host_ip, host_port);
        close(sock);
        return false;
    }

    // Таймаут на приём: без него обрыв связи подвесил бы задачу навсегда,
    // а мост остался бы остановленным.
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(target, image_size, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "не удалось начать запись");
        close(sock);
        return false;
    }

    uint8_t *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(handle);
        close(sock);
        return false;
    }

    size_t received = 0;
    int last_percent = -1;
    bool ok = true;

    while (received < image_size) {
        const int n = recv(sock, buf, OTA_BUF_SIZE, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "обрыв связи на %u из %u байт", (unsigned)received,
                     (unsigned)image_size);
            ok = false;
            break;
        }

        if (esp_ota_write(handle, buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "ошибка записи во flash");
            ok = false;
            break;
        }

        received += n;

        // Клиент espota ждёт подтверждения размера после каждой порции.
        char ack[16];
        const int ack_len = snprintf(ack, sizeof(ack), "%u", (unsigned)received);
        send(sock, ack, ack_len, 0);

        const int percent = (int)((received * 100) / image_size);
        if (percent != last_percent && percent % 10 == 0) {
            last_percent = percent;
            ESP_LOGI(TAG, "%d%%", percent);
        }
    }

    free(buf);

    if (!ok) {
        esp_ota_abort(handle);
        close(sock);
        return false;
    }

    // esp_ota_end проверяет контрольную сумму записанного образа.
    // Без этой проверки битая прошивка окирпичила бы плату.
    const esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "образ не прошёл проверку, раздел не переключён");
        } else {
            ESP_LOGE(TAG, "ошибка завершения записи: %s", esp_err_to_name(err));
        }
        close(sock);
        return false;
    }

    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        ESP_LOGE(TAG, "не удалось переключить загрузочный раздел");
        close(sock);
        return false;
    }

    send(sock, "OK", 2, 0);
    close(sock);
    return true;
}

// Разбирает приглашение вида "0 <порт> <размер> <md5>".
static bool parse_invitation(const char *msg, int *cmd, uint16_t *port,
                             size_t *size, char *md5_out, size_t md5_cap)
{
    unsigned p = 0, s = 0;
    char md5[64] = {0};

    if (sscanf(msg, "%d %u %u %63s", cmd, &p, &s, md5) != 4) {
        return false;
    }
    if (p == 0 || p > 65535 || s == 0) {
        return false;
    }

    *port = (uint16_t)p;
    *size = s;
    strlcpy(md5_out, md5, md5_cap);
    return true;
}

static void ota_task(void *arg)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "не удалось создать UDP-сокет");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(OTA_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "не удалось занять порт %d", OTA_UDP_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "готов на порту %d, версия %s", OTA_UDP_PORT, app->version);

    char msg[256];
    char nonce[33];
    char expected[33];

    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        const int n = recvfrom(sock, msg, sizeof(msg) - 1, 0,
                               (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        msg[n] = '\0';

        int cmd = 0;
        uint16_t host_port = 0;
        size_t image_size = 0;
        char file_md5[64] = {0};

        if (!parse_invitation(msg, &cmd, &host_port, &image_size, file_md5,
                              sizeof(file_md5))) {
            continue;
        }

        if (cmd != OTA_CMD_FLASH) {
            // Образы файловой системы не поддерживаем: раздела под неё нет.
            sendto(sock, "ERR: only flash supported", 25, 0,
                   (struct sockaddr *)&from, from_len);
            continue;
        }

        char host_ip[16];
        inet_ntoa_r(from.sin_addr, host_ip, sizeof(host_ip));

        ESP_LOGI(TAG, "запрос прошивки с %s, образ %u байт", host_ip,
                 (unsigned)image_size);

        // Аутентификация. Без неё любой в сети мог бы залить свою прошивку
        // в модуль — при работе через общую сеть это недопустимо.
        snprintf(nonce, sizeof(nonce), "%08lx%08lx",
                 (unsigned long)esp_random(), (unsigned long)esp_random());

        char auth_msg[64];
        const int auth_len = snprintf(auth_msg, sizeof(auth_msg), "AUTH %s",
                                      nonce);
        sendto(sock, auth_msg, auth_len, 0, (struct sockaddr *)&from, from_len);

        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const int rn = recvfrom(sock, msg, sizeof(msg) - 1, 0,
                                (struct sockaddr *)&from, &from_len);

        // Возвращаем блокирующий режим: дальше ждём следующего приглашения.
        tv.tv_sec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (rn <= 0) {
            ESP_LOGW(TAG, "клиент не ответил на запрос пароля");
            continue;
        }
        msg[rn] = '\0';

        int auth_cmd = 0;
        char cnonce[64] = {0};
        char response[64] = {0};
        if (sscanf(msg, "%d %63s %63s", &auth_cmd, cnonce, response) != 3 ||
            auth_cmd != OTA_CMD_AUTH) {
            ESP_LOGW(TAG, "неверный формат ответа на аутентификацию");
            sendto(sock, "ERR: bad auth", 13, 0, (struct sockaddr *)&from,
                   from_len);
            continue;
        }

        // Ожидаемый ответ: md5(md5(пароль) + ":" + nonce + ":" + cnonce)
        char pass_md5[33];
        md5_hex(OTA_PASSWORD, strlen(OTA_PASSWORD), pass_md5);

        char combined[160];
        const int cl = snprintf(combined, sizeof(combined), "%s:%s:%s",
                                pass_md5, nonce, cnonce);
        md5_hex(combined, cl, expected);

        if (strcmp(expected, response) != 0) {
            ESP_LOGW(TAG, "неверный пароль, прошивка отклонена");
            sendto(sock, "ERR: authentication failed", 26, 0,
                   (struct sockaddr *)&from, from_len);
            continue;
        }

        sendto(sock, "OK", 2, 0, (struct sockaddr *)&from, from_len);

        ESP_LOGW(TAG, "начало прошивки, мост остановлен");
        s_in_progress = true;

        const bool ok = receive_firmware(host_ip, host_port, image_size);

        if (ok) {
            ESP_LOGW(TAG, "прошивка записана, перезагрузка");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        s_in_progress = false;
        ESP_LOGE(TAG, "прошивка не удалась, работа продолжается на старой");
    }
}

void ota_start(void)
{
    xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL);
}

// --- Загрузка через веб-интерфейс --------------------------------------------
//
// Отдельный набор функций, а не переиспользование receive_firmware():
// там данные забираются из сокета самой платой, здесь — приходят порциями
// из обработчика HTTP-запроса, и владелец потока другой.

static esp_ota_handle_t s_upload_handle = 0;
static const esp_partition_t *s_upload_target = NULL;
static size_t s_upload_total = 0;
static size_t s_upload_written = 0;
static int s_upload_last_percent = -1;

size_t ota_upload_max_size(void)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    return target ? target->size : 0;
}

bool ota_upload_begin(size_t total_size)
{
    if (s_in_progress) {
        ESP_LOGW(TAG, "прошивка уже идёт");
        return false;
    }

    s_upload_target = esp_ota_get_next_update_partition(NULL);
    if (!s_upload_target) {
        ESP_LOGE(TAG, "нет свободного раздела для прошивки");
        return false;
    }

    if (total_size > s_upload_target->size) {
        ESP_LOGE(TAG, "образ %u КБ не помещается в раздел %u КБ",
                 (unsigned)(total_size / 1024),
                 (unsigned)(s_upload_target->size / 1024));
        return false;
    }

    // OTA_WITH_SEQUENTIAL_WRITES, а не точный размер. С точным размером
    // драйвер стирает весь нужный участок флеша одним вызовом — для 460 КБ
    // это несколько секунд, в течение которых обработчик не читает сокет.
    // Буферы TCP переполняются, и браузер получает обрыв связи.
    //
    // В этом режиме стирание идёт порциями по мере записи, и длинных пауз
    // не возникает.
    if (esp_ota_begin(s_upload_target, OTA_WITH_SEQUENTIAL_WRITES,
                      &s_upload_handle) != ESP_OK) {
        ESP_LOGE(TAG, "не удалось начать запись");
        return false;
    }

    s_upload_total = total_size;
    s_upload_written = 0;
    s_upload_last_percent = -1;

    // Флаг ставится до первой записи: задачи моста читают его и уходят
    // в простой, освобождая процессорное время для приёма файла.
    s_in_progress = true;

    ESP_LOGW(TAG, "загрузка через браузер: %u КБ в раздел %s, мост остановлен",
             (unsigned)(total_size / 1024), s_upload_target->label);
    return true;
}

bool ota_upload_write(const uint8_t *data, size_t len)
{
    if (!s_in_progress || s_upload_handle == 0) {
        return false;
    }

    if (esp_ota_write(s_upload_handle, data, len) != ESP_OK) {
        ESP_LOGE(TAG, "ошибка записи во flash на %u байте",
                 (unsigned)s_upload_written);
        ota_upload_abort();
        return false;
    }

    s_upload_written += len;

    if (s_upload_total > 0) {
        const int percent = (int)((s_upload_written * 100) / s_upload_total);
        if (percent != s_upload_last_percent && percent % 20 == 0) {
            s_upload_last_percent = percent;
            ESP_LOGI(TAG, "%d%%", percent);
        }
    }
    return true;
}

bool ota_upload_end(void)
{
    if (!s_in_progress || s_upload_handle == 0) {
        return false;
    }

    // Проверяет контрольную сумму записанного образа. Без этого битая
    // прошивка попала бы в загрузочный раздел.
    const esp_err_t err = esp_ota_end(s_upload_handle);
    s_upload_handle = 0;

    if (err != ESP_OK) {
        s_in_progress = false;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "образ не прошёл проверку — не тот файл?");
        } else {
            ESP_LOGE(TAG, "ошибка завершения: %s", esp_err_to_name(err));
        }
        return false;
    }

    if (esp_ota_set_boot_partition(s_upload_target) != ESP_OK) {
        s_in_progress = false;
        ESP_LOGE(TAG, "не удалось переключить загрузочный раздел");
        return false;
    }

    ESP_LOGW(TAG, "прошивка записана (%u КБ), нужна перезагрузка",
             (unsigned)(s_upload_written / 1024));

    // Флаг снимаем: запись закончена, а до перезагрузки мост может работать.
    s_in_progress = false;
    return true;
}

void ota_upload_abort(void)
{
    if (s_upload_handle != 0) {
        esp_ota_abort(s_upload_handle);
        s_upload_handle = 0;
    }
    s_in_progress = false;
    ESP_LOGW(TAG, "загрузка прервана, работает старая прошивка");
}
