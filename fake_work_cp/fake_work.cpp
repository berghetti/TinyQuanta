#include "fake_work_cp.h"
#include <stdint.h>

template< unsigned N > struct Nops{
  static void generate() __attribute__((always_inline)){
    asm volatile ("nop");
    Nops< N - 1 >::generate();
  }
};
template<> struct Nops<0>{ static inline void generate(){} };

void fake_work_noop(unsigned int nloops) {
    for (unsigned int i = 0; i++ < nloops;) {
	//asm volatile("nop");
	Nops<100>::generate();
    }
}

unsigned int fake_work_rand_gen(unsigned int g_seed, unsigned int nloops) {
    for (unsigned int i = 0; i++ < nloops;) {
    	g_seed = ((214013 * g_seed+2531011) >> 16) & 0x7FFF;
    }
    return g_seed;
}
