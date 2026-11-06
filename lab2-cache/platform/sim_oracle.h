/* sim_oracle.h - ground truth from the simulator, for graders only (never for attack code). */
#ifndef SIM_ORACLE_H
#define SIM_ORACLE_H
#include <stdint.h>
/* One probe latency drawn from the model's distribution for a load served by `level` (1..4). */
uint64_t sim_oracle_sample(int level);
/* Level (1..4) at which the calling party would find p right now, without touching anything. */
int sim_oracle_where(const void *p);
#endif
