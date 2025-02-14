#include "fake_work_cp.h"
#include <stdint.h>

/* this is previus calibrated using 300 nops loops with original compiler code (whitout passes)*/
#define ITERATIONS_500NS 17
#define ITERATIONS_1US 34
#define ITERATIONS_2500NS 86
#define ITERATIONS_10US 341
#define ITERATIONS_100US 3412
#define ITERATIONS_500US 16997

#define X10( x ) x x x x x x x x x x
#define X100( x ) \
  X10( x ) X10( x ) X10( x ) X10( x ) X10( x ) \
  X10( x ) X10( x ) X10( x ) X10( x ) X10( x )
#define X300( x ) X100(x) X100(x) X100(x)


// clang-format off
#define X10( x ) x x x x x x x x x x
#define X100( x ) \
  X10( x ) X10( x ) X10( x ) X10( x ) X10( x ) \
  X10( x ) X10( x ) X10( x ) X10( x ) X10( x )
#define X300( x ) X100(x) X100(x) X100(x)
// clang-format on

__attribute__ ( ( noinline ) ) static void
work ( unsigned long i )
{
  while ( i-- )
    {
      X300 ( asm volatile( "nop" ); )
    }
}

void
fake_work_ns ( unsigned target_ns )
{
  switch ( target_ns )
    {
      case 500:
        work ( ITERATIONS_500NS );
        break;
      case 1000:
        work ( ITERATIONS_1US );
        break;
      case 2500:
        work ( ITERATIONS_2500NS );
        break;
      case 10000:
        work ( ITERATIONS_10US );
        break;
      case 100000:
        work ( ITERATIONS_100US );
        break;
      case 500000:
        work ( ITERATIONS_500US );
        break;
    }
}


//template< unsigned N > struct Nops{
//  static void generate() __attribute__((always_inline)){
//    asm volatile ("nop");
//    Nops< N - 1 >::generate();
//  }
//};
//template<> struct Nops<0>{ static inline void generate(){} };
//
//void fake_work_noop(unsigned int nloops) {
//    for (unsigned int i = 0; i++ < nloops;) {
//	//asm volatile("nop");
//	//Nops<300>::generate();
//    }
//}
//
//unsigned int fake_work_rand_gen(unsigned int g_seed, unsigned int nloops) {
//    for (unsigned int i = 0; i++ < nloops;) {
//    	g_seed = ((214013 * g_seed+2531011) >> 16) & 0x7FFF;
//    }
//    return g_seed;
//}
