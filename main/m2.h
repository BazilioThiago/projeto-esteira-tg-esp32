#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#ifndef CONFIG_ESTEIRA_M1_BASELINE
#define CONFIG_ESTEIRA_M1_BASELINE 0
#endif
#ifndef CONFIG_ESTEIRA_DM
#define CONFIG_ESTEIRA_DM 0
#endif
#ifndef CONFIG_ESTEIRA_TCP
#define CONFIG_ESTEIRA_TCP 0
#endif
enum { M2_ENC, M2_CTRL, M2_SORT, M2_SAFE, M2_MONITOR, M2_TIME, M2_HMI, M2_TASKS };
enum { M2_PERIODIC, M2_CHAIN, M2_TOUCH, M2_PC };
void m2_init(void);
void m2_start_periodic(void);
void m2_wait_release(unsigned task, int64_t *release);
void m2_monitor_task(void *arg);
void m2_time_task(void *arg);
void m2_record(unsigned task, unsigned source, int64_t evt, int64_t start, int64_t end, int64_t block);
void m2_drop(unsigned task);
void m2_drop_isr(unsigned task);
bool m2_activate(bool safety, uint32_t source, int64_t evt);
