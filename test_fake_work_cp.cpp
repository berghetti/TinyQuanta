#include "fake_work_cp.h"
#include "ci_lib.h"
#include <iostream>
#include <cassert>

__thread uint64_t sample_count = 0;
__thread long total_ic = 0;
__thread uint64_t outlier_count = 0;

//  Windows
#ifdef _WIN32
uint64_t rdtsc(){
    return __rdtsc();
}
//  Linux/GCC
#else
uint64_t rdtsc(){
    unsigned int lo,hi;
    __asm__ __volatile__ ("lfence\n\t" "rdtsc": "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}
#endif

void simplest_handler(long ic) {
        if(ic < 20000) {
                total_ic += ic;
                sample_count += 1;
        } else {
                outlier_count += 1;
		sample_count += 1;
        }
}

int main()
{
	register_ci(1000, 5000, simplest_handler);
	unsigned int gseed = rand() % 10000;
	LastCycleTS = rdtsc();
	uint64_t start_time, end_time;
	start_time = rdtsc();
	gseed = fake_work_rand_gen(gseed, 100000 * 2.1/6);
	end_time = rdtsc();
	std::cout << gseed << std::endl;
	std::cout << "It takes " << float(end_time - start_time)/2.1  << " ns" << std::endl;
	if(sample_count > 0)
        {
                printf("Average CI interval %ld IC with %ld samples\n", total_ic/sample_count, sample_count);
                printf("Outlier percentage %f%%\n", (float)(100 * outlier_count)/(float)(sample_count + outlier_count));
        }
}
