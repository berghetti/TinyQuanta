#!/bin/bash

func_thre=60
quantum_cycle=5000
num_worker_coros=8

echo "Building CheapPreemptions"
pushd ./CheapPreemptions/src > /dev/null 
make -j
popd

echo "Building RocksDB"
pushd ./RocksDB-TQ >/dev/null
make -j clean_cp >/dev/null 2>&1
make -j test_cp FUNC_THRE=$func_thre>/dev/null 2>&1
popd

echo "Buidling Fakework"
pushd ./fake_work_cp > /dev/null
make -j libfake
make -j libfake_cp
popd

echo "Building TQ"
make clean
make -j QUANTUM_CYCLE=$quantum_cycle NUM_WORKER_COROS=$num_worker_coros

echo "Setting up RocksDB"
./renew_db.sh

echo "Running TQ server"
./run_server.sh

