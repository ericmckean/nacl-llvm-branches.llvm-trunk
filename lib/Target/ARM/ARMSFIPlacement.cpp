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
    void InsertBicMask(MachineBasicBlock &MBB,
                       MachineBasicBlock::iterator MBBI,
                       MachineInstr &MI,
                       int AddrOperand);
  };
  char ARMSFIPlacement::ID = 0;
}

/*
 * Inserts a single BIC-based mask instruction immediately before the given
 * instruction/iterator position.
 */
void ARMSFIPlacement::InsertBicMask(MachineBasicBlock &MBB,
                                    MachineBasicBlock::iterator MBBI,
                                    MachineInstr &MI,
                                    int AddrOperand) {
  int PIdx = MI.findFirstPredOperandIdx();
  ARMCC::CondCodes Pred =
      PIdx != -1 ? (ARMCC::CondCodes)MI.getOperand(PIdx).getImm()
                 : ARMCC::AL;

  MachineOperand &Addr = MI.getOperand(AddrOperand);

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


bool ARMSFIPlacement::PlaceMBB(MachineBasicBlock &MBB) {
  bool Modified = false;

  MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
  while (MBBI != E) {
    MachineInstr &MI = *MBBI;
    MachineBasicBlock::iterator NMBBI = next(MBBI);

    unsigned Opcode = MI.getOpcode();
    switch (Opcode) {
    default: break;

    // Instructions with base address register in position 0...
    case ARM::VSTMD:
    case ARM::VSTMS:
      InsertBicMask(MBB, MBBI, MI, 0);
      Modified = true;
      break;
    // Instructions with base address register in position 1...
    case ARM::STR:
    case ARM::STRB:
    case ARM::STRH:
    case ARM::VSTRS:
    case ARM::VSTRD:
      InsertBicMask(MBB, MBBI, MI, 1);
      Modified = true;
      break;

    // Instructions with base address register in position 2...
    case ARM::STR_PRE:
    case ARM::STR_POST:
    case ARM::STRB_PRE:
    case ARM::STRB_POST:
    case ARM::STRH_PRE:
    case ARM::STRH_POST:
    case ARM::STRD:
      InsertBicMask(MBB, MBBI, MI, 2);
      Modified = true;
      break;
    }

    MBBI = NMBBI;
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
