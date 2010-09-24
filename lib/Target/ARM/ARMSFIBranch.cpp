//===-- ARMSFIBranch.cpp - Place SFI mask instructions ---------*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "arm-sfi"
#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/CommandLine.h"

#include <set>
#include <stdio.h>

using namespace llvm;

namespace {
  class ARMSFIBranch : public MachineFunctionPass {
  public:
    static char ID;
    ARMSFIBranch() : MachineFunctionPass(ID) {}

    const ARMBaseInstrInfo *TII;

    virtual void getAnalysisUsage(AnalysisUsage &AU) const;
    virtual bool runOnMachineFunction(MachineFunction &Fn);

    virtual const char *getPassName() const {
      return "ARM SFI branch sandboxing";
    }

  private:
    bool SandboxBranchesInBlock(MachineBasicBlock &MBB);
  };
  char ARMSFIBranch::ID = 0;
}

void ARMSFIBranch::getAnalysisUsage(AnalysisUsage &AU) const {
  // Slight (possibly unnecessary) efficiency tweak:
  // Promise not to modify the CFG.
  AU.setPreservesCFG();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool ARMSFIBranch::runOnMachineFunction(MachineFunction &MF) {
  TII = static_cast<const ARMBaseInstrInfo*>(MF.getTarget().getInstrInfo());

  bool Modified = false;
  for (MachineFunction::iterator MFI = MF.begin(), E = MF.end();
       MFI != E;
       ++MFI)
    Modified |= SandboxBranchesInBlock(*MFI);
  return Modified;
}

static bool IsReturn(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
   default: return false;

   case ARM::BX_RET:
    return true;
  }
}

static bool IsIndirectJump(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
   default: return false;

   case ARM::BRIND:
   case ARM::TAILJMPr:
   case ARM::TAILJMPrND:
    return true;
  }
}

static bool IsIndirectCall(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
   default: return false;

   case ARM::BLXr9:
    assert(0 && "we do not support usage of r9");

   case ARM::BLX:
    return true;
  }
}

static bool IsDirectCall(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
   default: return false;

   case ARM::BLr9:
   case ARM::BLr9_pred:
    assert(0 && "This should not have happend. We do not support usage of r9.");
    return true;

   case ARM::BL:
   case ARM::BL_pred:
   case ARM::TPsoft:
    return true;
  }
}

bool ARMSFIBranch::SandboxBranchesInBlock(MachineBasicBlock &MBB) {
  bool Modified = false;

  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {
    MachineInstr &MI = *MBBI;

    if (IsReturn(MI)) {
      ARMCC::CondCodes Pred = TII->getPredicate(&MI);
      BuildMI(MBB, MBBI, MI.getDebugLoc(),
              TII->get(ARM::SFI_GUARD_RETURN))
        .addImm((int64_t) Pred)  // predicate condition
        .addReg(ARM::CPSR);      // predicate source register (CPSR)
      Modified = true;
    }

    if (IsIndirectJump(MI)) {
      MachineOperand &Addr = MI.getOperand(0);
      ARMCC::CondCodes Pred = TII->getPredicate(&MI);
      BuildMI(MBB, MBBI, MI.getDebugLoc(),
              TII->get(ARM::SFI_GUARD_INDIRECT_JMP))
        .addOperand(Addr)        // rD
        .addReg(0)               // apparently unused source register?
        .addImm((int64_t) Pred)  // predicate condition
        .addReg(ARM::CPSR);      // predicate source register (CPSR)
      Modified = true;
    }

    if (IsDirectCall(MI)) {
      ARMCC::CondCodes Pred = TII->getPredicate(&MI);
      BuildMI(MBB, MBBI, MI.getDebugLoc(),
              TII->get(ARM::SFI_GUARD_CALL))
        .addImm((int64_t) Pred)  // predicate condition
        .addReg(ARM::CPSR);      // predicate source register (CPSR)
      Modified = true;
    }

    if (IsIndirectCall(MI)) {
      MachineOperand &Addr = MI.getOperand(0);
      ARMCC::CondCodes Pred = TII->getPredicate(&MI);
      BuildMI(MBB, MBBI, MI.getDebugLoc(),
              TII->get(ARM::SFI_GUARD_INDIRECT_CALL))
        .addOperand(Addr)        // rD
        .addReg(0)               // apparently unused source register?
        .addImm((int64_t) Pred)  // predicate condition
        .addReg(ARM::CPSR);      // predicate source register (CPSR)
        Modified = true;
    }
  }

  return Modified;
}

/// createARMSFIStorePass - returns an instance of the SFI placement pass.
FunctionPass *llvm::createARMSFIBranchPass() {
  return new ARMSFIBranch();
}
