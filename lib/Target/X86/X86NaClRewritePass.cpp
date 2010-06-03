//===-- X86NaClRewritePAss.cpp - Rewrite Pseudo into instructions ---------*- C++ -*-=//
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

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormattedStream.h"

#include <set>
#include <stdio.h>

using namespace llvm;

namespace {
  class X86NaClRewritePass : public MachineFunctionPass {
  public:
    static char ID;
    X86NaClRewritePass() : MachineFunctionPass(&ID) {}

    virtual bool runOnMachineFunction(MachineFunction &Fn);

    virtual const char *getPassName() const {
      return "ARM SFI mask placement";
    }

  private:
    bool PassSandboxingStack(MachineBasicBlock &MBB);
    void PassLighweightValidator(MachineBasicBlock &MBB);
  };

  char X86NaClRewritePass::ID = 0;
}

// Note: this is a little adhoc and needs more work
static bool IsStackChange(MachineInstr &MI) {
  return MI.modifiesRegister(N86::ESP) ||
         MI.modifiesRegister(X86::RSP);
}


static bool IsIndirectControlFlowChange(MachineInstr &MI) {
  const unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
   default:
    return false;

   // Returns
   case X86::RET:
   case X86::RETI:
   case X86::TCRETURNdi:
   case X86::TCRETURNri:
   case X86::TCRETURNmi:
   case X86::TCRETURNdi64:
   case X86::TCRETURNri64:
   case X86::TCRETURNmi64:
   case X86::EH_RETURN:
   case X86::EH_RETURN64:
    return true;


    // Indirect Jumps
   case X86::JMP32r:
    //case X86::JMP32m:  // already banned in the td file
    return true;
    // Probably overkill - we do not expect these
   case X86::FARJMP16i:
   case X86::FARJMP32i:
   case X86::FARJMP16m:
   case X86::FARJMP32m:
    return true;
   case X86::CALL32r:
    return true;
  }
}


static bool IsFunctionCall(MachineInstr &MI) {
  const unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
   default:
    return false;
   case X86::CALL32r:
   case X86::CALLpcrel32:
    return true;
  }
}

static bool IsSandboxedStackChange(MachineInstr &MI) {
 const unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
   default:
    return false;
   case X86::NACL_ADD_SP:
   case X86::NACL_SUB_SP:
    return true;
  }
}


static bool DumpInstructionVerbose(MachineInstr &MI) {
  errs() << MI;
  errs() << MI.getNumOperands() << " operands:" << "\n";
  for (unsigned i = 0; i < MI.getNumOperands(); ++i) {
    const MachineOperand& op = MI.getOperand(i);
    errs() << "  " << i << ":" << op << "\n";

  }
  errs() << "\n";
}



/*
 * A primitive validator to catch problems at compile time
 */
void X86NaClRewritePass::PassLighweightValidator(MachineBasicBlock &MBB) {
  const TargetMachine &TM = MBB.getParent()->getTarget();

  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {

    MachineInstr &MI = *MBBI;

    if (TM.getSubtarget<X86Subtarget>().is64Bit()) {
      if (IsStackChange(MI)) {
        if (!IsSandboxedStackChange(MI)) {
            errs() << "@VALIDATOR: BAD STACKCHANGE\n\n";
            DumpInstructionVerbose(MI);
          }
      }

      if (IsIndirectControlFlowChange(MI)) {
        errs() << "@VALIDATOR: BAD INDIRECT JUMP\n\n";
        DumpInstructionVerbose(MI);
      }

      if (IsFunctionCall(MI)) {
        errs() << "@VALIDATOR: BAD FUNCTION CALL\n\n";
        DumpInstructionVerbose(MI);
      }
    }
  }
}

/*
 * Sandboxes stack changes (64 bit only)
 */

bool X86NaClRewritePass::PassSandboxingStack(MachineBasicBlock &MBB) {
  bool Modified = false;
  // TODO: disable this once we are more confident
  bool verbose = 1;
  const TargetInstrInfo* TII = MBB.getParent()->getTarget().getInstrInfo();

  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {

    MachineInstr &MI = *MBBI;
    if (IsStackChange(MI)) {
      const unsigned Opcode = MI.getOpcode();
      switch (Opcode) {
       default:
        errs() << "@PassSandboxingStack UNEXPEXTED STACK CHANGE\n\n";
        DumpInstructionVerbose(MI);
        assert(0);
       case X86::ADD64ri8:
        if (verbose) {
          errs() << "@PassSandboxingStack: BEFORE\n";
          DumpInstructionVerbose(MI);
        }
        MI.setDesc(TII->get(X86::NACL_ADD_SP));
        if (verbose) {
          errs() << "@PassSandboxingStack: AFTER\n";
          DumpInstructionVerbose(MI);
        }
        Modified = true;
        break;
       case X86::SUB64ri8:
        if (verbose) {
          errs() << "@PassSandboxingStack: BEFORE\n";
          DumpInstructionVerbose(MI);
        }
        MI.setDesc(TII->get(X86::NACL_SUB_SP));
        if (verbose) {
          errs() << "@PassSandboxingStack: AFTER\n";
          DumpInstructionVerbose(MI);
        }

        Modified = true;
        break;
      }
    }
  }
  return Modified;
}



bool X86NaClRewritePass::runOnMachineFunction(MachineFunction &MF) {
  const TargetMachine &TM = MF.getTarget();

  bool Modified = false;
  for (MachineFunction::iterator MFI = MF.begin(), E = MF.end();
       MFI != E;
       ++MFI) {
    // TODO: this should be controlled by a command line flag
    if (TM.getSubtarget<X86Subtarget>().is64Bit()) {
      Modified |= PassSandboxingStack(*MFI);
    }

    PassLighweightValidator(*MFI);
  }
  return Modified;
}


/// createX86NaClRewritePassPass - returns an instance of the pass.
namespace llvm {
FunctionPass* createX86NaClRewritePass() {
  return new X86NaClRewritePass();
}
}
