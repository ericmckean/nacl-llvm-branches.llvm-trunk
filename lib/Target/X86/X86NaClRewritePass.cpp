//=== X86NaClRewritePAss.cpp - Rewrite instructions for NaCl SFI --*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that ensures stores and loads and stack/frame
// pointer addresses are within the NaCl sandbox (for x86-64).
// It also ensures that indirect control flow follows NaCl requirments.
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "x86-sandboxing"

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
  class X86NaClRewritePass : public MachineFunctionPass {
  public:
    static char ID;
    X86NaClRewritePass() : MachineFunctionPass(ID) {}

    virtual bool runOnMachineFunction(MachineFunction &Fn);

    virtual const char *getPassName() const {
      return "NaCl Rewrites";
    }

  private:

    const TargetMachine *TM;
    const TargetInstrInfo *TII;
    const TargetRegisterInfo *TRI;
    const X86Subtarget *Subtarget;

    bool IsStackChange(MachineInstr &MI);

    bool PassSandboxingStack(MachineBasicBlock &MBB,
                             const TargetInstrInfo* TII);
    bool PassSandboxingControlFlow(MachineBasicBlock &MBB,
                                   const TargetInstrInfo* TII);
    bool PassSandboxingLoadStore(MachineBasicBlock &MBB);
    bool PassSandboxingPopRbp(MachineBasicBlock &MBB,
                              const TargetInstrInfo* TII);
    void PassLightWeightValidator(MachineBasicBlock &MBB, bool is64bit);
    bool AlignJumpTableTargets(MachineFunction &MF);
  };

  char X86NaClRewritePass::ID = 0;

}

static void DumpInstructionVerbose(const MachineInstr &MI);


// Note: this is a little adhoc and needs more work
bool X86NaClRewritePass::IsStackChange(MachineInstr &MI) {
  return MI.modifiesRegister(X86::ESP, TRI) ||
         MI.modifiesRegister(X86::RSP, TRI);
}

static bool IsStore(MachineInstr &MI) {
  return MI.getDesc().mayStore();
}

static bool IsLoad(MachineInstr &MI) {
  return MI.getDesc().mayLoad();
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
    if (X86::AddrNumOperands < (signed)numOps
        && isMem(&MI, X86::AddrNumOperands)) {
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


static bool IsUnsandboxedControlFlow(MachineInstr &MI) {
  const unsigned Opcode = MI.getOpcode();
  switch (Opcode) {
   default:
    return false;

   case X86::TRAP:
     return true;

   // Returns
   case X86::RET:
   case X86::RETI:
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
   case X86::NACL_SET_SPr:
   case X86::NACL_SET_SPm:
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
void X86NaClRewritePass::PassLightWeightValidator(MachineBasicBlock &MBB,
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

      if (IsUnsandboxedControlFlow(MI)) {
        // TODO(robertm): add proper test
        dbgs() << "@VALIDATOR: BAD 64-bit INDIRECT JUMP\n\n";
        DumpInstructionVerbose(MI);
      }

      if (IsFunctionCall(MI)) {
        // TODO(robertm): add proper test
        dbgs() << "@VALIDATOR: BAD 64-bit FUNCTION CALL\n\n";
        DumpInstructionVerbose(MI);
      }

      if ((IsStore(MI) || IsLoad(MI)) && !IsPushPop(MI) ) {
        bool found;
        unsigned memOperand = FindMemoryOperand(MI, found);
        assert (found && "Load / Store without mem operand?");
        MachineOperand &BaseReg  = MI.getOperand(memOperand + 0);
        const unsigned breg = BaseReg.getReg();
        // Base should be a safe reg.
        if ((breg != X86::RSP && breg != X86::RBP &&
             breg != X86::R15 && breg != X86::RIP)) {
          // TODO(robertm): add proper test
          dbgs() << "@VALIDATOR: MEM OP WITH BAD BASE\n\n";
          DumpInstructionVerbose(MI);
          // assert (false);
        }
      }
    }
  }
}

bool X86NaClRewritePass::AlignJumpTableTargets(MachineFunction &MF) {
  bool Modified = true;

  MF.setAlignment(5); // log2, 32 = 2^5

  MachineJumpTableInfo *jt_info = MF.getJumpTableInfo();
  if (jt_info != NULL) {
    const std::vector<MachineJumpTableEntry> &JT = jt_info->getJumpTables();
    for (unsigned i = 0; i < JT.size(); ++i) {
      const std::vector<MachineBasicBlock*>& MBBs(JT[i].MBBs);
      for (unsigned j = 0; j < MBBs.size(); ++j) {
        MBBs[j]->setAlignment(32); // in bits
        Modified |= true;
      }
    }
  }
  return Modified;
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

    // TODO(pdox): We really need to generalize these modifications
    //             instead of handling them case-by-case.
    //             Unfortunately, this will have to wait until we can
    //             do bundle-aware assembly/code emission.

    if (IsStackChange(MI) && !IsSandboxedStackChange(MI)) {
      const unsigned Opcode = MI.getOpcode();
      switch (Opcode) {
       default:
         dbgs() << "@PassSandboxingStack UNEXPECTED STACK CHANGE\n\n";
         DumpInstructionVerbose(MI);
         assert(0);
         break;
       case X86::MOV32rr:
         DEBUG(dbgs() << "@PassSandboxingStack: BEFORE\n");
         DEBUG(DumpInstructionVerbose(MI));
         MI.setDesc(TII->get(X86::NACL_SET_SPr));
         DEBUG(dbgs() << "@PassSandboxingStack: AFTER\n");
         DEBUG(DumpInstructionVerbose(MI));
         Modified = true;
         break;
       case X86::MOV32rm:
         DEBUG(dbgs() << "@PassSandboxingStack: BEFORE\n");
         DEBUG(DumpInstructionVerbose(MI));
         MI.setDesc(TII->get(X86::NACL_SET_SPm));
         DEBUG(dbgs() << "@PassSandboxingStack: AFTER\n");
         DEBUG(DumpInstructionVerbose(MI));
         Modified = true;
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
 * Sandboxes loads and stores (64-bit only)
 */
static bool SandBoxMemoryOperand(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MBBI,
                                 unsigned Op) {
  bool Modified = false;
  MachineInstr &MI = *MBBI;
  assert(isMem(&MI, Op));
  MachineOperand &BaseReg  = MI.getOperand(Op + 0);
  MachineOperand &Scale = MI.getOperand(Op + 1);
  MachineOperand &IndexReg  = MI.getOperand(Op + 2);
  MachineOperand &Disp = MI.getOperand(Op + 3);
  MachineOperand &SegmentReg = MI.getOperand(Op + 4);

  // We need to make sure the index is using a 64-bit reg.
  const unsigned reg64bit = getX86SubSuperRegister(IndexReg.getReg(),
                                                   MVT::i64,
                                                   false);
  if (reg64bit) {
    IndexReg.setReg(reg64bit);

    DEBUG(dbgs() << "SandBoxMemoryOperand doIndex on (64 vs 32) "
          << reg64bit << " vs " << IndexReg.getReg() << "\n");

    DEBUG(DumpInstructionVerbose(MI));
    Modified = true;
  }

  bool isIndexAbsolute = (IndexReg.getReg() == X86::RSP ||
                          IndexReg.getReg() == X86::RBP ||
                          IndexReg.getReg() == X86::RIP);

  // Sneak in r15 as the base if needed
  // TODO: if a base reg is present, check whether it is a permissible reg
  if (!isIndexAbsolute && !BaseReg.getReg()) {
    BaseReg.setReg(X86::R15);

    DEBUG(dbgs() << "SandBoxMemoryOperand doBase\n");
    DEBUG(DumpInstructionVerbose(MI));
    Modified = true;
  }

  if (isIndexAbsolute)
    assert(BaseReg.getReg() == 0 &&
           "Unexpected base register with absolute index");

  // Is the index something we control / not present?
  // Otherwise, we need to clear the upper 32bits...
  bool isIndexSafe = IndexReg.getReg() == 0 || isIndexAbsolute;

  // Add an instruction immediately prior to truncate the upper 32-bits
  if (!isIndexSafe && !SegmentReg.getReg()) {
    SegmentReg.setReg(X86::PSEUDO_NACL_SEG);

    DEBUG(dbgs() << "SandBoxMemoryOperand doSeg\n");
    DEBUG(DumpInstructionVerbose(MI));

    Modified = true;
  }
  return Modified;
}


bool X86NaClRewritePass::PassSandboxingLoadStore(MachineBasicBlock &MBB) {
  bool Modified = false;

  assert(Subtarget->isTargetNaCl64());

  for (MachineBasicBlock::iterator MBBI = MBB.begin(); 
                                   MBBI != MBB.end(); ++MBBI) {
    MachineInstr &MI = *MBBI;

    if (IsPushPop(MI))
      continue;

    if (IsStore(MI)) {
      bool found = false;
      unsigned memOperand = FindMemoryOperand(MI, found);
      if (found) {
        if (SandBoxMemoryOperand(MBB, MBBI, memOperand)) {
          DEBUG(dbgs() << "@PassSandboxingLoadStore after massage op #"
                << memOperand << "\n");
          DEBUG(DumpInstructionVerbose(MI));
          Modified = true;
        }
      } else {
        dbgs() << "@MassageLoadStore: UNEXPECTED memory operand\n";
        DumpInstructionVerbose(MI);
      }
    }

    if (IsLoad(MI)) {
      bool found = false;
      unsigned memOperand = FindMemoryOperand(MI, found);
      if (found) {
        if (SandBoxMemoryOperand(MBB, MBBI, memOperand)) {
          DEBUG(dbgs() << "@PassSandboxingLoadStore after massage op #"
                << memOperand << "\n");
          DEBUG(DumpInstructionVerbose(MI));
          Modified = true;
        }
      } else {
        dbgs() << "@MassageLoadStore: UNEXPECTED memory operand\n";
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
bool X86NaClRewritePass::PassSandboxingControlFlow(
                             MachineBasicBlock &MBB, 
                             const TargetInstrInfo* TII) {
  bool is64Bit = Subtarget->is64Bit();
  bool Modified = false;

  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {

    MachineInstr &MI = *MBBI;

    if (!IsUnsandboxedControlFlow(MI)) continue;

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
     case X86::TRAP:
       if (is64Bit) {
         MI.setDesc(TII->get(X86::NACL_TRAP64));
       } else {
         MI.setDesc(TII->get(X86::NACL_TRAP32));
       }
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

     case X86::EH_RETURN:
     case X86::EH_RETURN64:
      // EH_RETURN has a single argment which is not actually used directly.
      // The argument gives the location where to reposition the stack pointer before returning.
      // EmitPrologue takes care of that repositioning.
      // So EH_RETURN just ultimately emits a plain "ret"

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

/* (This shouldn't ever be used anyway) */
//     case X86::JMP64r:{
//       MI.setDesc(TII->get(X86::NACL_JMP64r));
//       const MachineOperand &IndexReg  = MI.getOperand(0);
//       const unsigned reg32 = getX86SubSuperRegister(IndexReg.getReg(),
//                                                    MVT::i32,
//                                                    false)
//       assert (reg32 > 0);
//       const_cast<MachineOperand&>(IndexReg).setReg(reg32);
//       Modified = true;
//       break;
//     }

     case X86::CALL64r: {
// Made unnecessary by pattern matching
#if 0
      MI.setDesc(TII->get(X86::NACL_CALL64r));
      const MachineOperand &IndexReg  = MI.getOperand(0);
      const unsigned reg32 = getX86SubSuperRegister(IndexReg.getReg(),
                                                    MVT::i32,
                                                    false);
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

  TM = &MF.getTarget();
  TII = TM->getInstrInfo();
  TRI = TM->getRegisterInfo();
  Subtarget = &TM->getSubtarget<X86Subtarget>();


  assert(Subtarget->isTargetNaCl() &&
         "Unexpected target type in NaClRewritePass!");

  DEBUG(dbgs() << "*************** NaCl Rewrite Pass ***************\n");
  for (MachineFunction::iterator MFI = MF.begin(), E = MF.end();
       MFI != E;
       ++MFI) {
    if (Subtarget->isTargetNaCl64()) {
      Modified |= PassSandboxingStack(*MFI, TII);
      Modified |= PassSandboxingLoadStore(*MFI);
      Modified |= PassSandboxingPopRbp(*MFI, TII);
    }

    Modified |= PassSandboxingControlFlow(*MFI, TII);
    PassLightWeightValidator(*MFI, Subtarget->is64Bit());
  }

  AlignJumpTableTargets(MF);
  return Modified;
}


/// createX86NaClRewritePassPass - returns an instance of the pass.
namespace llvm {
FunctionPass* createX86NaClRewritePass() {
  return new X86NaClRewritePass();
}
}
