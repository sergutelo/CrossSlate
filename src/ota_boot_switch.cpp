#include "ota_boot_switch.h"
#include "config.h"

#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <esp_spi_flash.h>
#include <cstring>

namespace {
struct __attribute__((packed)) OtaSelectEntry {
  uint32_t sequence;
  uint8_t label[20];
  uint32_t state;
  uint32_t crc;
};
static_assert(sizeof(OtaSelectEntry) == 32, "Unexpected otadata entry size");

constexpr uint32_t OTA_STATE_INVALID = 3;
constexpr uint32_t OTA_STATE_ABORTED = 4;
constexpr int OTA_PARTITION_COUNT = 2;  // partitions.csv: ota_0 + ota_1
const esp_partition_t* crossInkPartition = nullptr;

uint32_t sequenceCrc(uint32_t sequence) {
  return esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&sequence), sizeof(sequence));
}

bool validOtaEntry(const OtaSelectEntry& entry) {
  return entry.sequence != UINT32_MAX && entry.crc == sequenceCrc(entry.sequence) &&
         entry.state != OTA_STATE_INVALID && entry.state != OTA_STATE_ABORTED;
}

// Runtime image verification rejects some Xteink-patched images. Validate the
// immutable image and application headers directly instead of accepting magic alone.
bool hasPlausibleApplication(const esp_partition_t* partition) {
  if (!partition || partition->type != ESP_PARTITION_TYPE_APP ||
      partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
      partition->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_15) return false;

  esp_image_header_t image{};
  if (esp_partition_read(partition, 0, &image, sizeof(image)) != ESP_OK) return false;
  if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.segment_count == 0 ||
      image.segment_count > ESP_IMAGE_MAX_SEGMENTS) return false;
  // CrossInk only ships for the Xteink X3/X4 (ESP32-C3). Do not offer the
  // switch when the alternate slot holds an image for a different chip.
  if (image.chip_id != 5 /* ESP_CHIP_ID_ESP32C3 */) return false;

  esp_app_desc_t app{};
  constexpr size_t APP_DESC_OFFSET = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  if (APP_DESC_OFFSET + sizeof(app) > partition->size ||
      esp_partition_read(partition, APP_DESC_OFFSET, &app, sizeof(app)) != ESP_OK) return false;
  return app.magic_word == ESP_APP_DESC_MAGIC_WORD;
}
}  // namespace

void otaBootConfirmRunningImage() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) {
    DBG_PRINTLN("[BOOT] No running OTA partition to confirm");
    return;
  }
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const esp_err_t stateErr = esp_ota_get_state_partition(running, &state);
  if (stateErr != ESP_OK) {
    DBG_PRINTF("[BOOT] OTA state unavailable: %s\n", esp_err_to_name(stateErr));
    return;
  }
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

  const esp_err_t confirmErr = esp_ota_mark_app_valid_cancel_rollback();
  if (confirmErr == ESP_OK) {
    DBG_PRINTLN("[BOOT] CrossSlate OTA image confirmed");
  } else {
    DBG_PRINTF("[BOOT] CrossSlate OTA confirmation failed: %s\n", esp_err_to_name(confirmErr));
  }
}

bool otaBootFindCrossInk() {
  crossInkPartition = nullptr;
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_partition_iterator_t iterator = esp_partition_find(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (iterator) {
    const esp_partition_t* candidate = esp_partition_get(iterator);
    if (candidate && (!running || candidate->address != running->address) &&
        hasPlausibleApplication(candidate)) {
      crossInkPartition = candidate;
      break;
    }
    iterator = esp_partition_next(iterator);
  }
  esp_partition_iterator_release(iterator);
  DBG_PRINTF("[BOOT] CrossInk alternate slot: %s\n", crossInkPartition ? crossInkPartition->label : "none");
  return crossInkPartition != nullptr;
}

bool otaBootCrossInkAvailable() { return crossInkPartition != nullptr; }

bool otaBootSwitchToCrossInk() {
  const esp_partition_t* destination = crossInkPartition;
  if (!hasPlausibleApplication(destination)) {
    DBG_PRINTLN("[BOOT] Destination validation failed");
    return false;
  }

  const esp_partition_t* otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata || otadata->size < 2 * SPI_FLASH_SEC_SIZE) {
    DBG_PRINTLN("[BOOT] Invalid otadata partition");
    return false;
  }

  OtaSelectEntry current[2]{};
  esp_err_t err0 = esp_partition_read(otadata, 0, &current[0], sizeof(current[0]));
  esp_err_t err1 = esp_partition_read(otadata, SPI_FLASH_SEC_SIZE, &current[1], sizeof(current[1]));
  if (err0 != ESP_OK || err1 != ESP_OK) {
    DBG_PRINTF("[BOOT] otadata read failed: %s / %s\n", esp_err_to_name(err0), esp_err_to_name(err1));
    return false;
  }

  int activeCopy = -1;
  uint32_t activeSequence = 0;
  for (int i = 0; i < 2; ++i) {
    if (validOtaEntry(current[i]) && (activeCopy < 0 || current[i].sequence > activeSequence)) {
      activeCopy = i;
      activeSequence = current[i].sequence;
    }
  }

  const uint32_t destinationIndex = static_cast<uint32_t>(destination->subtype) -
                                    static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (destinationIndex >= OTA_PARTITION_COUNT) return false;
  uint32_t nextSequence = activeSequence + 1;
  if (nextSequence == 0 || nextSequence == UINT32_MAX) nextSequence = 1;
  while (((nextSequence - 1) % OTA_PARTITION_COUNT) != destinationIndex) ++nextSequence;

  OtaSelectEntry next{};
  next.sequence = nextSequence;
  std::memset(next.label, 0xFF, sizeof(next.label));
  next.state = 0;  // ESP_OTA_IMG_NEW
  next.crc = sequenceCrc(next.sequence);

  const int targetCopy = activeCopy == 0 ? 1 : 0;
  const size_t offset = static_cast<size_t>(targetCopy) * SPI_FLASH_SEC_SIZE;
  esp_err_t err = esp_partition_erase_range(otadata, offset, SPI_FLASH_SEC_SIZE);
  if (err != ESP_OK) {
    DBG_PRINTF("[BOOT] otadata erase failed: %s\n", esp_err_to_name(err));
    return false;
  }
  err = esp_partition_write(otadata, offset, &next, sizeof(next));
  if (err != ESP_OK) {
    DBG_PRINTF("[BOOT] otadata write failed: %s\n", esp_err_to_name(err));
    return false;
  }

  OtaSelectEntry verified{};
  err = esp_partition_read(otadata, offset, &verified, sizeof(verified));
  if (err != ESP_OK || std::memcmp(&verified, &next, sizeof(next)) != 0 || !validOtaEntry(verified)) {
    DBG_PRINTF("[BOOT] otadata verification failed: %s\n", esp_err_to_name(err));
    return false;
  }
  DBG_PRINTF("[BOOT] Selected %s using otadata copy %d, sequence %u\n",
             destination->label, targetCopy, static_cast<unsigned>(nextSequence));
  return true;
}
