#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/adc.h"          // legacy ADC (ง่ายและพอสำหรับแลบ)
#include "esp_adc_cal.h"         // legacy calibration
#include "esp_random.h"
#include "esp_system.h"

static const char *TAG = "TIMER_APPS_EXP2";

/* ================== Pin Definitions ================== */
#define STATUS_LED       GPIO_NUM_2
#define WATCHDOG_LED     GPIO_NUM_4
#define PATTERN_LED_1    GPIO_NUM_5
#define PATTERN_LED_2    GPIO_NUM_18
#define PATTERN_LED_3    GPIO_NUM_19
#define SENSOR_POWER     GPIO_NUM_21     // ใช้เปิด/ปิดไฟเลี้ยงให้เซนเซอร์ผ่านทรานซิสเตอร์ (ถ้ามี)
#define SENSOR_PIN       GPIO_NUM_36     // ADC1_CHANNEL_0 บน ESP32 (ขา GPIO36)

/* ================== Timer Periods (ms) ================== */
#define WATCHDOG_TIMEOUT_MS     5000    // 5 seconds
#define WATCHDOG_FEED_MS        2000    // Feed every 2 seconds
#define PATTERN_BASE_MS         500     // Base pattern timing
#define SENSOR_SAMPLE_MS        1000    // Sensor sampling rate
#define STATUS_UPDATE_MS        3000    // Status update interval

/* ================== Pattern Types ================== */
typedef enum {
    PATTERN_OFF = 0,
    PATTERN_SLOW_BLINK,
    PATTERN_FAST_BLINK,
    PATTERN_HEARTBEAT,
    PATTERN_SOS,
    PATTERN_RAINBOW,
    PATTERN_MAX
} led_pattern_t;

/* ================== Sensor / Health Structs ================== */
typedef struct {
    float value;
    uint32_t timestamp;
    bool valid;
} sensor_data_t;

typedef struct {
    uint32_t watchdog_feeds;
    uint32_t watchdog_timeouts;
    uint32_t pattern_changes;
    uint32_t sensor_readings;
    uint32_t system_uptime_sec;
    bool system_healthy;
} system_health_t;

/* ================== Globals ================== */
static TimerHandle_t watchdog_timer;
static TimerHandle_t feed_timer;
static TimerHandle_t pattern_timer;
static TimerHandle_t sensor_timer;
static TimerHandle_t status_timer;

static QueueHandle_t sensor_queue;
static QueueHandle_t pattern_queue;

static led_pattern_t current_pattern = PATTERN_OFF;
static int pattern_step = 0;
static system_health_t health_stats = {0, 0, 0, 0, 0, true};

/* complex pattern state */
typedef struct {
    int step;
    int direction;
    int intensity;
    bool state;
} pattern_state_t;

static pattern_state_t pattern_state = {0, 1, 0, false};

/* legacy ADC calibration */
static esp_adc_cal_characteristics_t *adc_chars;

/* ================== Prototypes (ประกาศก่อนใช้) ================== */
static void watchdog_timeout_callback(TimerHandle_t timer);
static void feed_watchdog_callback(TimerHandle_t timer);
static void recovery_callback(TimerHandle_t timer);

static void set_pattern_leds(bool led1, bool led2, bool led3);
static void pattern_timer_callback(TimerHandle_t timer);
static void change_led_pattern(led_pattern_t new_pattern);

static float read_sensor_value(void);
static void sensor_timer_callback(TimerHandle_t timer);

static void status_timer_callback(TimerHandle_t timer);

static void sensor_processing_task(void *parameter);
static void system_monitor_task(void *parameter);

static void init_hardware(void);
static void create_timers(void);
static void create_queues(void);
static void start_system(void);

/* ================== WATCHDOG SYSTEM ================== */

static void watchdog_timeout_callback(TimerHandle_t timer)
{
    health_stats.watchdog_timeouts++;
    health_stats.system_healthy = false;

    ESP_LOGE(TAG, "🚨 WATCHDOG TIMEOUT! System may be hung!");
    ESP_LOGE(TAG, "Stats: Feeds=%lu, Timeouts=%lu",
             (unsigned long)health_stats.watchdog_feeds,
             (unsigned long)health_stats.watchdog_timeouts);

    /* Flash watchdog LED rapidly */
    for (int i = 0; i < 10; i++) {
        gpio_set_level(WATCHDOG_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(WATCHDOG_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGW(TAG, "In production you might call esp_restart() here.");

    /* reset watchdog for demo to continue */
    xTimerReset(watchdog_timer, 0);
    health_stats.system_healthy = true;
}

static void recovery_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "🔄 System recovered - resume watchdog feeds");
    xTimerStart(feed_timer, 0);
    xTimerDelete(timer, 0);
}

static void feed_watchdog_callback(TimerHandle_t timer)
{
    static int feed_count = 0;
    feed_count++;

    /* simulate 1-time hang */
    if (feed_count == 15) {
        ESP_LOGW(TAG, "🐛 Simulate hang: stop feeds for 8s");
        xTimerStop(feed_timer, 0);

        TimerHandle_t recovery = xTimerCreate("Recovery",
                                              pdMS_TO_TICKS(8000),
                                              pdFALSE, (void*)0,
                                              recovery_callback);
        if (recovery) xTimerStart(recovery, 0);
        return;
    }

    health_stats.watchdog_feeds++;
    ESP_LOGI(TAG, "🍖 Feed watchdog (%lu)",
             (unsigned long)health_stats.watchdog_feeds);

    xTimerReset(watchdog_timer, 0);

    gpio_set_level(STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(STATUS_LED, 0);
}

/* ================== LED PATTERN SYSTEM ================== */

static void set_pattern_leds(bool led1, bool led2, bool led3)
{
    gpio_set_level(PATTERN_LED_1, led1);
    gpio_set_level(PATTERN_LED_2, led2);
    gpio_set_level(PATTERN_LED_3, led3);
}

static void change_led_pattern(led_pattern_t new_pattern)
{
    const char* names[] = {"OFF","SLOW_BLINK","FAST_BLINK","HEARTBEAT","SOS","RAINBOW"};

    ESP_LOGI(TAG, "🎨 Pattern: %s -> %s",
             names[current_pattern], names[new_pattern]);

    current_pattern = new_pattern;
    pattern_step = 0;
    pattern_state.step = 0;
    pattern_state.state = false;
    health_stats.pattern_changes++;

    xTimerReset(pattern_timer, 0);
}

static void pattern_timer_callback(TimerHandle_t timer)
{
    static uint32_t pattern_cycle = 0;
    pattern_cycle++;

    switch (current_pattern) {
    default:
    case PATTERN_OFF:
        set_pattern_leds(0,0,0);
        xTimerChangePeriod(timer, pdMS_TO_TICKS(1000), 0);
        break;

    case PATTERN_SLOW_BLINK:
        pattern_state.state = !pattern_state.state;
        set_pattern_leds(pattern_state.state, 0, 0);
        xTimerChangePeriod(timer, pdMS_TO_TICKS(1000), 0);
        break;

    case PATTERN_FAST_BLINK:
        pattern_state.state = !pattern_state.state;
        set_pattern_leds(0, pattern_state.state, 0);
        xTimerChangePeriod(timer, pdMS_TO_TICKS(200), 0);
        break;

    case PATTERN_HEARTBEAT: {
        /* ..  .. (double pulse) */
        int step = pattern_step % 10;
        bool pulse = (step < 2) || (step >= 3 && step < 5);
        set_pattern_leds(0, 0, pulse);
        pattern_step++;
        xTimerChangePeriod(timer, pdMS_TO_TICKS(100), 0);
        if (step == 9) ESP_LOGI(TAG, "💓 Heartbeat pulse");
        break;
    }

    case PATTERN_SOS: {
        static const char* sos = "...---...";
        static int sos_pos = 0;
        bool dot = (sos[sos_pos] == '.');
        int duration = dot ? 200 : 600;
        set_pattern_leds(1,1,1);
        vTaskDelay(pdMS_TO_TICKS(duration));
        set_pattern_leds(0,0,0);
        sos_pos = (sos_pos + 1) % (int)strlen(sos);
        if (sos_pos == 0) {
            ESP_LOGI(TAG, "🆘 SOS complete");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        /* keep base period */
        xTimerChangePeriod(timer, pdMS_TO_TICKS(PATTERN_BASE_MS), 0);
        break;
    }

    case PATTERN_RAINBOW: {
        int step = pattern_step % 8;
        bool l1 = (step & 1);
        bool l2 = (step & 2);
        bool l3 = (step & 4);
        set_pattern_leds(l1, l2, l3);
        pattern_step++;
        if (step == 7) ESP_LOGI(TAG, "🌈 Rainbow cycle");
        xTimerChangePeriod(timer, pdMS_TO_TICKS(300), 0);
        break;
    }
    }

    /* เปลี่ยน pattern อัตโนมัติทุก ~50 รอบ */
    if (pattern_cycle % 50 == 0) {
        led_pattern_t next = (current_pattern + 1) % PATTERN_MAX;
        change_led_pattern(next);
    }
}

/* ================== SENSOR SYSTEM ================== */

static float read_sensor_value(void)
{
    /* เปิดไฟเลี้ยง (ถ้าควบคุมผ่านทรานซิสเตอร์) */
    gpio_set_level(SENSOR_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* อ่าน ADC (GPIO36 = ADC1_CHANNEL_0) */
    uint32_t raw = adc1_get_raw(ADC1_CHANNEL_0);
    uint32_t mv  = esp_adc_cal_raw_to_voltage(raw, adc_chars);

    /* แปลงเป็นค่า “อุณหภูมิจำลอง” 0–50°C */
    float temp = (mv / 1000.0f) * 50.0f;

    /* เติม noise เล็กน้อย */
    temp += (int32_t)(esp_random() % 101 - 50) / 100.0f;

    /* ปิดไฟเลี้ยงประหยัดพลังงาน */
    gpio_set_level(SENSOR_POWER, 0);

    return temp;
}

static void sensor_timer_callback(TimerHandle_t timer)
{
    sensor_data_t s;
    s.value = read_sensor_value();
    s.timestamp = xTaskGetTickCount();
    s.valid = (s.value >= 0 && s.value <= 50);

    health_stats.sensor_readings++;

    /* ใน timer callback = context ของ timer task → ใช้ xQueueSend() ปกติ */
    if (xQueueSend(sensor_queue, &s, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Sensor queue full - drop");
    }

    /* ปรับคาบ sampling ตามค่า */
    TickType_t new_period;
    if (s.value > 40.0f)       new_period = pdMS_TO_TICKS(500);
    else if (s.value > 25.0f)  new_period = pdMS_TO_TICKS(1000);
    else                       new_period = pdMS_TO_TICKS(2000);

    xTimerChangePeriod(timer, new_period, 0);
}

/* ================== STATUS SYSTEM ================== */

static void status_timer_callback(TimerHandle_t timer)
{
    health_stats.system_uptime_sec = (uint32_t)(pdTICKS_TO_MS(xTaskGetTickCount()) / 1000);

    ESP_LOGI(TAG, "\n══════ SYSTEM STATUS ══════");
    ESP_LOGI(TAG, "Uptime: %lus", (unsigned long)health_stats.system_uptime_sec);
    ESP_LOGI(TAG, "Health: %s", health_stats.system_healthy ? "✅ OK" : "❌ ISSUE");
    ESP_LOGI(TAG, "Watchdog Feeds: %lu  Timeouts: %lu",
             (unsigned long)health_stats.watchdog_feeds,
             (unsigned long)health_stats.watchdog_timeouts);
    ESP_LOGI(TAG, "Pattern Changes: %lu  Sensor Readings: %lu",
             (unsigned long)health_stats.pattern_changes,
             (unsigned long)health_stats.sensor_readings);
    ESP_LOGI(TAG, "Current Pattern: %d", (int)current_pattern);
    ESP_LOGI(TAG, "Timers: WD=%s  Feed=%s  Pat=%s  Sensor=%s",
             xTimerIsTimerActive(watchdog_timer) ? "ON" : "OFF",
             xTimerIsTimerActive(feed_timer)     ? "ON" : "OFF",
             xTimerIsTimerActive(pattern_timer)  ? "ON" : "OFF",
             xTimerIsTimerActive(sensor_timer)   ? "ON" : "OFF");
    ESP_LOGI(TAG, "═══════════════════════════\n");

    /* flash status LED */
    gpio_set_level(STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(STATUS_LED, 0);
}

/* ================== TASKS ================== */

static void sensor_processing_task(void *parameter)
{
    sensor_data_t s;
    float sum = 0.0f;
    int cnt = 0;

    ESP_LOGI(TAG, "SensorProc started");

    for (;;) {
        if (xQueueReceive(sensor_queue, &s, portMAX_DELAY) == pdTRUE) {
            if (s.valid) {
                sum += s.value;
                cnt++;

                ESP_LOGI(TAG, "🌡️ Sensor: %.2f°C @ %lums",
                         s.value, (unsigned long)pdTICKS_TO_MS(s.timestamp));

                if (cnt >= 10) {
                    float avg = sum / (float)cnt;
                    ESP_LOGI(TAG, "📊 Moving Avg(10): %.2f°C", avg);

                    if (avg > 35.0f) {
                        ESP_LOGW(TAG, "🔥 High temp warning → FAST_BLINK");
                        change_led_pattern(PATTERN_FAST_BLINK);
                    } else if (avg < 15.0f) {
                        ESP_LOGW(TAG, "🧊 Low temp warning → SOS");
                        change_led_pattern(PATTERN_SOS);
                    }

                    sum = 0.0f;
                    cnt = 0;
                }
            } else {
                ESP_LOGW(TAG, "Invalid reading: %.2f", s.value);
            }
        }
    }
}

static void system_monitor_task(void *parameter)
{
    ESP_LOGI(TAG, "SysMonitor started");
    uint32_t last_sensor_count = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // 60s

        if (health_stats.watchdog_timeouts > 5) {
            ESP_LOGE(TAG, "🚨 Too many watchdog timeouts!");
            health_stats.system_healthy = false;
        }

        if (health_stats.sensor_readings == last_sensor_count) {
            ESP_LOGW(TAG, "⚠️ Sensor stuck?");
            /* restart sensor timer if needed */
        }
        last_sensor_count = health_stats.sensor_readings;

        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "💾 Free heap: %u bytes", (unsigned)free_heap);
        if (free_heap < 10000) {
            ESP_LOGW(TAG, "⚠️ Low memory!");
        }
    }
}

/* ================== INIT / CREATE / START ================== */

static void init_hardware(void)
{
    /* LEDs */
    gpio_set_direction(STATUS_LED,      GPIO_MODE_OUTPUT);
    gpio_set_direction(WATCHDOG_LED,    GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_1,   GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_2,   GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_3,   GPIO_MODE_OUTPUT);
    gpio_set_direction(SENSOR_POWER,    GPIO_MODE_OUTPUT);

    gpio_set_level(STATUS_LED, 0);
    gpio_set_level(WATCHDOG_LED, 0);
    gpio_set_level(PATTERN_LED_1, 0);
    gpio_set_level(PATTERN_LED_2, 0);
    gpio_set_level(PATTERN_LED_3, 0);
    gpio_set_level(SENSOR_POWER, 0);

    /* ADC: ใช้ ADC1_CHANNEL_0 = GPIO36 */
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);

    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12,
                             1100, adc_chars);

    ESP_LOGI(TAG, "Hardware init OK (Sensor on GPIO36/ADC1_CH0)");
}

static void create_timers(void)
{
    watchdog_timer = xTimerCreate("WatchdogTimer",
                                  pdMS_TO_TICKS(WATCHDOG_TIMEOUT_MS),
                                  pdFALSE, (void*)1, watchdog_timeout_callback);

    feed_timer     = xTimerCreate("FeedTimer",
                                  pdMS_TO_TICKS(WATCHDOG_FEED_MS),
                                  pdTRUE, (void*)2, feed_watchdog_callback);

    pattern_timer  = xTimerCreate("PatternTimer",
                                  pdMS_TO_TICKS(PATTERN_BASE_MS),
                                  pdTRUE, (void*)3, pattern_timer_callback);

    sensor_timer   = xTimerCreate("SensorTimer",
                                  pdMS_TO_TICKS(SENSOR_SAMPLE_MS),
                                  pdTRUE, (void*)4, sensor_timer_callback);

    status_timer   = xTimerCreate("StatusTimer",
                                  pdMS_TO_TICKS(STATUS_UPDATE_MS),
                                  pdTRUE, (void*)5, status_timer_callback);

    if (!watchdog_timer || !feed_timer || !pattern_timer || !sensor_timer || !status_timer) {
        ESP_LOGE(TAG, "Create timer FAILED");
    } else {
        ESP_LOGI(TAG, "All timers created");
    }
}

static void create_queues(void)
{
    sensor_queue  = xQueueCreate(20, sizeof(sensor_data_t));
    pattern_queue = xQueueCreate(10, sizeof(led_pattern_t));

    if (!sensor_queue || !pattern_queue) {
        ESP_LOGE(TAG, "Create queue FAILED");
    } else {
        ESP_LOGI(TAG, "Queues created");
    }
}

static void start_system(void)
{
    ESP_LOGI(TAG, "Starting timers & tasks...");

    xTimerStart(watchdog_timer, 0);
    xTimerStart(feed_timer, 0);
    xTimerStart(pattern_timer, 0);
    xTimerStart(sensor_timer, 0);
    xTimerStart(status_timer, 0);

    xTaskCreate(sensor_processing_task, "SensorProc", 3072, NULL, 6, NULL);
    xTaskCreate(system_monitor_task,    "SysMonitor",  3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "🚀 System Started");
}

void app_main(void)
{
    ESP_LOGI(TAG, "===== Timer Applications: EXP2 (Pattern Evolution) =====");

    init_hardware();
    create_queues();
    create_timers();
    start_system();

    /* เริ่มต้นด้วย SLOW_BLINK */
    change_led_pattern(PATTERN_SLOW_BLINK);

    ESP_LOGI(TAG, "Ready. Observe LEDs & logs.");
}