#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include "ErrorDisplay.h"
#include "PayloadConfig.h"
#include "SD.h"
#include "Storage.h"

/**
 * @brief Implementation of a Storage device to interface with an SD card
 *
 */
class SDStorage : public Storage {
 private:
  String file_name;
  File file_;
  uint8_t pending_flush_ = 0;
  bool space_low_ = false;
  bool open_log();
#if SD_SPI1
  SPIClassRP2040 sd_spi_1 = SPIClassRP2040(spi1, SD_MISO_PIN, SD_CS_PIN,
                                           SD_SCK_PIN, SD_MOSI_PIN);
#endif

 public:
  SDStorage();
  bool verify() override;
  void store(String data) override;
  void storePacket(uint8_t* packet) override;

  /**
   * @brief Re-read the capture file and report packet/checksum counts.
   * @return true if the card and file are usable.
   */
  bool printSummary();

  bool isOpen() const { return (bool)this->file_; }
  uint32_t fileSize();
};

#endif
