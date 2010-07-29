//===- CodeMetrics.h - Measures the weight of a function---------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements various weight measurements for a function, helping
// the Inliner and PartialSpecialization decide whether to duplicate its
// contents.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_CODEMETRICS_H
#define LLVM_ANALYSIS_CODEMETRICS_H

namespace llvm {
  // CodeMetrics - Calculate size and a few similar metrics for a set of
  // basic blocks.
  struct CodeMetrics {
    /// NeverInline - True if this callee should never be inlined into a
    /// caller.
    // bool NeverInline;

    // True if this function contains a call to setjmp or _setjmp
    bool callsSetJmp;

    // True if this function calls itself
    bool isRecursive;

    // True if this function contains one or more indirect branches
    bool containsIndirectBr;

    /// usesDynamicAlloca - True if this function calls alloca (in the C sense).
    bool usesDynamicAlloca;

    /// NumInsts, NumBlocks - Keep track of how large each function is, which
    /// is used to estimate the code size cost of inlining it.
    unsigned NumInsts, NumBlocks;

    /// NumBBInsts - Keeps track of basic block code size estimates.
    DenseMap<const BasicBlock *, unsigned> NumBBInsts;

    /// NumCalls - Keep track of the number of calls to 'big' functions.
    unsigned NumCalls;

    /// NumVectorInsts - Keep track of how many instructions produce vector
    /// values.  The inliner is being more aggressive with inlining vector
    /// kernels.
    unsigned NumVectorInsts;

    /// NumRets - Keep track of how many Ret instructions the block contains.
    unsigned NumRets;

    CodeMetrics() : callsSetJmp(false), isRecursive(false),
                    containsIndirectBr(false), usesDynamicAlloca(false), 
                    NumInsts(0), NumBlocks(0), NumCalls(0), NumVectorInsts(0), 
                    NumRets(0) {}

    /// analyzeBasicBlock - Add information about the specified basic block
    /// to the current structure.
    void analyzeBasicBlock(const BasicBlock *BB);

    /// analyzeFunction - Add information about the specified function
    /// to the current structure.
    void analyzeFunction(Function *F);
  };
}

#endif
