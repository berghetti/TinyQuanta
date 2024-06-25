#ifndef fake_h__
#define fake_h__

#include <stdint.h>

void fake_work_noop(unsigned int nloops);
unsigned int fake_work_rand_gen(unsigned int g_seed, unsigned int nloops);

#endif  // fake_h__
