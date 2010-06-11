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
    bool PassSandboxingStack(MachineBasicBlock &MBB,
                             const TargetInstrInfo* TII);
    bool PassSandboxingControlFlow(MachineBasicBlock &MBB,
                                   const TargetInstrInfo* TII,
                                   bool is64bit);
    bool PassSandboxingMassageLoadStore(MachineBasicBlock &MBB);
    bool PassSandboxingPopRbp(MachineBasicBlock &MBB,
                              const TargetInstrInfo* TII);
    void PassLighweightValidator(MachineBasicBlock &MBB, bool is64bit);
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
  // NOTE: for reference, but incomplete
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
  // NOTE: for reference, but incomplete
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


static bool IsPushPop(MachineInstr &MI) {
  const unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
   default:
    return false;
   case X86::PUSH64r:
   case X86::POP64r:
    return true;
  }
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
    return true;

    // Indirect Jumps
   case X86::JMP32r:
   case X86::JMP64r:
    return true;

   case X86::CALL32r:
   case X86::CALL64r:
   case X86::TAILJMPr64:
   case X86::TAILJMPr:
    return true;

    // Probably overkill - we do not expect these
   case X86::FARJMP16i:
   case X86::FARJMP32i:
   case X86::FARJMP16m:
   case X86::FARJMP32m:

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

   // trivially sandboxed
   case X86::PUSH64r:
   case X86::POP64r:
   case X86::NACL_POP_RBP:
    return true;
  }
}


static void DumpInstructionVerbose(MachineInstr &MI) {
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
void X86NaClRewritePass::PassLighweightValidator(MachineBasicBlock &MBB,
                                                 bool is64bit) {
  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {

    MachineInstr &MI = *MBBI;

    if (is64bit) {
      if (IsStackChange(MI)) {
        if (!IsSandboxedStackChange(MI)) {
            errs() << "@VALIDATOR: BAD STACKCHANGE\n\n";
            DumpInstructionVerbose(MI);
          }
      }

      if (IsIndirectControlFlowChange(MI)) {
        // TODO(robertm): add proper test
        errs() << "@VALIDATOR: BAD INDIRECT JUMP\n\n";
        DumpInstructionVerbose(MI);
      }

      if (IsFunctionCall(MI)) {
        // TODO(robertm): add proper test
        errs() << "@VALIDATOR: BAD FUNCTION CALL\n\n";
        DumpInstructionVerbose(MI);
      }

      if (IsStore(MI) && !IsPushPop(MI) ) {
        const MachineOperand &BaseReg  = MI.getOperand(0);
        const int reg = BaseReg.getReg();
        if (reg != X86::RSP &&
            reg != X86::RBP &&
            reg != X86::R15) {
          // TODO(robertm): add proper test
          errs() << "@VALIDATOR: STORE WITH BAD BASE\n\n";
          DumpInstructionVerbose(MI);
        }
      }
    }
  }
}

/*
 * Sandboxes stack changes (64 bit only)
 */

bool X86NaClRewritePass::PassSandboxingStack(MachineBasicBlock &MBB,
                                             const TargetInstrInfo* TII) {
  bool Modified = false;
  // TODO: disable this once we are more confident
  bool verbose = 0;

  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {

    MachineInstr &MI = *MBBI;

    if (IsStackChange(MI) && !IsSandboxedStackChange(MI)) {
      const unsigned Opcode = MI.getOpcode();
      switch (Opcode) {
       default:
        errs() << "@PassSandboxingStack UNEXPEXTED STACK CHANGE\n\n";
        DumpInstructionVerbose(MI);
        assert(0);

       case X86::ADD64ri8:
       case X86::ADD64ri32:
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
       case X86::SUB64ri32:
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

  // don't do anything if a safe register base is used
  if (MI.getOperand(Op + 2).getReg() == X86::RSP ||
      MI.getOperand(Op + 0).getReg() == X86::RSP ||
      MI.getOperand(Op + 2).getReg() == X86::RIP ||
      MI.getOperand(Op + 0).getReg() == X86::RIP ||
      MI.getOperand(Op + 2).getReg() == X86::RBP ||
      MI.getOperand(Op + 0).getReg() == X86::RBP)
    return false;

  // sneak in r15 as the base if possible
  // TODO: if a base reg is present, check whether it is a permissible reg
  if (doBase) {
    const MachineOperand &BaseReg  = MI.getOperand(Op + 0);
    if (!BaseReg.getReg()) {
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
    if (!SegmentReg.getReg()) {
      const_cast<MachineOperand&>(SegmentReg).setReg(X86::PSEUDO_NACL_SEG);
      Modified = true;

    }
  }
  return Modified;
}


bool X86NaClRewritePass::PassSandboxingMassageLoadStore(MachineBasicBlock &MBB) {
  bool Modified = false;
  // TODO: disable this once we are more confident
  bool verbose = 0;


  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {

    MachineInstr &MI = *MBBI;

    if (IsStore(MI) && !IsPushPop(MI)) {
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

    if (IsLoad(MI)&& !IsPushPop(MI)) {
      if (!isMem(&MI, 0)) {
        errs() << "@PassSandboxingMassageLoadStore: UNEXPECTED memory operand location\n";
        DumpInstructionVerbose(MI);
      } else {
        if (MassageMemoryOp(MI, 0, true, false, false)) {
          if (verbose) {
            errs() << "@PassSandboxingMassageLoadStore after massage\n";
            DumpInstructionVerbose(MI);

          }
          Modified = true;
        }
      }
    }
  }
  return Modified;
}


/*
 * Handle rbp restores
 */
bool X86NaClRewritePass::PassSandboxingPopRbp(MachineBasicBlock &MBB,
                                              const TargetInstrInfo* TII) {
  bool Modified = false;

  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {
    MachineInstr &MI = *MBBI;
    const unsigned Opcode = MI.getOpcode();
    if (Opcode != X86::POP64r) continue;
    const MachineOperand &Reg = MI.getOperand(0);
    if (Reg.getReg() != X86::RBP) continue;
    MI.setDesc(TII->get(X86::NACL_POP_RBP));
  }
  return Modified;
}

/*
 * Sandboxes stack changes (64 bit only)
 */

bool X86NaClRewritePass::PassSandboxingControlFlow(MachineBasicBlock &MBB,
                                                   const TargetInstrInfo* TII,
                                                   bool is64Bit) {
  bool Modified = false;

  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {

    MachineInstr &MI = *MBBI;

    if (!IsIndirectControlFlowChange(MI)) continue;

    const unsigned Opcode = MI.getOpcode();
    switch (Opcode) {
     default:
      errs() << "@PassSandboxingStack UNEXPECTED CONTROL FLOW CHANGE\n\n";
      DumpInstructionVerbose(MI);
      assert(0);
     case X86::CALL32r:
      MI.setDesc(TII->get(X86::NACL_CALL32r));
      Modified = true;
      break;

     case X86::JMP32r:
      MI.setDesc(TII->get(X86::NACL_JMP32r));
      Modified = true;
      break;

     case X86::TAILJMPr:
      MI.setDesc(TII->get(X86::NACL_TAILJMPr));
      Modified = true;
      break;

     case X86::RET:
      if (is64Bit) {
        MI.setDesc(TII->get(X86::NACL_RET64));
      } else {
        MI.setDesc(TII->get(X86::NACL_RET32));
      }
      Modified = true;
      break;

     case X86::JMP64r:
     case X86::CALL64r: {
      MI.setDesc(TII->get(X86::NACL_CALL64r));
      const MachineOperand &IndexReg  = MI.getOperand(0);
      const int reg32bit = Get32BitRegFor64BitReg(IndexReg.getReg());
      assert (reg32bit > 0);
      const_cast<MachineOperand&>(IndexReg).setReg(reg32bit);
      Modified = true;
      break;
     }

#if 0
      // We have not yet encountered this one
     case X86::TAILJMPr64:
      MI.setDesc(TII->get(X86::NACL_TAILJMPr64));
      Modified = true;
      break;
#endif
    }
  }
  return Modified;
}


bool X86NaClRewritePass::runOnMachineFunction(MachineFunction &MF) {
  bool Modified = false;

  const TargetMachine &TM = MF.getTarget();
  const TargetInstrInfo* TII = TM.getInstrInfo();

  const bool is64bit = TM.getSubtarget<X86Subtarget>().is64Bit();

  for (MachineFunction::iterator MFI = MF.begin(), E = MF.end();
       MFI != E;
       ++MFI) {
    // TODO: the passes should be controllable by a command line flag
    if (is64bit) {
      Modified |= PassSandboxingStack(*MFI, TII);
      Modified |= PassSandboxingMassageLoadStore(*MFI);
      Modified |= PassSandboxingPopRbp(*MFI, TII);
    }

    Modified |= PassSandboxingControlFlow(*MFI, TII, is64bit);
    PassLighweightValidator(*MFI, is64bit);
  }

  return Modified;
}


/// createX86NaClRewritePassPass - returns an instance of the pass.
namespace llvm {
FunctionPass* createX86NaClRewritePass() {
  return new X86NaClRewritePass();
}
}
