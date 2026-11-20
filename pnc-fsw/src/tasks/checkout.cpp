#include "tasks/checkout.h"

#include <LEAmDNS.h>
#include <WiFi.h>
#include <hardware/watchdog.h>
#include <math.h>

#include "SysVar.h"

namespace {
constexpr uint32_t kStaleMs = 10000;
constexpr uint32_t kClientTimeoutMs = 10000;
WiFiServer server(80);
WiFiClient client;
bool active = false;
bool mdns_active = false;
uint32_t client_started = 0;
char request[160];
size_t request_len = 0;
size_t header_len = 0;
bool first_line_done = false;
bool line_has_content = false;
char response[4096];
size_t response_len = 0;
size_t response_sent = 0;

const char page[] = R"HTML(<!doctype html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PnC checkout</title><style>
body{font:16px system-ui;max-width:800px;margin:2rem auto;padding:0 1rem;color:#17212c}
table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ccc;padding:.6rem;text-align:left}
.bad{color:#a02020}.good{color:#176c36}small{color:#555}
</style></head><body><h1>PnC checkout</h1>
<p id="connection">Connecting...</p><p id="remaining"></p>
<table><thead><tr><th>Device</th><th>Status</th><th>Reading</th></tr></thead><tbody id="rows"></tbody></table>
<p><small>Values are unavailable when no successful sample was received in the last 10 seconds. GPS fix is separate from receiver status. RadiaCode clock sync runs after checkout; its own recording is independent.</small></p>
<script>
const rows=document.querySelector('#rows'),connection=document.querySelector('#connection');
function row(name,s,value){return `<tr><td>${name}</td><td class="${s.ok?'good':'bad'}">${s.ok?'Fresh':s.present?'Waiting / stale':'Not detected'}${s.age_ms===null?'':` (${(s.age_ms/1000).toFixed(1)}s)`}</td><td>${s.ok?value:'—'}</td></tr>`}
async function update(){try{let r=await fetch('/status',{cache:'no-store'});if(!r.ok)throw Error();let d=await r.json();
connection.textContent='Connected';connection.className='good';
document.querySelector('#remaining').textContent=`Wi-Fi closes in ${Math.ceil(d.remaining_ms/1000)} seconds (two minutes after boot).`;
rows.innerHTML=row('Pico temperature',d.pico,`${d.pico.c} °C`)+
row('RTC',d.rtc,`${d.rtc.unix} Unix seconds`)+
row('BME688',d.bme,`${d.bme.c} °C / ${d.bme.pa} Pa / ${d.bme.rh} % / ${d.bme.ohm} Ω`)+
row('INA260',d.ina,`${d.ina.ma} mA / ${d.ina.mv} mV / ${d.ina.mw} mW`)+
row('GPS receiver',d.gps,`fix ${d.gps.fix} (${d.gps.fix_ok?'valid':'no valid fix'}), ${d.gps.siv} satellites; UTC ${d.gps.unix}${d.gps.fix_ok?`, ${d.gps.lat_e7/1e7}, ${d.gps.lon_e7/1e7}, ${d.gps.alt_mm/1000} m; N/E/D ${d.gps.vn}/${d.gps.ve}/${d.gps.vd} mm/s; accuracy H/V ${d.gps.hacc}/${d.gps.vacc} mm`:''}`)+
`<tr><td>SD logging</td><td>${d.sd?'Available':'Unavailable or low space'}</td><td>—</td></tr><tr><td>CAN</td><td>${d.can?'Initialized':'Unavailable'}</td><td>—</td></tr>`;
}catch(e){connection.textContent='Disconnected / checkout ended';connection.className='bad'} }
update();setInterval(update,1500);
</script></body></html>)HTML";

void stop_checkout() {
  // Step-by-step teardown with progress prints: a stall here trips the
  // watchdog in ~8 s, and the last print before silence identifies the
  // blocking call (see LAUNCH_CHECKLIST 2026-11-19).
  if (!pnc_binary_mode) Serial.println("[PNC] checkout closing: client/server");
  if (client) client.stop(1);
  server.end();
  if (!pnc_binary_mode) Serial.println("[PNC] checkout closing: mDNS");
  if (mdns_active) MDNS.end();
  // Drain in-flight CYW43/lwIP work from any still-associated station
  // before pulling the AP interface: cyw43_wifi_leave() below is a
  // synchronous firmware call, and issuing it while the driver's async
  // context is busy has stalled the loop and tripped the watchdog.
  for (uint32_t t0 = millis(); millis() - t0 < 250;) watchdog_update();
  // On this core softAPdisconnect() only tears down the AP in AP_STA mode,
  // and mode(WIFI_OFF) merely disconnects the STA — in plain WIFI_AP mode
  // neither stops the AP, which kept beaconing past the checkout window.
  // disconnectAP() deinits the AP interface and its DHCP server and is the
  // only real teardown.
  if (!pnc_binary_mode) Serial.println("[PNC] checkout closing: AP interface");
  WiFi.disconnectAP();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  active = false;
  if (!pnc_binary_mode) Serial.println("[PNC] checkout ended");
}

bool fresh(const SysVarFreshness& f, uint8_t bit, uint32_t timestamp,
           uint32_t now, bool present) {
  return present && (f.valid & bit) && now - timestamp <= kStaleMs;
}

long age(const SysVarFreshness& f, uint8_t bit, uint32_t timestamp,
         uint32_t now) {
  return (f.valid & bit) ? (long)(now - timestamp) : -1;
}

void status_response(const CheckoutHealth& h) {
  float pico = 0;
  uint32_t rtc = 0;
  BMESensorData bme = {};
  INASensorData ina = {};
  GPSSensorData gps = {};
  SysVarFreshness f = {};
  const bool got = sysvar_get_freshness(&f) == 0;
  bool p = sysvar_get_pico_temp_c(&pico) == 0;
  bool r = sysvar_get_rtc_time(&rtc) == 0;
  bool b = sysvar_get_bme_data(&bme) == 0;
  bool i = sysvar_get_ina_data(&ina) == 0;
  bool g = sysvar_get_gps_data(&gps) == 0;
  uint32_t now = millis();
  const char* yesno[] = {"false", "true"};
  char ages[5][16];
  const uint32_t times[] = {f.pico_ms, f.rtc_ms, f.bme_ms, f.ina_ms, f.gps_ms};
  const uint8_t bits[] = {1, 2, 8, 4, 16};
  for (int n = 0; n < 5; n++) {
    long value = got ? age(f, bits[n], times[n], now) : -1;
    if (value < 0) snprintf(ages[n], sizeof(ages[n]), "null");
    else snprintf(ages[n], sizeof(ages[n]), "%ld", value);
  }
  // JSON cannot represent NaN or infinity.
  if (!isfinite(pico)) { p = false; pico = 0; }
  if (!isfinite(bme.BMETemp) || !isfinite(bme.BMEHumidity)) {
    b = false; bme = {};
  }
  if (!isfinite(ina.INACurrent) || !isfinite(ina.INABusVoltage) ||
      !isfinite(ina.INAPower)) { i = false; ina = {}; }
  char body[1800];
  int n = snprintf(body, sizeof(body),
      "{\"remaining_ms\":%lu,"
      "\"pico\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"c\":%.2f},"
      "\"rtc\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"unix\":%lu},"
      "\"bme\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"c\":%.2f,\"pa\":%lu,\"rh\":%.2f,\"ohm\":%lu},"
      "\"ina\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"ma\":%.2f,\"mv\":%.2f,\"mw\":%.2f},"
      "\"gps\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"unix\":%lu,\"fix\":%u,\"fix_ok\":%s,\"siv\":%u,\"lat_e7\":%ld,\"lon_e7\":%ld,\"alt_mm\":%ld,\"vn\":%ld,\"ve\":%ld,\"vd\":%ld,\"hacc\":%ld,\"vacc\":%ld},"
      "\"sd\":%s,\"can\":%s}",
      (unsigned long)(now < CHECKOUT_DURATION_MS ? CHECKOUT_DURATION_MS - now : 0),
      yesno[h.sensors[0]], yesno[got && p && fresh(f,1,f.pico_ms,now,h.sensors[0])], ages[0], (double)pico,
      yesno[h.sensors[1]], yesno[got && r && fresh(f,2,f.rtc_ms,now,h.sensors[1])], ages[1], (unsigned long)rtc,
      yesno[h.sensors[2]], yesno[got && b && fresh(f,8,f.bme_ms,now,h.sensors[2])], ages[2], (double)bme.BMETemp, (unsigned long)bme.BMEPressure, (double)bme.BMEHumidity, (unsigned long)bme.BMEGasResistance,
      yesno[h.sensors[3]], yesno[got && i && fresh(f,4,f.ina_ms,now,h.sensors[3])], ages[3], (double)ina.INACurrent, (double)ina.INABusVoltage, (double)ina.INAPower,
      yesno[h.sensors[4]], yesno[got && g && fresh(f,16,f.gps_ms,now,h.sensors[4])], ages[4], (unsigned long)gps.unix_time_s, (unsigned)gps.fix_type, yesno[!!gps.fix_ok], (unsigned)gps.siv, (long)gps.lat_e7, (long)gps.lon_e7, (long)gps.alt_msl_mm, (long)gps.vel_n_mmps, (long)gps.vel_e_mmps, (long)gps.vel_d_mmps, (long)gps.hacc_mm, (long)gps.vacc_mm,
      yesno[h.sd], yesno[h.can]);
  if (n < 0 || (size_t)n >= sizeof(body)) {
    response_len = 0;
    return;
  }
  int written = snprintf(response, sizeof(response),
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: %d\r\n\r\n%s", n, body);
  response_len = written > 0 && (size_t)written < sizeof(response) ? written : 0;
}

void page_response() {
  int n = snprintf(response, sizeof(response),
      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: %u\r\n\r\n%s",
      (unsigned)strlen(page), page);
  response_len = n > 0 && (size_t)n < sizeof(response) ? n : 0;
}
}  // namespace

void checkout_init() {
  // A watchdog reset means the previous boot already ran its checkout window
  // (or wedged in the post-window BLE path); do not re-arm the AP on the
  // recovery boot, or every reset would resurrect the checkout stage.
  if (watchdog_caused_reboot()) {
    if (!pnc_binary_mode) Serial.println("[PNC] checkout skipped (watchdog reset)");
    return;
  }
  if (millis() >= CHECKOUT_DURATION_MS) return;
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP("STRATODEVILS-CHECKOUT")) {
    WiFi.mode(WIFI_OFF);
    return;
  }
  if (millis() >= CHECKOUT_DURATION_MS) {
    stop_checkout();
    return;
  }
  server.begin();
  mdns_active = MDNS.begin("pnc");
  // Advertise the http service so clients can discover the checkout server
  // via mDNS; MDNS.begin() alone answers no _http._tcp queries.
  if (mdns_active && !MDNS.addService("pnc", "http", "tcp", 80) && !pnc_binary_mode)
    Serial.println("[PNC] mDNS service registration failed");
  active = true;
  if (!pnc_binary_mode) {
    Serial.print("[PNC] checkout: http://pnc.local/ or http://");
    Serial.println(WiFi.softAPIP());
    if (!mdns_active) Serial.println("[PNC] mDNS unavailable; use IP address");
  }
}

void checkout_poll(const CheckoutHealth& health) {
  if (!active) return;
  if (millis() >= CHECKOUT_DURATION_MS) {
    stop_checkout();
    return;
  }
  if (mdns_active) MDNS.update();
  if (!client) {
    client = server.accept();
    if (!client) return;
    client.setTimeout(1);
    client_started = millis();
    request_len = response_len = response_sent = 0;
    header_len = 0;
    first_line_done = line_has_content = false;
  }
  if (!client.connected() || millis() - client_started >= kClientTimeoutMs) {
    client.stop(1);
    return;
  }
  if (!response_len) {
    while (client.available() && header_len++ < 2048) {
      char c = client.read();
      if (c == '\n') {
        if (!first_line_done) {
          request[request_len] = '\0';
          first_line_done = true;
        } else if (!line_has_content) {
          if (strncmp(request, "GET /status ", 12) == 0) status_response(health);
          else if (strncmp(request, "GET / ", 6) == 0) page_response();
          if (!response_len) client.stop(1);
          break;
        }
        line_has_content = false;
      } else if (c != '\r') {
        line_has_content = true;
        if (!first_line_done) {
          if (request_len == sizeof(request) - 1) {
            client.stop(1);
            break;
          }
          request[request_len++] = c;
        }
      }
    }
    if (header_len >= 2048 && !response_len) client.stop(1);
  }
  if (client && response_len) {
    int space = client.availableForWrite();
    if (space > 0) {
      size_t count = response_len - response_sent;
      if (count > (size_t)space) count = space;
      if (count > 1024) count = 1024;
      response_sent += client.write((const uint8_t*)response + response_sent, count);
      if (response_sent == response_len) client.stop(1);
    }
  }
}

bool checkout_is_active() { return active; }
