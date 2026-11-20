#include "tasks/checkout.h"

#include <LEAmDNS.h>
#include <WiFi.h>
#include <hardware/watchdog.h>
#include <math.h>

#include "SysVar.h"
#include "PayloadTelemetry.h"

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
<script>
const rows=document.querySelector('#rows'),connection=document.querySelector('#connection');
function row(name,s,value){return `<tr><td>${name}</td><td class="${s.ok?'good':'bad'}">${s.ok?'Fresh':s.present?'Waiting / stale':'Not detected'}${s.age_ms===null?'':` (${(s.age_ms/1000).toFixed(1)}s)`}</td><td>${s.ok?value:'—'}</td></tr>`}
function payloadRows(p){
const link=`<tr><td>Payload board (CAN)</td><td class="${p.ok?'good':'bad'}">${p.ok?'Fresh':p.present?'Waiting / stale':'Not detected'}${p.age_ms===null?'':` (${(p.age_ms/1000).toFixed(1)}s)`}</td><td>node ${p.node}, ${p.frames} packets, payload uptime ${Math.round(p.uptime_ms/1000)} s</td></tr>`;
return link
+row('Payload Pico temperature',p.pico,`${p.pico.c} °C`)
+row('Payload TMP117 (External)',p.tmp117,`${p.tmp117.c} °C`)
+row('Payload BME688 (External)',p.bme,`${p.bme.c} °C / ${p.bme.pa} Pa / ${p.bme.rh} % / ${p.bme.ohm} Ω`)
+row('Payload ENS160 (External)',p.ens,`AQI ${p.ens.aqi}; TVOC ${p.ens.tvoc} ppb; eCO₂ ${p.ens.eco2} ppm`)
+row('Payload SHTC3 (External)',p.shtc,`${p.shtc.c} °C / ${p.shtc.rh} %`)
+row('Payload UV (AS7331)',p.uv,`UVA ${p.uv.a} / UVB ${p.uv.b} / UVC ${p.uv.c} µW/cm²`)
+row('Payload BMP390 (Science)',p.bmp390,`${p.bmp390.c} °C / ${p.bmp390.pa} Pa / ${p.bmp390.alt} m`)
+row('Payload ozone',p.ozone,`${p.ozone.ppb} ppb`)
+row('Payload TMP117 (Science)',p.tmp_sci,`${p.tmp_sci.c} °C`)}
async function update(){try{let r=await fetch('/status',{cache:'no-store'});if(!r.ok)throw Error();let d=await r.json();
connection.textContent='Connected';connection.className='good';
document.querySelector('#remaining').textContent=`Wi-Fi closes in ${Math.ceil(d.remaining_ms/1000)} seconds`;
rows.innerHTML=row('Pico temperature',d.pico,`${d.pico.c} °C`)+
row('RTC',d.rtc,`${d.rtc.unix} Unix seconds`)+
row('BME688',d.bme,`${d.bme.c} °C / ${d.bme.pa} Pa / ${d.bme.rh} % / ${d.bme.ohm} Ω`)+
row('INA260',d.ina,`${d.ina.ma} mA / ${d.ina.mv} mV / ${d.ina.mw} mW`)+
row('GPS receiver',d.gps,`fix ${d.gps.fix} (${d.gps.fix_ok?'valid':'no valid fix'}), ${d.gps.siv} satellites; UTC ${d.gps.unix}${d.gps.fix_ok?`, ${d.gps.lat_e7/1e7}, ${d.gps.lon_e7/1e7}, ${d.gps.alt_mm/1000} m; N/E/D ${d.gps.vn}/${d.gps.ve}/${d.gps.vd} mm/s; accuracy H/V ${d.gps.hacc}/${d.gps.vacc} mm`:''}`)+
(d.payload?payloadRows(d.payload):'')+
`<tr><td>SD logging</td><td>${d.sd?'Available':'Unavailable or low space'}</td><td>—</td></tr><tr><td>CAN</td><td>${d.can?'Initialized':'Unavailable'}</td><td>—</td></tr>`;
}catch(e){connection.textContent='Disconnected / checkout ended';connection.className='bad'} }
update();setInterval(update,1500);
</script></body></html>)HTML";

void stop_checkout() {
  // Step-by-step teardown with progress prints: a stall here trips the
  // watchdog in ~8 s, and the last print before silence identifies the
  // blocking call (see LAUNCH_CHECKLIST 2026-11-19).
  pnc_event_log("ap_down");
  if (!pnc_binary_mode) Serial.println("PNC checkout closing: client/server");
  if (client) client.stop(1);
  server.end();
  if (!pnc_binary_mode) Serial.println("PNC checkout closing: mDNS");
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
  if (!pnc_binary_mode) Serial.println("PNC checkout closing: AP interface");
  WiFi.disconnectAP();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  active = false;
  if (!pnc_binary_mode) Serial.println("PNC checkout ended");
}

bool fresh(const SysVarFreshness& f, uint8_t bit, uint32_t timestamp,
           uint32_t now, bool present) {
  return present && (f.valid & bit) && now - timestamp <= kStaleMs;
}

long age(const SysVarFreshness& f, uint8_t bit, uint32_t timestamp,
         uint32_t now) {
  return (f.valid & bit) ? (long)(now - timestamp) : -1;
}

// JSON body for the payload board's CAN telemetry (the value of the
// "payload" key, without the key). Any overflow empties the buffer so
// status_response drops the response instead of sending cut JSON.
void payload_json(char* out, size_t cap, const PayloadTelemetry& pt,
                  uint32_t now) {
  const char* yesno[] = {"false", "true"};
  size_t off = 0;
  char agebuf[16];
  auto slot_age = [&](uint8_t slot) -> const char* {
    uint32_t t = pt.slot_ms[slot];
    if (t == 0) return "null";
    snprintf(agebuf, sizeof(agebuf), "%lu", (unsigned long)(now - t));
    return agebuf;
  };
  auto slot_ok = [&](uint8_t slot) -> const char* {
    return yesno[pt.slot_ms[slot] != 0 &&
                 now - pt.slot_ms[slot] <= kStaleMs];
  };
  auto slot_present = [&](uint8_t slot) -> const char* {
    return yesno[pt.slot_ms[slot] != 0];
  };
  if (pt.last_rx_ms == 0) snprintf(agebuf, sizeof(agebuf), "null");
  else snprintf(agebuf, sizeof(agebuf), "%lu",
                (unsigned long)(now - pt.last_rx_ms));
#define PAPPEND(...) do { \
    int m_ = snprintf(out + off, cap - off, __VA_ARGS__); \
    if (m_ < 0 || (size_t)m_ >= cap - off) { \
      snprintf(out, cap, "null"); \
      return; \
    } \
    off += (size_t)m_; \
  } while (0)
  PAPPEND(
      "{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"node\":%u,"
      "\"frames\":%lu,\"pkt_len\":%u,\"uptime_ms\":%lu,",
      yesno[pt.frames_seen > 0],
      yesno[pt.last_rx_ms != 0 && now - pt.last_rx_ms <= kStaleMs], agebuf,
      (unsigned)pt.node, (unsigned long)pt.packets,
      (unsigned)pt.last_length, (unsigned long)pt.payload_uptime_ms);
  PAPPEND("\"pico\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"c\":%.2f},",
          slot_present(PAYLOAD_SLOT_PICO), slot_ok(PAYLOAD_SLOT_PICO),
          slot_age(PAYLOAD_SLOT_PICO), (double)pt.pico_temp_c);
  PAPPEND("\"tmp117\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"c\":%.2f},",
          slot_present(PAYLOAD_SLOT_TMP117_EXT), slot_ok(PAYLOAD_SLOT_TMP117_EXT),
          slot_age(PAYLOAD_SLOT_TMP117_EXT), (double)pt.tmp117_ext_c);
  PAPPEND(
      "\"bme\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,"
      "\"c\":%.2f,\"pa\":%lu,\"rh\":%.2f,\"ohm\":%lu},",
      slot_present(PAYLOAD_SLOT_BME688_EXT), slot_ok(PAYLOAD_SLOT_BME688_EXT),
      slot_age(PAYLOAD_SLOT_BME688_EXT), (double)pt.bme_temp_c,
      (unsigned long)pt.bme_pressure_pa, (double)pt.bme_humidity_pct,
      (unsigned long)pt.bme_gas_ohm);
  PAPPEND(
      "\"ens\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,"
      "\"aqi\":%u,\"tvoc\":%u,\"eco2\":%u},",
      slot_present(PAYLOAD_SLOT_ENS160), slot_ok(PAYLOAD_SLOT_ENS160),
      slot_age(PAYLOAD_SLOT_ENS160), (unsigned)pt.ens_aqi,
      (unsigned)pt.ens_tvoc_ppb, (unsigned)pt.ens_eco2_ppm);
  PAPPEND("\"shtc\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,"
          "\"c\":%.2f,\"rh\":%.2f},",
          slot_present(PAYLOAD_SLOT_SHTC3), slot_ok(PAYLOAD_SLOT_SHTC3),
          slot_age(PAYLOAD_SLOT_SHTC3), (double)pt.shtc3_temp_c,
          (double)pt.shtc3_humidity_pct);
  PAPPEND("\"uv\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,"
          "\"a\":%.3f,\"b\":%.3f,\"c\":%.3f},",
          slot_present(PAYLOAD_SLOT_AS7331), slot_ok(PAYLOAD_SLOT_AS7331),
          slot_age(PAYLOAD_SLOT_AS7331), (double)pt.uv_a_uw_cm2,
          (double)pt.uv_b_uw_cm2, (double)pt.uv_c_uw_cm2);
  PAPPEND("\"bmp390\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,"
          "\"c\":%.2f,\"pa\":%.2f,\"alt\":%.2f},",
          slot_present(PAYLOAD_SLOT_BMP390_SCI), slot_ok(PAYLOAD_SLOT_BMP390_SCI),
          slot_age(PAYLOAD_SLOT_BMP390_SCI), (double)pt.bmp390_temp_c,
          (double)pt.bmp390_pressure_pa, (double)pt.bmp390_alt_m);
  PAPPEND("\"ozone\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"ppb\":%d},",
          slot_present(PAYLOAD_SLOT_OZONE), slot_ok(PAYLOAD_SLOT_OZONE),
          slot_age(PAYLOAD_SLOT_OZONE), (int)pt.ozone_ppb);
  PAPPEND("\"tmp_sci\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"c\":%.2f}}",
          slot_present(PAYLOAD_SLOT_TMP117_SCI), slot_ok(PAYLOAD_SLOT_TMP117_SCI),
          slot_age(PAYLOAD_SLOT_TMP117_SCI), (double)pt.tmp117_sci_c);
#undef PAPPEND
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
  char body[2800];
  // Payload board values from CAN; null until the first valid packet.
  char pbody[1600];
  PayloadTelemetry pt;
  if (payload_telemetry_get(&pt)) {
    payload_json(pbody, sizeof(pbody), pt, now);
  } else {
    snprintf(pbody, sizeof(pbody), "null");
  }
  int n = snprintf(body, sizeof(body),
      "{\"remaining_ms\":%lu,"
      "\"pico\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"c\":%.2f},"
      "\"rtc\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"unix\":%lu},"
      "\"bme\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"c\":%.2f,\"pa\":%lu,\"rh\":%.2f,\"ohm\":%lu},"
      "\"ina\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"ma\":%.2f,\"mv\":%.2f,\"mw\":%.2f},"
      "\"gps\":{\"present\":%s,\"ok\":%s,\"age_ms\":%s,\"unix\":%lu,\"fix\":%u,\"fix_ok\":%s,\"siv\":%u,\"lat_e7\":%ld,\"lon_e7\":%ld,\"alt_mm\":%ld,\"vn\":%ld,\"ve\":%ld,\"vd\":%ld,\"hacc\":%ld,\"vacc\":%ld},"
      "\"sd\":%s,\"can\":%s,\"payload\":%s}",
      (unsigned long)(now < CHECKOUT_DURATION_MS ? CHECKOUT_DURATION_MS - now : 0),
      yesno[h.sensors[0]], yesno[got && p && fresh(f,1,f.pico_ms,now,h.sensors[0])], ages[0], (double)pico,
      yesno[h.sensors[1]], yesno[got && r && fresh(f,2,f.rtc_ms,now,h.sensors[1])], ages[1], (unsigned long)rtc,
      yesno[h.sensors[2]], yesno[got && b && fresh(f,8,f.bme_ms,now,h.sensors[2])], ages[2], (double)bme.BMETemp, (unsigned long)bme.BMEPressure, (double)bme.BMEHumidity, (unsigned long)bme.BMEGasResistance,
      yesno[h.sensors[3]], yesno[got && i && fresh(f,4,f.ina_ms,now,h.sensors[3])], ages[3], (double)ina.INACurrent, (double)ina.INABusVoltage, (double)ina.INAPower,
      yesno[h.sensors[4]], yesno[got && g && fresh(f,16,f.gps_ms,now,h.sensors[4])], ages[4], (unsigned long)gps.unix_time_s, (unsigned)gps.fix_type, yesno[!!gps.fix_ok], (unsigned)gps.siv, (long)gps.lat_e7, (long)gps.lon_e7, (long)gps.alt_msl_mm, (long)gps.vel_n_mmps, (long)gps.vel_e_mmps, (long)gps.vel_d_mmps, (long)gps.hacc_mm, (long)gps.vacc_mm,
      yesno[h.sd], yesno[h.can], pbody);
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
    if (!pnc_binary_mode) Serial.println("PNC checkout skipped (watchdog reset)");
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
    Serial.println("PNC mDNS service registration failed");
  active = true;
  {
    char line[88];
    snprintf(line, sizeof(line), "ap_up ip=%s remaining_ms=%lu",
             WiFi.softAPIP().toString().c_str(),
             (unsigned long)(CHECKOUT_DURATION_MS - millis()));
    pnc_event_log(line);
  }
  if (!pnc_binary_mode) {
    Serial.print("PNC checkout: http://pnc.local/ or http://");
    Serial.println(WiFi.softAPIP());
    if (!mdns_active) Serial.println("PNC mDNS unavailable; use IP address");
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
    {
      char line[64];
      snprintf(line, sizeof(line), "client ip=%s",
               client.remoteIP().toString().c_str());
      pnc_event_log(line);
      if (!pnc_binary_mode) {
        Serial.print("PNC checkout: ");
        Serial.println(line);
      }
    }
  }
  if (!client.connected() || millis() - client_started >= kClientTimeoutMs) {
    // No complete request arrived before the connection dropped or timed out.
    pnc_event_log("client closed without a served request");
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
          // Log every request line: when a phone cannot load the page, the
          // first line shows what it actually asked for (page, status, or a
          // captive-portal probe).
          {
            char line[96];
            snprintf(line, sizeof(line), "req %.70s", request);
            pnc_event_log(line);
            if (!pnc_binary_mode) {
              Serial.print("PNC checkout: ");
              Serial.println(line);
            }
          }
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
