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
#define DEBUG_TYPE "x86-sandboxing"

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
      return "NaCl Rewrites";
    }

  private:
    bool PassSandboxingStack(MachineBasicBlock &MBB);
    bool PassSandboxingMassageLoadStore(MachineBasicBlock &MBB);
    void PassLighweightValidator(MachineBasicBlock &MBB);
  };

  char X86NaClRewritePass::ID = 0;
}

// Note: this is a little adhoc and needs more work
static bool IsStackChange(MachineInstr &MI) {
  return MI.modifiesRegister(N86::ESP) ||
         MI.modifiesRegister(X86::RSP);
}


// TODO(robertm): There may be better ways to figure out whether an
// instruction is a store
static bool IsStore(MachineInstr &MI) {
  return MI.getDesc().mayStore();
#if 0
  const unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
   default:
    return false;
   case X86::MOV8mr:
   case X86::MOV16mr:
   case X86::MOV32mr:
   case X86::MOV64mr:
   case X86::ST_FpP64m:
   case X86::MOVSSmr:
   case X86::MOVSDmr:
   case X86::MOVAPSmr:
   case X86::MOVAPDmr:
   case X86::MOVDQAmr:
   case X86::MMX_MOVD64mr:
   case X86::MMX_MOVQ64mr:
   case X86::MMX_MOVNTQmr:
    return true;
  }
#endif
}

static bool IsLoad(MachineInstr &MI) {
  return MI.getDesc().mayLoad();
#if 0
  const unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
   default:
    return false;
   case X86::MOV8rm:
   case X86::MOV16rm:
   case X86::MOV32rm:
   case X86::MOV64rm:
   case X86::LD_Fp64m:
   case X86::MOVSSrm:
   case X86::MOVSDrm:
   case X86::MOVAPSrm:
   case X86::MOVAPDrm:
   case X86::MOVDQArm:
   case X86::MMX_MOVD64rm:
   case X86::MMX_MOVQ64rm:
    return true;
  }
#endif
}

#define CASE(src, dst)  case X86:: src: return X86:: dst
static int Get32BitRegFor64BitReg(int reg64) {
  switch(reg64) {
   default:
    return 0;

  CASE(RAX,EAX);
  CASE(RDX,EDX);
  CASE(RCX,ECX);
  CASE(RBX,EBX);
  CASE(RSI,ESI);
  CASE(RDI,EDI);
  CASE(RBP,EBP);
  CASE(RSP,ESP);

  CASE(R8 ,R8D);
  CASE(R9 ,R9D);
  CASE(R10,R10D);
  CASE(R11,R11D);
  CASE(R12,R12D);
  CASE(R13,R13D);
  CASE(R14,R14D);
  CASE(R15,R15D);
  CASE(RIP,EIP);
  }
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
    errs() << "  " << i << "(" << op.getType() << "):" << op << "\n";

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


      if (IsStore(MI) ){
        errs() << "@VALIDATOR: STORE\n\n";
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
  bool verbose = 0;
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

       case X86::NACL_SUB_SP:
       case X86::NACL_ADD_SP:
        // these are our sandboxed versions and hence OK
        // not sure why we see them again here
        break;

       case X86::PUSH64r:
       case X86::POP64r:
        errs() << "@PassSandboxingStack STACK CHANGE NYI\n\n";
        DumpInstructionVerbose(MI);
        Modified = true;
        break;

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

/*
 * Sandboxes loads and stores via extra MachineOperand &BaseRegMachineOperand &BaseRegMachineOperand &BaseRegMachineOperand &BaseRegbase reg (64 bit only)
 */

static bool MassageMemoryOp(MachineInstr &MI, int Op, bool doBase, bool doIndex, bool doSeg) {
  bool Modified = false;
  assert(isMem(&MI, Op));

  // sneak in r15 as the base if possible
  // TODO: if a base reg is present, check whether it is a permissible reg
  if (doBase) {
    const MachineOperand &BaseReg  = MI.getOperand(Op + 0);
    if(!BaseReg.getReg()) {
      const_cast<MachineOperand&>(BaseReg).setReg((int)X86::R15);
      Modified = true;
    }
  }

  // we do not really need this as this is currently part of the assembler responsibility
  if (doIndex){
    const MachineOperand &IndexReg  = MI.getOperand(Op + 2);
    const int reg32bit = Get32BitRegFor64BitReg(IndexReg.getReg());
    if (reg32bit) {
      const_cast<MachineOperand&>(IndexReg).setReg(reg32bit);
      Modified = true;
    }
  }

  if (doSeg) {
    const MachineOperand &SegmentReg = MI.getOperand(Op + 4);
    if(!SegmentReg.getReg()) {
      const_cast<MachineOperand&>(SegmentReg).setReg(X86::PSEUDO_NACL_SEG);
      Modified = true;

    }
  }
  return Modified;
}

bool X86NaClRewritePass::PassSandboxingMassageLoadStore(MachineBasicBlock &MBB) {
  bool Modified = false;
  // TODO: disable this once we are more confident
  bool verbose = 1;


  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {

    MachineInstr &MI = *MBBI;

    if (IsStore(MI)) {
      if (!isMem(&MI, 0)) {
        errs() << "@PassSandboxingMassageLoadStore: UNEXPECTED memory operand location\n";
        DumpInstructionVerbose(MI);
      } else {
        if (MassageMemoryOp(MI, 0, true, false, true)) {
          if (verbose) {
            errs() << "@PassSandboxingMassageLoadStore after massage";
            DumpInstructionVerbose(MI);
          }
        }
        Modified = true;
      }
    }

    if (IsLoad(MI)) {
      if (!isMem(&MI, 0)) {
        errs() << "@PassSandboxingMassageLoadStore: UNEXPECTED memory operand location\n";
        DumpInstructionVerbose(MI);
      } else {
        if (MassageMemoryOp(MI, 0, true, false, false)) {
          if (verbose) {
            errs() << "@PassSandboxingMassageLoadStore after massage";
            DumpInstructionVerbose(MI);

          }
          Modified = true;
        }
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
      Modified |= PassSandboxingMassageLoadStore(*MFI);
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
