CFLAGS = -Wno-register -O3 -mtune=intel -DNDEBUG

libfake:
	$(CC) -c $(CFLAGS) -fPIC fake_work.c
	$(CC) -shared -o libfake.so fake_work.o

clean:
	rm -rf libfake.so fake_work.h.gch fake_work.o mb

# added by ZL
CFLAGS_CP = -Wno-register -O3 -DNDEBUG
LLVM_CC := clang-12
LLVM_CXX := clang++-12
LLVM_OPT := opt-12
LLVM_AR := llvm-ar-12
TQ_ROOT = ..
CP_LIB_HOME = $(TQ_ROOT)/CheapPreemptions
CP_PASS = $(CP_LIB_HOME)/lib/CheapPreemption.so
LLVM_OPT_FLAGS = -strip-debug -postdomtree -mem2reg -indvars -loop-simplify -branch-prob -scalar-evolution
ARFLAGS = -rs

CP_FLAGS = -load $(CP_PASS) -cheap_preempt
CMT_INTV ?= 800
EXT_COST ?= 1
MAX_E2E  ?= 1000
FUNC_THRE ?= 100000000
CP_FLAGS += -commit-intv=$(CMT_INTV) -ext-lib-cost=$(EXT_COST) -max-e2e-length=$(MAX_E2E) -func-call-threshold=$(FUNC_THRE) -will-update-last-cycle-ts

libfake_cp:
	$(LLVM_CXX) $(CFLAGS_CP) -S -emit-llvm -o fake_work.ll fake_work.cpp -fPIC
	$(LLVM_OPT) $(LLVM_OPT_FLAGS) -S < fake_work.ll > fake_work_simplified.ll  
	$(LLVM_OPT) $(CP_FLAGS) -S < fake_work_simplified.ll > fake_work_cp.ll
	$(LLVM_CXX) -c fake_work_cp.ll -o fake_work_cp.o $(CFLAGS_CP) -fPIC -flto
	#$(CC) -shared -o libfake_cp.so fake_work_cp.o
	$(LLVM_AR) $(ARFLAGS) libfake_cp.a fake_work_cp.o
	$(LLVM_CXX) -c fake_work_simplified.ll -o fake_work_cp_uinst.o $(CFLAGS_CP) -fPIC -flto
	$(LLVM_AR) $(ARFLAGS) libfake_cp_uinst.a fake_work_cp_uinst.o
