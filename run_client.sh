#!/bin/bash

server_mac=0c:42:a1:0b:19:71
sudo ./tq_client -l 28-31 --socket-mem=128 -- 192.168.10.3 12.12.12.12 ${server_mac}
