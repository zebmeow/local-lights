// Standalone ESP32 Matter light controller.
#include "local_web.h"
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <platform/internal/BLEManager.h>

static const char *TAG = "lights";
static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t) {
    if (event->Type == chip::DeviceLayer::DeviceEventType::kESPSystemEvent &&
        event->Platform.ESPSystemEvent.Base == IP_EVENT &&
        event->Platform.ESPSystemEvent.Id == IP_EVENT_STA_GOT_IP) local_web_start();
    if (event->Type == chip::DeviceLayer::DeviceEventType::kBLEDeinitialized)
        ESP_LOGI(TAG, "Bluetooth released: free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
extern "C" void app_main() {
    // Never erase pairing storage automatically during an upgrade.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_matter::start(app_event_cb));
    esp_matter::lock::chip_stack_lock(portMAX_DELAY);
    auto &controller = esp_matter::controller::matter_controller_client::get_instance();
    ESP_ERROR_CHECK(controller.init(112233, 1, 5580));
    ESP_ERROR_CHECK(controller.setup_commissioner());
    local_web_ready();
    if (!local_web_pairing_mode()) chip::DeviceLayer::Internal::BLEMgr().Shutdown();
    esp_matter::lock::chip_stack_unlock();
    ESP_LOGI(TAG, "Controller ready: free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
