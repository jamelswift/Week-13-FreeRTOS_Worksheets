#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "MUTEX_LAB_EXP3";

#define LED_TASK1    GPIO_NUM_2   
#define LED_TASK2    GPIO_NUM_4   
#define LED_TASK3    GPIO_NUM_5   
#define LED_CRITICAL GPIO_NUM_18  


static SemaphoreHandle_t xMutex;


typedef struct {
    uint32_t counter;
    char     shared_buffer[100];
    uint32_t checksum;
    uint32_t access_count;
} shared_resource_t;

static shared_resource_t shared_data = {0, "", 0, 0};

/* ===== Stats ===== */
typedef struct {
    uint32_t successful_access;
    uint32_t failed_access;
    uint32_t corruption_detected;
    uint32_t priority_inversions;
} access_stats_t;

static access_stats_t stats = {0, 0, 0, 0};

/* ===== Helpers ===== */
static uint32_t calculate_checksum(const char* data, uint32_t counter) {
    uint32_t sum = counter;
    for (int i = 0; data[i] != '\0'; i++) {
        sum += (uint32_t)data[i] * (i + 1);
    }
    return sum;
}

/* ===== Critical section (with MUTEX) ===== */
static void access_shared_resource(int task_id, const char* task_name, gpio_num_t led_pin) {
    char     temp_buffer[100];
    uint32_t temp_counter;
    uint32_t expected_checksum;

    ESP_LOGI(TAG, "[%s] Requesting mutex...", task_name);

    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        ESP_LOGI(TAG, "[%s] ✓ Mutex acquired", task_name);
        stats.successful_access++;

        gpio_set_level(led_pin, 1);
        gpio_set_level(LED_CRITICAL, 1);

        // --- BEGIN CRITICAL ---
        temp_counter      = shared_data.counter;
        strcpy(temp_buffer, shared_data.shared_buffer);
        expected_checksum = shared_data.checksum;

        uint32_t calc = calculate_checksum(temp_buffer, temp_counter);
        if (calc != expected_checksum && shared_data.access_count > 0) {
            ESP_LOGE(TAG, "[%s] ⚠ DATA CORRUPTION DETECTED (pre)", task_name);
            stats.corruption_detected++;
        }

        ESP_LOGI(TAG, "[%s] Current  Cnt:%lu  Buf:'%s'", task_name, temp_counter, temp_buffer);

        // เวลาประมวลผล (เปิดช่องให้ context switch)
        vTaskDelay(pdMS_TO_TICKS(400 + (esp_random() % 800)));

        shared_data.counter = temp_counter + 1;
        snprintf(shared_data.shared_buffer, sizeof(shared_data.shared_buffer),
                 "Modified by %s #%lu", task_name, shared_data.counter);
        shared_data.checksum = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
        shared_data.access_count++;

        ESP_LOGI(TAG, "[%s] ✓ Modified Cnt:%lu  Buf:'%s'",
                 task_name, shared_data.counter, shared_data.shared_buffer);

        // เวลาประมวลผลต่อ
        vTaskDelay(pdMS_TO_TICKS(150 + (esp_random() % 400)));
        // --- END CRITICAL ---

        gpio_set_level(led_pin, 0);
        gpio_set_level(LED_CRITICAL, 0);

        xSemaphoreGive(xMutex);
        ESP_LOGI(TAG, "[%s] Mutex released", task_name);

    } else {
        ESP_LOGW(TAG, "[%s] ✗ Mutex timeout", task_name);
        stats.failed_access++;
        for (int i = 0; i < 2; i++) {
            gpio_set_level(led_pin, 1); vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(led_pin, 0); vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ===== Tasks ===== */
static void high_priority_task(void *pv) {
    ESP_LOGI(TAG, "High Priority Task (EXP3) started (prio:%d)", uxTaskPriorityGet(NULL));
    while (1) {
        access_shared_resource(1, "HIGH_PRI", LED_TASK1);
        vTaskDelay(pdMS_TO_TICKS(5000 + (esp_random() % 3000))); // 5–8s
    }
}

static void medium_priority_task(void *pv) {
    ESP_LOGI(TAG, "Medium Priority Task (EXP3) started (prio:%d)", uxTaskPriorityGet(NULL));
    while (1) {
        access_shared_resource(2, "MED_PRI", LED_TASK2);
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 2000))); // 3–5s
    }
}

static void low_priority_task(void *pv) {
    ESP_LOGI(TAG, "Low Priority Task (EXP3) started (prio:%d)", uxTaskPriorityGet(NULL));
    while (1) {
        access_shared_resource(3, "LOW_PRI", LED_TASK3);
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 1000))); // 2–3s
    }
}

/* งานโหลด CPU เพื่อเน้นผลการชิง CPU/priority */
static void cpu_load_task(void *pv) {
    ESP_LOGI(TAG, "CPU Load Task started (prio:%d)", uxTaskPriorityGet(NULL));
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "🔄 Doing CPU-intensive work (with mutex system)...");
        uint32_t t0 = xTaskGetTickCount();
        for (volatile int i = 0; i < 1000000; i++) { /* busy */ }
        uint32_t t1 = xTaskGetTickCount();
        ESP_LOGI(TAG, "CPU work took %lu ms", (t1 - t0) * portTICK_PERIOD_MS);
    }
}

/* มอนิเตอร์ระบบ */
static void monitor_task(void *pv) {
    ESP_LOGI(TAG, "System monitor started (prio:%d)", uxTaskPriorityGet(NULL));
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGI(TAG, "\n═══ MUTEX SYSTEM MONITOR (EXP#3 Priority Swap) ═══");
        ESP_LOGI(TAG, "Mutex Available: %s", uxSemaphoreGetCount(xMutex) ? "YES" : "NO (held)");
        ESP_LOGI(TAG, "Shared Resource:");
        ESP_LOGI(TAG, "  Counter      : %lu", shared_data.counter);
        ESP_LOGI(TAG, "  Buffer       : '%s'", shared_data.shared_buffer);
        ESP_LOGI(TAG, "  Access Count : %lu", shared_data.access_count);
        ESP_LOGI(TAG, "  Checksum     : %lu", shared_data.checksum);

        uint32_t chk = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
        if (chk != shared_data.checksum && shared_data.access_count > 0) {
            ESP_LOGE(TAG, "⚠ CURRENT DATA CORRUPTION DETECTED!");
            stats.corruption_detected++;
        }

        ESP_LOGI(TAG, "Access Stats:");
        ESP_LOGI(TAG, "  Successful : %lu", stats.successful_access);
        ESP_LOGI(TAG, "  Failed     : %lu", stats.failed_access);
        ESP_LOGI(TAG, "  Corrupted  : %lu", stats.corruption_detected);
        float rate = (stats.successful_access + stats.failed_access) ?
            (float)stats.successful_access / (stats.successful_access + stats.failed_access) * 100.0f : 0.0f;
        ESP_LOGI(TAG, "  Success Rate: %.1f%%", rate);
        ESP_LOGI(TAG, "══════════════════════════════════════════════════\n");
    }
}

/* ===== app_main ===== */
void app_main(void) {
    ESP_LOGI(TAG, "Experiment #3: Adjust Task Priorities (use MUTEX)");

    // GPIO init
    gpio_set_direction(LED_TASK1,    GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TASK2,    GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TASK3,    GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CRITICAL, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_TASK1, 0);
    gpio_set_level(LED_TASK2, 0);
    gpio_set_level(LED_TASK3, 0);
    gpio_set_level(LED_CRITICAL, 0);

    // Mutex
    xMutex = xSemaphoreCreateMutex();
    if (!xMutex) {
        ESP_LOGE(TAG, "Failed to create mutex!");
        return;
    }

    // Init shared data
    shared_data.counter = 0;
    strcpy(shared_data.shared_buffer, "Initial state");
    shared_data.checksum = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
    shared_data.access_count = 0;

    /* ===== Create tasks with requested priorities =====
       High=2, Low=5, Medium=3, CPU=4, Monitor=1
    */
    xTaskCreate(high_priority_task,   "HighPri",  3072, NULL, 2, NULL); // ↓ ลด priority
    xTaskCreate(cpu_load_task,        "CPULoad",  2048, NULL, 4, NULL);
    xTaskCreate(medium_priority_task, "MedPri",   3072, NULL, 3, NULL);
    xTaskCreate(low_priority_task,    "LowPri",   3072, NULL, 5, NULL); // ↑ เพิ่ม priority
    xTaskCreate(monitor_task,         "Monitor",  3072, NULL, 1, NULL);

    ESP_LOGI(TAG, "Priorities (EXP3): Low=5, CPU=4, Med=3, High=2, Monitor=1");

    // LED startup sequence
    for (int i = 0; i < 2; i++) {
        gpio_set_level(LED_TASK1, 1); vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(LED_TASK1, 0);
        gpio_set_level(LED_TASK2, 1); vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(LED_TASK2, 0);
        gpio_set_level(LED_TASK3, 1); vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(LED_TASK3, 0);
        gpio_set_level(LED_CRITICAL, 1); vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(LED_CRITICAL, 0);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    ESP_LOGI(TAG, "System running — คาดว่า LOW_PRI (prio 5) จะชิงเข้า critical section ได้บ่อยกว่า HIGH_PRI (prio 2)");
}