// A/B firmware slots: switch, clone and validate.
//
// 16 MB flash holds two application slots (ota_0 / ota_1).  The bootloader
// picks one according to the 8 KB otadata partition, so switching systems is
// "write otadata, reboot" - no USB, no computer.
//
// Safety rules (from the design review):
//   * never erase the target before the source has been verified;
//   * copy only the real image length, not the whole slot;
//   * abort() on any write error, end() only after everything was written;
//   * hold the power gate around every flash operation;
//   * only confirm a PENDING_VERIFY image, and only after a self test.
#include <hal/hal.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_image_format.h>
#include <mooncake_log.h>
#include <cstring>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
const char* TAG = "HAL-OTA";
std::mutex g_ota_mutex;              /* one OTA operation at a time */

/* Full image verification of a slot: magic + segments + checksum/hash. */
bool slot_verify(const esp_partition_t* p, uint32_t* image_len)
{
    if (p == nullptr) return false;
    esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
    esp_image_metadata_t meta = {};
    const esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
    if (err != ESP_OK) {
        mclog::tagWarn(TAG, "%s verify failed: %s", p->label, esp_err_to_name(err));
        return false;
    }
    if (image_len) *image_len = meta.image_len;
    return true;
}

}  // namespace

/* Read-only helper for the UI: does the other slot hold a valid image? */
bool Hal::otaOtherSlotValid()
{
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    return slot_verify(other, nullptr);
}

int Hal::otaSlots(OtaSlot* out, int max)
{
    if (out == nullptr || max <= 0) return 0;
    const esp_partition_t* running = esp_ota_get_running_partition();
    int n = 0;
    for (int i = 0; i < 2 && n < max; ++i) {
        const esp_partition_t* p = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP,
            (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_0 + i), nullptr);
        if (p == nullptr) continue;

        out[n].name    = (i == 0) ? 'A' : 'B';
        out[n].size_kb = p->size / 1024;
        out[n].valid   = slot_verify(p, &out[n].image_kb);
        out[n].image_kb /= 1024;
        if (!out[n].valid) out[n].image_kb = 0;
        out[n].running = (p == running);

        /* OTA state is only meaningful for a slot the bootloader selected */
        esp_ota_img_states_t st;
        out[n].state = ESP_OTA_IMG_UNDEFINED;
        if (out[n].running && esp_ota_get_state_partition(p, &st) == ESP_OK) {
            out[n].state = (int)st;
        }
        ++n;
    }
    return n;
}

esp_err_t Hal::otaSwitchToOther()
{
    std::lock_guard<std::mutex> lock(g_ota_mutex);
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    if (other == nullptr) return ESP_ERR_NOT_FOUND;
    if (!slot_verify(other, nullptr)) return ESP_ERR_INVALID_STATE;

    GetHAL().powerBusy("ota-switch");
    const esp_err_t err = esp_ota_set_boot_partition(other);
    GetHAL().powerIdle("ota-switch");

    mclog::tagInfo(TAG, "switch -> %s: %s", other->label, esp_err_to_name(err));
    return err;
}

esp_err_t Hal::otaCloneRunningToOther()
{
    std::lock_guard<std::mutex> lock(g_ota_mutex);

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* other   = esp_ota_get_next_update_partition(nullptr);
    if (running == nullptr || other == nullptr) return ESP_ERR_NOT_FOUND;

    /* Never destroy the backup before knowing the source is complete. */
    uint32_t image_len = 0;
    if (!slot_verify(running, &image_len)) return ESP_ERR_INVALID_STATE;
    if (image_len == 0 || image_len > other->size) return ESP_ERR_INVALID_SIZE;

    GetHAL().powerBusy("ota-clone");

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(other, image_len, &handle);
    if (err != ESP_OK) {
        GetHAL().powerIdle("ota-clone");
        mclog::tagWarn(TAG, "ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    static uint8_t buf[8192];
    for (uint32_t off = 0; off < image_len; off += sizeof(buf)) {
        size_t chunk = image_len - off;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        err = esp_partition_read(running, off, buf, chunk);
        if (err == ESP_OK) err = esp_ota_write(handle, buf, chunk);
        if (err != ESP_OK) {
            mclog::tagWarn(TAG, "clone failed at %u: %s", (unsigned)off, esp_err_to_name(err));
            esp_ota_abort(handle);          /* never end() a partial image */
            GetHAL().powerIdle("ota-clone");
            return err;
        }
        vTaskDelay(1);                      /* let the idle task breathe */
    }

    err = esp_ota_end(handle);           /* validates the written image */
    GetHAL().powerIdle("ota-clone");
    mclog::tagInfo(TAG, "clone %s (%u bytes): %s", other->label, (unsigned)image_len,
                   esp_err_to_name(err));
    return err;
}

esp_err_t Hal::otaConfirmIfPending(bool self_test_ok)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) return ESP_ERR_NOT_FOUND;

    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK) return ESP_ERR_NOT_SUPPORTED;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;   /* nothing to do */

    if (!self_test_ok) return ESP_ERR_INVALID_STATE;

    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    mclog::tagInfo(TAG, "confirm PENDING image: %s", esp_err_to_name(err));
    return err;
}
