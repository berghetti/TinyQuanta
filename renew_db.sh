#!/bin/bash

# Set RocksDB
sudo mkdir -p /tmpfs
mountpoint -q /tmpfs || sudo mount -t tmpfs -o size=8G,mode=1777 tmpfs /tmpfs
mkdir -p /tmpfs/experiments/

sudo rm -rf /tmpfs/experiments/my_db/
sudo ./create_db
