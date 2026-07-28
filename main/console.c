#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "debug.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_eth.h"
#include "port.h"

static const char *TAG = "cli";

static port_t *s_port0;
static port_t *s_port1;

static void print_help(void)
{
    printf("\n");
    printf("Команды:\n");
    printf("  help              эта справка\n");
    printf("  stat              счётчики портов\n");
    printf("  dbg               какие флаги отладки включены\n");
    printf("  dbg <флаги>       включить отладку, флаги через пробел:\n");
    printf("                      traffic  размеры пакетов\n");
    printf("                      hex      содержимое в hex\n");
    printf("                      crsf     разобранные фреймы\n");
    printf("                      pacer    очередь выдачи\n");
    printf("                      net      события сокетов\n");
    printf("                      timing   интервалы и джиттер\n");
    printf("                      all      всё сразу\n");
    printf("                      off      выключить\n");
    printf("  mode <0|1> <t|c>  режим порта: t прозрачный, c CRSF\n");
    printf("  baud <0|1> <bps>  скорость порта\n");
    printf("  pps <0|1> <Гц>    частота выдачи CRSF, 25-250\n");
    printf("  reboot            перезагрузка\n");
    printf("\n");
}

static void print_stats(void)
{
    port_stats_t s;
    port_config_t c;

    printf("\n");
    printf("линк: %s\n", eth_bridge_link_up() ? "есть" : "нет");

    for (int i = 0; i < 2; i++) {
        port_t *p = (i == 0) ? s_port0 : s_port1;
        port_get_stats(p, &s);
        port_get_config(p, &c);

        printf("\n[%s] %lu бод, %s", port_name(p), (unsigned long)c.baud,
               c.mode == PORT_MODE_CRSF ? "CRSF" : "прозрачный");
        if (c.mode == PORT_MODE_CRSF) {
            printf(" %u Гц", c.pps);
        }
        printf(", партнёр %s\n", port_peer_alive(p) ? "на связи" : "молчит");

        printf("  UART  принято %lu Б, передано %lu Б, потери %lu\n",
               (unsigned long)s.uart_rx_bytes, (unsigned long)s.uart_tx_bytes,
               (unsigned long)s.uart_tx_dropped);
        printf("  UDP   отправлено %lu, принято %lu, потери %lu\n",
               (unsigned long)s.udp_tx_packets, (unsigned long)s.udp_rx_packets,
               (unsigned long)s.udp_tx_dropped);

        if (c.mode == PORT_MODE_CRSF) {
            printf("  CRSF  собрано %lu, ошибок CRC %lu, выдано %lu\n",
                   (unsigned long)s.crsf_frames_ok,
                   (unsigned long)s.crsf_frames_bad_crc,
                   (unsigned long)s.crsf_frames_sent);
            printf("        очередь %lu/%d, переполнений %lu, простоев %lu\n",
                   (unsigned long)s.crsf_queued, PACER_QUEUE_DEPTH_PUBLIC,
                   (unsigned long)s.crsf_frames_dropped,
                   (unsigned long)s.crsf_starved);
        }
    }
    printf("\n");
}

static void cmd_dbg(char *args)
{
    if (!args || *args == '\0') {
        const uint32_t f = debug_get();
        printf("флаги 0x%02lx:%s%s%s%s%s%s%s\n", (unsigned long)f,
               f & DBG_TRAFFIC ? " traffic" : "",
               f & DBG_HEX ? " hex" : "",
               f & DBG_CRSF ? " crsf" : "",
               f & DBG_PACER ? " pacer" : "",
               f & DBG_NET ? " net" : "",
               f & DBG_TIMING ? " timing" : "",
               f == 0 ? " (выключено)" : "");
        return;
    }

    uint32_t flags = 0;
    char *tok = strtok(args, " ");
    while (tok) {
        if      (!strcmp(tok, "traffic")) flags |= DBG_TRAFFIC;
        else if (!strcmp(tok, "hex"))     flags |= DBG_HEX | DBG_TRAFFIC;
        else if (!strcmp(tok, "crsf"))    flags |= DBG_CRSF;
        else if (!strcmp(tok, "pacer"))   flags |= DBG_PACER;
        else if (!strcmp(tok, "net"))     flags |= DBG_NET;
        else if (!strcmp(tok, "timing"))  flags |= DBG_TIMING;
        else if (!strcmp(tok, "all"))     flags = 0xFF;
        else if (!strcmp(tok, "off"))     flags = 0;
        else printf("неизвестный флаг: %s\n", tok);
        tok = strtok(NULL, " ");
    }

    debug_set(flags);
}

static void cmd_mode(char *args)
{
    int idx = 0;
    char m = 0;
    if (!args || sscanf(args, "%d %c", &idx, &m) != 2 || (idx != 0 && idx != 1)) {
        printf("использование: mode <0|1> <t|c>\n");
        return;
    }

    port_t *p = (idx == 0) ? s_port0 : s_port1;
    port_config_t c;
    port_get_config(p, &c);

    if (m == 't') {
        c.mode = PORT_MODE_TRANSPARENT;
    } else if (m == 'c') {
        c.mode = PORT_MODE_CRSF;
    } else {
        printf("режим: t (прозрачный) или c (CRSF)\n");
        return;
    }

    port_apply_config(p, &c);
    printf("порт %d: %s\n", idx,
           c.mode == PORT_MODE_CRSF ? "CRSF" : "прозрачный");
}

static void cmd_baud(char *args)
{
    int idx = 0;
    unsigned baud = 0;
    if (!args || sscanf(args, "%d %u", &idx, &baud) != 2 ||
        (idx != 0 && idx != 1) || baud < 1200 || baud > 5000000) {
        printf("использование: baud <0|1> <1200..5000000>\n");
        return;
    }

    port_t *p = (idx == 0) ? s_port0 : s_port1;
    port_config_t c;
    port_get_config(p, &c);
    c.baud = baud;
    port_apply_config(p, &c);
    printf("порт %d: %u бод\n", idx, baud);
}

static void cmd_pps(char *args)
{
    int idx = 0;
    unsigned pps = 0;
    if (!args || sscanf(args, "%d %u", &idx, &pps) != 2 ||
        (idx != 0 && idx != 1)) {
        printf("использование: pps <0|1> <25..250>\n");
        return;
    }

    port_t *p = (idx == 0) ? s_port0 : s_port1;
    port_config_t c;
    port_get_config(p, &c);
    c.pps = (uint16_t)pps;
    port_apply_config(p, &c);

    port_get_config(p, &c);
    printf("порт %d: %u Гц\n", idx, c.pps);
}

static void handle_line(char *line)
{
    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    char *args = strchr(line, ' ');
    if (args) {
        *args++ = '\0';
        while (*args == ' ') {
            args++;
        }
    }

    if      (!strcmp(line, "help"))   print_help();
    else if (!strcmp(line, "stat"))   print_stats();
    else if (!strcmp(line, "dbg"))    cmd_dbg(args);
    else if (!strcmp(line, "mode"))   cmd_mode(args);
    else if (!strcmp(line, "baud"))   cmd_baud(args);
    else if (!strcmp(line, "pps"))    cmd_pps(args);
    else if (!strcmp(line, "reboot")) {
        printf("перезагрузка\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        printf("неизвестная команда: %s (help — список)\n", line);
    }
}

static void console_task(void *arg)
{
    char line[128];
    size_t pos = 0;

    printf("\nКонсоль ESPBridge. Введи help для списка команд.\n");

    while (1) {
        const int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (c == '\r' || c == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                printf("\n");
                handle_line(line);
                pos = 0;
            }
            printf("> ");
            fflush(stdout);
            continue;
        }

        if ((c == '\b' || c == 0x7F) && pos > 0) {
            pos--;
            printf("\b \b");
            fflush(stdout);
            continue;
        }

        if (c >= ' ' && pos < sizeof(line) - 1) {
            line[pos++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
    }
}

void console_start(port_t *port0, port_t *port1)
{
    s_port0 = port0;
    s_port1 = port1;

    // Чтение с UART0 через stdin: драйвер уже установлен системой под
    // консоль, второй раз его ставить нельзя.
    setvbuf(stdin, NULL, _IONBF, 0);

    xTaskCreate(console_task, "console", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "консоль на UART0, команда help");
}
