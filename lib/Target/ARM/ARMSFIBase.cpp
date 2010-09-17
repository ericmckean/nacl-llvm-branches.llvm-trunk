//===-- ARMSFIBase.cpp - Helper functions for NaCl SFI ---------*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMBaseInstrInfo.h"

using namespace llvm;

namespace ARM_SFI {

ARMCC::CondCodes GetPredicate(MachineInstr &MI) {
  int PIdx = MI.findFirstPredOperandIdx();
  if (PIdx != -1) {
    return (ARMCC::CondCodes)MI.getOperand(PIdx).getImm();
  } else {
    return ARMCC::AL;
  }
}

} // namespace ARM_sfi
