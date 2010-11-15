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
    bool Is64Bit;

    typedef enum {
      SFIStack,       // Stack Pointer (RSP) Modification
      SFIControl,     // CALL / RET / etc
      SFIFrame,       // Frame Pointer (RBP) Modification
      SFIMemory       // Load/Store
    } SFIType;

    struct SFIPattern {
      SFIType  Type;
      unsigned Arch;
      unsigned OrigOpcode;
      unsigned NewOpcode;
    };

    void InitPatternTable();
    const SFIPattern *SFIPatternTable;
    unsigned NumSFIPatterns;

    bool runOnMachineBasicBlock(MachineBasicBlock &MBB);

    bool ApplyStackSFI(MachineBasicBlock &MBB,
                       MachineBasicBlock::iterator MBBI);

    bool ApplyMemorySFI(MachineBasicBlock &MBB,
                        MachineBasicBlock::iterator MBBI);

    bool ApplyFrameSFI(MachineBasicBlock &MBB,
                       MachineBasicBlock::iterator MBBI);

    bool ApplyControlSFI(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator MBBI);

    const SFIPattern *FindPatternMatch(const MachineInstr &MI,
                                       SFIType Type);

    bool ApplyPattern(MachineInstr &MI,
                      const SFIPattern *Pat);


    void PassLightWeightValidator(MachineBasicBlock &MBB);
    bool AlignJumpTableTargets(MachineFunction &MF);
  };

  char X86NaClRewritePass::ID = 0;

}

// TODO(pdox): Get rid of this table when we've switched completely to
//             the new style for pseudo-instructions.

void X86NaClRewritePass::InitPatternTable() {
  static const SFIPattern TheTable[] = {
    { SFIStack,   64, X86::MOV32rr,     X86::NACL_SET_SPr  },
    { SFIStack,   64, X86::MOV64rr,     X86::NACL_SET_SPr  },
    { SFIStack,   64, X86::MOV32rm,     X86::NACL_SET_SPm  },
    { SFIStack,   64, X86::ADD64ri8,    X86::NACL_ADD_SP   },
    { SFIStack,   64, X86::ADD64ri32,   X86::NACL_ADD_SP   },
    { SFIStack,   64, X86::SUB64ri8,    X86::NACL_SUB_SP   },
    { SFIStack,   64, X86::SUB64ri32,   X86::NACL_SUB_SP   },
    { SFIStack,   64, X86::LEA64r,      X86::NACL_ADJ_SP   },

    { SFIControl, 32, X86::JMP32r,      X86::NACL_JMP32r   },
    { SFIControl, 32, X86::TAILJMPr,    X86::NACL_TAILJMPr },
    { SFIControl, 32, X86::TRAP,        X86::NACL_TRAP32   },
    { SFIControl, 64, X86::TRAP,        X86::NACL_TRAP64   },

    { SFIControl, 32, X86::RET,         X86::NACL_RET32,   },
    { SFIControl, 64, X86::RET,         X86::NACL_RET64    },
    { SFIControl, 32, X86::RETI,        X86::NACL_RETI32   },

    // EH_RETURN has a single argment which is not actually used directly.
    // The argument gives the location where to reposition the stack pointer
    // before returning. EmitPrologue takes care of that repositioning.
    // So EH_RETURN just ultimately emits a plain "ret"
    { SFIControl, 32, X86::EH_RETURN,   X86::NACL_RET32    },
    { SFIControl, 64, X86::EH_RETURN,   X86::NACL_RET64    },

    // Opcodes below are already safe
    { SFIControl, 32, X86::NACL_CALLpcrel32,   0 },
    { SFIControl, 32, X86::NACL_CALL32r,       0 },
    { SFIControl, 64, X86::NACL_CALL64r,       0 },
    { SFIControl, 64, X86::NACL_CALL64pcrel32, 0 },

    { SFIControl, 32, X86::NACL_TAILJMPr,      0 },
    { SFIControl, 32, X86::NACL_TAILJMPd,      0 },
    { SFIControl, 32, X86::NACL_TCRETURNri,    0 },
    { SFIControl, 32, X86::NACL_TCRETURNdi,    0 },

    { SFIControl, 64, X86::NACL_TCRETURNdi64,  0 },
    { SFIControl, 64, X86::NACL_TCRETURNri64,  0 },
    { SFIControl, 64, X86::NACL_TAILJMPd64,    0 },
    { SFIControl, 64, X86::NACL_TAILJMPr64,    0 },
    { SFIControl, 64, X86::NACL_JMP64r,        0 }
};
  SFIPatternTable = TheTable;
  NumSFIPatterns = sizeof(TheTable)/sizeof(*TheTable);
}

const X86NaClRewritePass::SFIPattern *
X86NaClRewritePass::FindPatternMatch(const MachineInstr &MI,
                                     SFIType Type) {
  unsigned Arch = Is64Bit ? 64 : 32;
  unsigned Opc = MI.getOpcode();

  for (unsigned i=0; i < NumSFIPatterns; i++) {
    const SFIPattern &Pat = SFIPatternTable[i];
    if (Pat.Type == Type &&
        Pat.Arch == Arch &&
        Pat.OrigOpcode == Opc) {
      return &Pat;
    }
  }
  return NULL;
}

// Apply the pattern instruction change.
// If a change occured, return true.
bool X86NaClRewritePass::ApplyPattern(MachineInstr &MI,
                                      const SFIPattern *Pat) {
  if (Pat->NewOpcode) {
    DEBUG(dbgs() << "@ApplyPattern INSTRUCTION BEFORE:\n");
    DEBUG(dbgs() << MI << "\n");
    MI.setDesc(TII->get(Pat->NewOpcode));
    DEBUG(dbgs() << "@ApplyPattern INSTRUCTION AFTER:\n");
    DEBUG(dbgs() << MI << "\n");
    return true;
  } else {
    return false;
  }
}

static void DumpInstructionVerbose(const MachineInstr &MI);

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

static bool IsStore(MachineInstr &MI) {
  return MI.getDesc().mayStore();
}

static bool IsLoad(MachineInstr &MI) {
  return MI.getDesc().mayLoad();
}

static bool IsFrameChange(MachineInstr &MI) {
  return MI.modifiesRegister(X86::EBP, NULL) ||
         MI.modifiesRegister(X86::RBP, NULL);
}

static bool IsStackChange(MachineInstr &MI) {
  return MI.modifiesRegister(X86::ESP, NULL) ||
         MI.modifiesRegister(X86::RSP, NULL);
}


static bool HasControlFlow(const MachineInstr &MI) {
 return MI.getDesc().isBranch() ||
        MI.getDesc().isCall() ||
        MI.getDesc().isReturn() ||
        MI.getDesc().isTerminator() ||
        MI.getDesc().isBarrier();
}

static bool IsDirectBranch(const MachineInstr &MI) {
  return  MI.getDesc().isBranch() &&
         !MI.getDesc().isIndirectBranch();
}

static bool IsRegAbsolute(unsigned Reg) {
  return (Reg == X86::RSP || Reg == X86::RBP ||
          Reg == X86::R15 || Reg == X86::RIP);
}

static unsigned FindMemoryOperand(const MachineInstr &MI) {
  int NumFound = 0;
  unsigned MemOp = 0;
  for (unsigned i = 0; i < MI.getNumOperands(); ) {
    if (isMem(&MI, i)) {
      NumFound++;
      MemOp = i;
      i += X86::AddrNumOperands;
    } else {
      i++;
    }
  }

  if (NumFound == 0)
    llvm_unreachable("Unable to find memory operands in load/store!");

  if (NumFound > 1)
    llvm_unreachable("Too many memory operands in instruction!");

  return MemOp;
}

static unsigned PromoteRegTo64(unsigned RegIn) {
  if (RegIn == 0)
    return 0;
  unsigned RegOut = getX86SubSuperRegister(RegIn, MVT::i64, false);
  assert(RegOut != 0);
  return RegOut;
}

//
// True if this MI restores RSP from RBP with a slight adjustment offset.
//
static bool MatchesSPAdj(const MachineInstr &MI) {
  assert (MI.getOpcode() == X86::LEA64r && "Call to MatchesSPAdj w/ non LEA");
  const MachineOperand &DestReg = MI.getOperand(0);
  const MachineOperand &BaseReg = MI.getOperand(1);
  const MachineOperand &Scale = MI.getOperand(2);
  const MachineOperand &IndexReg = MI.getOperand(3);
  const MachineOperand &Offset = MI.getOperand(4);
  return (DestReg.isReg() && DestReg.getReg() == X86::RSP &&
          BaseReg.isReg() && BaseReg.getReg() == X86::RBP &&
          Scale.getImm() == 1 &&
          IndexReg.isReg() && IndexReg.getReg() == 0 &&
          Offset.isImm());
}

bool X86NaClRewritePass::ApplyStackSFI(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator MBBI) {
  assert(Is64Bit);
  MachineInstr &MI = *MBBI;

  if (!IsStackChange(MI))
    return false;

  if (IsPushPop(MI))
    return false;

  unsigned Opc = MI.getOpcode();
  unsigned DestReg = MI.getOperand(0).getReg();
  assert(DestReg == X86::ESP || DestReg == X86::RSP);

  // Promote "MOV ESP, EBP" to a 64-bit move
  if (Opc == X86::MOV32rr && MI.getOperand(1).getReg() == X86::EBP) {
    MI.getOperand(0).setReg(X86::RSP);
    MI.getOperand(1).setReg(X86::RBP);
    MI.setDesc(TII->get(X86::MOV64rr));
    Opc = X86::MOV64rr;
  }

  // "MOV RBP, RSP" is already safe
  if (Opc == X86::MOV64rr && MI.getOperand(1).getReg() == X86::RBP) {
    return true;
  }

  //  Promote 32-bit lea to 64-bit lea (does this ever happen?)
  assert(Opc != X86::LEA32r && "Invalid opcode in 64-bit mode!");
  if (Opc == X86::LEA64_32r) {
    unsigned DestReg = MI.getOperand(0).getReg();
    unsigned BaseReg = MI.getOperand(1).getReg();
    unsigned Scale   = MI.getOperand(2).getImm();
    unsigned IndexReg = MI.getOperand(3).getReg();
    assert(DestReg == X86::ESP);
    assert(Scale == 1);
    assert(BaseReg == X86::EBP);
    assert(IndexReg == 0);
    MI.getOperand(0).setReg(X86::RSP);
    MI.getOperand(1).setReg(X86::RBP);
    MI.setDesc(TII->get(X86::LEA64r));
    Opc = X86::LEA64r;
  }

  // Make sure LEA64r matches the safe pattern
  if (Opc == X86::LEA64r) {
    assert(MatchesSPAdj(MI));
  }

  // General case
  const SFIPattern *Pat = FindPatternMatch(MI, SFIStack);
  if (Pat)
    return ApplyPattern(MI, Pat);

  DumpInstructionVerbose(MI);
  llvm_unreachable("Unhandled Stack SFI");
}

bool X86NaClRewritePass::ApplyFrameSFI(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator MBBI) {
  assert(Is64Bit);
  MachineInstr &MI = *MBBI;

  if (!IsFrameChange(MI))
    return false;

  unsigned Opc = MI.getOpcode();

  // MOV RBP, RSP is safe
  if (Opc == X86::MOV64rr) {
    assert(MI.getOperand(0).getReg() == X86::RBP);
    assert(MI.getOperand(1).getReg() == X86::RSP);
    return false;
  }

  // Popping onto RBP
  if (Opc == X86::POP64r) {
    MI.setDesc(TII->get(X86::NACL_POP_RBP));
    return true;
  }

  DumpInstructionVerbose(MI);
  llvm_unreachable("Unhandled Frame SFI");
}

bool X86NaClRewritePass::ApplyControlSFI(MachineBasicBlock &MBB,
                                         MachineBasicBlock::iterator MBBI) {
  MachineInstr &MI = *MBBI;

  if (!HasControlFlow(MI))
    return false;

  // Direct branches are OK
  if (IsDirectBranch(MI))
    return false;

  unsigned Opc = MI.getOpcode();

  // General Case
  const SFIPattern *Pat = FindPatternMatch(MI, SFIControl);
  if (Pat)
    return ApplyPattern(MI, Pat);

  DumpInstructionVerbose(MI);
  llvm_unreachable("Unhandled control flow");
}

//
// Sandboxes loads and stores (64-bit only)
//
bool X86NaClRewritePass::ApplyMemorySFI(MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator MBBI) {
  assert(Is64Bit);
  MachineInstr &MI = *MBBI;

  if (!IsLoad(MI) && !IsStore(MI))
    return false;

  if (IsPushPop(MI))
    return false;

  unsigned MemOp = FindMemoryOperand(MI);
  assert(isMem(&MI, MemOp));
  MachineOperand &BaseReg  = MI.getOperand(MemOp + 0);
  MachineOperand &Scale = MI.getOperand(MemOp + 1);
  MachineOperand &IndexReg  = MI.getOperand(MemOp + 2);
  //MachineOperand &Disp = MI.getOperand(MemOp + 3);
  MachineOperand &SegmentReg = MI.getOperand(MemOp + 4);

  // Make sure the base and index are 64-bit registers.
  IndexReg.setReg(PromoteRegTo64(IndexReg.getReg()));
  BaseReg.setReg(PromoteRegTo64(BaseReg.getReg()));
  assert(IndexReg.getSubReg() == 0);
  assert(BaseReg.getSubReg() == 0);

  bool AbsoluteBase = IsRegAbsolute(BaseReg.getReg());
  bool AbsoluteIndex = IsRegAbsolute(IndexReg.getReg());
  unsigned AddrReg = 0;

  if (AbsoluteBase && AbsoluteIndex) {
    llvm_unreachable("Unexpected absolute register pair");
  } else if (AbsoluteBase) {
    AddrReg = IndexReg.getReg();
  } else if (AbsoluteIndex) {
    assert(!BaseReg.getReg() && "Unexpected base register");
    assert(Scale.getImm() == 1);
    AddrReg = 0;
  } else {
    assert(!BaseReg.getReg() && "Unexpected relative register pair");
    BaseReg.setReg(X86::R15);
    AddrReg = IndexReg.getReg();
  }

  if (AddrReg) {
   assert(!SegmentReg.getReg() && "Unexpected segment register");
   SegmentReg.setReg(X86::PSEUDO_NACL_SEG);
  }
  return true;
}

bool X86NaClRewritePass::AlignJumpTableTargets(MachineFunction &MF) {
  bool Modified = true;

  MF.setAlignment(5); // log2, 32 = 2^5

  MachineJumpTableInfo *JTI = MF.getJumpTableInfo();
  if (JTI != NULL) {
    const std::vector<MachineJumpTableEntry> &JT = JTI->getJumpTables();
    for (unsigned i = 0; i < JT.size(); ++i) {
      const std::vector<MachineBasicBlock*> &MBBs = JT[i].MBBs;
      for (unsigned j = 0; j < MBBs.size(); ++j) {
        MBBs[j]->setAlignment(32); // in bits
        Modified |= true;
      }
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
  Is64Bit = Subtarget->is64Bit();

  assert(Subtarget->isTargetNaCl() && "Unexpected target in NaClRewritePass!");

  InitPatternTable();

  DEBUG(dbgs() << "*************** NaCl Rewrite Pass ***************\n");
  for (MachineFunction::iterator MFI = MF.begin(), E = MF.end();
       MFI != E;
       ++MFI) {
    Modified |= runOnMachineBasicBlock(*MFI);
    PassLightWeightValidator(*MFI);
  }
  Modified |= AlignJumpTableTargets(MF);
  return Modified;
}

bool X86NaClRewritePass::runOnMachineBasicBlock(MachineBasicBlock &MBB) {
  bool Modified = false;
  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {
    if (Is64Bit) {
      Modified |= ApplyStackSFI(MBB, MBBI);
      Modified |= ApplyMemorySFI(MBB, MBBI);
      Modified |= ApplyFrameSFI(MBB, MBBI);
    }
    Modified |= ApplyControlSFI(MBB, MBBI);
  }

  return Modified;
}


/// createX86NaClRewritePassPass - returns an instance of the pass.
namespace llvm {
  FunctionPass* createX86NaClRewritePass() {
    return new X86NaClRewritePass();
  }
}

// ======================== LIGHT-WEIGHT VALIDATOR ======================= //


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

//
// A primitive validator to catch problems at compile time
//
void X86NaClRewritePass::PassLightWeightValidator(MachineBasicBlock &MBB) {
  for (MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
       MBBI != E;
       ++MBBI) {

    MachineInstr &MI = *MBBI;

    if (Is64Bit) {
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

      if ((IsStore(MI) || IsLoad(MI)) && !IsPushPop(MI)) {
        unsigned memOperand = FindMemoryOperand(MI);
        // Base should be a safe reg.
        // If not, base should be unspecified, index should be a safe reg,
        // and Scale should be one.
        MachineOperand &BaseReg  = MI.getOperand(memOperand + 0);
        MachineOperand &Scale     = MI.getOperand(memOperand + 1);
        MachineOperand &IndexReg  = MI.getOperand(memOperand + 2);
        unsigned base_reg = BaseReg.getReg();
        unsigned maybe_safe_reg =
            base_reg ? base_reg : IndexReg.getReg();
        bool scale_safe = base_reg ? true : Scale.getImm() == 1;
        if (!scale_safe || !IsRegAbsolute(maybe_safe_reg)) {
          // TODO(robertm): add proper test
          dbgs() << "@VALIDATOR: MEM OP WITH BAD BASE\n\n";
          DumpInstructionVerbose(MI);
          // assert (false);
        }
      }
    }
  }
}
