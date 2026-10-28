#pragma once
/* Forced into every application/IDF translation unit, including the kernel.
 * IDF 6.1 fixes these options to 1; changing only main would be ineffective. */
#include "freertos/FreeRTOSConfig.h"
#undef configUSE_PREEMPTION
#define configUSE_PREEMPTION CONFIG_ESTEIRA_PREEMPTION
#undef configUSE_TIME_SLICING
#define configUSE_TIME_SLICING CONFIG_ESTEIRA_SLICING
