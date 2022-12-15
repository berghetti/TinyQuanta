#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include "rocksdb/c.h"
#include "ci_lib.h"

#define BUCKET_SIZE 3000
#ifndef QUANTUM_CYCLE
#define QUANTUM_CYCLE 3000
#endif

uint64_t time_elapsed;
__thread uint64_t sample_count = 0;
__thread uint64_t total_tsc = 0;
__thread long total_ic = 0;
__thread uint64_t *tsc_buckets = NULL;
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

void scan_db(rocksdb_t *db) {
	const char *retr_key;	
	size_t klen;

	rocksdb_readoptions_t *readoptions = rocksdb_readoptions_create();
	
	uint64_t start = rdtsc();
	rocksdb_iterator_t *iter = rocksdb_create_iterator(db, readoptions);
	rocksdb_iter_seek_to_first(iter);
	while (rocksdb_iter_valid(iter)) {
		retr_key = rocksdb_iter_key(iter, &klen);
    		rocksdb_iter_next(iter);
  	}
  	rocksdb_iter_destroy(iter);
	//rocksdb_scan(db, readoptions);
	uint64_t end = rdtsc();
	uint64_t time_elapsed = end - start;

	if(sample_count > 0)
        {
                printf("Average CI interval %ld IC, %ld cycles\n", total_ic/sample_count, total_tsc/sample_count);
        	printf("Outlier percentage %f%%\n", (float)(100 * outlier_count)/(float)(sample_count + outlier_count));
	}
	printf("Average SCAN time %ld us\n", time_elapsed/(uint64_t)(1000 * 2.1));
}	

void pin_to_cpu(int core){
        int ret;
        cpu_set_t cpuset;
        pthread_t thread;

        thread = pthread_self();
        CPU_ZERO(&cpuset);
        CPU_SET(core, &cpuset);
        ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
        if (ret != 0)
            printf("Cannot pin thread\n");
}

void interrupt_handler_tsc_hist(long ic) {
        total_ic += ic;
        static __thread uint64_t last_tsc = 0;
        uint64_t curr_tsc = rdtsc();
        if(last_tsc > 0)
        {
                uint64_t tsc = curr_tsc - last_tsc;
                //tsc = tsc - 7500;
                if(tsc > 0 && tsc < BUCKET_SIZE)
                {
                        tsc_buckets[tsc - 1] += 1;
                } else {
                        tsc_buckets[BUCKET_SIZE - 1] += 1;
                        outlier_count += 1;
                }
                sample_count +=1;
        }
        last_tsc = rdtsc();
}

void simplest_handler(long ic) {
        if(ic < 20000) {
                total_ic += ic;
                sample_count += 1;
        } else {
		outlier_count += 1;
	}
        //LastCycleTS = rdtsc();
}

int main(int argc, char **argv) {

        pin_to_cpu(30);
        //register_ci(1000/*doesn't matter*/, QUANTUM_CYCLE, simplest_handler);

        if(!tsc_buckets)
                tsc_buckets = (uint64_t*)malloc(BUCKET_SIZE * sizeof(uint64_t));
        rocksdb_t *db;
        // Initialize RocksDB
        rocksdb_options_t *options = rocksdb_options_create();
        rocksdb_options_set_allow_mmap_reads(options, 1);
        rocksdb_options_set_allow_mmap_writes(options, 1);
        rocksdb_slicetransform_t * prefix_extractor = rocksdb_slicetransform_create_capped_prefix(8);
        rocksdb_options_set_prefix_extractor(options, prefix_extractor);
        rocksdb_options_set_plain_table_factory(options, 0, 10, 0.75, 3);
        // Optimize RocksDB. This is the easiest way to
        // get RocksDB to perform well
        rocksdb_options_increase_parallelism(options, 0);
        rocksdb_options_optimize_level_style_compaction(options, 0);
        // create the DB if it's not already present
        rocksdb_options_set_create_if_missing(options, 1);
	rocksdb_options_set_disable_auto_compactions(options, 1);
        
	// open DB
        char *err = NULL;
        char DBPath[] = "/tmpfs/experiments/my_db";
        db = rocksdb_open(options, DBPath, &err);
	if(err)
		printf("%s\n", err);
	assert(!err);
	for(int i = 0; i < 5; i++) {
		total_ic = 0;
                sample_count = 0;
                outlier_count = 0;
                time_elapsed = 0;
		scan_db(db);
	}
        rocksdb_options_destroy(options);
        rocksdb_close(db);
        if(tsc_buckets)
                free(tsc_buckets);
}

