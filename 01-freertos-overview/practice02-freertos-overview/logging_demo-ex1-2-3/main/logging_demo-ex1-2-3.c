#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"

#include "custom_logger.h"   // Exercise 1
#include "exercises.h"       // Exercise 2 & 3

static const char *TAG = "LOGGING_DEMO";

// ช่วยพิมพ์ CONFIG_IDF_TARGET เป็นสตริง
#define STRINGIFY2(x) #x
#define STRINGIFY(x)  STRINGIFY2(x)

void app_main(void)
{
    // ปรับระดับ log (ย้ายขึ้นก่อน log ใด ๆ)
    esp_log_level_set("*", ESP_LOG_INFO);        // Global = INFO
    esp_log_level_set("LOGGING_DEMO", ESP_LOG_DEBUG);

    // ข้อมูลระบบเล็กน้อย
    ESP_LOGI(TAG, "=== ESP32 Logging Exercises ===");
    ESP_LOGI(TAG, "IDF Version : %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Chip Target : %s", STRINGIFY(CONFIG_IDF_TARGET));
    ESP_LOGI(TAG, "Free Heap   : %" PRIu32 " bytes",
             (uint32_t)esp_get_free_heap_size());

    // ขนาด Flash (อ่านผ่าน esp_flash API)
    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "Flash Size  : %" PRIu32 " MB", flash_bytes / (1024 * 1024));
    }

    // ---------- Exercise 1: Custom Logger ----------
    custom_log("SENSOR", "Temperature: %d°C", 25);
    custom_log("NET",    "Link status: %s", "UP");

    // ---------- Exercise 2: Performance Monitoring ----------
    performance_demo();

    // ---------- Exercise 3: Error Handling Demo ----------
    error_handling_demo();

    // ลูปหลัก (เดโมสถานะ)
    int counter = 0;
    while (1) {
        ESP_LOGI(TAG, "Main loop iteration: %d", counter++);
        if (counter % 10 == 0) {
            ESP_LOGD(TAG, "Debug tick (every 10 iters).");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}