//===-- ARM.h - Top-level interface for ARM representation---- --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the entry points for global functions defined in the LLVM
// ARM back-end.
//
//===----------------------------------------------------------------------===//

#ifndef TARGET_ARM_H
#define TARGET_ARM_H

#include "ARMBaseInfo.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetMachine.h"
#include <cassert>

// @LOCALMOD (for LowerARMMachineInstrToMCInstPCRel)
#include "llvm/MC/MCSymbol.h"

namespace llvm {

class ARMBaseTargetMachine;
class FunctionPass;
class JITCodeEmitter;
class formatted_raw_ostream;
class MCCodeEmitter;
class TargetAsmBackend;
class MachineInstr;
class AsmPrinter;
class MCInst;

MCCodeEmitter *createARMMCCodeEmitter(const Target &,
                                      TargetMachine &TM,
                                      MCContext &Ctx);

TargetAsmBackend *createARMAsmBackend(const Target &, const std::string &);

FunctionPass *createARMISelDag(ARMBaseTargetMachine &TM,
                               CodeGenOpt::Level OptLevel);

FunctionPass *createARMJITCodeEmitterPass(ARMBaseTargetMachine &TM,
                                          JITCodeEmitter &JCE);

FunctionPass *createARMLoadStoreOptimizationPass(bool PreAlloc = false);
FunctionPass *createARMExpandPseudoPass();
FunctionPass *createARMGlobalMergePass(const TargetLowering* tli);
FunctionPass *createARMConstantIslandPass();
FunctionPass *createNEONMoveFixPass();
FunctionPass *createThumb2ITBlockPass();
FunctionPass *createThumb2SizeReductionPass();

/* @LOCALMOD-START */
FunctionPass *createARMNaClRewritePass();
/* @LOCALMOD-END */

extern Target TheARMTarget, TheThumbTarget;

void LowerARMMachineInstrToMCInst(const MachineInstr *MI, MCInst &OutMI,
                                  AsmPrinter &AP);

/* @LOCALMOD-START */
// Used to lower the pc-relative MOVi16PIC / MOVTi16PIC pseudo instructions
// into the real MOVi16 / MOVTi16 instructions.
// See comment on MOVi16PIC for more details.
void LowerARMMachineInstrToMCInstPCRel(const MachineInstr *MI,
                                       MCInst &OutMI,
                                       AsmPrinter &AP,
                                       unsigned ImmIndex,
                                       unsigned PCIndex,
                                       MCSymbol *PCLabel,
                                       unsigned PCAdjustment);
/* @LOCALMOD-END */


} // end namespace llvm;

#endif
