#include "web.h"

#include <ArduinoJson.h>
#include <ETH.h>
#include <Preferences.h>
#include <WebServer.h>

#include "config.h"
#include "net_eth.h"

namespace {

WebServer g_server(WEB_SERVER_PORT);
Preferences g_prefs;

BridgePort* g_port0 = nullptr;
BridgePort* g_port1 = nullptr;

// Профили из выпадающего списка. Скорости соответствуют распространённым
// конфигурациям: CRSF между пультом и передатчиком идёт на 400 кбод,
// CRSF в полётный контроллер — на 420 кбод, MAVLink обычно 57.6к или 115.2к.
struct Profile {
  const char* name;
  uint32_t baud;
  PortMode mode;
  uint16_t pps;
};

const Profile kProfiles[] = {
    {"CRSF-400", 400000, PortMode::CRSF, 150},
    {"CRSF-115", 115200, PortMode::CRSF, 150},
    {"CRSF-FC", 420000, PortMode::CRSF, 150},
    {"UART-115k", 115200, PortMode::TRANSPARENT, 0},
    {"UART-57k", 57600, PortMode::TRANSPARENT, 0},
    {"UART-460k", 460800, PortMode::TRANSPARENT, 0},
};
constexpr size_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESPBridge</title><style>
body{font-family:system-ui,sans-serif;margin:0;background:#f4f6f8;color:#111}
header{background:#111;color:#fff;padding:12px 16px;display:flex;
gap:12px;align-items:baseline}
header b{font-size:18px}
#link{font-weight:600}
main{max-width:760px;margin:0 auto;padding:16px}
.card{background:#fff;border-radius:8px;padding:16px;margin-bottom:16px;
box-shadow:0 1px 3px rgba(0,0,0,.1)}
h2{margin:0 0 12px;font-size:16px}
label{display:block;margin:8px 0 4px;font-size:13px;color:#555}
select,input{width:100%;padding:8px;border:1px solid #ccc;border-radius:4px;
font-size:14px;background:#fff}
button{background:#111;color:#fff;border:0;padding:10px 20px;border-radius:4px;
font-size:14px;cursor:pointer;margin-top:12px}
button:hover{background:#333}
table{width:100%;border-collapse:collapse;font-size:13px;margin-top:8px}
td{padding:4px 0;border-bottom:1px solid #eee}
td:last-child{text-align:right;font-variant-numeric:tabular-nums}
.ok{color:#0a0}.bad{color:#c00}
</style></head><body>
<header><b>ESPBridge</b><span id="role"></span><span id="link">—</span></header>
<main>
<div class="card"><h2>Порт 0 — UART1 (GPIO32/33)</h2>
<label>Профиль</label><select id="p0profile"></select>
<label>Скорость, бод</label><input id="p0baud" type="number">
<div id="p0ppsbox"><label>Частота выдачи, Гц (25–250)</label>
<input id="p0pps" type="number" min="25" max="250"></div>
<table id="p0stats"></table></div>

<div class="card"><h2>Порт 1 — UART2 (GPIO14/15)</h2>
<label>Профиль</label><select id="p1profile"></select>
<label>Скорость, бод</label><input id="p1baud" type="number">
<div id="p1ppsbox"><label>Частота выдачи, Гц (25–250)</label>
<input id="p1pps" type="number" min="25" max="250"></div>
<table id="p1stats"></table></div>

<button onclick="save()">Сохранить и применить</button>
</main>
<script>
let profiles=[];
// Поле частоты имеет смысл только в режиме CRSF — в прозрачном скрываем,
// чтобы не создавать впечатление, что оно на что-то влияет.
function syncPpsBox(prefix){
 const p=profiles[document.getElementById(prefix+'profile').value];
 const box=document.getElementById(prefix+'ppsbox');
 box.style.display=p&&p.crsf?'block':'none';
}
function fillProfiles(){
 for(const prefix of ['p0','p1']){
  const s=document.getElementById(prefix+'profile');s.innerHTML='';
  profiles.forEach((p,i)=>{const o=document.createElement('option');
   o.value=i;o.textContent=p.name;s.appendChild(o);});
  s.onchange=()=>{const p=profiles[s.value];
   document.getElementById(prefix+'baud').value=p.baud;
   if(p.crsf&&p.pps)document.getElementById(prefix+'pps').value=p.pps;
   syncPpsBox(prefix);};
 }
}
function statRow(k,v,cls){return `<tr><td>${k}</td>
 <td class="${cls||''}">${v}</td></tr>`;}
function renderStats(el,s){
 let h=statRow('Партнёр',s.alive?'на связи':'нет данных',s.alive?'ok':'bad')+
  statRow('UART принято',s.uartRx+' Б')+
  statRow('UART передано',s.uartTx+' Б')+
  statRow('UDP отправлено',s.udpTx+' пак.')+
  statRow('UDP принято',s.udpRx+' пак.')+
  statRow('Потери UDP',s.udpDrop,s.udpDrop>0?'bad':'')+
  statRow('Потери UART',s.uartDrop,s.uartDrop>0?'bad':'');
 if(s.crsf){
  h+=statRow('— CRSF —','')+
   statRow('Фреймов собрано',s.cOk)+
   statRow('Ошибок CRC',s.cBad,s.cBad>0?'bad':'')+
   statRow('Выдано в UART',s.cSent)+
   statRow('Очередь',s.cQueued+' / 8')+
   statRow('Переполнений',s.cDrop,s.cDrop>0?'bad':'')+
   statRow('Нехватка данных',s.cStarve,s.cStarve>0?'bad':'');
 }
 document.getElementById(el).innerHTML=h;
}
async function refresh(){
 try{
  const r=await fetch('/api/status');const d=await r.json();
  document.getElementById('role').textContent=d.role;
  const l=document.getElementById('link');
  l.textContent=d.link?`линк ${d.speed} Мбит/с · ${d.ip}`:'нет линка';
  l.className=d.link?'ok':'bad';
  renderStats('p0stats',d.p0);renderStats('p1stats',d.p1);
 }catch(e){document.getElementById('link').textContent='нет связи с модулем';}
}
async function load(){
 const r=await fetch('/api/config');const d=await r.json();
 profiles=d.profiles;fillProfiles();
 for(const prefix of ['p0','p1']){
  document.getElementById(prefix+'baud').value=d[prefix].baud;
  document.getElementById(prefix+'pps').value=d[prefix].pps;
  document.getElementById(prefix+'profile').value=d[prefix].profile;
  syncPpsBox(prefix);
 }
}
async function save(){
 const body={p0:{profile:+document.getElementById('p0profile').value,
   baud:+document.getElementById('p0baud').value,
   pps:+document.getElementById('p0pps').value},
  p1:{profile:+document.getElementById('p1profile').value,
   baud:+document.getElementById('p1baud').value,
   pps:+document.getElementById('p1pps').value}};
 const r=await fetch('/api/config',{method:'POST',
  headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
 alert(r.ok?'Применено':'Ошибка сохранения');
}
load();refresh();setInterval(refresh,1000);
</script></body></html>
)HTML";

void addStats(JsonObject obj, BridgePort* port) {
  const PortStats& s = port->stats();
  obj["uartRx"] = s.uartRxBytes;
  obj["uartTx"] = s.uartTxBytes;
  obj["udpTx"] = s.udpTxPackets;
  obj["udpRx"] = s.udpRxPackets;
  obj["udpDrop"] = s.udpTxDropped;
  obj["uartDrop"] = s.uartTxDropped;
  obj["alive"] = port->peerAlive();

  const bool crsf = port->config().mode == PortMode::CRSF;
  obj["crsf"] = crsf;
  if (crsf) {
    obj["cOk"] = s.crsfFramesOk;
    obj["cBad"] = s.crsfFramesBadCrc;
    obj["cSent"] = s.crsfFramesSent;
    obj["cDrop"] = s.crsfFramesDropped;
    obj["cStarve"] = s.crsfStarved;
    obj["cQueued"] = s.crsfQueued;
  }
}

void handleStatus() {
  JsonDocument doc;
  doc["role"] = BRIDGE_ROLE_NAME;
  doc["link"] = ethLinkUp();
  doc["ip"] = ETH.localIP().toString();
  doc["speed"] = ETH.linkSpeed();
  addStats(doc["p0"].to<JsonObject>(), g_port0);
  addStats(doc["p1"].to<JsonObject>(), g_port1);

  String out;
  serializeJson(doc, out);
  g_server.send(200, "application/json", out);
}

// Ищет индекс профиля, наиболее подходящий текущей настройке порта.
// Точного соответствия может не быть, если скорость правили вручную.
int profileIndexFor(const PortConfig& cfg) {
  for (size_t i = 0; i < kProfileCount; i++) {
    if (kProfiles[i].baud == cfg.baud && kProfiles[i].mode == cfg.mode) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void handleGetConfig() {
  JsonDocument doc;

  JsonArray profiles = doc["profiles"].to<JsonArray>();
  for (size_t i = 0; i < kProfileCount; i++) {
    JsonObject p = profiles.add<JsonObject>();
    p["name"] = kProfiles[i].name;
    p["baud"] = kProfiles[i].baud;
    p["pps"] = kProfiles[i].pps;
    p["crsf"] = kProfiles[i].mode == PortMode::CRSF;
  }

  doc["p0"]["baud"] = g_port0->config().baud;
  doc["p0"]["pps"] = g_port0->config().pps;
  doc["p0"]["profile"] = profileIndexFor(g_port0->config());
  doc["p1"]["baud"] = g_port1->config().baud;
  doc["p1"]["pps"] = g_port1->config().pps;
  doc["p1"]["profile"] = profileIndexFor(g_port1->config());

  String out;
  serializeJson(doc, out);
  g_server.send(200, "application/json", out);
}

// Применяет одну секцию настроек и сохраняет её в NVS.
void applySection(JsonObjectConst src, BridgePort* port, const char* keyBaud,
                  const char* keyMode, const char* keyPps) {
  const int profileIdx = src["profile"] | 0;

  // Скорость и частоту берём из полей ввода: пользователь мог задать
  // нестандартные, не совпадающие ни с одним профилем. Режим — из профиля.
  PortConfig cfg;
  cfg.baud = src["baud"] | 115200U;
  cfg.mode = (profileIdx >= 0 && profileIdx < static_cast<int>(kProfileCount))
                 ? kProfiles[profileIdx].mode
                 : PortMode::TRANSPARENT;

  uint32_t pps = src["pps"] | 150U;
  if (pps < 25) {
    pps = 25;
  } else if (pps > 250) {
    pps = 250;
  }
  cfg.pps = static_cast<uint16_t>(pps);

  port->applyConfig(cfg);

  g_prefs.putUInt(keyBaud, cfg.baud);
  g_prefs.putUChar(keyMode, static_cast<uint8_t>(cfg.mode));
  g_prefs.putUShort(keyPps, cfg.pps);
}

void handlePostConfig() {
  JsonDocument doc;
  if (deserializeJson(doc, g_server.arg("plain")) != DeserializationError::Ok) {
    g_server.send(400, "text/plain", "bad json");
    return;
  }

  applySection(doc["p0"], g_port0, "p0baud", "p0mode", "p0pps");
  applySection(doc["p1"], g_port1, "p1baud", "p1mode", "p1pps");

  g_server.send(200, "text/plain", "ok");
}

}  // namespace

// Читает сохранённую конфигурацию порта из NVS. Если ничего не сохранено,
// возвращает значения по умолчанию.
PortConfig webLoadPortConfig(int index, uint32_t defaultBaud,
                             PortMode defaultMode) {
  g_prefs.begin("espbridge", false);

  const char* keyBaud = (index == 0) ? "p0baud" : "p1baud";
  const char* keyMode = (index == 0) ? "p0mode" : "p1mode";
  const char* keyPps = (index == 0) ? "p0pps" : "p1pps";

  PortConfig cfg;
  cfg.baud = g_prefs.getUInt(keyBaud, defaultBaud);
  cfg.mode = static_cast<PortMode>(
      g_prefs.getUChar(keyMode, static_cast<uint8_t>(defaultMode)));
  cfg.pps = g_prefs.getUShort(keyPps, 150);
  return cfg;
}

void webBegin(BridgePort* port0, BridgePort* port1) {
  g_port0 = port0;
  g_port1 = port1;

  g_server.on("/", HTTP_GET, []() {
    g_server.send_P(200, "text/html", kIndexHtml);
  });
  g_server.on("/api/status", HTTP_GET, handleStatus);
  g_server.on("/api/config", HTTP_GET, handleGetConfig);
  g_server.on("/api/config", HTTP_POST, handlePostConfig);

  g_server.begin();
  Serial.printf("[web] интерфейс доступен на http://%s/\n",
                BRIDGE_LOCAL_IP.toString().c_str());
}

void webLoop() {
  g_server.handleClient();
}
