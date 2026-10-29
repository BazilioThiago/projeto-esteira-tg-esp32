#pragma once
/* Applied by the wrapper AFTER the official FreeRTOSConfig.h is included.
 * Do not force-include FreeRTOS headers in unrelated IDF/libc sources.
 * IDF 6.1 fixes these options to 1; changing only main would be ineffective. */
#undef configUSE_PREEMPTION
#define configUSE_PREEMPTION CONFIG_ESTEIRA_PREEMPTION
#undef configUSE_TIME_SLICING
#define configUSE_TIME_SLICING CONFIG_ESTEIRA_SLICING
