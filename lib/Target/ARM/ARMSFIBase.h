//===-- ARMSFIBase.h - Helper functions for NaCl SFI  ---- --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TARGET_ARMSFIBASE_H
#define TARGET_ARMSFIBASE_H

namespace ARM_SFI {

llvm::ARMCC::CondCodes GetPredicate(llvm::MachineInstr &MI);

}

#endif
