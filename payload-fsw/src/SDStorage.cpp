#include "SDStorage.h"

#include <string.h>

#include "SdFat.h"

static constexpr uint16_t MIN_ASU_PACKET_SIZE = 23;

/**
 * @brief Send CMD0 straight over SPI and return the R1 response.
 *
 * 0x01 means the card answered and is idle; 0xFF means nothing drove MISO.
 * This separates "card/socket not responding" from "card responded but the
 * filesystem could not be mounted".
 */
static uint8_t sd_raw_cmd0() {
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);

  digitalWrite(SD_CS_PIN, LOW);
  SPI.transfer(0x40);  // CMD0
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x95);  // CRC for CMD0

  uint8_t response = 0xFF;
  for (int i = 0; i < 16 && response == 0xFF; i++) {
    response = SPI.transfer(0xFF);
  }

  digitalWrite(SD_CS_PIN, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();
  return response;
}

static uint8_t sd_send_command(uint8_t cmd, uint32_t arg) {
  SPI.transfer(0x40 | cmd);
  SPI.transfer((uint8_t)(arg >> 24));
  SPI.transfer((uint8_t)(arg >> 16));
  SPI.transfer((uint8_t)(arg >> 8));
  SPI.transfer((uint8_t)arg);
  uint8_t crc = 0x01;
  if (cmd == 0) crc = 0x95;
  if (cmd == 8) crc = 0x87;
  SPI.transfer(crc);

  uint8_t response = 0xFF;
  for (int i = 0; i < 32 && (response & 0x80); i++) {
    response = SPI.transfer(0xFF);
  }
  return response;
}

/**
 * @brief Walk the SD SPI init sequence and report where it stops.
 *
 * The mount can fail while the card itself is fine. CMD0 alone only proves the
 * four SPI lines are alive; the ACMD41 result tells us whether the card
 * actually reaches ready state.
 */
static void sd_print_layout() {
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.setRX(SD_MISO_PIN);
  SPI.setTX(SD_MOSI_PIN);
  SPI.setSCK(SD_SCK_PIN);
  SPI.begin();
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));

  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);

  digitalWrite(SD_CS_PIN, LOW);
  uint8_t r0 = sd_send_command(0, 0);
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.transfer(0xFF);

  digitalWrite(SD_CS_PIN, LOW);
  uint8_t r8 = sd_send_command(8, 0x000001AA);
  uint8_t r7[4] = {0, 0, 0, 0};
  if (r8 != 0xFF) {
    for (int i = 0; i < 4; i++) r7[i] = SPI.transfer(0xFF);
  }
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.transfer(0xFF);

  auto poll_acmd41 = [](uint32_t arg, uint16_t max_tries) -> uint8_t {
    uint8_t response = 0xFF;
    digitalWrite(SD_CS_PIN, LOW);
    for (uint16_t i = 0; i < max_tries; i++) {
      sd_send_command(55, 0);
      SPI.transfer(0xFF);  // at least 8 clocks between the two commands
      response = sd_send_command(41, arg);
      if (response == 0x00) break;
      delay(2);
    }
    digitalWrite(SD_CS_PIN, HIGH);
    SPI.transfer(0xFF);
    return response;
  };

  uint8_t r41 = poll_acmd41(0x40000000UL, 500);
  uint8_t r41_nohcs = 0xFF;
  if (r41 != 0x00) {
    r41_nohcs = poll_acmd41(0x00000000UL, 100);
  }

  uint8_t r1_mmc = 0xFF;
  if (r41 != 0x00 && r41_nohcs != 0x00) {
    // Last resort: plain CMD1, in case this is an MMC that still echoed CMD8.
    digitalWrite(SD_CS_PIN, LOW);
    for (int i = 0; i < 100; i++) {
      r1_mmc = sd_send_command(1, 0);
      if (r1_mmc == 0x00) break;
      delay(2);
    }
    digitalWrite(SD_CS_PIN, HIGH);
    SPI.transfer(0xFF);
  }
  if (r41 != 0x00 && r41_nohcs == 0x00) r41 = r41_nohcs;

  uint8_t ocr[4] = {0, 0, 0, 0};
  digitalWrite(SD_CS_PIN, LOW);
  uint8_t r58 = sd_send_command(58, 0);
  if (r58 == 0x00) {
    for (int i = 0; i < 4; i++) ocr[i] = SPI.transfer(0xFF);
  }
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.transfer(0xFF);

  digitalWrite(SD_CS_PIN, LOW);
  uint8_t r16 = sd_send_command(16, 512);
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.transfer(0xFF);

  // If CMD0 still answers, the card did not die: it refused or browned out on
  // ACMD41 rather than losing the SPI link.
  digitalWrite(SD_CS_PIN, LOW);
  uint8_t r0_after = sd_send_command(0, 0);
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.transfer(0xFF);

  log_core_printf(
      "SD raw: CMD0=0x%02X CMD8=0x%02X R7=%02X%02X%02X%02X "
      "ACMD41hcs=0x%02X ACMD41plain=0x%02X CMD1=0x%02X CMD58=0x%02X "
      "OCR=%02X%02X%02X%02X CMD16=0x%02X CMD0_again=0x%02X\n",
      r0, r8, r7[0], r7[1], r7[2], r7[3], r41, r41_nohcs, r1_mmc, r58, ocr[0],
      ocr[1], ocr[2], ocr[3], r16, r0_after);

  if (r41 == 0x00) {
    uint8_t sector[512];
    digitalWrite(SD_CS_PIN, LOW);
    uint8_t r17 = sd_send_command(17, 0);
    if (r17 == 0x00) {
      uint8_t token = 0xFF;
      for (int i = 0; i < 20000 && token == 0xFF; i++) {
        token = SPI.transfer(0xFF);
      }
      if (token == 0xFE) {
        for (int i = 0; i < 512; i++) sector[i] = SPI.transfer(0xFF);
        bool mbr = sector[510] == 0x55 && sector[511] == 0xAA;
        const uint8_t* entry = sector + 446;
        uint32_t lba = entry[8] | (entry[9] << 8) | (entry[10] << 16) |
                       ((uint32_t)entry[11] << 24);
        log_core_printf(
            "SD raw: MBR=%d part1_type=0x%02X part1_lba=%lu boot_sig=%02X%02X\n",
            mbr ? 1 : 0, entry[4], (unsigned long)lba, sector[0], sector[1]);
      }
    }
    digitalWrite(SD_CS_PIN, HIGH);
    SPI.transfer(0xFF);
  }

  SPI.endTransaction();
}

/**
 * @brief Construct a new SDStorage object
 *
 */
SDStorage::SDStorage() : Storage("SD Card") {}

/**
 * @brief Verify SD card connection and create a new, unique file
 *
 * The FA26 Internal board wires the microSD socket to the Pico's default SPI0
 * pins: GP16 MISO, GP17 CS, GP18 SCK, GP19 MOSI (J400).
 *
 * @return true if SD card is connected and file is successfully created
 * @return false otherwise
 */
bool SDStorage::verify() {
  if (this->file_) {
    this->file_.close();
  }

// initialize SD card w/ instance
#if SD_SPI1
  // setup SPI1
  if (!SD.begin(SD_CS_PIN, this->sd_spi_1)) {
    ErrorDisplay::instance().addCode(Error::SD_CARD_FAIL);
    return false;
  }
#else
  SPI.setRX(SD_MISO_PIN);
  SPI.setTX(SD_MOSI_PIN);
  SPI.setSCK(SD_SCK_PIN);
  SPI.begin();

  bool mounted = SD.begin(SD_CS_PIN);
  if (!mounted) {
    log_core("SD mount failed at default clock, retrying at 1 MHz");
    SD.end();
    mounted = SD.begin(SD_CS_PIN, SD_SCK_MHZ(1));
  }
  if (!mounted) {
    // The raw walk takes a couple of seconds, so only run it once instead of
    // on every recovery attempt.
    static bool diagnosed = false;
    if (!diagnosed) {
      diagnosed = true;
      uint8_t r1 = sd_raw_cmd0();
      log_core_printf(
          "SD mount FAILED; raw CMD0 response 0x%02X (%s)\n", r1,
          r1 == 0x01 ? "card responds, filesystem/clock problem"
                     : "no card response, check socket and GP16/GP17/GP18/GP19");
      if (r1 == 0x01) {
        sd_print_layout();
      }
    } else {
      log_core("SD mount still failing");
    }
    ErrorDisplay::instance().addCode(Error::SD_CARD_FAIL);
    return false;
  }
  log_core_printf("SD mounted: %lu MB type=%u fat=%u\n",
                  (unsigned long)(SD.size64() / (1024ULL * 1024ULL)),
                  (unsigned)SD.type(), (unsigned)SD.fatType());
#endif

// find unused file name
#if STORING_PACKETS
  // for bin (eventually just have 1 of these)
  int num = 0;
  while (SD.exists("RAWDATA" + String(num) + ".BIN")) num++;
  if (num != 0) ErrorDisplay::instance().addCode(Error::POWER_CYCLED);
  this->file_name = "RAWDATA" + String(num) + ".BIN";
  log_core("Created file: " + this->file_name);
#else
  int num = 0;
  while (SD.exists("DATA" + String(num) + ".CSV")) num++;
  if (num != 0) ErrorDisplay::instance().addCode(Error::POWER_CYCLED);
  this->file_name = "DATA" + String(num) + ".CSV";
#endif

  log_core("SD Filename: " + this->file_name);

  // Keep the file open instead of open/close on every packet: the FAT metadata
  // churn is slow and makes the write path much less predictable.
  this->file_ = SD.open(this->file_name, FILE_WRITE);
  if (!this->file_) return false;

  return true;  // recovery system will handle this now
}

/**
 * @brief Store data on the SD card, ending with newline
 *
 * @param data Data to store
 */
void SDStorage::store(String data) {
  if (!this->file_) {
    log_core("SD card write failed");
    ErrorDisplay::instance().addCode(Error::SD_CARD_FAIL);
    SD.end();
    this->verified = false;
    return;
  }

  this->file_.println(data);
  if (++this->pending_flush_ >= 8) {
    this->pending_flush_ = 0;
    this->file_.flush();
  }
}

/**
 * @brief Store data on the SD card
 *
 * @param packet Pointer to packet bytes
 */
void SDStorage::storePacket(uint8_t* packet) {
  if (!this->file_) {
    log_core("SD card write failed");
    ErrorDisplay::instance().addCode(Error::SD_CARD_FAIL);
    SD.end();
    this->verified = false;
    return;
  }

  // get length from the packet, after sync bytes (4) and sensor presense (4)
  uint16_t packet_len;
  memcpy(&packet_len, (packet + 8), sizeof(uint16_t));

  if (packet_len >= MIN_ASU_PACKET_SIZE && packet_len <= QT_ENTRY_SIZE) {
    size_t wrote = this->file_.write(packet, packet_len);
    if (wrote != packet_len) {
      log_core("SD short write");
      ErrorDisplay::instance().addCode(Error::SD_CARD_FAIL);
      this->file_.close();
      SD.end();
      this->verified = false;
      return;
    }
  }

  if (++this->pending_flush_ >= 8) {
    this->pending_flush_ = 0;
    this->file_.flush();
  }
}

uint32_t SDStorage::fileSize() {
  if (!this->file_) return 0;
  this->file_.flush();
  return this->file_.size();
}

bool SDStorage::printSummary() {
  if (!this->file_) {
    return false;
  }

  this->file_.flush();
  uint32_t size = this->file_.size();
  uint32_t packets = 0;
  uint32_t bad = 0;
  uint32_t offset = 0;

  this->file_.seek(0);
  while (offset + MIN_ASU_PACKET_SIZE <= size) {
    uint8_t header[14];
    this->file_.seek(offset);
    if (this->file_.read(header, sizeof(header)) != sizeof(header)) break;

    if (memcmp(header, "ASU!", 4) != 0) {
      bad++;
      offset++;
      continue;
    }

    uint16_t len;
    memcpy(&len, header + 8, sizeof(len));
    if (len < MIN_ASU_PACKET_SIZE || len > QT_ENTRY_SIZE ||
        offset + len > size) {
      bad++;
      offset++;
      continue;
    }

    uint8_t buffer[QT_ENTRY_SIZE];
    memcpy(buffer, header, sizeof(header));
    this->file_.seek(offset + sizeof(header));
    size_t rest = len - sizeof(header);
    if (this->file_.read(buffer + sizeof(header), rest) != rest) break;

    uint8_t sum = 0;
    for (uint16_t i = 0; i < len; i++) sum += buffer[i];
    if (sum == 0) {
      packets++;
    } else {
      bad++;
    }
    offset += len;
  }

  this->file_.seek(size);

  Serial.printf("[PNC] %s bytes=%lu packets=%lu bad=%lu\n",
                this->file_name.c_str(), (unsigned long)size,
                (unsigned long)packets, (unsigned long)bad);
  return true;
}
