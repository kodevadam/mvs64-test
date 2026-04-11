/* FAME 68K adapter API for MVS64 */
#ifndef FAME_ADAPTER_H
#define FAME_ADAPTER_H

#include <stdint.h>

void     fame_adapter_init(void);
void     fame_adapter_reset(void);
int      fame_adapter_execute(int cycles);
void     fame_adapter_set_virq(int level, int active);
uint32_t fame_adapter_get_pc(void);
void     fame_adapter_end_timeslice(void);

#endif
