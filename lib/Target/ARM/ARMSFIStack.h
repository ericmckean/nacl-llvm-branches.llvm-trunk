//===-- ARMSFIStack.h - NaCl SFI Stack Pointer updates ------- --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef TARGET_ARMSFISTACK_H
#define TARGET_ARMSFISTACK_H

namespace ARM_SFI {

bool IsStackChange(const llvm::MachineInstr &MI,
                   const llvm::TargetRegisterInfo *TRI);
bool IsSandboxedStackChange(const llvm::MachineInstr &MI);
bool NeedSandboxStackChange(const llvm::MachineInstr &MI,
                               const llvm::TargetRegisterInfo *TRI);

} // namespace ARM_SFI

#endif
