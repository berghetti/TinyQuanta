CC = gcc
LLVM_CXX = clang++-12

ifeq ($(DEBUG),y)
CFLAGS += -D__DEBUG__ -O0 -g -ggdb
else
QUANTUM_CYCLE ?= 3000
NUM_WORKER_COROS ?= 8
CFLAGS += -O3 -g -DQUANTUM_CYCLE=${QUANTUM_CYCLE} -DNUM_WORKER_COROS=${NUM_WORKER_COROS} -DBASE_CPU=28 -DNEW_DISPATCHER -DSYNTHETIC -DNDEBUG -DSERVER_LAT #-DQUEUE_SIZE #-DSERVER_LAT #-DRECORD_NUM_PRE #-DTIME_STAGE #-DNDEBUG
endif

PKGCONF ?= pkg-config

# overwrite the dpdk installed in /opt/mellanox/dpdk/ 
PKG_CONFIG_PATH = /usr/local/lib/x86_64-linux-gnu/pkgconfig 

PC_FILE := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKGCONF) --cflags libdpdk)

LDFLAGS_SHARED = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKGCONF) --libs libdpdk) -lrte_net_mlx5 -lrte_bus_pci -lrte_bus_vdev -lpthread -lm -lstdc++

CFLAGS += -DALLOW_EXPERIMENTAL_API -lm -lstdc++

# for boost 
CFLAGS += -I /usr/include 
BOOST_LDFLAGS += -lboost_coroutine -lboost_context

# for RocksDB
CFLAGS += -I /home/zhihong/RocksDB-TQ/include
ROCKSDB_LDFLAGS  = -lrt -pthread -lm -lnuma -ldl -lconfig -lgflags -lsnappy -lz -llz4 -ljemalloc  -no-pie -lbz2
ROCKSDB_LIB = /home/zhihong/RocksDB-TQ/test_llvm/librocksdb_cp.a
ROCKSDB_LIB_UNINST = /home/zhihong/RocksDB-TQ/test_llvm/librocksdb.a

# for CP
CP_LIB_HOME = /home/zhihong/CheapPreemptions
CFLAGS += -I$(CP_LIB_HOME)/src
CFLAGS += -Wl,-rpath=$(CP_LIB_HOME)/lib
CP_LDFLAGS += -L$(CP_LIB_HOME)/lib -lci
CP_LDFLAGS += -Wl,--wrap=pthread_mutex_lock

FAKE_WORK_LIB_HOME = /home/zhihong/fake_work_cp
FAKE_WORK_LIB = $(FAKE_WORK_LIB_HOME)/libfake_cp.a
CFLAGS += -I$(FAKE_WORK_LIB_HOME)

#OPT = -O2 -fno-omit-frame-pointer -momit-leaf-frame-pointer

all: tq_server tq_server_las tq_client create_db profile_rocksdb_get profile_rocksdb_scan

tq_server: tq_server.cpp Makefile $(PC_FILE)
	$(LLVM_CXX) $< -flto $(ROCKSDB_LIB) $(FAKE_WORK_LIB) -o $@ $(CFLAGS) $(LDFLAGS) $(LDFLAGS_SHARED) $(ROCKSDB_LDFLAGS) $(CP_LDFLAGS) $(BOOST_LDFLAGS)

tq_server_las: tq_server.cpp Makefile $(PC_FILE)
	$(LLVM_CXX) $< -flto $(ROCKSDB_LIB) $(FAKE_WORK_LIB) -o $@ $(CFLAGS) -DLAS $(LDFLAGS) $(LDFLAGS_SHARED) $(ROCKSDB_LDFLAGS) $(CP_LDFLAGS) $(BOOST_LDFLAGS)

tq_client: tq_client.cpp Makefile $(PC_FILE)
	$(LLVM_CXX) $< -o $@ $(CFLAGS) $(LDFLAGS) $(LDFLAGS_SHARED) $(CP_LDFLAGS)

create_db: create_db.c
	$(LLVM_CXX) $< -flto $(ROCKSDB_LIB) -o $@ $(CFLAGS) $(LDFLAGS) $(LDFLAGS_SHARED) $(ROCKSDB_LDFLAGS) $(CP_LDFLAGS)

profile_rocksdb_get: profile_rocksdb_get.c
	$(LLVM_CXX) $< -flto $(ROCKSDB_LIB) -o $@ $(CFLAGS) $(LDFLAGS) $(LDFLAGS_SHARED) $(ROCKSDB_LDFLAGS) $(CP_LDFLAGS)

profile_rocksdb_scan: profile_rocksdb_scan.c
	$(LLVM_CXX) $< -flto $(ROCKSDB_LIB) -o $@ $(CFLAGS) $(LDFLAGS) $(LDFLAGS_SHARED) $(ROCKSDB_LDFLAGS) $(CP_LDFLAGS)

test_fake_work_cp: test_fake_work_cp.cpp
	$(LLVM_CXX) $< -flto $(FAKE_WORK_LIB) -o $@ $(CFLAGS) $(CP_LDFLAGS)

clean:
	rm -f tq_server tq_server_empty tq_server_las tq_client create_db profile_rocksdb_get profile_rocksdb_scan
