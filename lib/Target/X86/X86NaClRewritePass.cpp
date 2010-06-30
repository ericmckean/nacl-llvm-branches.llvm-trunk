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
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/raw_ostream.h"

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

static void DumpInstructionVerbose(const MachineInstr &MI);


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

static unsigned FindMemoryOperand(const MachineInstr &MI, bool &found) {
  unsigned numOps = MI.getNumOperands();
  unsigned memPos = 0;

  found = false;
  if (numOps == 0) {
    return 0;
  }

  // Typical Store
  if (isMem(&MI, 0)) {
    found = true;
    if (X86AddrNumOperands < (signed)numOps
        && isMem(&MI, X86AddrNumOperands)) {
      dbgs() << "FindMemoryOperand multiple memory ops\n";
      DumpInstructionVerbose(MI);
      assert(false);
    }
    return 0;
  }

  // Typical Load
  if (MI.getOperand(0).isReg()) {
    if (numOps > 1 && isMem(&MI, 1)) {
      found = true;
      return 1;
    }
  }

  // Typical Arithmetic
  if (numOps > 2 && MI.getOperand(0).isReg() && MI.getOperand(1).isReg()) {
    if (isMem(&MI, 2)) {
      found = true;
      return 2;
    }
  }

  DumpInstructionVerbose(MI);
  assert(false && "FindMemoryOperand unknown case!");

  return memPos;
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


static bool is32BitReg(unsigned reg) {
  return (X86::GR32RegClass.contains(reg));
}

static bool is64BitReg(unsigned reg) {
  return (X86::GR64RegClass.contains(reg));
}

#define CASE(src, dst)  case X86:: src: return X86:: dst
static unsigned Get32BitRegFor64BitReg(unsigned reg64) {
  switch(reg64) {
    default: {
      if (is64BitReg(reg64)) {
        dbgs() << "Missed 64bit reg case in Get32BitRegFor64BitReg: "
               << reg64 << "\n";
      } else if (is32BitReg(reg64)) {
        dbgs() << "Get 32bit reg given 32bit reg\n";
        return reg64;
      } else {
        dbgs() << "Get 32bit Reg for 64bit reg, not given 64bit reg "
               << reg64 << "\n";
      }
      return 0;
    }

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

static unsigned Get64BitRegFor32BitReg(unsigned reg32) {
  switch(reg32) {
    default: {
      if (is32BitReg(reg32)) {
        dbgs() << "Missed 32bit reg case in Get64BitRegFor32BitReg: "
               << reg32 << "\n";
      } else if (is64BitReg(reg32)) {
        dbgs() << "Get 64bit reg given 64bit reg\n";
        return reg32;
      } else {
        dbgs() << "Get 64bit Reg for 32bit reg, not given 32bit reg "
               << reg32 << "\n";
      }
      return 0;
    }

  CASE(EAX,RAX);
  CASE(EDX,RDX);
  CASE(ECX,RCX);
  CASE(EBX,RBX);
  CASE(ESI,RSI);
  CASE(EDI,RDI);
  CASE(EBP,RBP);
  CASE(ESP,RSP);

  CASE(R8D,R8);
  CASE(R9D,R9);
  CASE(R10D,R10);
  CASE(R11D,R11);
  CASE(R12D,R12);
  CASE(R13D,R13);
  CASE(R14D,R14);
  CASE(R15D,R15);
  CASE(EIP,RIP);
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
   case X86::NACL_ADJ_SP:
    return true;

   // trivially sandboxed
   case X86::PUSH64r:
   case X86::POP64r:
   case X86::NACL_POP_RBP:
    return true;

   // copy from safe regs
   case X86::MOV64rr:
     const MachineOperand &DestReg = MI.getOperand(0);
     const MachineOperand &SrcReg = MI.getOperand(1);
     return DestReg.getReg() == X86::RSP && SrcReg.getReg() == X86::RBP;
  }
}


static void DumpInstructionVerbose(const MachineInstr &MI) {
  dbgs() << MI;
  dbgs() << MI.getNumOperands() << " operands:" << "\n";
  for (unsigned i = 0; i < MI.getNumOperands(); ++i) {
    const MachineOperand& op = MI.getOperand(i);
    dbgs() << "  " << i << "(" << op.getType() << "):" << op << "\n";
  }
  dbgs() << "\n";
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
            dbgs() << "@VALIDATOR: BAD STACKCHANGE\n\n";
            DumpInstructionVerbose(MI);
          }
      }

      if (IsIndirectControlFlowChange(MI)) {
        // TODO(robertm): add proper test
        dbgs() << "@VALIDATOR: BAD 64-bit INDIRECT JUMP\n\n";
        DumpInstructionVerbose(MI);
      }

      if (IsFunctionCall(MI)) {
        // TODO(robertm): add proper test
        // for now, we are restricting 64-bit to 32-bit regs
        // so we do get 32-bit calls
        dbgs() << "@VALIDATOR: BAD 64-bit FUNCTION CALL\n\n";
        //DumpInstructionVerbose(MI);
      }

      if (IsStore(MI) && !IsPushPop(MI) ) {
        const MachineOperand &BaseReg  = MI.getOperand(0);
        const unsigned reg = BaseReg.getReg();
        if (reg != X86::RSP &&
            reg != X86::RBP &&
            reg != X86::R15 &&
            reg != X86::RIP) {
          // TODO(robertm): add proper test
          dbgs() << "@VALIDATOR: STORE WITH BAD BASE\n\n";
          DumpInstructionVerbose(MI);
        }
      }
    }
  }
}

/*
 * True if this MI restores RSP from RBP with a slight adjustment offset.
 */
static bool MatchesSPAdj(const MachineInstr &MI) {
  assert (MI.getOpcode() == X86::LEA64r && "Call to MatchesSPAdj w/ non LEA");
  const MachineOperand &DestReg = MI.getOperand(0);
  const MachineOperand &BaseReg = MI.getOperand(1);
  const MachineOperand &Scale = MI.getOperand(2);
  const MachineOperand &IndexReg = MI.getOperand(3);
  const MachineOperand &Offset = MI.getOperand(4);
  return (DestReg.isReg() && DestReg.getReg() == X86::RSP &&
          BaseReg.isReg() && BaseReg.getReg() == X86::RBP &&
          Scale.isImm() && Scale.getImm() == 1 &&
          IndexReg.isReg() && IndexReg.getReg() == 0 &&
          Offset.isImm());
}

/*
 * Sandboxes stack changes (64 bit only)
 */
bool X86NaClRewritePass::PassSandboxingStack(MachineBasicBlock &MBB,
                                             const TargetInstrInfo* TII) {
  bool Modified = false;

  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {

    MachineInstr &MI = *MBBI;

    if (IsStackChange(MI) && !IsSandboxedStackChange(MI)) {
      const unsigned Opcode = MI.getOpcode();
      switch (Opcode) {
       default:
         dbgs() << "@PassSandboxingStack UNEXPECTED STACK CHANGE\n\n";
         DumpInstructionVerbose(MI);
         assert(0);
         break;

       case X86::ADD64ri8:
       case X86::ADD64ri32:
         DEBUG(dbgs() << "@PassSandboxingStack: BEFORE\n");
         DEBUG(DumpInstructionVerbose(MI));
         MI.setDesc(TII->get(X86::NACL_ADD_SP));
         DEBUG(dbgs() << "@PassSandboxingStack: AFTER\n");
         DEBUG(DumpInstructionVerbose(MI));
         Modified = true;
         break;

       case X86::SUB64ri8:
       case X86::SUB64ri32:
         DEBUG(dbgs() << "@PassSandboxingStack: BEFORE\n");
         DEBUG(DumpInstructionVerbose(MI));
         MI.setDesc(TII->get(X86::NACL_SUB_SP));
         DEBUG(dbgs() << "@PassSandboxingStack: AFTER\n");
         DEBUG(DumpInstructionVerbose(MI));
         Modified = true;
         break;

       // Restore from RBP with adjustment
       case X86::LEA64r:
         if (MatchesSPAdj(MI)) {
           DEBUG(dbgs() << "@PassSandboxingStack: BEFORE\n");
           DEBUG(DumpInstructionVerbose(MI));
           MI.setDesc(TII->get(X86::NACL_ADJ_SP));
           DEBUG(dbgs() << "@PassSandboxingStack: AFTER\n");
           DEBUG(DumpInstructionVerbose(MI));
         } else {
           dbgs() << "@PassSandboxingStack UNEXPECTED STACK CHANGE\n\n";
           DumpInstructionVerbose(MI);
           assert(false);
           break;
         }
      }
    }
  }
  return Modified;
}


/*
 * Sandboxes loads and stores via extra MachineOperand &BaseRegMachineOperand &BaseRegMachineOperand &BaseRegMachineOperand &BaseRegbase reg (64 bit only)
 */
static bool MassageMemoryOp(MachineInstr &MI, int Op,
                            bool doBase, bool doIndex, bool doSeg) {
  bool Modified = false;
  assert(isMem(&MI, Op));
  MachineOperand &BaseReg  = MI.getOperand(Op + 0);
  MachineOperand &IndexReg  = MI.getOperand(Op + 2);
  MachineOperand &SegmentReg = MI.getOperand(Op + 4);

  // We need to make sure the index is using a 64-bit reg.
  if (doIndex){
    const unsigned reg64bit = Get64BitRegFor32BitReg(IndexReg.getReg());
    if (reg64bit) {
      IndexReg.setReg(reg64bit);

      DEBUG(dbgs() << "MassageMemoryOp doIndex on (64 vs 32) "
            << reg64bit << " vs " << IndexReg.getReg() << "\n");

      DEBUG(DumpInstructionVerbose(MI));

      Modified = true;
    }
  }

  bool isIndexAbsolute = (IndexReg.getReg() == X86::RSP ||
                          IndexReg.getReg() == X86::RBP ||
                          IndexReg.getReg() == X86::RIP);

  // sneak in r15 as the base if possible
  // TODO: if a base reg is present, check whether it is a permissible reg
  if (doBase) {
    if (!isIndexAbsolute && !BaseReg.getReg()) {
      BaseReg.setReg(X86::R15);

      DEBUG(dbgs() << "MassageMemoryOp doBase\n");
      DEBUG(DumpInstructionVerbose(MI));
      Modified = true;
    }
  }

  if (isIndexAbsolute)
    assert(BaseReg.getReg() == 0 &&
           "Unexpected base register with absolute index");

  // Is the index something we control / not present?
  // Otherwise, we need to clear the upper 32bits...
  bool isIndexSafe = IndexReg.getReg() == 0 || isIndexAbsolute;

  if (doSeg) {
    if (!isIndexSafe && !SegmentReg.getReg()) {
      SegmentReg.setReg(X86::PSEUDO_NACL_SEG);

      DEBUG(dbgs() << "MassageMemoryOp doSeg\n");
      DEBUG(DumpInstructionVerbose(MI));

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

    if (IsPushPop(MI))
      continue;

    if (IsStore(MI)) {
      bool found = false;
      unsigned memOperand = FindMemoryOperand(MI, found);
      if (found) {
        dbgs() << "Massage Store Operand# " << memOperand << "\n";
        if (MassageMemoryOp(MI, memOperand, true, true, true)) {
          if (verbose) {
            dbgs() << "@PassSandboxingMassageLoadStore after massage";
            DumpInstructionVerbose(MI);
          }
        }
        Modified = true;
      } else {
        dbgs() << "@PassSandboxingMassageLoadStore: UNEXPECTED memory operand location\n";
        DumpInstructionVerbose(MI);
      }
    }

    if (IsLoad(MI)) {
      bool found = false;
      unsigned memOperand = FindMemoryOperand(MI, found);
      if (found) {
        dbgs() << "Massage Load Operand# " << memOperand << "\n";
        if (MassageMemoryOp(MI, memOperand, true, true, false)) {
            if (verbose) {
              dbgs() << "@PassSandboxingMassageLoadStore after massage\n";
              DumpInstructionVerbose(MI);
            }
            Modified = true;
          }
      } else {
        dbgs() << "@PassSandboxingMassageLoadStore: UNEXPECTED memory operand location\n";
        DumpInstructionVerbose(MI);
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


static void UnexpectedControlFlow(const MachineInstr &MI) {
  dbgs() << "@PassSandboxingControlFlow UNEXPECTED CONTROL FLOW CHANGE\n\n";
  DumpInstructionVerbose(MI);
  assert(false);
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
       UnexpectedControlFlow(MI);
     case X86::CALL32r:
// Made unnecessary by pattern matching
#if 0
       if (is64Bit) {
         // use NACL_CALL64r when in 64bit mode (so that rzp is inserted)
         DEBUG(dbgs() << "Switching CALL32r to NACL_CALL64r\n");
         MI.setDesc(TII->get(X86::NACL_CALL64r));
         assert (is32BitReg(MI.getOperand(0).getReg())
                 && "CALL32r w/ non-32bit reg");
       } else {
         MI.setDesc(TII->get(X86::NACL_CALL32r));
       }
      Modified = true;
#endif
      break;

     case X86::JMP32r:
       if (is64Bit) {
         // use NACL_JMP64r when in 64bit mode (so that rzp is inserted)
         DEBUG(dbgs() << "Switching JMP32r to NACL_JMP64r\n");
         MI.setDesc(TII->get(X86::NACL_JMP64r));
         assert (is32BitReg(MI.getOperand(0).getReg()) &&
                 "JMP32r w/ non-32bit reg");
       } else {
         MI.setDesc(TII->get(X86::NACL_JMP32r));
       }
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

     case X86::RETI:
       if (is64Bit) {
         // Not yet sure when this is needed.
         UnexpectedControlFlow(MI);
       } else {
         MI.setDesc(TII->get(X86::NACL_RETI32));
       }
       Modified = true;
       break;

     case X86::JMP64r:{
       MI.setDesc(TII->get(X86::NACL_JMP64r));
       const MachineOperand &IndexReg  = MI.getOperand(0);
       const unsigned reg32 = Get32BitRegFor64BitReg(IndexReg.getReg());
       assert (reg32 > 0);
       const_cast<MachineOperand&>(IndexReg).setReg(reg32);
       Modified = true;
       break;
     }
     case X86::CALL64r: {
// Made unnecessary by pattern matching
#if 0
      MI.setDesc(TII->get(X86::NACL_CALL64r));
      const MachineOperand &IndexReg  = MI.getOperand(0);
      const unsigned reg32 = Get32BitRegFor64BitReg(IndexReg.getReg());
      assert (reg32 > 0);
      const_cast<MachineOperand&>(IndexReg).setReg(reg32);
      Modified = true;
#endif
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
