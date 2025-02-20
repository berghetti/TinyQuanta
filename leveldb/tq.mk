# Leveldb build files
DB_FOLDER := builder c dbformat db_impl db_iter filename log_reader log_writer memtable repair table_cache version_edit version_set write_batch
TABLE_FOLDER := block_builder block filter_block format iterator merger table_builder table two_level_iterator
UTIL_FOLDER := arena bloom cache coding comparator crc32c env env_posix filter_policy hash histogram logging options status
BUILD_DIR := tq_build

LLVM_VERSION=12

OPT = opt-$(LLVM_VERSION)
LLVM_AR := llvm-ar-$(LLVM_VERSION)
CXX := clang++-$(LLVM_VERSION)

CXX_FLAGS := -emit-llvm -g -S -O3 -I. -I./include -pthread -DOS_LINUX -DLEVELDB_PLATFORM_POSIX -DSNAPPY -DNDEBUG -c -fPIC

TQ_MAIN=$(ROOT_DIR)..

CP_PASS=$(TQ_MAIN)/CheapPreemptions/lib/CheapPreemption.so

#OPT_CONFIG = -postdomtree -mem2reg -indvars -loop-simplify -branch-prob -scalar-evolution
OPT_FLAGS = -strip-debug -postdomtree -mem2reg -indvars -loop-simplify -branch-prob -scalar-evolution
ARFLAGS = -rs

CFLAGS_CP = -Wno-register -O3 -DNDEBUG

CP_FLAGS = -load $(CP_PASS) -cheap_preempt
CMT_INTV ?= 800
EXT_COST ?= 1
MAX_E2E  ?= 1000
#FUNC_THRE ?= 100000000
# taken from ../Makefile
FUNC_THRE ?= 60
CP_FLAGS += -commit-intv=$(CMT_INTV) -ext-lib-cost=$(EXT_COST) -max-e2e-length=$(MAX_E2E) -func-call-threshold=$(FUNC_THRE) -will-update-last-cycle-ts

all: clean build_db build_util build_table
	$(MAKE) -f tq.mk tq_pass

tq_pass:
	llvm-link-$(LLVM_VERSION) -o libleveldb.bc $(BUILD_DIR)/*.bc
	llvm-dis-$(LLVM_VERSION) libleveldb.bc -o leveldb.ll

	$(OPT) $(OPT_FLAGS) -S < leveldb.ll > leveldb.opt.ll
	$(OPT) $(CP_FLAGS) -S < leveldb.opt.ll > leveldb_cp.ll
	$(CXX) -c leveldb_cp.ll -o leveldb.o $(CFLAGS_CP) -fPIC -flto
	$(LLVM_AR) $(ARFLAGS) libleveldb_tq.a leveldb.o

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
	$(RM) $(BUILD_DIR)/*.o $(BUILD_DIR)/*.bc  $(BUILD_DIR)/*.o *.ll *.opt.ll *.bc libleveldb_tq.a
