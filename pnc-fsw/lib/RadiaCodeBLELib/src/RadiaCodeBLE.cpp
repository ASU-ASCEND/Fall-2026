#include "RadiacodeBLE.h"

// BENCH DIAGNOSTIC (remove once the RadiaCode link is settled): the BLE
// library keeps service/characteristic UUIDs behind `protected`, and there is
// no public accessor, so open them up for one printout.
#include <Arduino.h>
#include <list>
#include <hardware/watchdog.h>
#include <BLE.h>
#include <time.h>

// use this to print if 
#ifndef RC_BLE_DEBUG 
#define RC_BLE_DEBUG 1
#endif 

#ifndef debug_printf
#define debug_printf(...) if(RC_BLE_DEBUG) Serial.printf(__VA_ARGS__)
#endif 

#ifndef always_printf
#define always_printf(...) Serial.printf(__VA_ARGS__)
#endif 

// Keep this well under the 5 s hardware watchdog: a blocked core 0 wait must
// not outlive it.
#define BLE_RESPONSE_TIMEOUT 3000

#define RADIACODE_SERVICE_UUID "e63215e5-7003-49d8-96b0-b024798fb901"
#define RADIACODE_WRITE_FD_UUID "e63215e6-7003-49d8-96b0-b024798fb901"
#define RADIACODE_NOTIFY_FD_UUID "e63215e7-7003-49d8-96b0-b024798fb901"

// Build the UUIDs from raw bytes: BLEUUID(String) parses with sscanf("%llx"),
// which newlib here does not support, so the string form silently ends up as an
// all-zero UUID and every service/characteristic lookup fails.
static const uint8_t RC_SERVICE_B[16] = {0xe6, 0x32, 0x15, 0xe5, 0x70, 0x03,
                                         0x49, 0xd8, 0x96, 0xb0, 0xb0, 0x24,
                                         0x79, 0x8f, 0xb9, 0x01};
static const uint8_t RC_WRITE_B[16] = {0xe6, 0x32, 0x15, 0xe6, 0x70, 0x03,
                                       0x49, 0xd8, 0x96, 0xb0, 0xb0, 0x24,
                                       0x79, 0x8f, 0xb9, 0x01};
static const uint8_t RC_NOTIFY_B[16] = {0xe6, 0x32, 0x15, 0xe7, 0x70, 0x03,
                                        0x49, 0xd8, 0x96, 0xb0, 0xb0, 0x24,
                                        0x79, 0x8f, 0xb9, 0x01};

static BLEUUID rc_service_uuid(RC_SERVICE_B);
static BLEUUID rc_write_uuid(RC_WRITE_B);
static BLEUUID rc_notify_uuid(RC_NOTIFY_B);

static BLERemoteCharacteristic* rc_write_char;
static BLERemoteCharacteristic* rc_notify_char;

#define BLE_BUFFER_SIZE 4000
static BytesBuffer _resp_buffer(BLE_BUFFER_SIZE);
static BytesBuffer _response(BLE_BUFFER_SIZE);
static mutex_t _response_mutex;
static volatile bool _response_ready = false;
static BytesBuffer res_ret(BLE_BUFFER_SIZE);

static size_t _resp_size = 0;
static void notify(BLERemoteCharacteristic* c, const uint8_t* data,
                   uint32_t len) {
  // if(c == rc_notify_char){
  //   Serial.println("rc_notify_char notify");
  //   for(int i = 0; i < len; i++){
  //     debug_printf("%02x ", data[i]);
  //   }
  //   Serial.println();
  // }

  // if(c == rc_write_char){
  //   Serial.println("rc_write_char notify");
  // }

  if (_resp_size == 0) {
    if (len < 4) return;  // short notification: not a response header
    // read first 4 bytes as signed integer
    int size_buf;
    memcpy(&size_buf, data, sizeof(int));
    _resp_size = 4 + size_buf;
    // read in the rest of the bytes as data
    _resp_buffer.clear();
    _resp_buffer.fill(data + 4, len - 4);
  } else {
    // read in the bytes as data
    _resp_buffer.fill(data, len);
  }

  // reduce size
  _resp_size -= len;

  if (_resp_size == 0) {
    // copied entire message
    // Serial.println("message ended");
    // send to _response
    mutex_enter_blocking(&_response_mutex);
    _response.copy(_resp_buffer);
    mutex_exit(&_response_mutex);
    _response_ready = true;

    // wipe _resp_buffer
    _resp_buffer.clear();
  }
}

static BytesBuffer* ble_execute(uint8_t* data, size_t len) {
  for (int i = 0; i < len; i += 18) {
    // write write_fd
    size_t chunk_size = (len - i > 18) ? 18 : (len - i); 
    rc_write_char->setValue(data + i, chunk_size);
  }

  uint32_t start_time = millis();
  while (_response_ready == false) {
    // This used to be a tight, unfed spin. Feed the hardware watchdog and
    // yield so a slow BLE response cannot trip the 5 s WDT.
    watchdog_update();
    delay(1);
    if (millis() - start_time > BLE_RESPONSE_TIMEOUT) {
      Serial.println("timeout");
      return nullptr;
    }
  }

  res_ret.clear();  // reuse res_ret

  mutex_enter_blocking(&_response_mutex);
  res_ret.copy(_response);
  _response.clear();
  mutex_exit(&_response_mutex);
  _response_ready = false;

  return &res_ret;
}

struct __attribute__((packed)) BLERequestHeader {
  uint32_t length;
  uint16_t req_type;
  uint8_t zero = 0;
  uint8_t req_seq_no;
};

static BytesBuffer* execute(uint16_t req_type, uint8_t* args, size_t len) {
  static uint16_t seq = 0;
  // Serial.println("execute");
  uint8_t req_seq_no = 0x80 + seq;
  seq = (seq + 1) % 32;

  BLERequestHeader header;
  header.length =
      len + sizeof(BLERequestHeader) - 4;  // length does not include itself
  header.req_type = req_type;
  header.zero = 0;
  header.req_seq_no = req_seq_no;

  //                 header                data
  uint8_t buffer[sizeof(BLERequestHeader) + len];
  memcpy(buffer, &header, sizeof(BLERequestHeader));
  if (args != NULL) {
    memcpy(buffer + sizeof(BLERequestHeader), args, len);
  }

  // Serial.println("Sending buffer:");
  // for(auto c : buffer){
  //   debug_printf("%02x ", c);
  // }
  // Serial.println();

  BytesBuffer* response = ble_execute(buffer, len + sizeof(BLERequestHeader));

  // timeout occurred
  if(response == nullptr){
    return nullptr; 
  }

  // check header
  // get header
  uint16_t received_req_type = response->consume<uint16_t>();
  uint8_t received_zero = response->consume<uint8_t>();
  uint8_t received_req_seq_no = response->consume<uint8_t>();
  if (received_req_type != header.req_type || received_zero != header.zero ||
      received_req_seq_no != header.req_seq_no) {
    debug_printf("Response header does not match request header!\n");
    debug_printf(
        "(expected|received) req_type: %u|%u zero: %u|%u req_seq_no: %u|%u\n",
        header.req_type, received_req_type, header.zero, received_zero,
        header.req_seq_no, received_req_seq_no);

    return nullptr;
  }

  // remove compared header fields

  return response;
}

void radiacode_ble_init(){
  mutex_init(&_response_mutex); 

  debug_printf("Starting BLE Client\n");

  BLE.begin();

  debug_printf("Done."); 
}

uint8_t radiacode_ble_connect(String target_mac, bool verbose) {
  if (verbose) debug_printf("Scanning...\n");

  // 2 s, not the 5 s default: the default scan is exactly the hardware
  // watchdog interval, so a slow scan left nothing for the feed path.
  BLEScanReport* res = BLE.scan(2);


  for (BLEAdvertising item : *res) {
    if (item.getAddress().toString() == target_mac) {
      if (verbose) debug_printf("Found device, connecting...\n");
      // GATT discovery right after the link comes up sometimes returns an
      // empty service list; drop the link and try again.
      for (int attempt = 0; attempt < 3; attempt++) {
        if (BLE.client()->connect(item) == false) {
          if (verbose) debug_printf("Failed.\n");
          return 1;
        }
        if (!BLE.client()->services()->empty()) {
          break;
        }
        if (verbose) {
          debug_printf("No services discovered (attempt %u), retrying\n",
                       (unsigned)(attempt + 1));
        }
        BLE.client()->disconnect();
        delay(750);
      }
      break;
    }
  }

  if (verbose) debug_printf("Done.\n");

  if (verbose) debug_printf("Exploring service...\n");
  BLERemoteServiceList* services = BLE.client()->services();
  if (verbose) {
    debug_printf("Services discovered: %u\n",
                 (unsigned)(services ? services->size() : 0));
  }
  BLERemoteService* service = BLE.client()->service(rc_service_uuid);

  // Some RadiaCode firmware revisions advertise the custom service but the
  // UUID compare fails here (or the service is only listed in the scan
  // response). Fall back to whichever service actually carries the RadiaCode
  // write/notify pair.
  if (!service && services) {
    for (BLERemoteService* candidate : *services) {
      if (candidate->characteristic(rc_write_uuid) &&
          candidate->characteristic(rc_notify_uuid)) {
        service = candidate;
        if (verbose) debug_printf("Service matched by characteristics\n");
        break;
      }
    }
  }

  if (service) {
    rc_notify_char = service->characteristic(rc_notify_uuid);
    if (rc_notify_char) {
      if (verbose) debug_printf("Found notify char\n");
      rc_notify_char->onNotify(notify);
      rc_notify_char->enableNotifications();
    } else {
      return 2; 
    }

    rc_write_char = service->characteristic(rc_write_uuid);
    if (rc_write_char) {
      if (verbose) debug_printf("Found write char\n");
    } else {
      return 3; 
    }

  } else {
    if (verbose) debug_printf("RCError on service\n");
    // Leave the device free for the next attempt: it only serves one link.
    BLE.client()->disconnect();
    return 4; 
  }

  // init?
  uint8_t data[] = {0x01, 0xff, 0x12, 0xff};
  execute(Command::SET_EXCHANGE, data, sizeof(data));

  return 0;
}

void radiacode_ble_disconnect(){
  if(BLE.client() && BLE.client()->connected()){
    BLE.client()->disconnect(); 
  }

  rc_write_char = nullptr; 
  rc_notify_char = nullptr; 
}

uint8_t write_request(int command_id, uint8_t* data, size_t len) {
  uint8_t buf[4 + len];
  memcpy(buf, &command_id, sizeof(int));
  memcpy(buf + sizeof(int), data, len);

  BytesBuffer* r = execute(Command::WR_VIRT_SFR, buf, (4 + len));

  if (r == nullptr || r->empty()) {
    debug_printf("No r, likely timeout\n");
    return 1;
  }

  // match BytesBuffer behavior
  uint32_t retcode = r->consume<uint32_t>();

  if (retcode != 1) {
    debug_printf("Bad retcode %u\n", retcode);
  }

  return 0; 
}

/**
 * @brief Set the device clock from a UTC epoch.
 *
 * The RadiaCode timestamps its internal log with its own clock, so this is the
 * only thing a flight needs to do to it: match the PnC's UTC time authority.
 * Payload is day, month, year-2000, weekday(ignored), second, minute, hour, 0.
 */
uint8_t radiacode_set_time(uint32_t unix_s) {
  time_t t = (time_t)unix_s;
  struct tm tmv = {};
  gmtime_r(&t, &tmv);

  uint8_t d[8] = {(uint8_t)tmv.tm_mday, (uint8_t)(tmv.tm_mon + 1),
                  (uint8_t)((tmv.tm_year + 1900) % 100), 0,
                  (uint8_t)tmv.tm_sec, (uint8_t)tmv.tm_min,
                  (uint8_t)tmv.tm_hour, 0};

  BytesBuffer* r = execute(Command::SET_TIME, d, sizeof(d));
  return (r == nullptr) ? 1 : 0;
}

BytesBuffer* read_request(uint32_t command_id) {  // Serial.println("read_request");
  BytesBuffer* r =
      execute(Command::RD_VIRT_STRING, (uint8_t*)&command_id, sizeof(int));

  if (r == nullptr) return nullptr;

  uint32_t retcode = r->consume<uint32_t>();
  uint32_t flen = r->consume<uint32_t>();

  if (retcode != 1) {
    debug_printf("Bad retcode %x\n", retcode);
    debug_printf("flen: %x\n", flen);
    for (int i = 0; i < r->size(); i++) {
      debug_printf("%02x ", r->at(i));
    }
    return nullptr;
  }

  // hack?
  if (r->size() == flen + 1 && r->at(r->size() - 1) == 0x00) {
    r->pop_back();
  }
  // end

  if (r->size() != flen)
    debug_printf("Mismatch between buffer (%d) and header (%d)\n", r->size(),
                  flen);

  return r;
}

String decode_cp1251(BytesBuffer* data) {
  String res;

  while (data->empty() == false) {
    char c = data->consume<char>();
    if (c < 0x80) res += c;
    // probably don't need the cyrillic character - maybe later
    else if (c >= 0xC0 && c <= 0xDF)
      res += '_';  //(char)(0x0410 + (c - 0xC0));
    else if (c >= 0xE0 && c <= 0xFF)
      res += '_';  //(char)(0x0430 + (c - 0xE0));
    else if (c == 0xA8)
      res += '_';  //(char)0x0401;
    else if (c == 0xB8)
      res += '_';  //(char)0x0451;
    else
      res += c;  // unknown character
  }

  return res;
}

uint8_t decode_spectrum(BytesBuffer* data, int* ret, float& a0, float& a1,
                        float& a2, uint32_t& ts) {
  // ret is assumed to be an array of len 1024
  if(data == nullptr) return 1; 

  // read first bytes
  ts = data->consume<uint32_t>();
  a0 = data->consume<float>();
  a1 = data->consume<float>();
  a2 = data->consume<float>();

  size_t ret_it = 0;
  int last = 0;
  int v = 0;
  while (data->empty() == false) {  // place < data->data() + data->size()){
    uint16_t u16 = data->consume<uint16_t>();
    uint16_t cnt = (u16 >> 4) & 0x0FFF;
    uint16_t vlen = u16 & 0x0F;

    for (int i = 0; i < cnt; i++) {
      if (vlen == 0) {
        v = 0;
      } else if (vlen == 1) {
        // unpack('<B')
        uint8_t b = data->consume<uint8_t>();

        v = b;
      } else if (vlen == 2) {
        // last + unpack('<b')
        int8_t b = data->consume<int8_t>();

        v = last + b;
      } else if (vlen == 3) {
        // last + unpack('<h')
        int16_t h = data->consume<int16_t>();

        v = last + h;
      } else if (vlen == 4) {
        // a, b, c = unpack('<BBb')
        uint8_t a = data->consume<uint8_t>();
        uint8_t b = data->consume<uint8_t>();
        int8_t c = data->consume<int8_t>();

        v = last + ((c << 16) | (b << 8) | a);
      } else if (vlen == 5) {
        // last + unpack('<i')
        int32_t i = data->consume<int32_t>();

        v = last + i;
      } else {
        debug_printf("Unsupported vlen %u\n", vlen);
        return 1;
      }

      last = v;
      ret[ret_it++] = v;
    }
  }

  return 0;
}

BytesBuffer* readSpectrumData(){
  return read_request(VS::SPECTRUM);
}

void printSpectrum() {
  BytesBuffer* r = read_request(VS::SPECTRUM);

  if(r == nullptr) {
    debug_printf("Null r in printSpectrum"); 
    return; 
  }

  float a0, a1, a2;
  uint32_t ts;
  int spectrum[1024];
  decode_spectrum(r, spectrum, a0, a1, a2, ts);
  always_printf("a0: %f, a1: %f, a2: %f, ts: %u\n", a0, a1, a2, ts);
  always_printf("Spectrum:\n");
  for (int i = 0; i < 1024; i++) {
    always_printf("%d, ", spectrum[i]);
  }
  always_printf("\n");
}

DataPoint consume_data_buf(BytesBuffer* r) {
  if (r->size() < 7) {
    // too short?
    return RCError();
  }

  uint8_t seq = r->consume<uint8_t>();
  uint8_t eid = r->consume<uint8_t>();
  uint8_t gid = r->consume<uint8_t>();
  int32_t ts_offset = r->consume<int32_t>();

  dt_t dt = ts_offset * 10;

  if (eid == 0) {
    if (gid == 0) {
      // RealTimeData
      if(r->size() < (4 + 4 + 2 + 2 + 2 + 1)) return RCError(); 
      return RealTimeData{dt,
                          r->consume<float>(),
                          r->consume<float>(),
                          r->consume<uint16_t>(),
                          r->consume<uint16_t>(),
                          r->consume<uint16_t>(),
                          r->consume<uint8_t>()};
    } else if (gid == 1) {
      // RawData
      if(r->size() < (4 + 4)) return RCError(); 
      return RawData{dt, r->consume<float>(), r->consume<float>()};
    } else if (gid == 2) {
      // DoseRateDB
      if(r->size() < (4 + 4 + 4 + 2 + 2)) return RCError(); 
      return DoseRateDB{dt,
                        r->consume<uint32_t>(),
                        r->consume<float>(),
                        r->consume<float>(),
                        r->consume<uint16_t>(),
                        r->consume<uint16_t>()};
    } else if (gid == 3) {
      // RareData
      if(r->size() < (4 + 4 + 2 + 2 + 2)) return RCError(); 
      return RareData{dt,
                      r->consume<uint32_t>(),
                      r->consume<float>(),
                      r->consume<uint16_t>(),
                      r->consume<uint16_t>(),
                      r->consume<uint16_t>()};
    } else if (gid == 4) {
      // UserData - ?
      if(r->size() < (4 + 4 + 4 + 2 + 2)) return RCError(); 
      r->consume<uint32_t>();
      r->consume<float>();
      r->consume<float>();
      r->consume<uint16_t>();
      r->consume<uint16_t>();
      return RCNone{eid, gid};
    } else if (gid == 5) {
      // ScheduleData - ?
      if(r->size() < (4 + 4 + 4 + 2 + 2)) return RCError(); 
      r->consume<uint32_t>();
      r->consume<float>();
      r->consume<float>();
      r->consume<uint16_t>();
      r->consume<uint16_t>();
      return RCNone{eid, gid};
    } else if (gid == 6) {
      // AccelData - ?
      if(r->size() < (2 + 2 + 2)) return RCError(); 
      r->consume<uint16_t>();
      r->consume<uint16_t>();
      r->consume<uint16_t>();
      return RCNone{eid, gid};
    } else if (gid == 7) {
      // Event
      if(r->size() < (1 + 1 + 2)) return RCError(); 
      return Event{dt, r->consume<uint8_t>(), r->consume<uint8_t>(),
                   r->consume<uint16_t>()};
    } else if (gid == 8) {
      // RawCountRate
      if(r->size() < (4 + 2)) return RCError(); 
      return RawCountRate{dt, r->consume<float>(), r->consume<uint16_t>()};
    } else if (gid == 9) {
      // RawDoseRate
      if(r->size() < (4 + 2)) return RCError(); 
      return RawDoseRate{dt, r->consume<float>(), r->consume<uint16_t>()};
    } else {
      debug_printf("Unknown eid: %d gid: %d\n", eid, gid);
    }
  } else if (eid == 1) {
    if (gid == 1) {
      // ???
      if(r->size() < (2 + 4)) return RCError(); 
      uint16_t samples_num = r->consume<uint16_t>();
      uint32_t smpl_time_ms = r->consume<uint32_t>();

      if(r->size() < (8 * samples_num)) return RCError(); 
      r->drain(nullptr, 8 * samples_num);
      return RCNone{eid, gid};
    } else if (gid == 2) {
      // ???
      if(r->size() < (2 + 4)) return RCError(); 
      uint16_t samples_num = r->consume<uint16_t>();
      uint32_t smpl_time_ms = r->consume<uint32_t>();
      
      if(r->size() < (16 * samples_num)) return RCError(); 
      r->drain(nullptr, 16 * samples_num);
      return RCNone{eid, gid};
    } else if (gid == 3) {
      // ???
      if(r->size() < (2 + 4)) return RCError(); 
      uint16_t samples_num = r->consume<uint16_t>();
      uint32_t smpl_time_ms = r->consume<uint32_t>();

      if(r->size() < (14 * samples_num)) return RCError(); 
      r->drain(nullptr, 14 * samples_num);
      return RCNone{eid, gid};
    } else {
      debug_printf("Unknown eid: %d gid: %d\n", eid, gid);
    }
  } else {
    debug_printf("Unknown eid: %d gid: %d\n", eid, gid);
  }

  return RCError();
}
