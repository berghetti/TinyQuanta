CC = gcc

ifeq ($(DEBUG),y)
CFLAGS += -D__DEBUG__ -O0 -g -ggdb
else
CFLAGS += -O3 -g -DQUANTUM_CYCLE=3000 #-DTIME_STAGE #-DNDEBUG
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
CFLAGS += -I /home/zhihong/profile_rocksdb/rocksdb/include
ROCKSDB_LDFLAGS  = -lrt -pthread -lm -lnuma -ldl -lconfig -lgflags -lsnappy -lz -llz4 -ljemalloc  -no-pie
ROCKSDB_LIB = /home/zhihong/profile_rocksdb/rocksdb/test_llvm/librocksdb_cp.a

# for CP
CI_LIB_HOME = /home/zhihong/CompilerInterrupts
CFLAGS += -I$(CI_LIB_HOME)/src
CFLAGS += -Wl,-rpath=$(CI_LIB_HOME)/lib
CP_LDFLAGS += -L$(CI_LIB_HOME)/lib -lci
CP_LDFLAGS += -Wl,--wrap=pthread_mutex_lock

#OPT = -O2 -fno-omit-frame-pointer -momit-leaf-frame-pointer

all: tq_server tq_server_loop_yield tq_server_empty tq_server_las

tq_server: tq_server.cpp Makefile $(PC_FILE)
	$(CXX) $< $(ROCKSDB_LIB) -o $@ $(CFLAGS) $(LDFLAGS) $(LDFLAGS_SHARED) $(ROCKSDB_LDFLAGS) $(CP_LDFLAGS) $(BOOST_LDFLAGS)

tq_server_loop_yield: tq_server.cpp Makefile $(PC_FILE)
	$(CXX) $< $(ROCKSDB_LIB) -o $@ $(CFLAGS) -DLOOP_YIELD $(LDFLAGS) $(LDFLAGS_SHARED) $(ROCKSDB_LDFLAGS) $(CP_LDFLAGS) $(BOOST_LDFLAGS)

tq_server_empty: tq_server.cpp Makefile $(PC_FILE)
	$(CXX) $< $(ROCKSDB_LIB) -o $@ $(CFLAGS) -DUSE_EMPTY_HANDLER $(LDFLAGS) $(LDFLAGS_SHARED) $(ROCKSDB_LDFLAGS) $(CP_LDFLAGS) $(BOOST_LDFLAGS)

tq_server_las: tq_server.cpp Makefile $(PC_FILE)
	$(CXX) $< $(ROCKSDB_LIB) -o $@ $(CFLAGS) -DLAS $(LDFLAGS) $(LDFLAGS_SHARED) $(ROCKSDB_LDFLAGS) $(CP_LDFLAGS) $(BOOST_LDFLAGS)

tq_client: tq_client.cpp Makefile $(PC_FILE)
	$(CXX) $< $@ $(CFLAGS) $(LDFLAGS) $(LDFLAGS_SHARED) $(CP_LDFLAGS)

create_db: create_db.c
	$(CXX) $< $(ROCKSDB_LIB) -o $@ $(CFLAGS) $(LDFLAGS) $(LDFLAGS_SHARED) $(ROCKSDB_LDFLAGS) $(CP_LDFLAGS)

clean:
	rm -f tq_server tq_server_empty tq_server_las tq_client create_db
