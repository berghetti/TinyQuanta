#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include <fstream>
#include <set>
#include <functional> //for std::hash
#include "unistd.h"


using namespace llvm;


namespace {

typedef SmallVector<const BasicBlock *, 32> BBVector;
typedef SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 32> EdgeVector;
enum InstStatus { UNINST, NORM_INST, LOOP_INST }; // 0: uninstrumented, 1: normal instrumented, 2: loop instrumented 

// backEdges of the current function
EdgeVector backEdges;
// reversely topologically sorted BB
BBVector SortedBBs;
// map of each edge's instrumentation status
std::map<std::pair<const BasicBlock *, const BasicBlock *>, InstStatus> EdgeInstMap; 
// map of basic block's cost
// 0 if BB is instrumented due to external library call
std::map<const BasicBlock *, int> CostMap;
// instrumented call costs
std::map<StringRef, int> LibraryInstructionCosts; 
// number of external calls in the current BB
int NumExternalCalls;
// list of instrumented BBs
std::set<const BasicBlock *> InstrumentedBB; 
int NumExtLibInst;
int NumEdgeInst;
int NumBBInst;
// function calls to instrument because there are loops
std::map<StringRef, SmallVector<StringRef, 32> *> CallsToInstrument;
// functions to instruments
std::set<StringRef> FuncsToInstrument;
// expensive external functions
std::set<std::string> ExpensiveExtLibCalls;
// functions to not instrument
std::set<std::string> IgnoreExtLibCalls;

struct FuncInfo {
  int InstCount;
  int TotalNumProbe;
  int MaxUnistDist;
  SmallVector<int64_t, 32> ProbeSigs;
};
std::map<Function *, FuncInfo *> computedFuncInfo;

// map of each edge's instrumentation status
// maintain all these maps cuz we are not maintaining the LI, SE, or DT during the instrumentation
// so we just cache the results
std::map<std::pair<const BasicBlock *, const BasicBlock *>, BasicBlock *> PreheaderMap; 
std::map<std::pair<const BasicBlock *, const BasicBlock *>, Value *> LoopInitialValueMap; 
std::map<std::pair<const BasicBlock *, const BasicBlock *>, std::pair<Value *, int>> LoopStepValueMap; 
std::map<std::pair<const BasicBlock *, const BasicBlock *>, Value *> LoopFinalValueMap; 
std::map<std::pair<const BasicBlock *, const BasicBlock *>, BasicBlock *> UpdatePreheaderMap;
std::map<std::pair<const BasicBlock *, const BasicBlock *>, Value *> LoopInductionVariableMap; 
std::map<std::pair<const BasicBlock *, const BasicBlock *>, int> LoopDepthMap; 
std::map<std::pair<const BasicBlock *, const BasicBlock *>, SmallVector<BasicBlock*, 32> *> LoopExitBlocks;

// list of functions in call graph order (reversed topolically sorted)
SmallVector<StringRef, 128> CGOrderedFunc;
std::map<StringRef, bool> IsRecursiveFunc;
bool LoopCostMode;
// map of basic block's function call cost (how many function calls it made) -- used for loops
// -1 if BB is terminated because of an fully instrumented function call
std::map<const BasicBlock *, int> FuncCallCostMap;

LLVMContext *LLVMCtx;
DominatorTree *DT;
LoopInfo *LI;
ScalarEvolution *SE;

// fix this later
const BasicBlock* LargeBB;
int64_t ProbeIdx;
int64_t FuncHash;

bool isBackEdge(const BasicBlock *from, const BasicBlock *to) {
	for (auto backEdge : backEdges) {
		if (backEdge.first == from && backEdge.second == to) {
		  return true;
		}
	}
	return false;
}

// Runs a topological sort on the basic blocks of the given function. Uses
// the simple recursive DFS from "Introduction to algorithms", with 3-coloring
// of vertices. The coloring enables detecting cycles in the graph with a simple
// test.
class TopoSorter {
public:
  void runToposort(const Function &F) {
	// Initialize the color map by marking all the vertices white.
	for (Function::const_iterator I = F.begin(), IE = F.end(); I != IE; ++I) {
	  ColorMap[&*I] = TopoSorter::WHITE;
	}

	// The BB graph has a single entry vertex from which the other BBs should
	// be discoverable - the function entry block.
	bool success = recursiveDFSToposort(&F.getEntryBlock());
	if (success) {
	  // Now we have all the BBs inside SortedBBs in reverse topological order.
	  /*for (BBVector::const_reverse_iterator RI = SortedBBs.rbegin(),
											RE = SortedBBs.rend();
		   RI != RE; ++RI) {
		errs() << "  " << (*RI)->getName() << "\n";
	  }*/
	  // errs() << F.getName().str() << " Sorting succeeded\n";
	} else {
	  errs() << F.getName().str() << "  Sorting failed\n";
	}
  }

private:
  enum Color { WHITE, GREY, BLACK };
  // Color marks per vertex (BB).
  typedef DenseMap<const BasicBlock *, Color> BBColorMap;
  // Collects vertices (BBs) in "finish" order. The first finished vertex is
  // first, and so on.
  BBColorMap ColorMap;

  // Helper function to recursively run topological sort from a given BB.
  // Returns true if the sort succeeded and false otherwise; topological sort
  // may fail if, for example, the graph is not a DAG (detected a cycle).
  bool recursiveDFSToposort(const BasicBlock *BB) {
	ColorMap[BB] = TopoSorter::GREY;
	// For demonstration, using the lowest-level APIs here. A BB's successors
	// are determined by looking at its terminator instruction.
	const Instruction *TInst = BB->getTerminator();
	for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
	  BasicBlock *Succ = TInst->getSuccessor(I);
	  // skip if it is a backedge
	  if(isBackEdge(BB, Succ))
		continue;
	  Color SuccColor = ColorMap[Succ];
	  if (SuccColor == TopoSorter::WHITE) {
		if (!recursiveDFSToposort(Succ))
		  return false;
	  } else if (SuccColor == TopoSorter::GREY) {
		// This detects a cycle because grey vertices are all ancestors of the
		// currently explored vertex (in other words, they're "on the stack").
		errs() << "  Detected cycle: edge from " << BB->getName() << " to "
			   << Succ->getName() << "\n";
		return false;
	  }
	}
	// This BB is finished (fully explored), so we can add it to the vector.
	ColorMap[BB] = TopoSorter::BLACK;
	SortedBBs.push_back(BB);
	return true;
  }
};

/*********** Section: Command line configuration parameters ***********/
static cl::opt<int> CommitInterval(
	"commit-intv",
	cl::desc("Probe interval in terms of IR instructions"),
	cl::value_desc("positive integer"), cl::init(100), cl::Optional); 

static cl::opt<int> ExtLibFuncCost(
	"ext-lib-cost",
	cl::desc("Cost of external library calls"),
	cl::value_desc("positive integer"), cl::init(100), cl::Optional);

static cl::opt<int> MemOpsCost(
	"mem-ops-cost",
	cl::desc("Cost of memory operations"),
	cl::value_desc("cost"), cl::init(1),
	cl::Optional);

static cl::opt<int> FMulDivCost(
	"fmul-div-cost",
	cl::desc("Cost of FMul and FDiv"),
	cl::value_desc("cost"), cl::init(5),
	cl::Optional);

static cl::opt<int> MaxE2EUninst(
	"max-e2e-length",
	cl::desc("Maximum e2e uninstrumented length"),
	cl::value_desc("cost"), cl::init(0),
	cl::Optional);

static cl::opt<int> FuncCallThreshold(
	"func-call-threshold",
	cl::desc("Maximum number of function calls in a loop"),
	cl::value_desc("cost"), cl::init(40),
	cl::Optional);

static cl::opt<std::string> InCostFilePath(
	"in-cost-file",
	cl::desc("Cost file from where cost of library functions will be imported"),
	cl::value_desc("filepath"), cl::Optional);

static cl::opt<std::string> InFuncInstFilePath(
	"in-func-inst-file",
	cl::desc("Function instrumentation file will be imported"),
	cl::value_desc("filepath"), cl::Optional);

static cl::opt<std::string> OutCostFilePath(
	"out-cost-file",
	cl::desc("Cost file from where cost of library functions will be exported"),
	cl::value_desc("filepath"), cl::Optional);

static cl::opt<std::string> OutInfoFilePath(
	"out-info-file",
	cl::desc("Information file where information of library functions will be exported"),
	cl::value_desc("filepath"), cl::Optional);

static cl::opt<bool> WillUpdateLastCycleTS(
    "will-update-last-cycle-ts",
    cl::desc(
        "Choose whether to define clock in the pass. true: Yes, false: No"),
    cl::value_desc("true/false"), cl::init(false), cl::Optional);

/*********** Section: CheapPreemption pass ***********/

struct CheapPreemption : public ModulePass {
  static char ID;
  CheapPreemption() : ModulePass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
  	AU.addRequired<CallGraphWrapperPass>();
	AU.addRequired<LoopInfoWrapperPass>();
	// AU.addRequired<PostDominatorTreeWrapperPass>();
	AU.addRequired<DominatorTreeWrapperPass>();
	// AU.addRequired<MemoryDependenceWrapperPass>();
	AU.addRequired<ScalarEvolutionWrapperPass>();
  }

  void generateTopoSort(Function &F) {
	TopoSorter TS;
	TS.runToposort(F);
  }

  bool checkLoopsSimplified(Function &F) {
  	SmallVector<Loop*, 32> AllLoops;
  	getAllLoops(AllLoops);
	for (auto itLI = AllLoops.begin(); itLI != AllLoops.end(); ++itLI) {
		if(!(*itLI)->isLoopSimplifyForm())
			return false;
	}
	return true;
  }

 
  bool checkIfValueDominatesInst(Value *V, Instruction *I) {
    if (isa<Instruction>(V)) {
      Instruction *VInst = dyn_cast<Instruction>(V);
      if (!DT->dominates(VInst, I)) {
        return false;
      }
    }
    return true;
  }

  void getLoopsPreheader(Function &F) {
  	SmallVector<Loop*, 32> AllLoops;
  	getAllLoops(AllLoops);
	for (auto itLI = AllLoops.begin(); itLI != AllLoops.end(); ++itLI) {
		const BasicBlock *HeaderBB = (*itLI)->getHeader();
   		const BasicBlock *LatchBB = (*itLI)->getLoopLatch();
		BasicBlock *preheaderBB = (*itLI)->getLoopPreheader();
		PreheaderMap[std::make_pair(LatchBB, HeaderBB)] = preheaderBB;
		LoopDepthMap[std::make_pair(LatchBB, HeaderBB)] = (*itLI)->getLoopDepth();
		LoopExitBlocks[std::make_pair(LatchBB, HeaderBB)] = new SmallVector<BasicBlock*, 32>();
		(*itLI)->getExitBlocks(*LoopExitBlocks[std::make_pair(LatchBB, HeaderBB)]);
	}
  }

  bool checkIfIgnoreExtLibCall(Function *F) {
  	for (auto f : IgnoreExtLibCalls) {
 	   if (F->getName().compare(f) == 0) {
      	return true;
    	}
  	}
  	return false;
  }

  bool checkIfExpensiveExtLibCall(Function *F) {
  	for (auto f : ExpensiveExtLibCalls) {
 	   if (F->getName().compare(f) == 0) {
      	return true;
    	}
  	}
  	return false;
  }


  void getLoopsBound(Function &F) {
  	SmallVector<Loop*, 32> AllLoops;
  	getAllLoops(AllLoops);
	for (auto itLI = AllLoops.begin(); itLI != AllLoops.end(); ++itLI) {
		auto *indVarPhiInst = (*itLI)->getInductionVariable(*SE);
		if(!indVarPhiInst)
			continue;
		if(!indVarPhiInst->getType()->isIntegerTy()) {
			errs() << "Skip induction variable of type: " << *indVarPhiInst->getType() << "\n";
			continue;
		}
		assert(indVarPhiInst->getType()->isIntegerTy() && "Induction variable is not of integer type!");
  		auto lBounds = (*itLI)->getBounds(*SE);
  		Value *InitialIVValue = &lBounds->getInitialIVValue();
  		Value *StepValue = lBounds->getStepValue();
  		Value * StepInst =  &lBounds->getStepInst();
  		Value *FinalValue = &lBounds->getFinalIVValue();
  		if (!InitialIVValue || !StepValue || !isa<ConstantInt>(StepValue)) {
        	continue;
      	}
      	ConstantInt *StepCI = dyn_cast<ConstantInt>(StepValue);
      	if (StepCI->getBitWidth() > 64) {
      		continue;
      	}
        int StepVal = StepCI->getSExtValue();
        const BasicBlock *HeaderBB = (*itLI)->getHeader();
   		const BasicBlock *LatchBB = (*itLI)->getLoopLatch();
		// BasicBlock *preheaderBB = (*itLI)->getLoopPreheader();
  //       Value * InitValue = indVarPhiInst->getIncomingValueForBlock(preheaderBB);
   		LoopInitialValueMap[std::make_pair(LatchBB, HeaderBB)]  = InitialIVValue;
        LoopStepValueMap[std::make_pair(LatchBB, HeaderBB)]  = std::make_pair(StepInst, StepVal);
        LoopInductionVariableMap[std::make_pair(LatchBB, HeaderBB)]  = indVarPhiInst;
        if(FinalValue)
	        LoopFinalValueMap[std::make_pair(LatchBB, HeaderBB)]  = FinalValue;

	    BasicBlock *PreheaderBB = (*itLI)->getLoopPreheader();
	    assert(checkIfValueDominatesInst(InitialIVValue, PreheaderBB->getTerminator()));
		// assert(checkIfValueDominatesInst(StepValue, PreheaderBB->getTerminator()))
  	}
  }


  void populateEdgeInstMap() {
	for (BBVector::const_reverse_iterator RI = SortedBBs.rbegin(), RE = SortedBBs.rend(); RI != RE; ++RI) {
		const Instruction *TInst = (*RI)->getTerminator();
		for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
			const BasicBlock *Succ = TInst->getSuccessor(I);
			EdgeInstMap[std::make_pair((*RI), Succ)] =  UNINST;
		}
	}
	for (auto backEdge : backEdges)
	{
		auto found = EdgeInstMap.find(backEdge);
		// this must exist
		assert(found != EdgeInstMap.end());
		found->second = LOOP_INST;
		NumEdgeInst++;
	}

  }

  void longestFuncCallDistanceFrom(const BasicBlock *StartBB, std::map<const BasicBlock *, int> &LongestDistanceMap, std::map<const BasicBlock *, const BasicBlock *> *PrevBBMap=nullptr) {
	LongestDistanceMap[StartBB] = FuncCallCostMap[StartBB];
	if(PrevBBMap)
		(*PrevBBMap)[StartBB]  = StartBB; 
	for (BBVector::const_reverse_iterator RI = SortedBBs.rbegin(), RE = SortedBBs.rend(); RI != RE; ++RI) {
		if(LongestDistanceMap.find((*RI)) == LongestDistanceMap.end())
			continue;
		const Instruction *TInst = (*RI)->getTerminator();
		for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
			const BasicBlock *Succ = TInst->getSuccessor(I);
			if(isBackEdge((*RI), Succ)) {
				continue;
			}
			int dist = 0;
			// the path can propagate only when both the edge and the BB are uninstrumented
			if(EdgeInstMap[std::make_pair((*RI), Succ)] == UNINST && CostMap[(*RI)] > 0 && FuncCallCostMap[(*RI)] >= 0 && LongestDistanceMap[*(RI)] >= 0 && InstrumentedBB.find((*RI)) == InstrumentedBB.end() )
			{
				dist = LongestDistanceMap[(*RI)] + FuncCallCostMap[Succ];
			}
			auto found = LongestDistanceMap.find(Succ);
			bool update_dist = true;
			if(found == LongestDistanceMap.end()) 
				LongestDistanceMap[Succ] = dist;
			else if (dist > found->second) 
				found->second = dist;
			else
				update_dist = false;

			if(PrevBBMap && update_dist)
				(*PrevBBMap)[Succ] = (*RI);
		}
	}
  }

  void longestDistanceFrom(const BasicBlock *StartBB, std::map<const BasicBlock *, int> &LongestDistanceMap, std::map<const BasicBlock *, const BasicBlock *> *PrevBBMap=nullptr) {
	LongestDistanceMap[StartBB] = CostMap[StartBB];
	if(PrevBBMap)
		(*PrevBBMap)[StartBB]  = StartBB; 
	for (BBVector::const_reverse_iterator RI = SortedBBs.rbegin(), RE = SortedBBs.rend(); RI != RE; ++RI) {
		if(LongestDistanceMap.find((*RI)) == LongestDistanceMap.end())
			continue;
		const Instruction *TInst = (*RI)->getTerminator();
		for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
			const BasicBlock *Succ = TInst->getSuccessor(I);
			if(isBackEdge((*RI), Succ)) {
				continue;
			}
			int dist = 0;
			// the path can propagate only when both the edge and the BB are uninstrumented
			if(EdgeInstMap[std::make_pair((*RI), Succ)] == UNINST && CostMap[(*RI)] > 0 && LongestDistanceMap[*(RI)] > 0 && InstrumentedBB.find((*RI)) == InstrumentedBB.end())
			{
				dist = LongestDistanceMap[(*RI)] + CostMap[Succ];
			}
			auto found = LongestDistanceMap.find(Succ);
			bool update_dist = true;
			if(found == LongestDistanceMap.end()) 
				LongestDistanceMap[Succ] = dist;
			else if (dist > found->second) 
				found->second = dist;
			else
				update_dist = false;

			if(PrevBBMap && update_dist)
				(*PrevBBMap)[Succ] = (*RI);
		}
	}
  }

  std::pair<const BasicBlock *, int> generateDistanceMap(Function &F, std::map<const BasicBlock *, int> &DistanceMap, std::map<const BasicBlock *, const BasicBlock *> &PrevBBMap) {
	const BasicBlock *EntryBlock = &F.getEntryBlock();
	DistanceMap[EntryBlock] = CostMap[EntryBlock];
	PrevBBMap[EntryBlock]  = EntryBlock;  	
	// max distance and the corresponding ending node
	int MaxDist = CostMap[EntryBlock];
	const BasicBlock *MaxBB = EntryBlock;
	int TotalCost = 0;

	for (BBVector::const_reverse_iterator RI = SortedBBs.rbegin(), RE = SortedBBs.rend(); RI != RE; ++RI) {
		TotalCost += CostMap[(*RI)];
		const Instruction *TInst = (*RI)->getTerminator();
		for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
			const BasicBlock *Succ = TInst->getSuccessor(I);
			// skip if it is a backedge
			if(isBackEdge((*RI), Succ))
				continue;
			int dist = CostMap[Succ];
			// the path can propagate only when both the edge and the BB are uninstrumented
			bool propagate = false;
			if(EdgeInstMap[std::make_pair((*RI), Succ)] == UNINST && CostMap[(*RI)] > 0 && InstrumentedBB.find((*RI)) == InstrumentedBB.end())
			{
				propagate = true;
				dist += DistanceMap[(*RI)];
			}
			bool update_dist = false;
			auto found = DistanceMap.find(Succ);
			if(found == DistanceMap.end())
			{
				DistanceMap[Succ] = dist;
				update_dist = true;
			}
			else if (dist > found->second)
			{
				found->second = dist;
				update_dist = true;
			}

			if(update_dist)
			{
				// update PrevBBMap
				if(propagate)
					PrevBBMap[Succ] = (*RI);
				else
					PrevBBMap[Succ] = Succ;

				// update MaxDist and MaxBB
				if(dist > MaxDist)
				{
					MaxDist = dist;
					MaxBB = Succ;
				}
			}
		}   
	}
	// errs() << "Max distance is " << MaxDist << "\n";
	return std::make_pair(MaxBB, MaxDist);
  }

  bool decideInstLocations(std::map<const BasicBlock *, int> &DistanceMap, std::map<const BasicBlock *, const BasicBlock *> &PrevBBMap, const BasicBlock* MaxBB, int DistThreshold) {
	const BasicBlock* CurBB = MaxBB;
	while(true)
	{
		const BasicBlock* PrevBB = PrevBBMap[CurBB];
		if(DistanceMap[PrevBB] < DistThreshold) {
			assert(EdgeInstMap[std::make_pair(PrevBB, CurBB)] == UNINST);
			EdgeInstMap[std::make_pair(PrevBB, CurBB)] = NORM_INST;
			// double check that the PrevBB is not instrumented
			assert(InstrumentedBB.find(PrevBB) == InstrumentedBB.end());
			// errs() << "Instrument Edge\n";
			NumEdgeInst++;
			return true;
		}
		if(PrevBB == CurBB) {
			LargeBB = CurBB;
			break;
		}
		CurBB = PrevBB;
	}
	return false;
  }

  /* CI function prototype */
  Value *action_hook_prototype(Instruction *I, char *funcName) {
	Module *M = I->getParent()->getParent()->getParent();
	IRBuilder<> Builder(I);
	std::vector<Type *> funcArgs;
	funcArgs.push_back(Builder.getInt64Ty());
	// Value* funcPtr =
	// M->getGlobalVariable("intvActionHook",PointerType::getUnqual(FunctionType::get(Builder.getVoidTy(),
	// funcArgs, false)));
	/* Declare the  thread local interrupt handler pointer, if it is not present
	 * in the module. */
	Value *funcPtr = M->getOrInsertGlobal(
		funcName, PointerType::getUnqual(
					  FunctionType::get(Builder.getVoidTy(), funcArgs, false)));
	GlobalVariable *gCIFuncPtr = static_cast<GlobalVariable *>(funcPtr);
	gCIFuncPtr->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);
	assert(funcPtr && "Could not find CI handler function!");

	return funcPtr;
  }

  /* Call CI & reset all counters */
  void pushToMLCfromTLLC(Instruction *I, Value *currTSC, Value *loadedLC = nullptr) {
	Module *M = I->getModule();
	I->getParent()->setName("pushBlock");
	IRBuilder<> Builder(I);

	// either use the obtained current time stamp, or it will be updated externally 
	if(!WillUpdateLastCycleTS) {
		GlobalVariable *thenVar = M->getGlobalVariable("LastCycleTS");
		Builder.CreateStore(currTSC, thenVar);
	}
	/* Code for calling custom function at push time */
	/* Generate a per-probe signature */ 
	if(!loadedLC)
		loadedLC = Builder.getInt64(FuncHash + ProbeIdx);
	ProbeIdx++;

	std::vector<llvm::Value *> args;
	args.push_back(loadedLC);
	char fName[] = "intvActionHook";
	Value *hookFuncPtr = action_hook_prototype(I, fName);
	auto hookFunc = Builder.CreateLoad(hookFuncPtr->getType()->getPointerElementType(), hookFuncPtr, "ci_handler");
	Builder.CreateCall(cast<FunctionType>(hookFunc->getType()->getPointerElementType()), hookFunc, args);

	if(WillUpdateLastCycleTS) {
		GlobalVariable *thenVar = M->getGlobalVariable("LastCycleTS");
		CallInst *now = Builder.CreateIntrinsic(Intrinsic::readcyclecounter, {}, {}, nullptr, "currCycle");
		Builder.CreateStore(now, thenVar);
	}
  }

  IntegerType* getIntegerType(IRBuilder<> *IR, Value *Target) {
  	// target is either an integer or a pointer to integer
  	unsigned int BitWitdth = 0;
  	if(Target->getType()->isPointerTy()) {
  		BitWitdth = dyn_cast<IntegerType>(Target->getType()->getPointerElementType())->getBitWidth(); 
  	} else {
  		BitWitdth = dyn_cast<IntegerType>(Target->getType())->getBitWidth(); 
  	}
  	switch(BitWitdth) {
  		case 8: {
  			return IR->getInt8Ty();
  		}
  		case 16: {
  			return IR->getInt16Ty();
  		}
  		case 32: {
  			return IR->getInt32Ty();
  		}
  		case 64: {
  			return IR->getInt64Ty();
  		}
  		default: {
  			assert("Undefined bitwidth!\n");
  			return nullptr;
  		}
  	}
  	return nullptr;
  }

  Value *getConstInteger(IRBuilder<> *IR, Value *Target, int ConstValue) {
  	// target is either an integer or a pointer to integer
  	unsigned int BitWitdth = 0;
  	if(Target->getType()->isPointerTy()) {
  		BitWitdth = dyn_cast<IntegerType>(Target->getType()->getPointerElementType())->getBitWidth(); 
  	} else {
  		BitWitdth = dyn_cast<IntegerType>(Target->getType())->getBitWidth(); 
  	}
  	switch(BitWitdth) {
  		case 8: {
  			return IR->getInt8(ConstValue);
  		}
  		case 16: {
  			return IR->getInt16(ConstValue);
  		}
  		case 32: {
  			return IR->getInt32(ConstValue);
  		}
  		case 64: {
  			return IR->getInt64(ConstValue);
  		}
  		default: {
  			return nullptr;
  		}
  	}
  	return nullptr;
  }

  /* A simpler version */
  void insertCycleProbe(Instruction &I, Value *LoopIterations = nullptr, Value *LoopThreshold = nullptr, int IncrementBy = 0) {
	Function *F = I.getFunction();
	IRBuilder<> IR(&I);
	// added for NumProbes
    // GlobalVariable *np =  I.getModule()->getGlobalVariable("NumProbes");
    // LoadInst *NumProbesLoad = IR.CreateLoad(np);
    // Value *NumProbesInc = IR.CreateAdd(IR.getInt64(1), NumProbesLoad);
    // IR.CreateStore(NumProbesInc, np);

	if(LoopIterations)
		IR.CreateStore(getConstInteger(&IR, LoopIterations, 0), LoopIterations);
	if(LoopThreshold) {
		assert(!LoopIterations);
		LoadInst *Loopiter = IR.CreateLoad(LoopThreshold, "NextThreshold");
		Value *Inc = IR.CreateAdd(Loopiter, getConstInteger(&IR, LoopThreshold, IncrementBy));
		IR.CreateStore(Inc, LoopThreshold);
	}
	CallInst *now = IR.CreateIntrinsic(Intrinsic::readcyclecounter, {}, {}, nullptr, "currCycle");
	GlobalVariable *thenVar = F->getParent()->getGlobalVariable("LastCycleTS");
	LoadInst *then = IR.CreateLoad(thenVar, "lastCycle");
	Value *timeDiff = IR.CreateSub(now, then, "elapsedCycles");
	Value *ci_cycles_threshold = I.getModule()->getGlobalVariable("ci_cycles_threshold");
	Value *targetinterval = IR.CreateLoad(ci_cycles_threshold, "targetInCycle"); // CYCLES
	if (timeDiff->getType() != targetinterval->getType()) {
		errs() << "Wrongful loaded LC type: " << *timeDiff->getType() << "\n";
		timeDiff = IR.CreateZExt(timeDiff, targetinterval->getType(), "zeroExtendSLI");
	}
	Value *condition = IR.CreateICmpSGT(timeDiff, targetinterval, "commit");
	Instruction *ti = llvm::SplitBlockAndInsertIfThen(condition, &I, false);
	IR.SetInsertPoint(ti);
	Function::iterator blockItr(ti->getParent());
	blockItr++;
	blockItr->setName("postInstrumentation");
	// comment out timeDiff if you wanna receive unique per probe signature
	pushToMLCfromTLLC(ti, now, timeDiff);
  }

  void insertRecursionProbe(Function &F, Instruction &I, int NumIters) {
  	Module *M = F.getParent();
  	auto InitVal32 = llvm::ConstantInt::get(M->getContext(), llvm::APInt(32, 0, false));
  	GlobalVariable *RecursionVariable = new GlobalVariable(*M, Type::getInt32Ty(M->getContext()), false,
					   GlobalValue::ExternalLinkage, InitVal32, "RecursionVariable");
  	RecursionVariable->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);

  	// then instrument loop edge
	IRBuilder<> IR(&I);
	LoadInst *RecursionIterator = IR.CreateLoad(RecursionVariable, "RecursionIterator");
	Value *Inc = IR.CreateAdd(RecursionIterator, IR.getInt32(1));
	IR.CreateStore(Inc, RecursionVariable);
	Value *condition = IR.CreateICmpEQ(Inc, IR.getInt32(NumIters), "commit");
	Instruction *ti = llvm::SplitBlockAndInsertIfThen(condition, &I, false);
	IR.SetInsertPoint(ti);
	Function::iterator blockItr(ti->getParent());
	blockItr++;
	blockItr->setName("postRecursionLoop");
	insertCycleProbe(*ti, RecursionVariable);
  }

  void insertLoopProbe(Function &F, Instruction &I, int NumIters, BasicBlock *LatchBB, BasicBlock *HeaderBB) {
  	// LI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
  	// Loop *L = LI->getLoopFor(LatchBB);
  	// // if is nested, will record residual cycles
  	// bool IsNested = false;
  	// if(L->getLoopDepth() > 1) {
  	// 	IsNested = true;
  	// }
  	assert(LoopDepthMap.find(std::make_pair(LatchBB, HeaderBB)) != LoopDepthMap.end());
  	bool IsNested = false;
  	if(LoopDepthMap[std::make_pair(LatchBB, HeaderBB)] > 1)
  		IsNested = true;
  	// since all loops are simplified: 
  	// they have dedicated exits. That is, no exit block for the loop has a predecessor that is outside the loop. This implies that all exit blocks are dominated by the loop header.
	// SetInsertPointPastAllocas is not implemented in LLVM 12, do it mannualy 
	Instruction *InsertPoint = nullptr;
	// AllocaInsertPoint is also at the entry BB
	Instruction *AllocaInsertPoint = nullptr;
	for(Instruction &Inst : F.getEntryBlock()) {
		if(isa<AllocaInst>(&Inst) || isa<DbgInfoIntrinsic>(&Inst) || isa<PHINode>(&Inst))
			continue;
		AllocaInsertPoint = &Inst;
		break;
	}
	if(!AllocaInsertPoint) {
		errs() << "Can't find insertion point for the initial alloc\n";
		return;
	}
	auto PreHeaderFound = PreheaderMap.find(std::make_pair(LatchBB, HeaderBB));
	if(PreHeaderFound != PreheaderMap.end()) {
		BasicBlock *PreheaderBB = PreHeaderFound->second;
		InsertPoint = PreheaderBB->getTerminator();
	}
	else {
		InsertPoint = AllocaInsertPoint;
	}
	bool UsableIndVariable = false;
	Value *InitialValue = nullptr;
	Value *StepInst = nullptr;
	int StepValue = 0;
	Value *FinalValue = nullptr;
	bool UsableFinalValue = false;

	if(LoopInitialValueMap.find(std::make_pair(LatchBB, HeaderBB)) != LoopInitialValueMap.end())
	{
		assert(PreheaderMap.find(std::make_pair(LatchBB, HeaderBB)) != PreheaderMap.end());
		UsableIndVariable = true;
		InitialValue = LoopInitialValueMap[std::make_pair(LatchBB, HeaderBB)];
		auto found = LoopStepValueMap.find(std::make_pair(LatchBB, HeaderBB));
		StepInst = found->second.first;
		StepValue = found->second.second;
		if(LoopFinalValueMap.find(std::make_pair(LatchBB, HeaderBB)) != LoopFinalValueMap.end()) {
			FinalValue = LoopFinalValueMap[std::make_pair(LatchBB, HeaderBB)];
			UsableFinalValue = checkIfValueDominatesInst(FinalValue, InsertPoint);
		}
	}

	if(IsNested && !UsableIndVariable) {
		// in this case, easier to just initialize at the EntryBB
		InsertPoint = AllocaInsertPoint;
	}

	if(!UsableIndVariable) {
		// always allocate at entryBB
		IRBuilder<> AllocaBuilder(AllocaInsertPoint);
		Value *LoopIterations = AllocaBuilder.CreateAlloca(AllocaBuilder.getInt32Ty(), 0, "LoopVariable");
		if(InsertPoint != AllocaInsertPoint) {
			AllocaBuilder.SetInsertPoint(AllocaInsertPoint);
			IRBuilder<> InitializeBuilder(InsertPoint);
			Value *valZero = InitializeBuilder.getInt32(0);
			InitializeBuilder.CreateStore(valZero, LoopIterations);
			InitializeBuilder.SetInsertPoint(InsertPoint);
		} else {
			Value *valZero = AllocaBuilder.getInt32(0);
			AllocaBuilder.CreateStore(valZero, LoopIterations);
			AllocaBuilder.SetInsertPoint(AllocaInsertPoint);
		}
		// then instrument loop edge
		IRBuilder<> IR(&I);
		LoadInst *Loopiter = IR.CreateLoad(LoopIterations, "LoopIterations");
		Value *Inc = IR.CreateAdd(Loopiter, IR.getInt32(1));
		IR.CreateStore(Inc, LoopIterations);
		Value *condition = IR.CreateICmpEQ(Inc, IR.getInt32(NumIters), "commit");
		Instruction *ti = llvm::SplitBlockAndInsertIfThen(condition, &I, false);
		IR.SetInsertPoint(ti);
		Function::iterator blockItr(ti->getParent());
		blockItr++;
		blockItr->setName("postLoop");
		insertCycleProbe(*ti, LoopIterations);
	}
	else {
		errs() << "Loop instrumentation based on induction variable with step value " << StepValue << "\n";
		// always allocate at entryBB
		IRBuilder<> AllocaBuilder(AllocaInsertPoint);
		Value *LoopThreshold = AllocaBuilder.CreateAlloca(getIntegerType(&AllocaBuilder, StepInst), 0, "LoopThreshold");
		Value *ResidualSteps = nullptr;
		if(IsNested) {
			ResidualSteps = AllocaBuilder.CreateAlloca(getIntegerType(&AllocaBuilder, StepInst), 0, "ResidualSteps");
			AllocaBuilder.CreateStore(getConstInteger(&AllocaBuilder, StepInst, NumIters * StepValue), ResidualSteps);
		}
		AllocaBuilder.SetInsertPoint(AllocaInsertPoint);
		
		IRBuilder<> InitializeBuilder(InsertPoint);
		if(isa<ConstantInt>(InitialValue)) {
			if(IsNested) {
				assert(ResidualSteps);
				int InitialConstValue = dyn_cast<ConstantInt>(InitialValue)->getSExtValue();
				LoadInst *LoopInst = InitializeBuilder.CreateLoad(ResidualSteps, "CurrResidualSteps");
				Value *Inc = InitializeBuilder.CreateAdd(LoopInst, getConstInteger(&InitializeBuilder, StepInst, InitialConstValue));
				InitializeBuilder.CreateStore(Inc, LoopThreshold);
			} else {
				int InitialConstValue = dyn_cast<ConstantInt>(InitialValue)->getSExtValue();
				int InitalThresholdValue = InitialConstValue + NumIters * StepValue;
				InitializeBuilder.CreateStore(getConstInteger(&InitializeBuilder, StepInst, InitalThresholdValue), LoopThreshold);
			}

		} else {
			if(IsNested) {
				assert(ResidualSteps);
				LoadInst *LoopInst = InitializeBuilder.CreateLoad(ResidualSteps, "CurrResidualSteps");
				Value *Inc = InitializeBuilder.CreateAdd(LoopInst, InitialValue);
				InitializeBuilder.CreateStore(Inc, LoopThreshold);						
			}
			else {
				Value *Inc = InitializeBuilder.CreateAdd(InitialValue, getConstInteger(&InitializeBuilder, StepInst, NumIters * StepValue));
				InitializeBuilder.CreateStore(Inc, LoopThreshold);
			}
		}

		// if(LatchBB != HeaderBB && ResidualSteps && UsableFinalValue) {
		// 	// the first condition added since we already have cloned self loop
		// 	errs() << F.getName() << ": nested loop with usable final value\n";
		// 	Value *ValRange = InitializeBuilder.CreateSub(FinalValue, InitialValue);
		// 	Value *condition = nullptr;
		// 	if(StepValue > 0)
	 //  			condition = InitializeBuilder.CreateICmpSLE(ValRange, getConstInteger(&InitializeBuilder, ValRange, NumIters * StepValue), "short_nested_loop_cond");
		// 	else
		// 		condition = InitializeBuilder.CreateICmpSGE(ValRange, getConstInteger(&InitializeBuilder, ValRange, NumIters * StepValue), "short_nested_loop_cond");
		
		// 	Instruction *ti = llvm::SplitBlockAndInsertIfThen(condition, InsertPoint, false);
		// 	InitializeBuilder.SetInsertPoint(InsertPoint);
		// 	Function::iterator blockItr(ti->getParent());
		// 	blockItr++;
		// 	blockItr->setName("postShortNestedLoop");
		// 	IRBuilder<> FinalIR(ti);
		// 	LoadInst *LoopInst = FinalIR.CreateLoad(ResidualSteps, "ResidualStepsToReduce");
		// 	Value *NewResidualSteps = FinalIR.CreateSub(LoopInst, ValRange);
		// 	FinalIR.CreateStore(NewResidualSteps, ResidualSteps);
		// 	FinalIR.SetInsertPoint(ti);
		// } else {
			InitializeBuilder.SetInsertPoint(InsertPoint);
		// }

		// then instrument loop edge
		IRBuilder<> IR(&I);
		LoadInst *LoopInst = IR.CreateLoad(LoopThreshold, "CurrThreshold");
		
		if (LoopInst->getType() != StepInst->getType()) {
			errs() << "Wrongful loaded LC type: " << *LoopInst->getType() << " " << *StepInst->getType() << "\n";
		}

		Value *condition = IR.CreateICmpEQ(StepInst, LoopInst, "commit");
		Instruction *ti = llvm::SplitBlockAndInsertIfThen(condition, &I, false);
		IR.SetInsertPoint(ti);
		Function::iterator blockItr(ti->getParent());
		blockItr++;
		blockItr->setName("postLoop");
		insertCycleProbe(*ti, nullptr, LoopThreshold, NumIters * StepValue);

		// update ResidualSteps at ExitBlocks
		if(IsNested && LatchBB != HeaderBB) {
			// self-loops will be cloned, hence the header no longer dominates all exit blocks
			// StepInst may not dominate all exit blocks, use induction variable instead
			assert(LoopInductionVariableMap.find(std::make_pair(LatchBB, HeaderBB)) != LoopInductionVariableMap.end());
			Value *IndVar = LoopInductionVariableMap[std::make_pair(LatchBB, HeaderBB)];
			for(auto BB : *LoopExitBlocks[std::make_pair(LatchBB, HeaderBB)]) {
				assert(checkIfValueDominatesInst(IndVar, BB->getFirstNonPHI()));
			}
			for(auto BB : *LoopExitBlocks[std::make_pair(LatchBB, HeaderBB)]) {
				IRBuilder<> IR(BB->getTerminator());
				LoadInst *LoopInst = IR.CreateLoad(LoopThreshold, "FinalThreshold");
				Value *RemainigSteps = IR.CreateSub(LoopInst, IndVar);
				IR.CreateStore(RemainigSteps, ResidualSteps);
				IR.SetInsertPoint(BB->getTerminator());
			}
		}
	}
  }

  // instrument F so that there is no uninstrumented e2e path
  void instrumentFunc(Function &F) {
	// for simplicity, instrument at the end of the entry BB
	// first uninstrument any outgoing edge of the entry BB
	BasicBlock *EntryBlock = &F.getEntryBlock();
	Instruction *TInst = EntryBlock->getTerminator();
	for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
		BasicBlock *Succ = TInst->getSuccessor(I);
		if(EdgeInstMap[std::make_pair(EntryBlock, Succ)] != UNINST)
			NumEdgeInst--;
		EdgeInstMap[std::make_pair(EntryBlock, Succ)] = UNINST;
	}
	// now we can instrument the entryBB
	insertCycleProbe(*(F.getEntryBlock().getTerminator()));
	errs() << "Instrumenting function " << F.getName().str() << "\n";
  }

  // instrument a function call in F
  void instrumentFuncCall(Function &F, StringRef &InstCall) {
	bool Instrumented = false;
	for (auto &BB : F) {
		for (auto &I : BB) {
			auto *Call = dyn_cast<CallBase>(&I);
			if(!Call)
				continue;
			Function *calledFunction = Call->getCalledFunction();
			if(!calledFunction)
				continue;
			if(calledFunction->getName().compare(InstCall) == 0) {
				insertCycleProbe(I);
				Instrumented = true;
				break;	
			}
		}
		if(Instrumented)
			break;
	}
	if(!Instrumented)
		errs() << "Instrumenting Call " << InstCall.str() << " in function " << F.getName().str() << " fails\n";
	else
		errs() << "Instrumenting Call " << InstCall.str() << " in function " << F.getName().str() << " succeeds\n";
  }

  bool decideSelfLoopCloneable(BasicBlock* BB) {
  	// decide whether a self-loop is cloneable
  	// initial value, constant step value, final value & available at preheader
  	if(PreheaderMap.find(std::make_pair(BB, BB)) == PreheaderMap.end())
  		return false;
  	if(LoopInitialValueMap.find(std::make_pair(BB, BB)) == LoopInitialValueMap.end())
  		return false;
  	if(LoopStepValueMap.find(std::make_pair(BB, BB)) == LoopStepValueMap.end())
  		return false;
 	if(LoopFinalValueMap.find(std::make_pair(BB, BB)) == LoopFinalValueMap.end())
  		return false;

  	BasicBlock* PreheaderBB = PreheaderMap[std::make_pair(BB, BB)];
  	Value* InitialValue = LoopInitialValueMap[std::make_pair(BB, BB)];
  	Value* FinalValue = LoopFinalValueMap[std::make_pair(BB, BB)];
  	if(!checkIfValueDominatesInst(InitialValue, PreheaderBB->getTerminator()))
  		return false;
  	if(!checkIfValueDominatesInst(FinalValue, PreheaderBB->getTerminator()))
  		return false;
  	return true;
  }

   BasicBlock *cloneSelfLoop(BasicBlock *BB, const char *Tag) const {
    ValueToValueMapTy Map;
    BasicBlock *Clone = CloneBasicBlock(BB, Map, Twine(".") + Tag, BB->getParent());

    auto GetClonedValue = [&Map](Value *V) {
      assert(V && "null values not in domain!");
      auto It = Map.find(V);
      if (It == Map.end())
        return V;
      return static_cast<Value *>(It->second);
    };

    Clone->getTerminator()->setMetadata(".clonedSingleBlockLoop", MDNode::get(*LLVMCtx, {}));
                         
    for (Instruction &I : *Clone)
      RemapInstruction(&I, Map, RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);

    // Exit blocks will now have one more predecessor and their PHI nodes need
    // to be edited to reflect that.  No phi nodes need to be introduced because
    // the loop is in LCSSA.

    Loop *OriginalLoop = LI->getLoopFor(BB);
    for (auto *SBB : successors(BB)) {
      if (OriginalLoop->contains(SBB))
        continue; // not an exit block

      for (PHINode &PN : SBB->phis()) {
        Value *OldIncoming = PN.getIncomingValueForBlock(BB);
        PN.addIncoming(GetClonedValue(OldIncoming), Clone);
      }
    }

    // change terminator of cloned block from original block to itself
    Instruction *cloneTermInst = Clone->getTerminator();
    int numOfSucc = cloneTermInst->getNumSuccessors();
    for (int idx = 0; idx < numOfSucc; idx++) {
      BasicBlock *succ = cloneTermInst->getSuccessor(idx);
      if (succ == BB) {
        // errs() << "Found BB " << succ->getName() << " at index " << idx <<
        // "\n";
        cloneTermInst->setSuccessor(idx, Clone);
        break;
      }
    }

    // change cloned block's phi's
    for (PHINode &PN : Clone->phis()) {
      int IDX = PN.getBasicBlockIndex(BB);
      while (IDX != -1) {
        PN.setIncomingBlock((unsigned)IDX, Clone);
        IDX = PN.getBasicBlockIndex(BB);
      }
    }

    return Clone;
  }

  void cloneSelfLoopOnCondition(Function &F, BasicBlock* BB, int NumIters, bool IsNested) {
  	BasicBlock *PreheaderBB = PreheaderMap[std::make_pair(BB, BB)];
  	int StepValue = LoopStepValueMap[std::make_pair(BB, BB)].second;
  	Value *InitialValue = LoopInitialValueMap[std::make_pair(BB, BB)];
  	Value *FinalValue = LoopFinalValueMap[std::make_pair(BB, BB)];

  	Value *SumLoopVal = nullptr;
 	if(IsNested) {
		Instruction *AllocaInsertPoint = nullptr;
		for(Instruction &Inst : F.getEntryBlock()) {
			if(isa<AllocaInst>(&Inst) || isa<DbgInfoIntrinsic>(&Inst) || isa<PHINode>(&Inst))
				continue;
			AllocaInsertPoint = &Inst;
			break;
		}
		IRBuilder<> AllocaBuilder(AllocaInsertPoint);
		SumLoopVal = AllocaBuilder.CreateAlloca(getIntegerType(&AllocaBuilder, FinalValue), 0, "SumLoopVal");
		AllocaBuilder.CreateStore(getConstInteger(&AllocaBuilder, FinalValue, 0), SumLoopVal);
		AllocaBuilder.SetInsertPoint(AllocaInsertPoint);
	}


  	// preheader may no longer be the immediate predecessor of BB, since a SplittedBB may be instrumented between them
  	// a temporary fix 	
  	BasicBlock *InstBB = PreheaderBB;
  	if(UpdatePreheaderMap.find(std::make_pair(PreheaderBB, BB)) != UpdatePreheaderMap.end())
  		InstBB = UpdatePreheaderMap[std::make_pair(PreheaderBB, BB)];

  	IRBuilder<> IRPreheader(InstBB->getTerminator());
  	Value *ValRange = IRPreheader.CreateSub(FinalValue, InitialValue);
  	Value *condition = nullptr;
  	if(StepValue > 0)
	  	condition = IRPreheader.CreateICmpSLE(ValRange, getConstInteger(&IRPreheader, ValRange, NumIters * StepValue), "short_loop_cond");
	else
		condition = IRPreheader.CreateICmpSGE(ValRange, getConstInteger(&IRPreheader, ValRange, NumIters * StepValue), "short_loop_cond");
  	BasicBlock *ClonedBB = cloneSelfLoop(BB, "clonedSelfLoop");
  	
  	assert(isa<BranchInst>(InstBB->getTerminator()) && "Self Loop's preheader blocks's terminator is not a branch instruction.");
    BranchInst *brI = dyn_cast<BranchInst>(InstBB->getTerminator());
    assert(!brI->isConditional() && "Self Loop's preheader has conditional branch terminator!");
    assert(InstBB->getTerminator()->getNumSuccessors() == 1);
    BranchInst *newTerm = BranchInst::Create(/*ifTrue*/ ClonedBB, /*ifFalse*/ BB, condition);
    ReplaceInstWithInst(InstBB->getTerminator(), newTerm);

    if(IsNested) {
    	// instrument at the end of the clonedBB
	    BasicBlock* ExitBB = nullptr;
		const Instruction *TInst = ClonedBB->getTerminator();
		assert(TInst->getNumSuccessors() == 2); 
		if(TInst->getSuccessor(0) == ClonedBB)
			ExitBB = TInst->getSuccessor(1);
		else
			ExitBB = TInst->getSuccessor(0);
		EdgeInstMap[std::make_pair(ClonedBB, ExitBB)] = NORM_INST;
		BasicBlock* SplittedBB = llvm::SplitEdge(ClonedBB, ExitBB, nullptr, nullptr, nullptr, "SplittedClonedSelfLoopEdge");

		// check SumLoopVal
		IRBuilder<> IR(SplittedBB->getTerminator());
		LoadInst *Loopiter = IR.CreateLoad(SumLoopVal, "CurrSumLoopVal");
		Value *Total = IR.CreateAdd(Loopiter, ValRange);
		IR.CreateStore(Total, SumLoopVal);
		if(StepValue > 0)
		  	condition = IR.CreateICmpSGE(Total, getConstInteger(&IR, Total, NumIters * StepValue), "short_loop_inst_cond");
		else
			condition = IR.CreateICmpSLE(Total, getConstInteger(&IR, Total, NumIters * StepValue), "short_loop_inst_cond");

		Instruction *ti = llvm::SplitBlockAndInsertIfThen(condition, SplittedBB->getTerminator(), false);
		IR.SetInsertPoint(ti);
		Function::iterator blockItr(ti->getParent());
		blockItr++;
		blockItr->setName("postClonedSelfLoop");
		insertCycleProbe(*ti, SumLoopVal);
    }

	// insertCycleProbe(*SplittedBB->getTerminator());
  }

  void insertProbes(Function &F) {
	// (1) if all the outgoing edges (more than 1) of a BB is instrumented, we could just instrument the BB
	for (auto &BB : F)
	{
		const Instruction *TInst = BB.getTerminator();
		if(TInst->getNumSuccessors() < 2) {
			continue;
		}
		bool EdgeAllInsted = true;
		for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
			BasicBlock *Succ = TInst->getSuccessor(I);
			if(EdgeInstMap[std::make_pair(&BB, Succ)] == UNINST) {
				EdgeAllInsted = false;
				break;
			}
		}
		if(EdgeAllInsted) {
			// errs() << "Found a BB with all outgoing edges instrumented\n";
			assert(InstrumentedBB.find(&BB) == InstrumentedBB.end());
			InstrumentedBB.insert(&BB);
			NumBBInst++;
			for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
				BasicBlock *Succ = TInst->getSuccessor(I);
				EdgeInstMap[std::make_pair(&BB, Succ)] = UNINST;
				NumEdgeInst--;
			}
		}
	}
	// (2) if recursive (hence entryBB instrumented), we need to find the cost of the function
	int RecursionCost = 0;
	if(IsRecursiveFunc[F.getName()]) {
		BasicBlock *EntryBlock = &F.getEntryBlock();
		Instruction *TInst = EntryBlock->getTerminator();
		// find all BBs that have a self call
		BBVector SelfCallBBs;
		for (auto &BB : F) {
			bool ContainSelfCall = false;
			for (auto &I : BB) {
				auto *Call = dyn_cast<CallBase>(&I);
				if(!Call)
					continue;
				Function *calledFunction = Call->getCalledFunction();
				if(!calledFunction)
					continue;
				if(calledFunction->getName().compare(F.getName()) == 0) {
					ContainSelfCall = true;
					break;	
				}
			}
			if(ContainSelfCall) {
				SelfCallBBs.push_back(&BB);
			}
		}
		// for all the successors of the entry block
		for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
			BasicBlock *Succ = TInst->getSuccessor(I);
			// map from BB to its longest distance
			std::map<const BasicBlock *, int> LongestDistanceMap;
			longestDistanceFrom(Succ, LongestDistanceMap);
			for (auto &BB : SelfCallBBs) {
				if(LongestDistanceMap.find(BB) == LongestDistanceMap.end())
					continue;
				if(LongestDistanceMap.find(BB)->second > RecursionCost) {
					RecursionCost = LongestDistanceMap.find(BB)->second;
				}
			}
		}
		if(RecursionCost > 0) {
			RecursionCost += CostMap[&F.getEntryBlock()];
			errs() << F.getName() << " has recursion cost: " << RecursionCost << "\n";
		}
	}

	// (3) for all the backedges, find the longest uninstrumented distance between start/end node; if 0, no need to instrument this backedge
	// for LoopCost, use LoopCostMode
	// LoopCostMode = true;
	// populateCostMap(F);
	populateFuncCallCostMap(F);
	std::map<std::pair<const BasicBlock *, const BasicBlock *>, int> LoopCost;
	std::map<std::pair<const BasicBlock *, const BasicBlock *>, int> LoopFuncCallCost;
	for(auto backEdge: backEdges) {
		if(EdgeInstMap[backEdge] == UNINST) {
			continue;
		}
		std::map<const BasicBlock *, int> LongestDistanceMap;
		longestDistanceFrom(backEdge.second, LongestDistanceMap);
		LoopCost[backEdge] = LongestDistanceMap[backEdge.first];
		if(LoopCost[backEdge] > CommitInterval)
			errs() << "LoopCost: " << LoopCost[backEdge] << "\n";

		std::map<const BasicBlock *, int> LongestCallCostDistanceMap;
		longestFuncCallDistanceFrom(backEdge.second, LongestCallCostDistanceMap);
		LoopFuncCallCost[backEdge] = LongestCallCostDistanceMap[backEdge.first];
	}
	for(auto it = LoopCost.begin(); it != LoopCost.end(); ++it) {
		if(it->second == 0)
		{
			errs() << "Cancel a backedge instrumentation for function " << F.getName().str() << "\n" ;
			// no need to instrument this back edge
			backEdges.erase(std::remove(backEdges.begin(), backEdges.end(), it->first), backEdges.end());
			assert(EdgeInstMap[it->first] == LOOP_INST);
			EdgeInstMap[it->first] = UNINST;
			NumEdgeInst--;
		}
	} 

	// first, instrument normal edges
	for(auto it = EdgeInstMap.begin(); it != EdgeInstMap.end(); ++it) {
		if(it->second == UNINST)
			continue;
		if(it->second == NORM_INST) {
			BasicBlock* FromBB = const_cast<BasicBlock*>(it->first.first);
			BasicBlock* ToBB = const_cast<BasicBlock*>(it->first.second);
			BasicBlock* SplittedBB = llvm::SplitEdge(FromBB, ToBB, nullptr, nullptr, nullptr, "SplittedEdge");
			insertCycleProbe(*SplittedBB->getTerminator());
			auto found = PreheaderMap.find(std::make_pair(ToBB, ToBB));
			if(found != PreheaderMap.end() && found->second == FromBB)
			{
				// ToBB is a self loop and we are insturmenting between preheader and it
				BasicBlock* PostInstBB = SplittedBB->getTerminator()->getSuccessor(1);
				UpdatePreheaderMap[std::make_pair(FromBB, ToBB)] = PostInstBB;
			}
		} 
	}
	// then instrument individual BB
	for(auto BB : InstrumentedBB) {
		if(RecursionCost > 0 && const_cast<BasicBlock*>(BB) == &F.getEntryBlock()) {
			int NumIters = CommitInterval/RecursionCost;
			// NumIters = 1;
			insertRecursionProbe(F, *(const_cast<BasicBlock*>(BB)->getTerminator()), NumIters);
		}
		else
			insertCycleProbe(*(const_cast<BasicBlock*>(BB)->getTerminator()));
	}
	// third, instrument loops -- need to do it last since we may copy self loops
	int NumSelfLoops = 0;
	int NumLoops = 0;
	for(auto it = EdgeInstMap.begin(); it != EdgeInstMap.end(); ++it) {
		if(it->second == UNINST)
			continue;
		if(it->second == LOOP_INST) {
			NumLoops++;
			if(LoopCost[it->first] == 0) // skip this edge
				continue;
			// NumIters: largest number of loops that the total cost stays <= CommitInterval
			int NumIters = CommitInterval/LoopCost[it->first]; 
			// NumItersFuncCall: largest number of loops that the total number of function calls <= FuncCallThreshold
			if(LoopFuncCallCost[it->first] > 0) {
				int NumItersFuncCall = FuncCallThreshold/LoopFuncCallCost[it->first];
				if(NumItersFuncCall == 0)
					NumItersFuncCall = 1;
				// errs() << F.getName() << ": NumIters: " << NumIters << " vs NumItersFuncCall: " << NumItersFuncCall << "\n";
				// pick the smaller one
				NumIters = (NumIters <= NumItersFuncCall)? NumIters : NumItersFuncCall;
			}
			// errs() << F.getName() << ": FinalNumIters: " << NumIters << "\n";

			BasicBlock* FromBB = const_cast<BasicBlock*>(it->first.first);
			BasicBlock* ToBB = const_cast<BasicBlock*>(it->first.second);
			assert(InstrumentedBB.find(FromBB) == InstrumentedBB.end());
			if(FromBB == ToBB) {
				NumSelfLoops++;
				if(LoopInitialValueMap.find(std::make_pair(FromBB, FromBB)) == LoopInitialValueMap.end() && LoopCost[it->first] < 50 && LoopFuncCallCost[it->first] == 0) {
					errs() << "Found a self loop that should be unrolled\n";
				}
				// A self loop, decide whether clonable
				if(decideSelfLoopCloneable(FromBB)) {
					errs() << "Found a cloneable self loop!\n";
					cloneSelfLoopOnCondition(F, FromBB, NumIters, /*LI->getLoopFor(FromBB)->getLoopDepth()*/LoopDepthMap[std::make_pair(FromBB, ToBB)] > 1);
					errs() << "Successfully cloned a self loop!\n";
				} 
			}
			BasicBlock* LoopBB = llvm::SplitEdge(FromBB, ToBB, nullptr, nullptr, nullptr, "LoopEdge");
			insertLoopProbe(F, *LoopBB->getTerminator(), NumIters, FromBB, ToBB);
		}
	}
	errs() << F.getName() << ": " << NumLoops << " loops, " << NumSelfLoops << " self loops\n";
	// lastly, external library calls
	for(auto it = CostMap.begin(); it != CostMap.end(); ++it) { 
		if(it->second > 0) 
			continue;
		if(it->second == -1)
			continue;
		BasicBlock* CurBB = const_cast<BasicBlock*>(it->first);
		SmallVector<Instruction*, 32> InstructionToProbe; 
		int TotalInstCost = 0; 
		for(Instruction &I: *CurBB) {
			int InstCost = getInstructionCost(&I);
			TotalInstCost += InstCost;
			if(TotalInstCost >= CommitInterval) {
				InstructionToProbe.push_back(&I);
				TotalInstCost = InstCost;
				NumExtLibInst++;
			}
		}
		for(auto I : InstructionToProbe) {
			insertCycleProbe(*I);
		}
	}
	// lastly,  instrument function calls if necessary
	// for (auto it = CallsToInstrument.begin(); it != CallsToInstrument.end(); ++it) {
	// 	if(F.getName().compare(it->first) == 0) {
	// 		for(auto FuncCall : *(it->second)) {
	// 			  instrumentFuncCall(F, FuncCall);
	// 		}
	// 		break;
	// 	}
	// }
	// for (auto Func : FuncsToInstrument) {
	// 	if(Func.compare(F.getName()) == 0) {
	// 		instrumentFunc(F);
	// 		break;
	// 	}
	// }
  }

  void generateInst(Function &F) {
  	// if the function is recursive, instrument the first BB
  	int E2EThreshold = MaxE2EUninst;
  	if(IsRecursiveFunc[F.getName()]) {
  		E2EThreshold = 0;
  		errs() << "Instrument the first BB of function " << F.getName() << "\n";
  	}
	// for uninst e2e path
	while(true)
	{
		// map from BB to its longest distance
		std::map<const BasicBlock *, int> LongestDistanceMap;
		// map from BB to its predecessor in its longest path (if it's the same, then BB is the start)
		std::map<const BasicBlock *, const BasicBlock *> PrevBBMap;

		int MaxDist = 0;
		BasicBlock* MaxBB = nullptr;
		longestDistanceFrom(&F.getEntryBlock(), LongestDistanceMap, &PrevBBMap);
		for (auto &BB : F) {
			if(LongestDistanceMap.find(&BB) == LongestDistanceMap.end())
				continue;
			if(isa<ReturnInst>(BB.getTerminator()))
			{
				if(LongestDistanceMap.find(&BB)->second > MaxDist) {
					MaxDist = LongestDistanceMap.find(&BB)->second;
					MaxBB = &BB;
				}
			}
		}
		if(MaxDist <= E2EThreshold) {
			// errs() << "No more E2E Instrumentation needed!\n";
			break;
		}
		if(!decideInstLocations(LongestDistanceMap, PrevBBMap, MaxBB, E2EThreshold)) {
			// check it is because the entry block > E2EThreshold
			assert(LongestDistanceMap[&F.getEntryBlock()] >= E2EThreshold);
			// instrument every outgoing edge of the entry BB
			BasicBlock *EntryBlock = &F.getEntryBlock();
			Instruction *TInst = EntryBlock->getTerminator();
			for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
				BasicBlock *Succ = TInst->getSuccessor(I);
				if(EdgeInstMap[std::make_pair(EntryBlock, Succ)] == UNINST) {
					EdgeInstMap[std::make_pair(EntryBlock, Succ)] = NORM_INST;
					NumEdgeInst++;
				}
			}
			break;
		} 
	}
	while(true)
	{
		// map from BB to its longest distance
		std::map<const BasicBlock *, int> DistanceMap;
		// map from BB to its predecessor in its longest path (if it's the same, then BB is the start)
		std::map<const BasicBlock *, const BasicBlock *> PrevBBMap;

		auto MaxBBAndDist = generateDistanceMap(F, DistanceMap, PrevBBMap);
		const BasicBlock* MaxBB = MaxBBAndDist.first;
		int MaxDist = MaxBBAndDist.second;
		if(MaxDist <= CommitInterval) {
			// errs() << "No more Instrumentation needed!\n";
			break;
		}
		if(!decideInstLocations(DistanceMap, PrevBBMap, MaxBB, CommitInterval)) {
			// errs() << DistanceMap[LargeBB] << "\n";
			assert(DistanceMap[LargeBB] >= CommitInterval);
			// instrument every outgoing edge of this BB
			Instruction *TInst = const_cast<BasicBlock*>(LargeBB)->getTerminator();
			for (unsigned I = 0, NSucc = TInst->getNumSuccessors(); I < NSucc; ++I) {
				BasicBlock *Succ = TInst->getSuccessor(I);
				if(EdgeInstMap[std::make_pair(LargeBB, Succ)] == UNINST) {
					EdgeInstMap[std::make_pair(LargeBB, Succ)] = NORM_INST;
					NumEdgeInst++;
				}
			}
			// a hack for now
			CostMap[LargeBB] = -1;
			errs() << "A Block instrumented!\n";
			// errs() << "Instrumentation fails\n";
			//return;
		}
	}
	insertProbes(F);
  }


  int getCallCost(Instruction *I, bool updateCounter = true) {
	auto *Call = dyn_cast<CallBase>(I);
	Function *calledFunction = Call->getCalledFunction();
	if(calledFunction)
	{	
		int foundInOwnLib = LibraryInstructionCosts.count(calledFunction->getName());
		if(foundInOwnLib) {
			if(LoopCostMode && computedFuncInfo.find(calledFunction) != computedFuncInfo.end()) {
				if(computedFuncInfo[calledFunction]->TotalNumProbe == 0) {
					errs() << "Using function " << calledFunction->getName() << "'s E2E Uninst Cost: " << computedFuncInfo[calledFunction]->MaxUnistDist << "\n";
					return computedFuncInfo[calledFunction]->MaxUnistDist;
				} 
			}
			return LibraryInstructionCosts[calledFunction->getName()];
		}
		else if (isa<DbgInfoIntrinsic>(I) || isa<IntrinsicInst>(I)) {
			if(calledFunction->getName().str().compare("llvm.memmove.p0i8.p0i8.i64") == 0)
				return CommitInterval;
			return 1;
		}
		else // unknown external library call
		{
			if(updateCounter) {
				NumExternalCalls++;
				// if(I == &(I->getParent()->back())) {
				// 	NumExtLibInst++;
				// }
				// else {
				// 	NumExtLibInst+= 2;
				// }
			}
			// errs() << "External library call: " << calledFunction->getName().str() << "\n";
			if(checkIfExpensiveExtLibCall(calledFunction)) 
				return CommitInterval;

			if(checkIfIgnoreExtLibCall(calledFunction))
				return 1;

			return ExtLibFuncCost;
		}
	}
	else
	{
		// indirect function call
		return 1;
		// return CommitInterval;
	}
  }

  bool isNOOPInstruction(Instruction *I) {
  if (isa<PHINode>(I) || isa<GetElementPtrInst>(I) || isa<CastInst>(I) ||
	  isa<AllocaInst>(I))
	return true;
  else
	return false;
	}


  int getInstructionCost(Instruction *I) {
	if (isNOOPInstruction(I)) {
		return 0;
	} else if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
		return MemOpsCost;
	} else if (dyn_cast<CallBase>(I)) {
		return getCallCost(I);
	} else if (std::string(I->getOpcodeName()).compare("fmul") == 0 || std::string(I->getOpcodeName()).compare("fdiv") == 0) {
		return FMulDivCost;
	} else {
		return 1;
	}
  }

  void populateCostMap(Function &F) {
	for (BasicBlock &BB : F)
	{
		int InstCount = 0;
		NumExternalCalls = 0;
		for (Instruction &I : BB)
		{
			InstCount += getInstructionCost(&I);
		}
		if(/*NumExternalCalls * ExtLibFuncCost*/InstCount >= CommitInterval)
			InstCount = 0;
		CostMap[&BB] = InstCount;
	}
  }

  void populateFuncCallCostMap(Function &F) {
  	// increment iff (1) indirect function call or (2) instrumented func with maxe2e > 0
  	// terminates if there is an func with max_e2e = 0
	for (BasicBlock &BB : F)
	{
		int NumFuncCall = 0;
		bool Terminated = false;
		for (Instruction &I : BB)
		{
			auto *Call = dyn_cast<CallBase>(&I);
			if(!Call)
				continue;
			Function *calledFunction = Call->getCalledFunction();
			if(!calledFunction){
				NumFuncCall += 1;
				continue;
			}
			int foundInOwnLib = LibraryInstructionCosts.count(calledFunction->getName());
			if(foundInOwnLib) {
				if(computedFuncInfo.find(calledFunction) != computedFuncInfo.end()) {
					if(computedFuncInfo[calledFunction]->MaxUnistDist == 0) {
						Terminated = true;
					}
				}
				NumFuncCall += 1;	
			}
		}
		FuncCallCostMap[&BB] = NumFuncCall;
		if(Terminated)
			FuncCallCostMap[&BB] = -1;
	}
  }

  void initializeFunc(Function &F) {
	backEdges.clear();
	SortedBBs.clear();
	EdgeInstMap.clear();
	CostMap.clear();
	FuncCallCostMap.clear();
	InstrumentedBB.clear();
	PreheaderMap.clear();
	UpdatePreheaderMap.clear();
	LoopInitialValueMap.clear();
	LoopStepValueMap.clear();
	LoopFinalValueMap.clear();
	LoopInductionVariableMap.clear();
	LoopDepthMap.clear();
	LoopExitBlocks.clear();
	NumExtLibInst = 0;
	NumEdgeInst = 0;
	NumBBInst = 0;
	ProbeIdx = 0;
	LoopCostMode = false;
	std::hash<std::string> hasher;
	FuncHash = int64_t(hasher(F.getName().str()));
	LLVMCtx = &F.getContext();
	DT = &getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
	LI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
	SE = &getAnalysis<ScalarEvolutionWrapperPass>(F).getSE();
  }

   /* read function call costs to file for CI-compliant libraries */
  bool readCost() {
	/* There may not be any library cost file supplied */
	if (InCostFilePath.empty()) {
	  errs() << "No library file supplied\n";
	  return true;
	}
	std::ifstream fin;
	fin.open(InCostFilePath);
	bool first = true;

	if (!fin.good()) {
	  errs() << "Can't open the library cost file\n";
	  return false;
	}

	while (!fin.eof()) {
	  char buf[1024];
	  char *token1, *token2;
	  fin.getline(buf, 1024);
	  std::string str(buf);
	  if (first) {
		first = false;
		if (str.compare("Cost File") != 0)
		{
			errs() << "The first line doesn't match" << str << ".\n";
			return false;
		}
		else
		  continue;
	  }
	  if (std::string::npos != str.find(':')) {
		token1 = strtok(buf, ":");
		if (token1) {
		  token2 = strtok(0, ":");
		  int iCost = atoi(token2);
		  //errs() << "Reading cost for func name: " << token1 << ":" << iCost << "\n";
		  std::string* funcName = new std::string(token1);
	  /* TODO: fix memory leak */
	  /* TODO: Write a string to instruction cost function */
		  LibraryInstructionCosts[StringRef(*funcName)] = iCost;
		}
	  }
	}
	return true;
  }

  // for some reason the UnrollLoop implementation with incorrectly merge unrolled BBs
  // so I used the up-to-date impelemnentation here https://llvm.org/doxygen/LoopUnroll_8cpp_source.html
  bool myUnrollLoop(Loop *L, unsigned Count) {
   // We are going to make changes to this loop. SCEV may be keeping cached info
   // about it, in particular about backedge taken count. The changes we make
   // are guaranteed to invalidate this information for our loop. It is tempting
   // to only invalidate the loop being unrolled, but it is incorrect as long as
   // all exiting branches from all inner loops have impact on the outer loops,
   // and if something changes inside them then any of outer loops may also
   // change. When we forget outermost loop, we also forget all contained loops
   // and this is what we need here.

   if (!L->getLoopPreheader()) {
     errs() << "Can't unroll; loop preheader-insertion failed.\n";
     return false;
   }
  
   if (!L->getLoopLatch()) {
     errs() << "Can't unroll; loop exit-block-insertion failed.\n";
     return false;
   }
  
   // Loops with indirectbr cannot be cloned.
   if (!L->isSafeToClone()) {
     errs() << "Can't unroll; Loop body cannot be cloned.\n";
     return false;
   }
  
   if (L->getHeader()->hasAddressTaken()) {
     // The loop-rotate pass can be helpful to avoid this in many cases.
     errs() << "Won't unroll loop: address of header block is taken.\n";
     return false;
   }
  
   assert(Count > 0);

   // All these values should be taken only after peeling because they might have
   // changed.
   // BasicBlock *Preheader = L->getLoopPreheader();
   BasicBlock *Header = L->getHeader();
   BasicBlock *LatchBlock = L->getLoopLatch();
   SmallVector<BasicBlock *, 4> ExitBlocks;
   L->getExitBlocks(ExitBlocks);
   std::vector<BasicBlock *> OriginalLoopBlocks = L->getBlocks();

   // The current loop unroll pass can unroll loops that have
   // (1) single latch; and
   // (2a) latch is unconditional; or
   // (2b) latch is conditional and is an exiting block
   // FIXME: The implementation can be extended to work with more complicated
   // cases, e.g. loops with multiple latches.
   BranchInst *LatchBI = dyn_cast<BranchInst>(LatchBlock->getTerminator());
  
   // A conditional branch which exits the loop, which can be optimized to an
   // unconditional branch in the unrolled loop in some cases.
   bool LatchIsExiting = L->isLoopExiting(LatchBlock);
   if (!LatchBI || (LatchBI->isConditional() && !LatchIsExiting)) {
     errs() << "Can't unroll; a conditional latch must exit the loop";
     return false;
   }

   if (SE) {
       SE->forgetTopmostLoop(L);
   }
  
   // For the first iteration of the loop, we should use the precloned values for
   // PHI nodes.  Insert associations now.
   ValueToValueMapTy LastValueMap;
   std::vector<PHINode*> OrigPHINode;
   for (BasicBlock::iterator I = Header->begin(); isa<PHINode>(I); ++I) {
     OrigPHINode.push_back(cast<PHINode>(I));
   }
  
   std::vector<BasicBlock *> Headers;
   std::vector<BasicBlock *> Latches;
   Headers.push_back(Header);
   Latches.push_back(LatchBlock);
  
   // The current on-the-fly SSA update requires blocks to be processed in
   // reverse postorder so that LastValueMap contains the correct value at each
   // exit.
   LoopBlocksDFS DFS(L);
   DFS.perform(LI);
  
   // Stash the DFS iterators before adding blocks to the loop.
   LoopBlocksDFS::RPOIterator BlockBegin = DFS.beginRPO();
   LoopBlocksDFS::RPOIterator BlockEnd = DFS.endRPO();
  
   std::vector<BasicBlock*> UnrolledLoopBlocks = L->getBlocks();
  
   // Loop Unrolling might create new loops. While we do preserve LoopInfo, we
   // might break loop-simplified form for these loops (as they, e.g., would
   // share the same exit blocks). We'll keep track of loops for which we can
   // break this so that later we can re-simplify them.
   SmallSetVector<Loop *, 4> LoopsToSimplify;
   for (Loop *SubLoop : *L)
     LoopsToSimplify.insert(SubLoop);
  
   // When a FSDiscriminator is enabled, we don't need to add the multiply
   // factors to the discriminators.
   if (Header->getParent()->isDebugInfoForProfiling())
     for (BasicBlock *BB : L->getBlocks())
       for (Instruction &I : *BB)
         if (!isa<DbgInfoIntrinsic>(&I))
           if (const DILocation *DIL = I.getDebugLoc()) {
             auto NewDIL = DIL->cloneByMultiplyingDuplicationFactor(Count);
             if (NewDIL)
               I.setDebugLoc(*NewDIL);
             else
               errs() << "Failed to create new discriminator: " << DIL->getFilename() << " Line: " << DIL->getLine();
           }
  
   // Identify what noalias metadata is inside the loop: if it is inside the
   // loop, the associated metadata must be cloned for each iteration.
   SmallVector<MDNode *, 6> LoopLocalNoAliasDeclScopes;
   llvm::identifyNoAliasScopesToClone(L->getBlocks(), LoopLocalNoAliasDeclScopes);
  
   // We place the unrolled iterations immediately after the original loop
   // latch.  This is a reasonable default placement if we don't have block
   // frequencies, and if we do, well the layout will be adjusted later.
   auto BlockInsertPt = std::next(LatchBlock->getIterator());
   for (unsigned It = 1; It != Count; ++It) {
     SmallVector<BasicBlock *, 8> NewBlocks;
     SmallDenseMap<const Loop *, Loop *, 4> NewLoops;
     NewLoops[L] = L;
  
     for (LoopBlocksDFS::RPOIterator BB = BlockBegin; BB != BlockEnd; ++BB) {
       ValueToValueMapTy VMap;
       BasicBlock *New = CloneBasicBlock(*BB, VMap, "." + Twine(It));
       Header->getParent()->getBasicBlockList().insert(BlockInsertPt, New);
  
       assert((*BB != Header || LI->getLoopFor(*BB) == L) &&
              "Header should not be in a sub-loop");
       // Tell LI about New.
       const Loop *OldLoop = addClonedBlockToLoopInfo(*BB, New, LI, NewLoops);
       if (OldLoop)
         LoopsToSimplify.insert(NewLoops[OldLoop]);
  
       if (*BB == Header)
         // Loop over all of the PHI nodes in the block, changing them to use
         // the incoming values from the previous block.
         for (PHINode *OrigPHI : OrigPHINode) {
           PHINode *NewPHI = cast<PHINode>(VMap[OrigPHI]);
           Value *InVal = NewPHI->getIncomingValueForBlock(LatchBlock);
           if (Instruction *InValI = dyn_cast<Instruction>(InVal))
             if (It > 1 && L->contains(InValI))
               InVal = LastValueMap[InValI];
           VMap[OrigPHI] = InVal;
           New->getInstList().erase(NewPHI);
         }
  
       // Update our running map of newest clones
       LastValueMap[*BB] = New;
       for (ValueToValueMapTy::iterator VI = VMap.begin(), VE = VMap.end();
            VI != VE; ++VI)
         LastValueMap[VI->first] = VI->second;
  
       // Add phi entries for newly created values to all exit blocks.
       for (BasicBlock *Succ : successors(*BB)) {
         if (L->contains(Succ))
           continue;
         for (PHINode &PHI : Succ->phis()) {
           Value *Incoming = PHI.getIncomingValueForBlock(*BB);
           ValueToValueMapTy::iterator It = LastValueMap.find(Incoming);
           if (It != LastValueMap.end())
             Incoming = It->second;
           PHI.addIncoming(Incoming, New);
           SE->forgetValue(&PHI);
         }
       }
       // Keep track of new headers and latches as we create them, so that
       // we can insert the proper branches later.
       if (*BB == Header)
         Headers.push_back(New);
       if (*BB == LatchBlock)
         Latches.push_back(New);
  
       // Keep track of the exiting block and its successor block contained in
       // the loop for the current iteration.
       // auto ExitInfoIt = ExitInfos.find(*BB);
       // if (ExitInfoIt != ExitInfos.end())
       //   ExitInfoIt->second.ExitingBlocks.push_back(New);
  
       NewBlocks.push_back(New);
       UnrolledLoopBlocks.push_back(New);
  
       // Update DomTree: since we just copy the loop body, and each copy has a
       // dedicated entry block (copy of the header block), this header's copy
       // dominates all copied blocks. That means, dominance relations in the
       // copied body are the same as in the original body.
       if (*BB == Header)
         DT->addNewBlock(New, Latches[It - 1]);
       else {
         auto BBDomNode = DT->getNode(*BB);
         auto BBIDom = BBDomNode->getIDom();
         BasicBlock *OriginalBBIDom = BBIDom->getBlock();
         DT->addNewBlock(
             New, cast<BasicBlock>(LastValueMap[cast<Value>(OriginalBBIDom)]));
       }
     }
  
     // Remap all instructions in the most recent iteration
     remapInstructionsInBlocks(NewBlocks, LastValueMap);
  
     {
       // Identify what other metadata depends on the cloned version. After
       // cloning, replace the metadata with the corrected version for both
       // memory instructions and noalias intrinsics.
       std::string ext = (Twine("It") + Twine(It)).str();
       cloneAndAdaptNoAliasScopes(LoopLocalNoAliasDeclScopes, NewBlocks,
                                  Header->getContext(), ext);
     }
   }
  
   // Loop over the PHI nodes in the original block, setting incoming values.
   for (PHINode *PN : OrigPHINode) {
      if (Count > 1) {
       Value *InVal = PN->removeIncomingValue(LatchBlock, false);
       // If this value was defined in the loop, take the value defined by the
       // last iteration of the loop.
       if (Instruction *InValI = dyn_cast<Instruction>(InVal)) {
         if (L->contains(InValI))
           InVal = LastValueMap[InVal];
       }
       assert(Latches.back() == LastValueMap[LatchBlock] && "bad last latch");
       PN->addIncoming(InVal, Latches.back());
     }
   }
  
   // Connect latches of the unrolled iterations to the headers of the next
   // iteration. Currently they point to the header of the same iteration.
   for (unsigned i = 0, e = Latches.size(); i != e; ++i) {
     unsigned j = (i + 1) % e;
     Latches[i]->getTerminator()->replaceSuccessorWith(Headers[i], Headers[j]);
   }
   // Update dominators of blocks we might reach through exits.
   // Immediate dominator of such block might change, because we add more
   // routes which can lead to the exit: we can now reach it from the copied
   // iterations too.
   if (Count > 1) {
     for (auto *BB : OriginalLoopBlocks) {
       auto *BBDomNode = DT->getNode(BB);
       SmallVector<BasicBlock *, 16> ChildrenToUpdate;
       for (auto *ChildDomNode : BBDomNode->children()) {
         auto *ChildBB = ChildDomNode->getBlock();
         if (!L->contains(ChildBB))
           ChildrenToUpdate.push_back(ChildBB);
       }
       // The new idom of the block will be the nearest common dominator
       // of all copies of the previous idom. This is equivalent to the
       // nearest common dominator of the previous idom and the first latch,
       // which dominates all copies of the previous idom.
       BasicBlock *NewIDom = DT->findNearestCommonDominator(BB, LatchBlock);
       for (auto *ChildBB : ChildrenToUpdate)
         DT->changeImmediateDominator(ChildBB, NewIDom);
     }
   }
   assert(DT->verify(DominatorTree::VerificationLevel::Fast));

    // At this point, the code is well formed.  We now simplify the unrolled loop,
   // doing constant propagation and dead code elimination as we go.
   llvm::simplifyLoopAfterUnroll(L, true, LI, SE, DT, nullptr, nullptr);

   return true;
 }

  void getAllLoopsHelper(SmallVector<Loop*, 32> &AllLoops, Loop* Parent) {
  	for (Loop* SubLoop : Parent->getSubLoops()) {
  		AllLoops.push_back(SubLoop);
  		getAllLoopsHelper(AllLoops, SubLoop);
  	}
  }
  void getAllLoops(SmallVector<Loop*, 32> &AllLoops) {
  	for (LoopInfo::iterator itLI = LI->begin(), itLIEnd = LI->end(); itLI != itLIEnd; ++itLI) {
  		AllLoops.push_back((*itLI));
  		getAllLoopsHelper(AllLoops, (*itLI));
  	}
  }

  void unrollSelfLoops(Function &F) {
  	// added for unrolling self loops
  	std::map<BasicBlock*, int> SelfLoopsToUnroll; 
  	SmallVector<Loop*, 32> AllLoops;
  	getAllLoops(AllLoops);

  	int NumSelfLoops = 0;
  	int NumLoops = 0;
  	for (auto itLI = AllLoops.begin(); itLI != AllLoops.end(); ++itLI) {
  		NumLoops++;
		BasicBlock *HeaderBB = (*itLI)->getHeader();
   		BasicBlock *LatchBB = (*itLI)->getLoopLatch();
   		if(HeaderBB == LatchBB) {
   			NumSelfLoops++;
   			// self loop & no induction variable
   			if((*itLI)->getInductionVariable(*SE)) {
   				errs() << F.getName() << ": Found an induction variable, skip this self loop!\n";
   				continue;
   			}
   			int InstCount = 0;
   			int FuncCallCount = 0;
			for (Instruction &I : *LatchBB)
			{
				InstCount += getInstructionCost(&I);
				if(dyn_cast<CallBase>(&I))
					FuncCallCount += 1;
			}
			if(InstCount < 50 && FuncCallCount == 0)
   				SelfLoopsToUnroll[LatchBB] = InstCount;
   		}
	}
	errs() << F.getName() << ": " << NumLoops << " loops, " << NumSelfLoops << " self loops\n";
	

	for(auto it = SelfLoopsToUnroll.begin(); it != SelfLoopsToUnroll.end(); ++it) {
		Loop *L = LI->getLoopFor(it->first);		
		assert(L->getLoopLatch() == L->getHeader());
		int Cost = it->second;
		int UnrollCount = int(50/Cost) + 1;
		if(UnrollCount > 10)
			UnrollCount = 10;
		// UnrollLoopOptions ULO;
  // 		ULO.Count = UnrollCount;
	 //  	llvm::UnrollLoop(L, ULO, LI, SE, DT, nullptr, nullptr, nullptr, true, nullptr);
	  	if(myUnrollLoop(L, unsigned(UnrollCount)))
			errs() << F.getName() << ": Unroll a self loop " << UnrollCount << " times\n";
	} 
  }

  bool checkBackEdges() {
  	bool AllBackEdgesAreLoops = true;
  	for(auto backEdge : backEdges) {
  		if(PreheaderMap.find(backEdge) == PreheaderMap.end()) {
  			// set depth so that we can continue for now
  			LoopDepthMap[backEdge] = 1;
  			AllBackEdgesAreLoops = false;
  		}
  	}
  	return AllBackEdgesAreLoops;
  }

  /* Analysis Pass will evaluate cost of functions and encode where to instrument */
  void analyzeAndInstrFunc(Function &F) {
	initializeFunc(F);
	// added to unroll tight self-loops
	// unrollSelfLoops(F);
	if(!checkLoopsSimplified(F)) {
		errs() << "There are unsimplified loops!\n";
		return;
	}
	populateCostMap(F);
	getLoopsPreheader(F);
	getLoopsBound(F);
	FindFunctionBackedges(F, backEdges);
	if(!checkBackEdges()) {
		errs() << F.getName() << ": There are backedges not found in LoopInfo!\n";
		//continue for now
		//return;
	}
	generateTopoSort(F);
	populateEdgeInstMap();
	generateInst(F);
	updateFuncInfo(F);
  }

  void updateFuncInfo(Function &F) {
	FuncInfo *FInfo = new FuncInfo();
	int InstCount = 0;
	for (BasicBlock& bb : F)
	{
		InstCount += std::distance(bb.begin(), bb.end());
	}
	int TotalNumProbe = NumExtLibInst + NumEdgeInst + NumBBInst;
	errs() << "Function " << F.getName().str() << ":" << InstCount << "," << TotalNumProbe << "\n";
	FInfo->InstCount = InstCount;
	FInfo->TotalNumProbe = TotalNumProbe;
	// errs() << "Ext Lib: " << NumExtLibInst << " BB: " << NumBBInst << " Edge: " << NumEdgeInst << "\n";
	// obtain longest uninstrumented e2e path
	std::map<const BasicBlock *, int> LongestDistanceMap;
	int MaxDist = 0;
	longestDistanceFrom(&F.getEntryBlock(), LongestDistanceMap);
	for (auto &BB : F) {
		if(LongestDistanceMap.find(&BB) == LongestDistanceMap.end())
			continue;
		if(isa<ReturnInst>(BB.getTerminator()))
		{
			if(LongestDistanceMap.find(&BB)->second > MaxDist)
				MaxDist = LongestDistanceMap.find(&BB)->second;
		}
	}
	FInfo->MaxUnistDist = MaxDist;
	for(int64_t i = 0; i < ProbeIdx; i++) 
		FInfo->ProbeSigs.push_back(FuncHash + i);
	computedFuncInfo[&F] = FInfo;
  }

  /* write function probing information to file for libraries */
  void writeInfo(Module &M) {
	if (OutInfoFilePath.empty())
	  return;
	unlink(OutInfoFilePath.c_str());

	std::error_code EC;
	sys::fs::remove(OutInfoFilePath);
	raw_fd_ostream fout(OutInfoFilePath, EC, sys::fs::F_Text);
	for (auto &F : M) {
	  if (F.isDeclaration())
		continue;
	  std::string funcName(F.getName());
	  auto found = computedFuncInfo.find(&F);
	  if (found != computedFuncInfo.end()) {
		fout << funcName << ":";
		fout << found->second->InstCount << ",";
		fout << found->second->TotalNumProbe << ",";
		fout << found->second->MaxUnistDist /*"\n"*/;
		for(auto it = found->second->ProbeSigs.begin(); it != found->second->ProbeSigs.end(); ++it)
		{
  			fout << "," << *it;
		}
		fout << ",end\n";
	  }
	}
	fout.close();
  }

  /* write cost information to file for libraries */
  void writeCost(Module &M) {
	if (OutCostFilePath.empty())
	  return;
	unlink(OutCostFilePath.c_str());

	std::error_code EC;
	sys::fs::remove(OutCostFilePath);
	raw_fd_ostream fout(OutCostFilePath, EC, sys::fs::F_Text);
	fout << "Cost File\n";
	for (auto &F : M) {
	  if (F.isDeclaration())
		continue;
	  std::string funcName(F.getName());
	  auto found = computedFuncInfo.find(&F);
	  if (found != computedFuncInfo.end()) {
		fout << funcName << ":";
		fout << found->second->MaxUnistDist << "\n";
	  }
	}
	fout.close();
  }

	/* read function calls that should be instrumented */
  bool readFuncInst() 
  {
	if (InFuncInstFilePath.empty()) {
		errs() << "No function instrumentation file supplied\n";
		return true;
	}
	std::ifstream fin;
	fin.open(InFuncInstFilePath);
	bool first = true;
	if (!fin.good()) {
		return false;
	}
	while (!fin.eof()) {
		char buf[1024];
		char *token1, *token2;
		fin.getline(buf, 1024);
		std::string str(buf);
		if (first) {
			first = false; 
			if (str.compare("Function Instrumentation File") != 0)
			{
				errs() << "The first line doesn't match" << str << ".\n";
				return false;
			}
			continue;
		} 
		// this is a loop
		if (std::string::npos != str.find(':')) {
			token1 = strtok(buf, ":");
			if (token1) {
				token2 = strtok(0, ":");
				//errs() << "Need to instrument function call from " << token1 << " to " << token2 << "\n";
				std::string* caller = new std::string(token1);
				std::string* callee = new std::string(token2);
				/* TODO: fix memory leak */
			if (CallsToInstrument.end() == CallsToInstrument.find(*caller)) {
					CallsToInstrument[*caller] = new SmallVector<StringRef, 32>();
				}
				CallsToInstrument[*caller]->push_back(*callee);
			}
		}
		else { // instrument a function
			/* TODO: fix memory leak */
			std::string* FuncName = new std::string(str);
			FuncsToInstrument.insert(*FuncName);
		}	 
	}
	return true;
  }
  
  void initializeGlobalVariables(Module &M) {
	new GlobalVariable(M, Type::getInt64Ty(M.getContext()), false,
					   GlobalValue::ExternalLinkage, 0, "ci_cycles_threshold",
					   nullptr, GlobalValue::GeneralDynamicTLSModel, 0, true);
	new GlobalVariable(M, Type::getInt64Ty(M.getContext()), false,
					   GlobalValue::ExternalLinkage, 0, "LastCycleTS",
					   nullptr, GlobalValue::GeneralDynamicTLSModel, 0, true);

	// added for number of probes
    // new GlobalVariable(M, Type::getInt64Ty(M.getContext()), false,
    //                    GlobalValue::ExternalLinkage, 0, "NumProbes",
    //                    nullptr, GlobalValue::GeneralDynamicTLSModel, 0, true);
  }

  // at least functions in this module
  void populateLibraryInstructionCosts(Module &M) {
  	for (auto &F : M) {
	  if (!F.isDeclaration()) {
	      // Add if not already present
		  if(LibraryInstructionCosts.find(F.getName()) == LibraryInstructionCosts.end())
			LibraryInstructionCosts[F.getName()] = 1;
	  }
	}
  }

  void readExpensiveExtLibCalls() {
  	// mannually now
  	ExpensiveExtLibCalls.insert("write");

  }

  void readIgnoreExtLibCalls() {
  	IgnoreExtLibCalls.insert("pthread_mutex_trylock");
  	IgnoreExtLibCalls.insert("pthread_mutex_lock");
  	IgnoreExtLibCalls.insert("pthread_mutex_unlock");
  	// IgnoreExtLibCalls.insert("_ZN7rocksdb4port23pthread_mutex_lock_coroEP15pthread_mutex_t");
  }

    /* Get the list of functions in module in call graph order */
  void getRecursiveFunc() {
    CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
    for (scc_iterator<CallGraph *> CGI = scc_begin(&CG), CGE = scc_end(&CG); CGI != CGE; ++CGI) {
      std::vector<CallGraphNode *> NodeVec = *CGI;
      for (std::vector<CallGraphNode *>::iterator I = NodeVec.begin(),E = NodeVec.end();I != E; ++I) {
        Function *F = (*I)->getFunction();
        if (F && !F->isDeclaration()) {
          CGOrderedFunc.push_back(F->getName());
          if (NodeVec.size() > 1) {
            // IsRecursiveFunc[F->getName()] = true;
          } else if (NodeVec.size() == 1 && CGI.hasCycle()) {
          	errs() << "Self-Recursive function: " << F->getName() << "\n";
            // IsRecursiveFunc[F->getName()] = true;
          } else {
            IsRecursiveFunc[F->getName()] = false;
          }
        }
      }
    }
  }

  void initializeModule(Module &M) {
	if (!readCost()) {
		assert("Unable to library's cost configuration file\n");
		errs() << "Error reading library's cost configuration file\n";
	}
	populateLibraryInstructionCosts(M);

	if (!readFuncInst()) {
		assert("Unable to function instrumentation file\n");
		errs() << "Error reading the function instrumentation file\n";
	}

	readExpensiveExtLibCalls();

	readIgnoreExtLibCalls();

	getRecursiveFunc();

	initializeGlobalVariables(M);
  }

  bool runOnModule(Module &M) override {
	initializeModule(M);
	
	// Temporary hack
	// for (auto &F : M) {
	//   if (F.isDeclaration() && F.getName().compare("pthread_mutex_lock") == 0) {
	// 	  F.setName("_ZN7rocksdb4port23pthread_mutex_lock_coroEP15pthread_mutex_t");
	//   }
	// }

	for(auto FuncName : CGOrderedFunc) {
		/* Analyze & instrument */
		analyzeAndInstrFunc(*M.getFunction(FuncName));
	}
	writeInfo(M);
	writeCost(M);
	return true;
  }
}; // end of struct CheapPreemption
}  // end of anonymous namespace

char CheapPreemption::ID = 0;
static RegisterPass<CheapPreemption> X("cheap_preempt", "Cheap Preemption Pass",
							 false /* Only looks at CFG */,
							 false /* Analysis Pass */);
