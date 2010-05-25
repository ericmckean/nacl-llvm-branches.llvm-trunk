//===-- ARMSFIPlacement.cpp - Place SFI mask instructions ---------*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that places mask instructions ahead of all stores.
// This must be run as late in the game as possible -- after all scheduling and
// constant island placement.  (This is set up in ARMTargetMachine.cpp.)
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "arm-pseudo"
#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

#include <set>
#include <stdio.h>

using namespace llvm;

namespace {
  class ARMSFIPlacement : public MachineFunctionPass {
  public:
    static char ID;
    ARMSFIPlacement() : MachineFunctionPass(&ID) {}

    const TargetInstrInfo *TII;

    virtual void getAnalysisUsage(AnalysisUsage &AU) const;
    virtual bool runOnMachineFunction(MachineFunction &Fn);

    virtual const char *getPassName() const {
      return "ARM SFI mask placement";
    }

  private:
    bool PlaceMBB(MachineBasicBlock &MBB);
    void SandboxStore(MachineBasicBlock &MBB,
                      MachineBasicBlock::iterator MBBI,
                      MachineInstr &MI,
                      int AddrIdx,
                      bool CPSRLive);
  };
  char ARMSFIPlacement::ID = 0;
}

static ARMCC::CondCodes GetPredicate(MachineInstr &MI) {
  int PIdx = MI.findFirstPredOperandIdx();
  if (PIdx != -1) {
    return (ARMCC::CondCodes)MI.getOperand(PIdx).getImm();
  } else {
    return ARMCC::AL;
  }
}

void ARMSFIPlacement::getAnalysisUsage(AnalysisUsage &AU) const {
  // Slight (possibly unnecessary) efficiency tweak:
  // Promise not to modify the CFG.
  AU.setPreservesCFG();
  MachineFunctionPass::getAnalysisUsage(AU);
}

/*
 * Sandboxes a store instruction by inserting an appropriate mask or check
 * operation before it.
 */
void ARMSFIPlacement::SandboxStore(MachineBasicBlock &MBB,
                                   MachineBasicBlock::iterator MBBI,
                                   MachineInstr &MI,
                                   int AddrIdx,
                                   bool CPSRLive) {
  ARMCC::CondCodes Pred = GetPredicate(MI);
  MachineOperand &Addr = MI.getOperand(AddrIdx);

  if (!TII->isPredicated(&MI) && !CPSRLive) {
    /*
     * For unconditional stores where CPSR is not in use, we can use a faster
     * sandboxing sequence by predicating the store -- assuming we *can*
     * predicate the store.
     */

    /*
     * ARM predicate operands use two actual MachineOperands: an immediate
     * holding the predicate condition, and a register referencing the flags.
     */
    SmallVector<MachineOperand, 2> PredOperands;
    PredOperands.push_back(MachineOperand::CreateImm((int64_t) ARMCC::EQ));
    PredOperands.push_back(MachineOperand::CreateReg(ARM::CPSR, false));

    // Attempt to rewrite the instruction into its predicated equivalent.
    if (TII->PredicateInstruction(&MI, PredOperands)) {
      // Instruction can be predicated -- use the new sandbox.
      BuildMI(MBB, MBBI, MI.getDebugLoc(),
              TII->get(ARM::SFISTRTST))
        .addOperand(Addr)   // rD
        .addReg(0);         // apparently unused source register?
      return;
    }
    // Otherwise, fall through to the old sandbox.
  }

  BuildMI(MBB, MBBI, MI.getDebugLoc(),
          TII->get(ARM::SFISTRMASK))
    .addOperand(Addr)        // rD
    .addReg(0)               // apparently unused source register?
    .addImm((int64_t) Pred)  // predicate condition
    .addReg(ARM::CPSR);      // predicate source register (CPSR)

  /*
   * This pseudo-instruction is intended to generate something resembling the
   * following, but with alignment enforced.
   * TODO(cbiffle): move alignment into this function, use the code below.
   *
   *  // bic<cc> Addr, Addr, #0xC0000000
   *  BuildMI(MBB, MBBI, MI.getDebugLoc(),
   *          TII->get(ARM::BICri))
   *    .addOperand(Addr)        // rD
   *    .addOperand(Addr)        // rN
   *    .addImm(0xC0000000)      // imm
   *    .addImm((int64_t) Pred)  // predicate condition
   *    .addReg(ARM::CPSR)       // predicate source register (CPSR)
   *    .addReg(0);              // flag output register (0 == no flags)
   */
}

static bool IsDangerousStore(const MachineInstr &MI, int *AddrIdx) {
  unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
  default: return false;

  // Instructions with base address register in position 0...
  case ARM::VSTMD:
  case ARM::VSTMS:
    *AddrIdx = 0;
    break;

  // Instructions with base address register in position 1...
  case ARM::STR:
  case ARM::STRB:
  case ARM::STRH:
  case ARM::VSTRS:
  case ARM::VSTRD:
    *AddrIdx = 1;
    break;

  // Instructions with base address register in position 2...
  case ARM::STR_PRE:
  case ARM::STR_POST:
  case ARM::STRB_PRE:
  case ARM::STRB_POST:
  case ARM::STRH_PRE:
  case ARM::STRH_POST:
  case ARM::STRD:
    *AddrIdx = 2;
    break;
  }

  if (MI.getOperand(*AddrIdx).getReg() == ARM::SP) {
    // The contents of SP do not require masking.
    return false;
  }

  return true;
}

static bool IsCPSRLiveOut(const MachineBasicBlock &MBB) {
  // CPSR is live-out if any successor lists it as live-in.
  for (MachineBasicBlock::const_succ_iterator SI = MBB.succ_begin(),
                                              E = MBB.succ_end();
       SI != E;
       ++SI) {
    const MachineBasicBlock *Succ = *SI;
    if (Succ->isLiveIn(ARM::CPSR)) return true;
  }
  return false;
}

bool ARMSFIPlacement::PlaceMBB(MachineBasicBlock &MBB) {
  /*
   * This is a simple local reverse-dataflow analysis to determine where CPSR
   * is live.  We cannot use the conditional store sequence anywhere that CPSR
   * is live, or we'd affect correctness.  The existing liveness analysis passes
   * barf when applied pre-emit, after allocation, so we must do it ourselves.
   */ 

  bool CPSRLive = IsCPSRLiveOut(MBB);

  // Given that, record which instructions should not be altered to trash CPSR:
  std::set<const MachineInstr *> InstrsWhereCPSRLives;
  for (MachineBasicBlock::const_reverse_iterator MBBI = MBB.rbegin(),
                                                 E = MBB.rend();
       MBBI != E;
       ++MBBI) {
    const MachineInstr &MI = *MBBI;
    // Check for kills first.
    if (MI.modifiesRegister(ARM::CPSR)) CPSRLive = false;
    // Then check for uses.
    if (MI.readsRegister(ARM::CPSR)) CPSRLive = true;

    if (CPSRLive) InstrsWhereCPSRLives.insert(&MI);
  }

  // Sanity check:
  assert(CPSRLive == MBB.isLiveIn(ARM::CPSR)
         && "CPSR Liveness analysis does not match cached live-in result.");

  // Now: find and sandbox stores.
  bool Modified = false;
  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {
    MachineInstr &MI = *MBBI;

    int AddrIdx;
    if (IsDangerousStore(MI, &AddrIdx)) {
      bool CPSRLive =
          (InstrsWhereCPSRLives.find(&MI) != InstrsWhereCPSRLives.end());
      SandboxStore(MBB, MBBI, MI, AddrIdx, CPSRLive);
      Modified = true;
    }
  }

  return Modified;
}

bool ARMSFIPlacement::runOnMachineFunction(MachineFunction &MF) {
  TII = MF.getTarget().getInstrInfo();

  bool Modified = false;
  for (MachineFunction::iterator MFI = MF.begin(), E = MF.end(); MFI != E;
       ++MFI)
    Modified |= PlaceMBB(*MFI);
  return Modified;
}

/// createARMSFIPlacementPass - returns an instance of the SFI placement pass.
FunctionPass *llvm::createARMSFIPlacementPass() {
  return new ARMSFIPlacement();
}
