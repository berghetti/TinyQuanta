# Leveldb build files
DB_FOLDER := builder c dbformat db_impl db_iter filename log_reader log_writer memtable repair table_cache version_edit version_set write_batch
TABLE_FOLDER := block_builder block filter_block format iterator merger table_builder table two_level_iterator
UTIL_FOLDER := arena bloom cache coding comparator crc32c env env_posix filter_policy hash histogram logging options status
BUILD_DIR := concord_build

LLVM_VERSION=9

OPT = opt-$(LLVM_VERSION)

CXX := clang++-$(LLVM_VERSION)
CXX_FLAGS := -emit-llvm -g -S -O3 -I. -I./include -pthread -DOS_LINUX -DLEVELDB_PLATFORM_POSIX -DSNAPPY -DNDEBUG -c -fPIC

ROOT_DIR:=$(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))

CONCORD_MAIN=$(ROOT_DIR)/../../deps/concord/src/

CONCORD_PASS=$(CONCORD_MAIN)/cache-line-pass/build/src/libConcordPass.so
RDTSC_PASS=$(CONCORD_MAIN)/rdtsc-pass/build/src/libConcordPass.so
CONCORD_DIR=$(CONCORD_MAIN)/lib/
INC_DIR=-I$(CONCORD_DIR)

PASS_CONFIG = -load $(CONCORD_PASS) -yield
OPT_CONFIG = -postdomtree -mem2reg -indvars -loop-simplify -branch-prob -scalar-evolution

all: clean build_db build_util build_table
	$(MAKE) -f concord.mk concord_pass

concord_pass:
	llvm-link-$(LLVM_VERSION) -o libleveldb.bc $(BUILD_DIR)/*.bc
	llvm-dis-$(LLVM_VERSION) libleveldb.bc -o leveldb.ll
	$(OPT) -S $(OPT_CONFIG) < leveldb.ll > leveldb.opt.ll

	$(OPT) -S -load $(CONCORD_PASS) -yield < leveldb.opt.ll > concord_leveldb.opt.ll
	$(OPT) -S -load $(RDTSC_PASS) -yield < leveldb.opt.ll > concord_leveldb.rdtsc.opt.ll

	$(CXX) -c -O3 -fPIC concord_leveldb.opt.ll -o concord_libleveldb.a
	$(CXX) -c -O3 -fPIC concord_leveldb.rdtsc.opt.ll -o concord_libleveldb_rdtsc.a
	$(CXX) -c -O3 -fPIC leveldb.ll -o concord_libleveldb_clear.a

build_db:
	@mkdir -p $(BUILD_DIR)
	for file in $(DB_FOLDER); do \
		$(CXX) $(CXX_FLAGS) db/$$file.cc -o $(BUILD_DIR)/$$file.bc $(INC_DIR); \
	done

build_util:
	for file in $(UTIL_FOLDER); do \
		$(CXX) $(CXX_FLAGS) util/$$file.cc -o $(BUILD_DIR)/$$file.bc $(INC_DIR); \
	done

build_table:
	for file in $(TABLE_FOLDER); do \
		$(CXX) $(CXX_FLAGS) table/$$file.cc -o $(BUILD_DIR)/$$file.bc $(INC_DIR); \
	done

clean:
	$(RM) $(BUILD_DIR)/*.o $(BUILD_DIR)/*.bc  $(BUILD_DIR)/*.o *.ll *.opt.ll *.bc concord_libleveldb.a
