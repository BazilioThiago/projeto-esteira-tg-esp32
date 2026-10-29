#pragma once
#include <stdint.h>

// M2 isolada para manter as quatro tasks da M1 no arquivo original.
void m2_init(void);
void m2_start(void);
void m2_log(const char *task, int64_t evt, int64_t start, int64_t end,
            uint32_t deadline, const char *origem);
void m2_command(const char *cmd, int64_t evt);
void m2_input_drop(void);
