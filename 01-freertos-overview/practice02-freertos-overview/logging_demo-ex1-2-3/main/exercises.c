#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"       // esp_timer_get_time()
#include "nvs_flash.h"
#include "esp_err.h"

static const char *TAG = "EXERCISES";

void performance_demo(void)
{
    ESP_LOGI(TAG, "=== Performance Monitoring ===");

    // เริ่มจับเวลา (ไมโครวินาที)
    uint64_t start_us = (uint64_t)esp_timer_get_time();

    // งานตัวอย่าง (มี volatile กันคอมไพเลอร์ optimize ทิ้ง)
    for (int i = 0; i < 1000000; i++) {
        volatile int dummy = i * 2;
        (void)dummy;
    }

    uint64_t end_us = (uint64_t)esp_timer_get_time();
    uint64_t exec_us = end_us - start_us;

    ESP_LOGI(TAG, "Execution time: %" PRIu64 " microseconds", exec_us);
    ESP_LOGI(TAG, "Execution time: %.2f milliseconds", exec_us / 1000.0);
}

void error_handling_demo(void)
{
    ESP_LOGI(TAG, "=== Error Handling Demo ===");

    // ตัวอย่างสถานการณ์ต่าง ๆ
    esp_err_t result = ESP_OK;
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Operation completed successfully");
    }

    result = ESP_ERR_NO_MEM;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Error: %s", esp_err_to_name(result));
    }

    result = ESP_ERR_INVALID_ARG;
    ESP_ERROR_CHECK_WITHOUT_ABORT(result);   // ไม่รีเซ็ตชิป แต่รายงานข้อผิดพลาด
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Non-fatal error: %s", esp_err_to_name(result));
    }

    // ตัวอย่างใช้งาน NVS และเช็คข้อผิดพลาด
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized successfully");
}