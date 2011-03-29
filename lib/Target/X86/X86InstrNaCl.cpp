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
  Tmp.addOperand(MCOperand::CreateReg(Reg64));    // DestReg
  Tmp.addOperand(MCOperand::CreateReg(Reg64));    // BaseReg
  Tmp.addOperand(MCOperand::CreateImm(1));        // Scale
  Tmp.addOperand(MCOperand::CreateReg(X86::R15)); // IndexReg
  Tmp.addOperand(MCOperand::CreateImm(0));        // Offset
  Tmp.addOperand(MCOperand::CreateReg(0));        // SegmentReg
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
  Tmp.addOperand(MCOperand::CreateImm(1));        // Scale
  Tmp.addOperand(MCOperand::CreateReg(0));        // IndexReg
  Tmp.addOperand(ImmOp);                          // Offset
  Tmp.addOperand(MCOperand::CreateReg(0));        // SegmentReg
  Out.EmitInstruction(Tmp);

  EmitRegFix(X86::RSP, Out);
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

static void EmitMoveRegReg(bool Is64Bit, unsigned ToReg,
                           unsigned FromReg, MCStreamer &Out) {
  MCInst Move;
  Move.setOpcode(Is64Bit ? X86::MOV64rr : X86::MOV32rr);
  Move.addOperand(MCOperand::CreateReg(ToReg));
  Move.addOperand(MCOperand::CreateReg(FromReg));
  Out.EmitInstruction(Move);
}

static void EmitMoveRegImm32(bool Is64Bit, unsigned ToReg,
                             unsigned Imm32, MCStreamer &Out) {
  MCInst MovInst;
  MovInst.setOpcode(X86::MOV32ri);
  MovInst.addOperand(MCOperand::CreateReg(X86::EBX));
  MovInst.addOperand(MCOperand::CreateImm(Imm32));
  Out.EmitInstruction(MovInst);
}

static void EmitCmove(bool Is64Bit, unsigned ToReg,
                      unsigned FromReg, MCStreamer &Out) {
  MCInst CmovInst;
  CmovInst.setOpcode(Is64Bit ? X86::CMOVE64rr : X86::CMOVE32rr);
  CmovInst.addOperand(MCOperand::CreateReg(ToReg));
  CmovInst.addOperand(MCOperand::CreateReg(ToReg));
  CmovInst.addOperand(MCOperand::CreateReg(FromReg));
  Out.EmitInstruction(CmovInst);
}

static void EmitClearReg(bool Is64Bit, unsigned Reg, MCStreamer &Out) {
  MCInst Clear;
  Clear.setOpcode(X86::XOR32rr);
  Clear.addOperand(MCOperand::CreateReg(Reg));
  Clear.addOperand(MCOperand::CreateReg(Reg));
  Clear.addOperand(MCOperand::CreateReg(Reg));
  Out.EmitInstruction(Clear);
}

static void EmitRegTruncate(unsigned Reg64, MCStreamer &Out) {
  unsigned Reg32 = getX86SubSuperRegister(Reg64, MVT::i32);
  EmitMoveRegReg(false, Reg32, Reg32, Out);
}

static void EmitLea(bool Is64Bit,
                    unsigned DestReg,
                    unsigned BaseReg,
                    unsigned Scale,
                    unsigned IndexReg,
                    unsigned Offset,
                    unsigned SegmentReg,
                    MCStreamer &Out) {
  MCInst Lea;
  Lea.setOpcode((Is64Bit ? X86::LEA64r : X86::LEA32r));
  Lea.addOperand(MCOperand::CreateReg(DestReg));
  Lea.addOperand(MCOperand::CreateReg(BaseReg));
  Lea.addOperand(MCOperand::CreateImm(Scale));
  Lea.addOperand(MCOperand::CreateReg(IndexReg));
  Lea.addOperand(MCOperand::CreateImm(Offset));
  Lea.addOperand(MCOperand::CreateReg(SegmentReg));
  Out.EmitInstruction(Lea);
}

static void EmitPushReg(bool Is64Bit, unsigned FromReg, MCStreamer &Out) {
  MCInst Push;
  Push.setOpcode(Is64Bit ? X86::PUSH64r : X86::PUSH32r);
  Push.addOperand(MCOperand::CreateReg(FromReg));
  Out.EmitInstruction(Push);
}

static void EmitPopReg(bool Is64Bit, unsigned ToReg, MCStreamer &Out) {
  MCInst Pop;
  Pop.setOpcode(Is64Bit ? X86::POP64r : X86::POP32r);
  Pop.addOperand(MCOperand::CreateReg(ToReg));
  Out.EmitInstruction(Pop);
}

static void EmitLoad(bool Is64Bit,
                     unsigned DestReg,
                     unsigned BaseReg,
                     unsigned Scale,
                     unsigned IndexReg,
                     unsigned Offset,
                     unsigned SegmentReg,
                     MCStreamer &Out) {
  // Load DestReg from address BaseReg + Scale * IndexReg + Offset
  MCInst Load;
  Load.setOpcode(Is64Bit ? X86::MOV64rm : X86::MOV32rm);
  Load.addOperand(MCOperand::CreateReg(DestReg));
  Load.addOperand(MCOperand::CreateReg(BaseReg));
  Load.addOperand(MCOperand::CreateImm(Scale));
  Load.addOperand(MCOperand::CreateReg(IndexReg));
  Load.addOperand(MCOperand::CreateImm(Offset));
  Load.addOperand(MCOperand::CreateReg(SegmentReg));
  Out.EmitInstruction(Load);
}

// Utility function for storing done by setjmp.
// Creates a store from Reg into the address PtrReg + Offset.
static void EmitStore(bool Is64Bit,
                      unsigned BaseReg,
                      unsigned Scale,
                      unsigned IndexReg,
                      unsigned Offset,
                      unsigned SegmentReg,
                      unsigned SrcReg,
                      MCStreamer &Out) {
  // Store SrcReg to address BaseReg + Scale * IndexReg + Offset
  MCInst Store;
  Store.setOpcode(Is64Bit ? X86::MOV64mr : X86::MOV32mr);
  Store.addOperand(MCOperand::CreateReg(BaseReg));
  Store.addOperand(MCOperand::CreateImm(Scale));
  Store.addOperand(MCOperand::CreateReg(IndexReg));
  Store.addOperand(MCOperand::CreateImm(Offset));
  Store.addOperand(MCOperand::CreateReg(SegmentReg));
  Store.addOperand(MCOperand::CreateReg(SrcReg));
  Out.EmitInstruction(Store);
}

static void EmitAndRegReg(bool Is64Bit, unsigned DestReg,
                          unsigned SrcReg, MCStreamer &Out) {
  MCInst AndInst;
  AndInst.setOpcode(X86::AND32rr);
  AndInst.addOperand(MCOperand::CreateReg(DestReg));
  AndInst.addOperand(MCOperand::CreateReg(DestReg));
  AndInst.addOperand(MCOperand::CreateReg(SrcReg));
  Out.EmitInstruction(AndInst);
}

static bool SandboxMemoryRef(MCInst *Inst,
                             unsigned *IndexReg,
                             MCStreamer &Out) {
  for (unsigned i = 0, last = Inst->getNumOperands(); i < last; i++) {
    if (!Inst->getOperand(i).isReg() ||
        Inst->getOperand(i).getReg() != X86::PSEUDO_NACL_SEG) {
      continue;
    }
    // Return the index register that will need to be truncated.
    // The order of operands on a memory reference is always:
    // (BaseReg, ScaleImm, IndexReg, DisplacementImm, SegmentReg),
    // So if we found a match for a segment register value, we know that
    // the index register is exactly two operands prior.
    *IndexReg = Inst->getOperand(i - 2).getReg();
    // Remove the PSEUDO_NACL_SEG annotation.
    Inst->getOperand(i).setReg(0);
    return true;
  }
  return false;
}

static void EmitREST(const MCInst &Inst, unsigned Reg32, bool IsMem, MCStreamer &Out) {
  unsigned Reg64 = getX86SubSuperRegister(Reg32, MVT::i64);
  Out.EmitBundleLock();
  if (!IsMem) {
    EmitMoveRegReg(false, Reg32, Inst.getOperand(0).getReg(), Out);
  } else {
    unsigned IndexReg;
    MCInst SandboxedInst = Inst;
    if (SandboxMemoryRef(&SandboxedInst, &IndexReg, Out)) {
      EmitRegTruncate(IndexReg, Out);
    }
    EmitLoad(false,
             Reg32,
             SandboxedInst.getOperand(0).getReg(),  // BaseReg
             SandboxedInst.getOperand(1).getImm(),  // Scale
             SandboxedInst.getOperand(2).getReg(),  // IndexReg
             SandboxedInst.getOperand(3).getImm(),  // Offset
             SandboxedInst.getOperand(4).getReg(),  // SegmentReg
             Out);
  }

  EmitRegFix(Reg64, Out);
  Out.EmitBundleUnlock();
}

// Does the platform specific portion of ELF start up.
// On entry 0(%esp) contains argc.  This function computes argv and envp
// from argc, and sets up a call to what will eventually be main.
// After popping argc, argv is simply the value of the stack pointer.
// Above argv[argc + 1] pointers is where envp should point.
// On 32 bit platforms we also mark the root frame for debuggers by clearing
// ebp.
// These need to be kept in sync with in lib/Target/ARM/ARMInstrInfo.td and
// lib/Target/X86/X86InstrNaCl.td.
static void EmitElfStart(bool Is64Bit, MCStreamer &Out) {
  unsigned StackPointer = Is64Bit ? X86::RSP : X86::ESP;
  unsigned ArgcReg = Is64Bit ? X86::RSI : X86::ESI;
  unsigned ArgvReg = Is64Bit ? X86::RCX : X86::ECX;
  unsigned EnvpReg = Is64Bit ? X86::RBX : X86::EBX;

  // Save argc.
  EmitPopReg(Is64Bit, ArgcReg, Out);
  // Save argv.
  EmitMoveRegReg(Is64Bit, ArgvReg, StackPointer, Out);
  // envp = argv + (4 * argc) + 4.
  EmitLea(Is64Bit, EnvpReg, ArgvReg, 4, ArgcReg, 4, 0, Out);
  // Align the stack 0mod16.
  MCInst AlignStack;
  AlignStack.setOpcode((Is64Bit ? X86::AND64ri32 : X86::AND32ri));
  AlignStack.addOperand(MCOperand::CreateReg(StackPointer));
  AlignStack.addOperand(MCOperand::CreateReg(StackPointer));
  AlignStack.addOperand(MCOperand::CreateImm(0xfffffff0));
  Out.EmitInstruction(AlignStack);

  if (Is64Bit) {
    // Set up the arguments to __nacl_startup.
    EmitMoveRegReg(true, ArgcReg, X86::RDI, Out);
    EmitMoveRegReg(true, ArgvReg, X86::RSI, Out);
    EmitMoveRegReg(true, EnvpReg, X86::RDX, Out);
  } else {
    // Set ebx to zero to indicate this is the root frame on the stack.
    EmitClearReg(false, X86::EBP, Out);
    // Align and set up the arguments to __nacl_startup.
    EmitPushReg(false, X86::EBP, Out);
    EmitPushReg(false, EnvpReg, Out);
    EmitPushReg(false, ArgvReg, Out);
    EmitPushReg(false, ArgcReg, Out);
  }
  EmitIndirectBranch(MCOperand::CreateReg(X86::EAX), Is64Bit, true, Out);
  MCInst Halt;
  Halt.setOpcode(X86::HLT);
  Out.EmitInstruction(Halt);
}

// Does the x86 platform specific work for setjmp.
// It expects that a pointer to a JMP_BUF in %ecx/%rdi, and that the return
// address is in %edx/%rdx.
// The JMP_BUF is a structure that has the maximum size over all supported
// architectures.  The callee-saves registers plus [er]ip and [er]sp are stored
// into the JMP_BUF.
static void EmitSetjmp(bool Is64Bit, MCStreamer &Out) {
  unsigned JmpBuf = Is64Bit ? X86::RDI : X86::ECX;
  unsigned RetAddr = Is64Bit ? X86::RDX : X86::EDX;
  if (Is64Bit) {
    unsigned BasePtr = X86::R15;
    unsigned Segment = X86::PSEUDO_NACL_SEG;
    // Save the registers.
    EmitStore(true, BasePtr, 1, JmpBuf,  0, Segment, X86::RBX, Out);
    EmitStore(true, BasePtr, 1, JmpBuf,  8, Segment, X86::RBP, Out);
    EmitStore(true, BasePtr, 1, JmpBuf, 16, Segment, X86::RSP, Out);
    EmitStore(true, BasePtr, 1, JmpBuf, 24, Segment, X86::R12, Out);
    EmitStore(true, BasePtr, 1, JmpBuf, 32, Segment, X86::R13, Out);
    EmitStore(true, BasePtr, 1, JmpBuf, 40, Segment, X86::R14, Out);
    EmitStore(true, BasePtr, 1, JmpBuf, 48, Segment, X86::RDX, Out);
  } else {
    // Save the registers.
    EmitStore(false, JmpBuf, 1, 0,  0, 0, X86::EBX, Out);
    EmitStore(false, JmpBuf, 1, 0,  4, 0, X86::EBP, Out);
    EmitStore(false, JmpBuf, 1, 0,  8, 0, X86::ESP, Out);
    EmitStore(false, JmpBuf, 1, 0, 12, 0, X86::ESI, Out);
    EmitStore(false, JmpBuf, 1, 0, 16, 0, X86::EDI, Out);
    EmitStore(false, JmpBuf, 1, 0, 20, 0, X86::EDX, Out);
  }
  // Return 0.
  EmitClearReg(false, X86::EAX, Out);
}

// Does the x86 platform specific work for longjmp other than normalizing the
// return parameter (returns of zero are changed to return 1 in the caller).
// It expects that a pointer to a JMP_BUF in %ecx/%rdi, and that the return
// value is in %eax.
// The JMP_BUF is a structure that has the maximum size over all supported
// architectures.  The saved registers are restored from the JMP_BUF.
static void EmitLongjmp(bool Is64Bit, MCStreamer &Out) {
  unsigned JmpBuf = Is64Bit ? X86::RDI : X86::ECX;
  // If the return value was 0, make it 1.
  EmitAndRegReg(false, X86::EAX, X86::EAX, Out);
  EmitMoveRegImm32(false, X86::EBX, 1, Out);
  EmitCmove(false, X86::EAX, X86::EBX, Out);
  if (Is64Bit) {
    unsigned BasePtr = X86::R15;
    unsigned Segment = X86::PSEUDO_NACL_SEG;
    // Restore the registers.
    EmitLoad(true, X86::RBX, BasePtr, 1, JmpBuf,  0, Segment, Out);
    EmitLoad(true, X86::RDX, BasePtr, 1, JmpBuf,  8, Segment, Out);
    // restbp
    Out.EmitBundleLock();
    EmitRegTruncate(X86::RBP, Out);
    EmitRegFix(X86::RBP, Out);
    Out.EmitBundleUnlock();
    EmitLoad(true, X86::RDX, BasePtr, 1, JmpBuf, 16, Segment, Out);
    // restsp
    Out.EmitBundleLock();
    EmitRegTruncate(X86::RSP, Out);
    EmitRegFix(X86::RSP, Out);
    Out.EmitBundleUnlock();
    EmitLoad(true, X86::R12, BasePtr, 1, JmpBuf, 24, Segment, Out);
    EmitLoad(true, X86::R13, BasePtr, 1, JmpBuf, 32, Segment, Out);
    EmitLoad(true, X86::R14, BasePtr, 1, JmpBuf, 40, Segment, Out);
    EmitLoad(true, X86::RDX, BasePtr, 1, JmpBuf, 48, Segment, Out);
  } else {
    // Restore the registers.
    EmitLoad(false, X86::EBX, JmpBuf, 1, 0,  0, 0, Out);
    EmitLoad(false, X86::EBP, JmpBuf, 1, 0,  4, 0, Out);
    EmitLoad(false, X86::ESP, JmpBuf, 1, 0,  8, 0, Out);
    EmitLoad(false, X86::ESI, JmpBuf, 1, 0, 12, 0, Out);
    EmitLoad(false, X86::EDI, JmpBuf, 1, 0, 16, 0, Out);
    EmitLoad(false, X86::ECX, JmpBuf, 1, 0, 20, 0, Out);
  }
  // Jmp to the saved return address.
  MCInst JMPInst;
  JMPInst.setOpcode(Is64Bit ? X86::NACL_JMP64r : X86::NACL_JMP32r);
  JMPInst.addOperand(MCOperand::CreateReg(X86::ECX));
  Out.EmitInstruction(JMPInst);
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
  // Intrinsics for eliminating platform specific .s code from the client
  // side link.  These are recognized in X86InstrNaCl.td.
  case X86::NACL_ELF_START32:
    EmitElfStart(false, Out);
    return true;
  case X86::NACL_ELF_START64:
    EmitElfStart(true, Out);
    return true;
  case X86::NACL_SETJ32:
    EmitSetjmp(false, Out);
    return true;
  case X86::NACL_SETJ64:
    EmitSetjmp(true, Out);
    return true;
  case X86::NACL_LONGJ32:
    EmitLongjmp(false, Out);
    return true;
  case X86::NACL_LONGJ64:
    EmitLongjmp(true, Out);
    return true;
  }

  unsigned IndexReg;
  MCInst SandboxedInst = Inst;
  if (SandboxMemoryRef(&SandboxedInst, &IndexReg, Out)) {
    unsigned PrefixLocal = PrefixSaved;
    PrefixSaved = 0;

    Out.EmitBundleLock();
    EmitRegTruncate(IndexReg, Out);
    if (PrefixLocal)
      EmitPrefix(PrefixLocal, Out);
    Out.EmitInstruction(SandboxedInst);
    Out.EmitBundleUnlock();
    return true;
  }

  if (PrefixSaved) {
    unsigned PrefixLocal = PrefixSaved;
    PrefixSaved = 0;
    EmitPrefix(PrefixLocal, Out);
  }
  return false;
}

} // namespace llvm
