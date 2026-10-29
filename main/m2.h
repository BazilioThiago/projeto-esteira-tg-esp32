#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

// M2 isolada para manter as quatro tasks da M1 no arquivo original.
void m2_init(void);
void m2_start(void);
void m2_log(const char *task, int64_t evt, int64_t start, int64_t end,
            uint32_t deadline, const char *origem);
void m2_command(const char *cmd, int64_t evt);
void m2_input_drop(void);
// ESP-IDF 6.1: alarme absoluto, sem misturar fase do tick e esp_timer.
typedef struct {
    esp_timer_handle_t timer;
    TaskHandle_t owner;
    int64_t next_us;
    uint32_t period_us;
} m2_periodic_t;
void m2_periodic_init(m2_periodic_t *p, uint32_t period_us);
int64_t m2_periodic_wait(m2_periodic_t *p);
