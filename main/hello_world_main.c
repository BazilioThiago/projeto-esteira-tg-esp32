/* Esteira industrial M2 - Guilherme Santana e Thiago Bazilio.
 * Base M1 preservada em docs/m1_original.c. Atuadores simulados. */
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/touch_sensor.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "m2.h"

_Static_assert(CONFIG_FREERTOS_UNICORE, "M2 requires UNICORE");
_Static_assert(CONFIG_FREERTOS_HZ == 1000, "Use a 1 ms tick");
_Static_assert(configUSE_PREEMPTION == CONFIG_ESTEIRA_PREEMPTION, "Kernel override missing");
#define TP_OBJ TOUCH_PAD_NUM7
#define TP_HMI TOUCH_PAD_NUM8
#define TP_ESTOP TOUCH_PAD_NUM9
#define Q_LEN 32
static QueueHandle_t q_ctrl, q_sort, q_safe, q_hmi;
static SemaphoreHandle_t belt_mutex;
static portMUX_TYPE latch_mux = portMUX_INITIALIZER_UNLOCKED;
static bool stopped;
static struct { float rpm, pos, pwm; uint32_t production; } belt;

typedef struct { int64_t evt; uint32_t source; } event_t;
/* Fixed instruction count, not a wall-time wait: frequency changes CPU cost.
 * These are approximate loads; measure actual C on the board. */
static void workload(unsigned units)
{
    volatile uint32_t value = 1;
    uint32_t n = units * 240U * CONFIG_ESTEIRA_WORK_SCALE / 100U;
    for (uint32_t i = 0; i < n; ++i) value = value * 1664525U + 1013904223U;
    (void)value;
}
bool m2_activate(bool safety, uint32_t source, int64_t evt)
{
    event_t e = {evt, source};
    bool ok = xQueueSend(safety ? q_safe : q_sort, &e, 0) == pdTRUE;
    if (!ok) m2_drop(safety ? M2_SAFE : M2_SORT);
    return ok;
}
static void IRAM_ATTR touch_isr(void *arg)
{
    (void)arg;
    uint32_t status = touch_pad_get_status();
    touch_pad_clear_status();
    BaseType_t wake = pdFALSE;
    event_t e = {esp_timer_get_time(), M2_TOUCH};
    if (status & (1UL << TP_OBJ)) {
        if (xQueueSendFromISR(q_sort, &e, &wake) != pdTRUE) m2_drop_isr(M2_SORT);
    }
    if (status & (1UL << TP_HMI)) {
        if (xQueueSendFromISR(q_hmi, &e, &wake) != pdTRUE) m2_drop_isr(M2_HMI);
    }
    if (status & (1UL << TP_ESTOP)) {
        portENTER_CRITICAL_ISR(&latch_mux);
        stopped = true;
        portEXIT_CRITICAL_ISR(&latch_mux);
        if (xQueueSendFromISR(q_safe, &e, &wake) != pdTRUE) m2_drop_isr(M2_SAFE);
    }
    /* In cooperative mode the touch ISR must not force a context switch. */
#if configUSE_PREEMPTION
    if (wake) portYIELD_FROM_ISR();
#endif
}
static void touch_init(void)
{
    ESP_ERROR_CHECK(touch_pad_init());
    ESP_ERROR_CHECK(touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER));
    ESP_ERROR_CHECK(touch_pad_set_trigger_mode(TOUCH_TRIGGER_BELOW));
    const touch_pad_t pads[] = {TP_OBJ, TP_HMI, TP_ESTOP};
    for (unsigned i = 0; i < 3; ++i) ESP_ERROR_CHECK(touch_pad_config(pads[i], 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    for (unsigned i = 0; i < 3; ++i) {
        uint16_t baseline;
        ESP_ERROR_CHECK(touch_pad_read(pads[i], &baseline));
        ESP_ERROR_CHECK(touch_pad_set_thresh(pads[i], baseline * 80U / 100U));
    }
    ESP_ERROR_CHECK(touch_pad_isr_register(touch_isr, NULL));
    ESP_ERROR_CHECK(touch_pad_intr_enable());
}
static void task_enc(void *arg)
{
    int64_t release;
    for (;;) {
        m2_wait_release(M2_ENC, &release);
        int64_t start = esp_timer_get_time(), lock = start;
        xSemaphoreTake(belt_mutex, portMAX_DELAY);
        int64_t blocked = esp_timer_get_time() - lock;
        belt.rpm += 0.05f * (belt.pwm * 2.0f - belt.rpm);
        belt.pos += belt.rpm / 60.0f * 0.005f * 100.0f;
        xSemaphoreGive(belt_mutex);
        workload(70);
        int64_t end = esp_timer_get_time();
        m2_record(M2_ENC, M2_PERIODIC, release, start, end, blocked);
        event_t e = {end, M2_CHAIN};
        if (xQueueSend(q_ctrl, &e, 0) != pdTRUE) m2_drop(M2_CTRL);
    }
}
static void task_ctrl(void *arg)
{
    float integral = 0;
    event_t e, hmi;
    for (;;) {
        xQueueReceive(q_ctrl, &e, portMAX_DELAY);
        int64_t start = esp_timer_get_time(), lock = start;
        xSemaphoreTake(belt_mutex, portMAX_DELAY);
        int64_t blocked = esp_timer_get_time() - lock;
        float err = 120.0f - belt.rpm;
        integral += err * 0.005f;
        float u = 0.4f * err + 0.1f * integral;
        if (u > 100) u = 100;
        if (u < 0) u = 0;
        portENTER_CRITICAL(&latch_mux);
        belt.pwm = stopped ? 0 : u;
        if (stopped) integral = 0;
        portEXIT_CRITICAL(&latch_mux);
        xSemaphoreGive(belt_mutex);
        workload(120);
        m2_record(M2_CTRL, e.source, e.evt, start, esp_timer_get_time(), blocked);
        if (xQueueReceive(q_hmi, &hmi, 0) == pdTRUE) {
            int64_t hs = esp_timer_get_time();
            workload(40);
            m2_record(M2_HMI, hmi.source, hmi.evt, hs, esp_timer_get_time(), 0);
        }
#if !configUSE_PREEMPTION
        taskYIELD();
#endif
    }
}
static void task_sort(void *arg)
{
    event_t e;
    for (;;) {
        xQueueReceive(q_sort, &e, portMAX_DELAY);
        int64_t start = esp_timer_get_time();
        workload(70);
        int64_t lock = esp_timer_get_time();
        xSemaphoreTake(belt_mutex, portMAX_DELAY);
        int64_t blocked = esp_timer_get_time() - lock;
        belt.production++;
        xSemaphoreGive(belt_mutex);
        m2_record(M2_SORT, e.source, e.evt, start, esp_timer_get_time(), blocked);
#if !configUSE_PREEMPTION
        taskYIELD();
#endif
    }
}
static void task_safe(void *arg)
{
    event_t e;
    for (;;) {
        xQueueReceive(q_safe, &e, portMAX_DELAY);
        int64_t start = esp_timer_get_time(), lock = start;
        xSemaphoreTake(belt_mutex, portMAX_DELAY);
        int64_t blocked = esp_timer_get_time() - lock;
        portENTER_CRITICAL(&latch_mux);
        stopped = true;
        belt.pwm = 0;
        portEXIT_CRITICAL(&latch_mux);
        xSemaphoreGive(belt_mutex);
#if CONFIG_ESTEIRA_LED_GPIO >= 0
        gpio_set_level(CONFIG_ESTEIRA_LED_GPIO, 1);
#endif
        workload(90);
        m2_record(M2_SAFE, e.source, e.evt, start, esp_timer_get_time(), blocked);
#if !configUSE_PREEMPTION
        taskYIELD();
#endif
    }
}
static void create(TaskFunction_t fn, const char *name, unsigned stack, unsigned priority)
{
    configASSERT(xTaskCreatePinnedToCore(fn, name, stack, NULL, priority, NULL, 0) == pdPASS);
}
void app_main(void)
{
    printf("M2 policy=%s preemption=%d slicing=%d MHz=%d cores=%d protocol=%s\n",
           CONFIG_ESTEIRA_DM ? "DM" : "CUSTOM", configUSE_PREEMPTION,
           configUSE_TIME_SLICING, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
           CONFIG_FREERTOS_NUMBER_OF_CORES,
           CONFIG_ESTEIRA_M1_BASELINE ? "UART" : CONFIG_ESTEIRA_TCP ? "TCP" : "UDP");
    belt_mutex = xSemaphoreCreateMutex();
    q_ctrl = xQueueCreate(Q_LEN, sizeof(event_t));
    q_sort = xQueueCreate(Q_LEN, sizeof(event_t));
    q_safe = xQueueCreate(Q_LEN, sizeof(event_t));
    q_hmi = xQueueCreate(Q_LEN, sizeof(event_t));
    configASSERT(belt_mutex && q_ctrl && q_sort && q_safe && q_hmi);
    m2_init();
#if CONFIG_ESTEIRA_LED_GPIO >= 0
    gpio_reset_pin(CONFIG_ESTEIRA_LED_GPIO);
    gpio_set_direction(CONFIG_ESTEIRA_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_ESTEIRA_LED_GPIO, 0);
#endif
    create(task_safe, "SAFETY_TASK", 3072, CONFIG_ESTEIRA_DM ? 6 : 7);
    create(task_sort, "SORT_ACT", 3072, CONFIG_ESTEIRA_DM ? 4 : 6);
    create(task_ctrl, "SPD_CTRL", 3072, CONFIG_ESTEIRA_DM ? 5 : 4);
    create(task_enc, "ENC_SENSE", 3072, CONFIG_ESTEIRA_DM ? 7 : 5);
    create(m2_monitor_task, "MONITOR_TASK", 8192, 3);
    if (!CONFIG_ESTEIRA_M1_BASELINE) create(m2_time_task, "TIME_TASK", 4096, 2);
    m2_start_periodic();
    touch_init();
}
