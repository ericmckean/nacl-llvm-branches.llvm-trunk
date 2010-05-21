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

#include <stdio.h>

using namespace llvm;

namespace {
  class ARMSFIPlacement : public MachineFunctionPass {
  public:
    static char ID;
    ARMSFIPlacement() : MachineFunctionPass(&ID) {}

    const TargetInstrInfo *TII;

    virtual bool runOnMachineFunction(MachineFunction &Fn);

    virtual const char *getPassName() const {
      return "ARM SFI mask placement";
    }

  private:
    bool PlaceMBB(MachineBasicBlock &MBB);
    void IsolateStore(MachineBasicBlock &MBB,
                      MachineBasicBlock::iterator MBBI,
                      MachineInstr &MI,
                      int AddrOperand);
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

/*
 * Isolates a store instruction by inserting an appropriate mask or check
 * operation before it.
 */
void ARMSFIPlacement::IsolateStore(MachineBasicBlock &MBB,
                                   MachineBasicBlock::iterator MBBI,
                                   MachineInstr &MI,
                                   int AddrOperand) {
  ARMCC::CondCodes Pred = GetPredicate(MI);
  MachineOperand &Addr = MI.getOperand(AddrOperand);

  if (!TII->isPredicated(&MI)) {
    /*
     * For unconditional stores, we can use a faster sandboxing sequence
     * by predicating the store -- assuming we *can* predicate the store.
     */
    SmallVector<MachineOperand, 2> PredOperands;
    PredOperands.push_back(MachineOperand::CreateImm((int64_t) ARMCC::EQ));
    PredOperands.push_back(MachineOperand::CreateReg(ARM::CPSR, false));
    if (TII->PredicateInstruction(&MI, PredOperands)) {
      BuildMI(MBB, MBBI, MI.getDebugLoc(),
              TII->get(ARM::SFISTRTST))
        .addOperand(Addr)   // rD
        .addReg(0);         // apparently unused source register?
      return;
    }
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

static bool IsDangerousStore(const MachineInstr &MI, int *AddrOperand) {
  unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
  default: return false;

  // Instructions with base address register in position 0...
  case ARM::VSTMD:
  case ARM::VSTMS:
    *AddrOperand = 0;
    break;

  // Instructions with base address register in position 1...
  case ARM::STR:
  case ARM::STRB:
  case ARM::STRH:
  case ARM::VSTRS:
  case ARM::VSTRD:
    *AddrOperand = 1;
    break;

  // Instructions with base address register in position 2...
  case ARM::STR_PRE:
  case ARM::STR_POST:
  case ARM::STRB_PRE:
  case ARM::STRB_POST:
  case ARM::STRH_PRE:
  case ARM::STRH_POST:
  case ARM::STRD:
    *AddrOperand = 2;
    break;
  }

  if (MI.getOperand(*AddrOperand).getReg() == ARM::SP) {
    // The contents of SP do not require masking.
    return false;
  }

  return true;
}

bool ARMSFIPlacement::PlaceMBB(MachineBasicBlock &MBB) {
  bool Modified = false;

  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       MBBI = next(MBBI)) {
    MachineInstr &MI = *MBBI;

    int AddrOperand;
    if (IsDangerousStore(MI, &AddrOperand)) {
      IsolateStore(MBB, MBBI, MI, AddrOperand);
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
