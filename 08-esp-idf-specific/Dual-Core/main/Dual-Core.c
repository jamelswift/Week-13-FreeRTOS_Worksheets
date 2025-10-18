// main.c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "DUAL_CORE";

// ====== โครงสร้าง message ระหว่างคอร์ ======
typedef struct {
    uint32_t seq;
    int64_t ts_us;
} core_msg_t;

static QueueHandle_t s_core_queue;

// ====== ตัวแปรเก็บเวลายุ่งของแต่ละคอร์ (หน่วย: us ต่อรอบ) ======
static volatile uint64_t s_busy_core0_us = 0;
static volatile uint64_t s_busy_core1_us = 0;

// ====== ค่าปรับจูนภาระงาน ======
#define MONITOR_PERIOD_MS      1000      // พิมพ์สรุปทุก 1s
#define CORE0_PERIOD_US        10000     // รอบทำงานคอร์ 0 = 10ms
#define CORE0_BUSY_US          7500      // ใช้เวลา busy ~7.5ms/10ms -> ~75%
#define CORE1_IO_PERIOD_US     20000     // รอบทำงานคอร์ 1 = 20ms
#define CORE1_BUSY_US          9000      // ใช้เวลา busy ~9ms/20ms -> ~45%

// ====== งานคอร์ 0: คอมพิวต์หนัก วนรอบเร็ว และส่งข้อความไปคอร์ 1 ======
static void compute_task_core0(void *arg)
{
    uint32_t iter = 0;
    TickType_t last = xTaskGetTickCount();
    int64_t next_msg_ts = esp_timer_get_time();

    for (;;)
    {
        int64_t t0 = esp_timer_get_time();

        // busy-loop จำลองภาระคอมพิวต์ ~ CORE0_BUSY_US
        while ((esp_timer_get_time() - t0) < CORE0_BUSY_US) {
            // งานจำลอง: ทำเลขลอยตัวเล็กน้อย
            volatile float acc = 0.f;
            for (int i = 0; i < 50; ++i) acc += i * 3.14159f;
            (void)acc;
        }
        s_busy_core0_us += (uint64_t)(esp_timer_get_time() - t0);

        iter++;

        // ส่งข้อความไปคอร์ 1 ทุก ~1s
        int64_t now = esp_timer_get_time();
        if (now >= next_msg_ts) {
            core_msg_t m = { .seq = iter, .ts_us = now };
            if (xQueueSend(s_core_queue, &m, 0) == pdTRUE) {
                ESP_LOGI(TAG, "Inter-core message: Core 0 -> Core 1");
            }
            next_msg_ts = now + (int64_t)MONITOR_PERIOD_MS * 1000;
        }

        // พิมพ์สถานะรอบนี้
        ESP_LOGI(TAG, "Core 0 compute task: iteration %u", iter);

        // คุมคาบรอบงานให้ ~CORE0_PERIOD_US
        vTaskDelayUntil(&last, pdMS_TO_TICKS(CORE0_PERIOD_US / 1000));
    }
}

// ====== งานคอร์ 1: I/O/ประมวลผลข้อความจากคอร์ 0 ======
static void io_task_core1(void *arg)
{
    core_msg_t m;
    for (;;)
    {
        // รอข้อความจากคอร์ 0 (timeout เล็กน้อยเพื่อยังคงมีรอบงาน)
        if (xQueueReceive(s_core_queue, &m, pdMS_TO_TICKS(50)) == pdTRUE) {
            ESP_LOGI(TAG, "Core 1 I/O task: processing data (seq=%u)", m.seq);
        } else {
            ESP_LOGI(TAG, "Core 1 I/O task: idle");
        }

        // จำลองภาระ I/O/แปลงข้อมูลให้ใช้เวลาประมาณ CORE1_BUSY_US
        int64_t t1 = esp_timer_get_time();
        while ((esp_timer_get_time() - t1) < CORE1_BUSY_US) {
            // งานจำลอง: คัดลอกบัฟเฟอร์เล็ก ๆ
            char buf[128], out[128];
            memset(buf, 0x5A, sizeof(buf));
            memcpy(out, buf, sizeof(buf));
        }
        s_busy_core1_us += (uint64_t)(esp_timer_get_time() - t1);

        // คุมคาบรอบงาน ~CORE1_IO_PERIOD_US
        vTaskDelay(pdMS_TO_TICKS(CORE1_IO_PERIOD_US / 1000));
    }
}

// ====== งาน Monitor: พิมพ์ “CPU utilization” ทุก 1s ======
static void monitor_task(void *arg)
{
    for (;;)
    {
        uint64_t busy0 = s_busy_core0_us;
        uint64_t busy1 = s_busy_core1_us;
        s_busy_core0_us = 0;
        s_busy_core1_us = 0;

        // ภายในหนึ่งรอบ MONITOR_PERIOD_MS มีทั้งหมด period_us ต่อคอร์เท่ากัน
        const double interval_us = (double)MONITOR_PERIOD_MS * 1000.0;
        double util0 = (busy0 / interval_us) * 100.0;
        double util1 = (busy1 / interval_us) * 100.0;

        ESP_LOGI(TAG, "CPU utilization - Core 0: %.0f%%, Core 1: %.0f%%", util0, util1);
        vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Main on Core %d", xPortGetCoreID());

    // คิวสำหรับส่งข้อความข้ามคอร์
    s_core_queue = xQueueCreate(10, sizeof(core_msg_t));

    // สร้างงานที่ “ปักหมุด” คอร์ชัดเจน
    xTaskCreatePinnedToCore(compute_task_core0, "Core0_Compute",
                            4096, NULL, 5, NULL, 0);   // PRO_CPU = 0
    xTaskCreatePinnedToCore(io_task_core1, "Core1_IO",
                            4096, NULL, 5, NULL, 1);   // APP_CPU = 1

    // งานสรุปผล
    xTaskCreatePinnedToCore(monitor_task, "Monitor",
                            3072, NULL, 4, NULL, 0);   // พิมพ์จาก Core0
}