//=== X86InstrNaCl.cpp - Expansion of NaCl pseudo-instructions  --*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "x86-sandboxing"

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

// This option makes it possible to overwrite the x86 jmp mask immediate.
// Setting it to -1 will effectively turn masking into a nop which will
// help with linking this code with non-sandboxed libs (at least for x86-32).
cl::opt<int> FlagSfiX86JmpMask("sfi-x86-jmp-mask", cl::init(-32));

static unsigned PrefixSaved = 0;
static bool PrefixPass = false;

static void EmitDirectCall(const MCOperand &Op, bool Is64Bit,
                           MCStreamer &Out) {
  Out.EmitBundleAlignEnd();
  Out.EmitBundleLock();
  MCInst CALLInst;
  CALLInst.setOpcode(Is64Bit ? X86::CALL64pcrel32 : X86::CALLpcrel32);
  CALLInst.addOperand(Op);
  Out.EmitInstruction(CALLInst);
  Out.EmitBundleUnlock();
}

static void EmitRegTruncate(unsigned Reg64, MCStreamer &Out) {
  unsigned Reg32 = getX86SubSuperRegister(Reg64, MVT::i32);
  MCInst MOVInst;
  MOVInst.setOpcode(X86::MOV32rr);
  MOVInst.addOperand(MCOperand::CreateReg(Reg32));
  MOVInst.addOperand(MCOperand::CreateReg(Reg32));
  Out.EmitInstruction(MOVInst);
}

static void EmitIndirectBranch(const MCOperand &Op, bool Is64Bit, bool IsCall,
                               MCStreamer &Out) {
  const int JmpMask = FlagSfiX86JmpMask;
  const unsigned Reg32 = Op.getReg();
  const unsigned Reg64 = getX86SubSuperRegister(Reg32, MVT::i64);

  if (IsCall)
    Out.EmitBundleAlignEnd();

  Out.EmitBundleLock();

  MCInst ANDInst;
  ANDInst.setOpcode(X86::AND32ri8);
  ANDInst.addOperand(MCOperand::CreateReg(Reg32));
  ANDInst.addOperand(MCOperand::CreateReg(Reg32));
  ANDInst.addOperand(MCOperand::CreateImm(JmpMask));
  Out.EmitInstruction(ANDInst);

  if (Is64Bit) {
    MCInst InstADD;
    InstADD.setOpcode(X86::ADD64rr);
    InstADD.addOperand(MCOperand::CreateReg(Reg64));
    InstADD.addOperand(MCOperand::CreateReg(Reg64));
    InstADD.addOperand(MCOperand::CreateReg(X86::R15));
    Out.EmitInstruction(InstADD);
  }

  if (IsCall) {
    MCInst CALLInst;
    CALLInst.setOpcode(Is64Bit ? X86::CALL64r : X86::CALL32r);
    CALLInst.addOperand(MCOperand::CreateReg(Is64Bit ? Reg64 : Reg32));
    Out.EmitInstruction(CALLInst);
  } else {
    MCInst JMPInst;
    JMPInst.setOpcode(Is64Bit ? X86::JMP64r : X86::JMP32r);
    JMPInst.addOperand(MCOperand::CreateReg(Is64Bit ? Reg64 : Reg32));
    Out.EmitInstruction(JMPInst);
  }

  Out.EmitBundleUnlock();
}

static void EmitRet(const MCOperand *AmtOp, bool Is64Bit, MCStreamer &Out) {
  MCInst POPInst;
  POPInst.setOpcode(Is64Bit ? X86::POP64r : X86::POP32r);
  POPInst.addOperand(MCOperand::CreateReg(Is64Bit ? X86::RCX : X86::ECX));
  Out.EmitInstruction(POPInst);

  if (AmtOp) {
    assert(!Is64Bit);
    MCInst ADDInst;
    unsigned ADDReg = X86::ESP;
    ADDInst.setOpcode(X86::ADD32ri);
    ADDInst.addOperand(MCOperand::CreateReg(ADDReg));
    ADDInst.addOperand(MCOperand::CreateReg(ADDReg));
    ADDInst.addOperand(*AmtOp);
    Out.EmitInstruction(ADDInst);
  }

  MCInst JMPInst;
  JMPInst.setOpcode(Is64Bit ? X86::NACL_JMP64r : X86::NACL_JMP32r);
  JMPInst.addOperand(MCOperand::CreateReg(X86::ECX));
  Out.EmitInstruction(JMPInst);
}

void EmitTrap(bool Is64Bit, MCStreamer &Out) {
  // Rewrite to:
  //    X86-32:  mov $0, 0
  //    X86-64:  mov $0, (%r15)
  unsigned BaseReg = Is64Bit ? X86::R15 : 0;
  MCInst Tmp;
  Tmp.setOpcode(X86::MOV32mi);
  Tmp.addOperand(MCOperand::CreateReg(BaseReg)); // BaseReg
  Tmp.addOperand(MCOperand::CreateImm(1)); // Scale
  Tmp.addOperand(MCOperand::CreateReg(0)); // IndexReg
  Tmp.addOperand(MCOperand::CreateImm(0)); // Offset
  Tmp.addOperand(MCOperand::CreateReg(0)); // SegmentReg
  Tmp.addOperand(MCOperand::CreateImm(0)); // Value

  Out.EmitInstruction(Tmp);
}

// Fix a register after being truncated to 32-bits.
static void EmitRegFix(unsigned Reg64, MCStreamer &Out) {
  // lea (%rsp, %r15, 1), %rsp
  MCInst Tmp;
  Tmp.setOpcode(X86::LEA64r);
  Tmp.addOperand(MCOperand::CreateReg(Reg64)); // DestReg
  Tmp.addOperand(MCOperand::CreateReg(Reg64)); // BaseReg
  Tmp.addOperand(MCOperand::CreateImm(1)); // Scale
  Tmp.addOperand(MCOperand::CreateReg(X86::R15)); // IndexReg
  Tmp.addOperand(MCOperand::CreateImm(0)); // Offset
  Tmp.addOperand(MCOperand::CreateReg(0)); // SegmentReg
  Out.EmitInstruction(Tmp);
}

static void EmitSPArith(unsigned Opc, const MCOperand &ImmOp,
                        MCStreamer &Out) {
  Out.EmitBundleLock();

  MCInst Tmp;
  Tmp.setOpcode(Opc);
  Tmp.addOperand(MCOperand::CreateReg(X86::RSP));
  Tmp.addOperand(MCOperand::CreateReg(X86::RSP));
  Tmp.addOperand(ImmOp);
  Out.EmitInstruction(Tmp);

  EmitRegFix(X86::RSP, Out);
  Out.EmitBundleUnlock();
}

static void EmitSPAdj(const MCOperand &ImmOp, MCStreamer &Out) {
  Out.EmitBundleLock();

  MCInst Tmp;
  Tmp.setOpcode(X86::LEA64_32r);
  Tmp.addOperand(MCOperand::CreateReg(X86::RSP)); // DestReg
  Tmp.addOperand(MCOperand::CreateReg(X86::RBP)); // BaseReg
  Tmp.addOperand(MCOperand::CreateImm(1)); // Scale
  Tmp.addOperand(MCOperand::CreateReg(0)); // IndexReg
  Tmp.addOperand(ImmOp); // Offset
  Tmp.addOperand(MCOperand::CreateReg(0)); // SegmentReg
  Out.EmitInstruction(Tmp);

  EmitRegFix(X86::RSP, Out);
  Out.EmitBundleUnlock();
}

static void EmitREST(const MCInst &Inst, unsigned Reg32, bool IsMem, MCStreamer &Out) {
  unsigned Reg64 = getX86SubSuperRegister(Reg32, MVT::i64);

  Out.EmitBundleLock();
  MCInst MOVInst;
  if (!IsMem) {
    MOVInst.setOpcode(X86::MOV32rr);
    MOVInst.addOperand(MCOperand::CreateReg(Reg32));
    MOVInst.addOperand(Inst.getOperand(0));
  } else {
    // Do load/store sandbox also if needed
    unsigned SegmentReg = Inst.getOperand(4).getReg();
    if (SegmentReg == X86::PSEUDO_NACL_SEG) {
      unsigned IndexReg = Inst.getOperand(2).getReg();
      EmitRegTruncate(IndexReg, Out);
      SegmentReg = 0;
    }
    MOVInst.setOpcode(X86::MOV32rm);
    MOVInst.addOperand(MCOperand::CreateReg(Reg32));
    MOVInst.addOperand(Inst.getOperand(0)); // BaseReg
    MOVInst.addOperand(Inst.getOperand(1)); // Scale
    MOVInst.addOperand(Inst.getOperand(2)); // IndexReg
    MOVInst.addOperand(Inst.getOperand(3)); // Offset
    MOVInst.addOperand(MCOperand::CreateReg(SegmentReg)); // Segment
  }
  Out.EmitInstruction(MOVInst);

  EmitRegFix(Reg64, Out);
  Out.EmitBundleUnlock();
}

static void EmitPrefix(unsigned Opc, MCStreamer &Out) {
  assert(PrefixSaved == 0);
  assert(PrefixPass == false);

  MCInst PrefixInst;
  PrefixInst.setOpcode(Opc);
  PrefixPass = true;
  Out.EmitInstruction(PrefixInst);

  assert(PrefixSaved == 0);
  assert(PrefixPass == false);
}

namespace llvm {
// CustomExpandInstNaCl -
//   If Inst is a NaCl pseudo instruction, emits the substitute
//   expansion to the MCStreamer and returns true.
//   Otherwise, returns false.
//
//   NOTE: Each time this function calls Out.EmitInstruction(), it will be
//   called again recursively to rewrite the new instruction being emitted.
//   Care must be taken to ensure that this does not result in an infinite
//   loop. Also, global state must be managed carefully so that it is
//   consistent during recursive calls.
//
//   We need global state to keep track of the explicit prefix (PREFIX_*)
//   instructions. Unfortunately, the assembly parser prefers to generate
//   these instead of combined instructions. At this time, having only
//   one explicit prefix is supported.
bool CustomExpandInstNaCl(const MCInst &Inst, MCStreamer &Out) {
  // If we are emitting to .s, just emit all pseudo-instructions directly.
  if (Out.hasRawTextSupport()) {
    return false;
  }
  unsigned Opc = Inst.getOpcode();
  DEBUG(dbgs() << "CustomExpandInstNaCl("; Inst.dump(); dbgs() << ")\n");
  switch (Opc) {
  case X86::LOCK_PREFIX:
  case X86::REP_PREFIX:
  case X86::REPNE_PREFIX:
  case X86::REX64_PREFIX:
    // Ugly hack because LLVM AsmParser is not smart enough to combine
    // prefixes back into the instruction they modify.
    if (PrefixPass) {
      PrefixPass = false;
      PrefixSaved = 0;
      return false;
    }
    assert(PrefixSaved == 0);
    PrefixSaved = Opc;
    return true;
  case X86::NACL_TRAP32:
    assert(PrefixSaved == 0);
    EmitTrap(false, Out);
    return true;
  case X86::NACL_TRAP64:
    assert(PrefixSaved == 0);
    EmitTrap(true, Out);
    return true;
  case X86::NACL_CALL32d:
    assert(PrefixSaved == 0);
    EmitDirectCall(Inst.getOperand(0), false, Out);
    return true;
  case X86::NACL_CALL64d:
    assert(PrefixSaved == 0);
    EmitDirectCall(Inst.getOperand(0), true, Out);
    return true;
  case X86::NACL_CALL32r:
    assert(PrefixSaved == 0);
    EmitIndirectBranch(Inst.getOperand(0), false, true, Out);
    return true;
  case X86::NACL_CALL64r:
    assert(PrefixSaved == 0);
    EmitIndirectBranch(Inst.getOperand(0), true, true, Out);
    return true;
  case X86::NACL_JMP32r:
    assert(PrefixSaved == 0);
    EmitIndirectBranch(Inst.getOperand(0), false, false, Out);
    return true;
  case X86::NACL_JMP64r:
    assert(PrefixSaved == 0);
    EmitIndirectBranch(Inst.getOperand(0), true, false, Out);
    return true;
  case X86::NACL_RET32:
    assert(PrefixSaved == 0);
    EmitRet(NULL, false, Out);
    return true;
  case X86::NACL_RET64:
    assert(PrefixSaved == 0);
    EmitRet(NULL, true, Out);
    return true;
  case X86::NACL_RETI32:
    assert(PrefixSaved == 0);
    EmitRet(&Inst.getOperand(0), false, Out);
    return true;
  case X86::NACL_ASPi8:
    assert(PrefixSaved == 0);
    EmitSPArith(X86::ADD32ri8, Inst.getOperand(0), Out);
    return true;
  case X86::NACL_ASPi32:
    assert(PrefixSaved == 0);
    EmitSPArith(X86::ADD32ri, Inst.getOperand(0), Out);
    return true;
  case X86::NACL_SSPi8:
    assert(PrefixSaved == 0);
    EmitSPArith(X86::SUB32ri8, Inst.getOperand(0), Out);
    return true;
  case X86::NACL_SSPi32:
    assert(PrefixSaved == 0);
    EmitSPArith(X86::SUB32ri, Inst.getOperand(0), Out);
    return true;
  case X86::NACL_SPADJi32:
    assert(PrefixSaved == 0);
    EmitSPAdj(Inst.getOperand(0), Out);
    return true;
  case X86::NACL_RESTBPm:
    assert(PrefixSaved == 0);
    EmitREST(Inst, X86::EBP, true, Out);
    return true;
  case X86::NACL_RESTBPr:
    assert(PrefixSaved == 0);
    EmitREST(Inst, X86::EBP, false, Out);
    return true;
  case X86::NACL_RESTSPm:
    assert(PrefixSaved == 0);
    EmitREST(Inst, X86::ESP, true, Out);
    return true;
  case X86::NACL_RESTSPr:
    assert(PrefixSaved == 0);
    EmitREST(Inst, X86::ESP, false, Out);
    return true;
  }

  for (unsigned i=0, e = Inst.getNumOperands(); i != e; i++) {
    if (Inst.getOperand(i).isReg() &&
        Inst.getOperand(i).getReg() == X86::PSEUDO_NACL_SEG) {
      // Sandbox memory access
      unsigned IndexReg = Inst.getOperand(i-2).getReg();

      MCInst InstClean = Inst;
      InstClean.getOperand(i).setReg(0);

      unsigned PrefixLocal = PrefixSaved;
      PrefixSaved = 0;

      Out.EmitBundleLock();
      EmitRegTruncate(IndexReg, Out);
      if (PrefixLocal)
        EmitPrefix(PrefixLocal, Out);
      Out.EmitInstruction(InstClean);
      Out.EmitBundleUnlock();
      return true;
    }
  }

  if (PrefixSaved) {
    unsigned PrefixLocal = PrefixSaved;
    PrefixSaved = 0;
    EmitPrefix(PrefixLocal, Out);
  }
  return false;
}

} // namespace llvm
