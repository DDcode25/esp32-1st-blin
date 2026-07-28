#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_eth.h"
#include "ota.h"

static const char *TAG = "web";

static port_t *s_port0;
static port_t *s_port1;
static settings_t *s_settings;

// Страница одна и целиком в прошивке: отдельной файловой системы нет,
// а раздел под неё занял бы место, нужное для второй копии прошивки под OTA.
static const char INDEX_HTML[] =
"<!doctype html><html lang=ru><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>ESPBridge</title><style>"
"body{font-family:system-ui,sans-serif;margin:0;background:#f4f6f8;color:#111}"
"header{background:#111;color:#fff;padding:12px 16px;display:flex;gap:12px;"
"align-items:baseline;flex-wrap:wrap}"
"header b{font-size:18px}#link{font-weight:600}"
"main{max-width:780px;margin:0 auto;padding:16px}"
".card{background:#fff;border-radius:8px;padding:16px;margin-bottom:16px;"
"box-shadow:0 1px 3px rgba(0,0,0,.1)}"
"h2{margin:0 0 12px;font-size:16px}"
"label{display:block;margin:8px 0 4px;font-size:13px;color:#555}"
"input,select{width:100%;padding:8px;border:1px solid #ccc;border-radius:4px;"
"font-size:14px;background:#fff;box-sizing:border-box}"
"button{background:#111;color:#fff;border:0;padding:10px 20px;"
"border-radius:4px;font-size:14px;cursor:pointer;margin:12px 8px 0 0}"
"button:hover{background:#333}button.sec{background:#666}"
"table{width:100%;border-collapse:collapse;font-size:13px;margin-top:8px}"
"td{padding:4px 0;border-bottom:1px solid #eee}"
"td:last-child{text-align:right;font-variant-numeric:tabular-nums}"
".ok{color:#0a0}.bad{color:#c00}.warn{background:#fff8e1;padding:8px;"
"border-radius:4px;font-size:13px;margin-top:8px}"
".row{display:flex;gap:12px}.row>div{flex:1}"
"</style></head><body>"
"<header><b>ESPBridge</b><span id=rolebadge></span>"
"<span id=link>—</span></header>"
"<main>"

"<div class=card><h2>Сеть</h2>"
"<label>Роль модуля</label><select id=role onchange=roleChanged()>"
"<option value=0>GROUND — наземный, у пульта</option>"
"<option value=1>AIR — бортовой, у полётного контроллера</option>"
"</select>"
"<div class=row>"
"<div><label>IP этого модуля</label><input id=ip></div>"
"<div><label>Маска</label><input id=mask></div></div>"
"<div class=row>"
"<div><label>Шлюз</label><input id=gw></div>"
"<div><label>IP партнёра</label><input id=peer></div></div>"
"<div class=warn>Сетевые настройки применяются после перезагрузки. "
"Смена роли подставляет типовые адреса — их можно изменить вручную.</div>"
"</div>"

"<div class=card><h2>Порт 0 — UART1 (GPIO32/33), CRSF</h2>"
"<div class=row>"
"<div><label>Скорость, бод</label><input id=p0baud type=number></div>"
"<div><label>Режим</label><select id=p0mode>"
"<option value=0>прозрачный</option><option value=1>CRSF</option>"
"</select></div></div>"
"<div id=p0ppsbox><label>Частота выдачи, Гц (25–250)</label>"
"<input id=p0pps type=number min=25 max=250></div>"
"<table id=p0stats></table></div>"

"<div class=card><h2>Порт 1 — UART2 (GPIO14/15), MAVLink</h2>"
"<div class=row>"
"<div><label>Скорость, бод</label><input id=p1baud type=number></div>"
"<div><label>Режим</label><select id=p1mode>"
"<option value=0>прозрачный</option><option value=1>CRSF</option>"
"</select></div></div>"
"<div id=p1ppsbox><label>Частота выдачи, Гц (25–250)</label>"
"<input id=p1pps type=number min=25 max=250></div>"
"<table id=p1stats></table></div>"

"<div class=card><h2>Терминал <button class=sec style='padding:4px 10px;"
"font-size:12px;margin:0 0 0 8px' onclick=termToggle()>показать</button></h2>"
"<div id=termbox style=display:none>"
"<div class=row>"
"<div><label>Порт</label><select id=termport>"
"<option value=0>Порт 0 — CRSF</option>"
"<option value=1>Порт 1 — MAVLink</option></select></div>"
"<div><label>Куда отправлять</label><select id=termdst>"
"<option value=net>партнёру через мост</option>"
"<option value=uart>в UART этой платы</option>"
"<option value=both>и туда, и туда</option>"
"</select></div></div>"
"<label>Вид</label><select id=termview>"
"<option value=text>текст</option><option value=hex>hex</option></select>"
"<pre id=termout style='background:#111;color:#0f0;padding:10px;"
"border-radius:4px;height:180px;overflow:auto;font-size:12px;"
"margin-top:12px;white-space:pre-wrap;word-break:break-all'></pre>"
"<div class=row style=margin-top:8px>"
"<div style=flex:4><input id=terminput placeholder='текст для отправки' "
"onkeydown='if(event.key==\"Enter\")termSend()'></div>"
"<div style=flex:1><button onclick=termSend() "
"style=margin:0;width:100%>Отправить</button></div></div>"
"<div class=warn>Принятое показывается с пометкой источника: "
"<b>U</b> — пришло с UART, <b>N</b> — из сети от партнёра. "
"Чтобы проверить мост целиком, замкни TX и RX на дальней плате: "
"отправленное вернётся обратно.<br><br>"
"Терминал — средство отладки, а не полноценный монитор: при плотном "
"трафике часть байт не попадёт на экран. Счётчики выше считают всё.</div>"
"</div></div>"

"<div class=card><h2>Обновление прошивки</h2>"
"<label>Файл firmware.bin</label>"
"<input type=file id=fw accept=.bin>"
"<div class=warn>Нужен <b>firmware.bin</b> из .pio/build/espbridge/, "
"а не espbridge_MERGED.bin. Через сеть обновляется только приложение; "
"загрузчик и таблица разделов остаются на месте.</div>"
"<div id=fwprog style=display:none>"
"<div style='background:#eee;border-radius:4px;height:8px;margin-top:12px'>"
"<div id=fwbar style='background:#111;height:8px;border-radius:4px;width:0%'>"
"</div></div><div id=fwtext style='font-size:13px;margin-top:6px'></div>"
"</div>"
"<button onclick=upload()>Прошить</button></div>"

"<button onclick=save()>Сохранить</button>"
"<button class=sec onclick=reboot()>Перезагрузить</button>"
"<button class=sec onclick=reset()>Сбросить настройки</button>"
"</main>"

"<script>"
"function g(i){return document.getElementById(i)}"
"function roleChanged(){const a=g('role').value=='1';"
"g('ip').value=a?'192.168.4.2':'192.168.4.1';"
"g('peer').value=a?'192.168.4.1':'192.168.4.2';"
"g('gw').value=g('ip').value}"
"function sync(p){g(p+'ppsbox').style.display=g(p+'mode').value=='1'"
"?'block':'none'}"
"function row(k,v,c){return `<tr><td>${k}</td><td class=\"${c||''}\">${v}"
"</td></tr>`}"
"function stats(el,s){let h=row('Партнёр',s.alive?'на связи':'нет данных',"
"s.alive?'ok':'bad')"
"+row('UART принято',s.urx+' Б')+row('UART передано',s.utx+' Б')"
"+row('UDP отправлено',s.dtx)+row('UDP принято',s.drx)"
"+row('Потери UDP',s.ddrop,s.ddrop>0?'bad':'')"
"+row('Потери UART',s.udrop,s.udrop>0?'bad':'');"
"if(s.crsf){h+=row('— CRSF —','')+row('Фреймов собрано',s.cok)"
"+row('Ошибок CRC',s.cbad,s.cbad>0?'bad':'')+row('Выдано в UART',s.csent)"
"+row('Очередь',s.cq+' / 8')"
"+row('Переполнений',s.cdrop,s.cdrop>0?'bad':'')"
"+row('Нехватка данных',s.cstarve,s.cstarve>0?'bad':'')}"
"g(el).innerHTML=h}"
"async function refresh(){try{"
"const d=await(await fetch('/api/status')).json();"
"g('rolebadge').textContent=d.role;const l=g('link');"
"l.textContent=d.link?'линк '+d.speed+' Мбит/с':'нет линка';"
"l.className=d.link?'ok':'bad';"
"stats('p0stats',d.p0);stats('p1stats',d.p1);"
"}catch(e){g('link').textContent='нет связи с модулем';g('link').className='bad'}}"
"async function load(){const d=await(await fetch('/api/config')).json();"
"g('role').value=d.role;g('ip').value=d.ip;g('mask').value=d.mask;"
"g('gw').value=d.gw;g('peer').value=d.peer;"
"for(const p of ['p0','p1']){g(p+'baud').value=d[p].baud;"
"g(p+'mode').value=d[p].mode;g(p+'pps').value=d[p].pps;"
"g(p+'mode').onchange=()=>sync(p);sync(p)}}"
"async function save(){const b={role:+g('role').value,ip:g('ip').value,"
"mask:g('mask').value,"
"gw:g('gw').value,peer:g('peer').value,"
"p0:{baud:+g('p0baud').value,mode:+g('p0mode').value,pps:+g('p0pps').value},"
"p1:{baud:+g('p1baud').value,mode:+g('p1mode').value,pps:+g('p1pps').value}};"
"const r=await fetch('/api/config',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});"
"alert(r.ok?'Сохранено. Настройки портов применены сразу, сетевые — после "
"перезагрузки.':'Ошибка: проверь формат адресов')}"
"async function reboot(){if(!confirm('Перезагрузить модуль?'))return;"
"await fetch('/api/reboot',{method:'POST'});"
"alert('Перезагрузка. Обнови страницу через несколько секунд.')}"
"async function reset(){if(!confirm('Сбросить все настройки к заводским?'))"
"return;await fetch('/api/reset',{method:'POST'});"
"alert('Сброшено. Нужна перезагрузка.')}"
"let termOn=false,termPos={0:0,1:0},termTimer=null;"
"function termToggle(){termOn=!termOn;"
"g('termbox').style.display=termOn?'block':'none';"
"event.target.textContent=termOn?'скрыть':'показать';"
"if(termOn){termPoll();termTimer=setInterval(termPoll,300)}"
"else{clearInterval(termTimer);termTimer=null}}"
"function termPoll(){const p=g('termport').value;"
"fetch('/api/term?p='+p+'&from='+termPos[p]).then(r=>r.json()).then(d=>{"
"termPos[p]=d.pos;if(!d.data.length)return;"
"const hex=g('termview').value=='hex';const out=g('termout');"
"let t='';let lastSrc=null;"
"for(const it of d.data){"
"if(it.s!==lastSrc){t+=(t?'\n':'')+(it.s?'N< ':'U< ');lastSrc=it.s}"
"if(hex){t+=it.b.toString(16).padStart(2,'0')+' '}"
"else{t+=(it.b>=32&&it.b<127)?String.fromCharCode(it.b):"
"(it.b==10?'\n'+(lastSrc?'N< ':'U< '):'.')}}"
"out.textContent+=t;"
"if(out.textContent.length>4000)"
"out.textContent=out.textContent.slice(-4000);"
"out.scrollTop=out.scrollHeight}).catch(()=>{})}"
"function termSend(){const v=g('terminput').value;if(!v)return;"
"fetch('/api/term?p='+g('termport').value+'&dst='+g('termdst').value,"
"{method:'POST',body:v}).then(()=>{"
"g('termout').textContent+='\n> '+v+'\n';"
"g('terminput').value='';"
"g('termout').scrollTop=g('termout').scrollHeight})}"
"let busy=false;"
"function upload(){const f=g('fw').files[0];"
"if(!f){alert('Сначала выбери файл firmware.bin');return}"
"if(f.size>2000000){alert('Файл '+Math.round(f.size/1024)+' КБ — это "
"слишком много. Похоже, выбран дамп флеша или склеенный образ.\n\n"
"Нужен firmware.bin из .pio/build/espbridge/ (около 460 КБ).');return}"
"if(f.name.includes('MERGED')){"
"if(!confirm('Похоже, это склеенный образ, а не firmware.bin. "
"Он не подойдёт для обновления по сети. Всё равно продолжить?'))return}"
"if(!confirm('Прошить '+f.name+' ('+Math.round(f.size/1024)+' КБ)? "
"Мост остановится на время записи.'))return;"
"busy=true;g('fwprog').style.display='block';"
"const x=new XMLHttpRequest();x.open('POST','/api/update');"
"x.upload.onprogress=e=>{if(e.lengthComputable){"
"const p=Math.round(e.loaded*100/e.total);"
"g('fwbar').style.width=p+'%';g('fwtext').textContent=p+'%'}};"
"x.onload=()=>{busy=false;"
"if(x.status==200){g('fwtext').textContent='Готово';"
"if(confirm('Прошивка записана. Перезагрузить сейчас?'))reboot()}"
"else{g('fwtext').textContent='Ошибка: '+x.responseText;"
"alert('Не удалось прошить: '+x.responseText)}};"
"x.onerror=()=>{busy=false;g('fwtext').textContent='Обрыв связи';"
"alert('Обрыв связи. Плата работает на старой прошивке.')};"
"x.send(f)}"
"load();refresh();setInterval(()=>{if(!busy)refresh()},1000);"
"</script></body></html>";

// --- вспомогательное для JSON ------------------------------------------------

// Ищет "ключ":значение в теле запроса. Полноценный разбор JSON здесь избыточен:
// структура запроса фиксирована и известна.
static bool json_str(const char *body, const char *key, char *out, size_t cap)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(body, pattern);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return false;
    }
    p = strchr(p, '"');
    if (!p) {
        return false;
    }
    p++;

    const char *end = strchr(p, '"');
    if (!end) {
        return false;
    }

    const size_t len = (size_t)(end - p);
    if (len >= cap) {
        return false;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool json_num(const char *body, const char *section, const char *key,
                     long *out)
{
    const char *scope = body;

    if (section) {
        char pattern[32];
        snprintf(pattern, sizeof(pattern), "\"%s\"", section);
        scope = strstr(body, pattern);
        if (!scope) {
            return false;
        }
    }

    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(scope, pattern);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return false;
    }

    *out = strtol(p + 1, NULL, 10);
    return true;
}

// --- обработчики -------------------------------------------------------------

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static void append_stats(char *buf, size_t cap, size_t *pos, const char *name,
                         port_t *p)
{
    port_stats_t s;
    port_config_t c;
    port_get_stats(p, &s);
    port_get_config(p, &c);

    *pos += snprintf(buf + *pos, cap - *pos,
        "\"%s\":{\"alive\":%s,\"urx\":%lu,\"utx\":%lu,\"dtx\":%lu,"
        "\"drx\":%lu,\"ddrop\":%lu,\"udrop\":%lu,\"crsf\":%s",
        name, port_peer_alive(p) ? "true" : "false",
        (unsigned long)s.uart_rx_bytes, (unsigned long)s.uart_tx_bytes,
        (unsigned long)s.udp_tx_packets, (unsigned long)s.udp_rx_packets,
        (unsigned long)s.udp_tx_dropped, (unsigned long)s.uart_tx_dropped,
        c.mode == PORT_MODE_CRSF ? "true" : "false");

    if (c.mode == PORT_MODE_CRSF) {
        *pos += snprintf(buf + *pos, cap - *pos,
            ",\"cok\":%lu,\"cbad\":%lu,\"csent\":%lu,\"cq\":%lu,"
            "\"cdrop\":%lu,\"cstarve\":%lu",
            (unsigned long)s.crsf_frames_ok,
            (unsigned long)s.crsf_frames_bad_crc,
            (unsigned long)s.crsf_frames_sent,
            (unsigned long)s.crsf_queued,
            (unsigned long)s.crsf_frames_dropped,
            (unsigned long)s.crsf_starved);
    }

    *pos += snprintf(buf + *pos, cap - *pos, "}");
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char buf[768];
    size_t pos = 0;

    pos += snprintf(buf, sizeof(buf),
                    "{\"role\":\"%s\",\"link\":%s,\"speed\":%d,",
                    settings_role_name(s_settings->role),
                    eth_bridge_link_up() ? "true" : "false",
                    eth_bridge_link_speed());

    append_stats(buf, sizeof(buf), &pos, "p0", s_port0);
    pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
    append_stats(buf, sizeof(buf), &pos, "p1", s_port1);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, pos);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char buf[512];
    const int n = snprintf(buf, sizeof(buf),
        "{\"role\":%d,\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\","
        "\"peer\":\"%s\","
        "\"p0\":{\"baud\":%lu,\"mode\":%d,\"pps\":%u},"
        "\"p1\":{\"baud\":%lu,\"mode\":%d,\"pps\":%u}}",
        (int)s_settings->role, s_settings->local_ip, s_settings->netmask,
        s_settings->gateway, s_settings->peer_ip,
        (unsigned long)s_settings->port0.baud, (int)s_settings->port0.mode,
        s_settings->port0.pps,
        (unsigned long)s_settings->port1.baud, (int)s_settings->port1.mode,
        s_settings->port1.pps);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static void apply_port_section(const char *body, const char *section,
                               port_t *port, port_config_t *cfg)
{
    long v;

    if (json_num(body, section, "baud", &v) && v >= 1200 && v <= 5000000) {
        cfg->baud = (uint32_t)v;
    }
    if (json_num(body, section, "mode", &v)) {
        cfg->mode = (v == 1) ? PORT_MODE_CRSF : PORT_MODE_TRANSPARENT;
    }
    if (json_num(body, section, "pps", &v) && v >= 25 && v <= 250) {
        cfg->pps = (uint16_t)v;
    }

    // Параметры портов применяются сразу: они не рвут соединение, через
    // которое пришла команда.
    port_apply_config(port, cfg);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char body[512];
    const int len = req->content_len < (int)sizeof(body) - 1
                        ? req->content_len
                        : (int)sizeof(body) - 1;

    const int received = httpd_req_recv(req, body, len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    settings_t next = *s_settings;

    long role;
    if (json_num(body, NULL, "role", &role)) {
        next.role = (role == 1) ? ROLE_AIR : ROLE_GROUND;
    }

    char tmp[IP_STR_LEN];
    if (json_str(body, "ip", tmp, sizeof(tmp)))   strlcpy(next.local_ip, tmp, IP_STR_LEN);
    if (json_str(body, "mask", tmp, sizeof(tmp))) strlcpy(next.netmask, tmp, IP_STR_LEN);
    if (json_str(body, "gw", tmp, sizeof(tmp)))   strlcpy(next.gateway, tmp, IP_STR_LEN);
    if (json_str(body, "peer", tmp, sizeof(tmp))) strlcpy(next.peer_ip, tmp, IP_STR_LEN);

    apply_port_section(body, "p0", s_port0, &next.port0);
    apply_port_section(body, "p1", s_port1, &next.port1);

    if (!settings_save(&next)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid settings");
        return ESP_FAIL;
    }

    *s_settings = next;

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", 2);
}

// Приём файла прошивки. Тело читается порциями: образ около 500 КБ,
// а свободной кучи меньше — целиком в память он не поместится.
static esp_err_t update_handler(httpd_req_t *req)
{
    const size_t total = req->content_len;

    if (total == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "приём прошивки: %u байт", (unsigned)total);

    // Размер проверяем до начала записи и отвечаем внятно: чаще всего сюда
    // приходит дамп флеша на 4 МБ или склеенный образ вместо firmware.bin,
    // и пользователю нужно понять, какой файл брать.
    const size_t limit = ota_upload_max_size();
    if (total > limit) {
        // Русский текст в UTF-8 занимает по два байта на букву, отсюда
        // размер буфера с запасом.
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Файл %u КБ не помещается в раздел %u КБ. "
                 "Нужен firmware.bin, а не дамп флеша и не *_MERGED.bin.",
                 (unsigned)(total / 1024), (unsigned)(limit / 1024));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_FAIL;
    }

    if (!ota_upload_begin(total)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "не удалось начать запись, см. лог");
        return ESP_FAIL;
    }

    // Порция 2 КБ: соответствует типичному сегменту TCP и не заставляет
    // обработчик надолго уходить из чтения сокета.
    enum { CHUNK = 2048, MAX_TIMEOUTS = 20 };

    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        ota_upload_abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    int timeouts = 0;

    while (received < total) {
        const size_t want = (total - received) < CHUNK ? (total - received)
                                                       : CHUNK;
        const int n = httpd_req_recv(req, (char *)buf, want);

        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            // Единичные таймауты нормальны — данные ещё в пути. Но крутиться
            // вечно нельзя: при реальном обрыве мост остался бы остановленным
            // навсегда.
            if (++timeouts > MAX_TIMEOUTS) {
                ESP_LOGE(TAG, "приём прерван: нет данных на %u из %u байт",
                         (unsigned)received, (unsigned)total);
                free(buf);
                ota_upload_abort();
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                    "timeout");
                return ESP_FAIL;
            }
            continue;
        }
        timeouts = 0;

        if (n <= 0) {
            ESP_LOGE(TAG, "обрыв на %u из %u байт (код %d)",
                     (unsigned)received, (unsigned)total, n);
            free(buf);
            ota_upload_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "connection lost");
            return ESP_FAIL;
        }

        if (!ota_upload_write(buf, n)) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "write failed");
            return ESP_FAIL;
        }
        received += n;
    }

    free(buf);
    ESP_LOGI(TAG, "принято %u байт, проверка образа", (unsigned)received);

    if (!ota_upload_end()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "image rejected: wrong file?");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", 2);
}

// Отдаёт накопленное в буфере терминала начиная с позиции from.
// Страница помнит свою позицию и запрашивает только новое.
static esp_err_t term_get_handler(httpd_req_t *req)
{
    char query[64];
    int port_idx = 0;
    uint32_t from = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[24];
        if (httpd_query_key_value(query, "p", val, sizeof(val)) == ESP_OK) {
            port_idx = atoi(val);
        }
        if (httpd_query_key_value(query, "from", val, sizeof(val)) == ESP_OK) {
            from = (uint32_t)strtoul(val, NULL, 10);
        }
    }

    port_t *p = (port_idx == 1) ? s_port1 : s_port0;

    // Порция ограничена: за 300 мс опроса на 400 кбод накопится больше,
    // чем стоит гнать в браузер одним ответом.
    uint8_t data[256];
    term_src_t src[256];
    const size_t n = port_terminal_read(p, &from, data, sizeof(data), src);

    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    size_t pos = snprintf(buf, 4096, "{\"pos\":%lu,\"data\":[",
                          (unsigned long)from);
    for (size_t i = 0; i < n; i++) {
        pos += snprintf(buf + pos, 4096 - pos, "%s{\"b\":%u,\"s\":%d}",
                        i ? "," : "", data[i], (int)src[i]);
    }
    pos += snprintf(buf + pos, 4096 - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    const esp_err_t r = httpd_resp_send(req, buf, pos);
    free(buf);
    return r;
}

// Отправляет данные из терминала в UART, партнёру или туда и туда.
static esp_err_t term_post_handler(httpd_req_t *req)
{
    char query[64];
    int port_idx = 0;
    bool to_uart = false;
    bool to_net = true;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[24];
        if (httpd_query_key_value(query, "p", val, sizeof(val)) == ESP_OK) {
            port_idx = atoi(val);
        }
        if (httpd_query_key_value(query, "dst", val, sizeof(val)) == ESP_OK) {
            to_uart = (strcmp(val, "uart") == 0) || (strcmp(val, "both") == 0);
            to_net = (strcmp(val, "net") == 0) || (strcmp(val, "both") == 0);
        }
    }

    char body[256];
    const int len = req->content_len < (int)sizeof(body) - 1
                        ? req->content_len : (int)sizeof(body) - 1;
    const int received = httpd_req_recv(req, body, len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
        return ESP_FAIL;
    }

    port_t *p = (port_idx == 1) ? s_port1 : s_port0;
    port_terminal_send(p, (const uint8_t *)body, received, to_uart, to_net);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", 2);
}

static void reboot_task(void *arg)
{
    // Даём ответу дойти до браузера: перезагрузка посреди отправки оставила
    // бы пользователя с ошибкой соединения вместо подтверждения.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_send(req, "ok", 2);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t reset_handler(httpd_req_t *req)
{
    settings_reset();
    httpd_resp_send(req, "ok", 2);
    return ESP_OK;
}

void web_start(port_t *port0, port_t *port1, settings_t *settings)
{
    s_port0 = port0;
    s_port1 = port1;
    s_settings = settings;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();

    // Приоритет ниже, чем у задач моста: веб-интерфейс не должен отнимать
    // время у передачи данных.
    cfg.task_priority = 4;

    // Загрузка прошивки занимает десятки секунд — стандартных 5 с мало.
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;

    // Обработчик update_handler держит соединение долго и работает с
    // буфером и вызовами esp_ota_write; стека по умолчанию не хватает.
    cfg.stack_size = 8192;

    // Запас соединений: во время загрузки прошивки браузер может держать
    // открытыми и другие. При нехватке сервер закрывает наименее свежее —
    // и под нож попадает именно долгая загрузка.
    cfg.max_open_sockets = 7;

    // lru_purge отключён по той же причине: он закрывает долгоживущие
    // соединения, а загрузка прошивки — самое долгое из них.
    cfg.lru_purge_enable = false;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "не удалось запустить веб-сервер");
        return;
    }

    const httpd_uri_t routes[] = {
        {"/",            HTTP_GET,  index_handler,       NULL},
        {"/api/status",  HTTP_GET,  status_handler,      NULL},
        {"/api/config",  HTTP_GET,  config_get_handler,  NULL},
        {"/api/config",  HTTP_POST, config_post_handler, NULL},
        {"/api/reboot",  HTTP_POST, reboot_handler,      NULL},
        {"/api/reset",   HTTP_POST, reset_handler,       NULL},
        {"/api/update",  HTTP_POST, update_handler,      NULL},
        {"/api/term",    HTTP_GET,  term_get_handler,    NULL},
        {"/api/term",    HTTP_POST, term_post_handler,   NULL},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "интерфейс доступен на http://%s/", settings->local_ip);
}
