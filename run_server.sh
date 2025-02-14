#!/bin/bash

#sudo ./tq_server -l 28-55 --socket-mem=0,1024 -- 192.168.1.3
sudo ./tq_server -n 2 -l 2,4,6,8,10,12,14,16,18,20,22,24,26,28,30 -a 18:00.1 -- 192.168.10.50
#12.12.12.12
#sudo rm -rf /tmpfs/experiments/my_db/
