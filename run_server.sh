#!/bin/bash

sudo ./tq_server -l 28-55 --socket-mem=0,1024 -- 192.168.1.3
#sudo perf stat -e L1-dcache-load-misses,L1-icache-load-misses,l2_rqsts.demand_data_rd_miss,l2_rqsts.code_rd_miss ./tq_server -l 28-55 --socket-mem=0,1024 -- 192.168.1.3
#12.12.12.12
#sudo rm -rf /tmpfs/experiments/my_db/
