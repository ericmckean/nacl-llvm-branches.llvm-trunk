//===- BlackfinRegisterInfo.cpp - Blackfin Register Information -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the Blackfin implementation of the TargetRegisterInfo
// class.
//
//===----------------------------------------------------------------------===//

#include "Blackfin.h"
#include "BlackfinRegisterInfo.h"
#include "BlackfinSubtarget.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineLocation.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/Target/TargetFrameInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Type.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"
using namespace llvm;

BlackfinRegisterInfo::BlackfinRegisterInfo(BlackfinSubtarget &st,
                                           const TargetInstrInfo &tii)
  : BlackfinGenRegisterInfo(BF::ADJCALLSTACKDOWN, BF::ADJCALLSTACKUP),
    Subtarget(st),
    TII(tii) {}

const unsigned*
BlackfinRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  using namespace BF;
  static const unsigned CalleeSavedRegs[] = {
    FP,
    R4, R5, R6, R7,
    P3, P4, P5,
    0 };
  return  CalleeSavedRegs;
}

const TargetRegisterClass* const *BlackfinRegisterInfo::
getCalleeSavedRegClasses(const MachineFunction *MF) const {
  using namespace BF;
  static const TargetRegisterClass * const CalleeSavedRegClasses[] = {
    &PRegClass,
    &DRegClass, &DRegClass, &DRegClass, &DRegClass,
    &PRegClass, &PRegClass, &PRegClass,
    0 };
  return CalleeSavedRegClasses;
}

BitVector
BlackfinRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  using namespace BF;
  BitVector Reserved(getNumRegs());
  Reserved.set(AZ);
  Reserved.set(AN);
  Reserved.set(AQ);
  Reserved.set(AC0);
  Reserved.set(AC1);
  Reserved.set(AV0);
  Reserved.set(AV0S);
  Reserved.set(AV1);
  Reserved.set(AV1S);
  Reserved.set(V);
  Reserved.set(VS);
  Reserved.set(CYCLES).set(CYCLES2);
  Reserved.set(L0);
  Reserved.set(L1);
  Reserved.set(L2);
  Reserved.set(L3);
  Reserved.set(SP);
  Reserved.set(RETS);
  if (hasFP(MF))
    Reserved.set(FP);
  return Reserved;
}

const TargetRegisterClass*
BlackfinRegisterInfo::getPhysicalRegisterRegClass(unsigned reg, EVT VT) const {
  assert(isPhysicalRegister(reg) && "reg must be a physical register");

  // Pick the smallest register class of the right type that contains
  // this physreg.
  const TargetRegisterClass* BestRC = 0;
  for (regclass_iterator I = regclass_begin(), E = regclass_end();
       I != E; ++I) {
    const TargetRegisterClass* RC = *I;
    if ((VT == MVT::Other || RC->hasType(VT)) && RC->contains(reg) &&
        (!BestRC || RC->getNumRegs() < BestRC->getNumRegs()))
      BestRC = RC;
  }

  assert(BestRC && "Couldn't find the register class");
  return BestRC;
}

// hasFP - Return true if the specified function should have a dedicated frame
// pointer register.  This is true if the function has variable sized allocas or
// if frame pointer elimination is disabled.
bool BlackfinRegisterInfo::hasFP(const MachineFunction &MF) const {
  const MachineFrameInfo *MFI = MF.getFrameInfo();
  return NoFramePointerElim || MFI->hasCalls() || MFI->hasVarSizedObjects();
}

bool BlackfinRegisterInfo::
requiresRegisterScavenging(const MachineFunction &MF) const {
  return true;
}

// Emit instructions to add delta to D/P register. ScratchReg must be of the
// same class as Reg (P).
void BlackfinRegisterInfo::adjustRegister(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator I,
                                          DebugLoc DL,
                                          unsigned Reg,
                                          unsigned ScratchReg,
                                          int delta) const {
  if (!delta)
    return;
  if (isInt<7>(delta)) {
    BuildMI(MBB, I, DL, TII.get(BF::ADDpp_imm7), Reg)
      .addReg(Reg)              // No kill on two-addr operand
      .addImm(delta);
    return;
  }

  // We must load delta into ScratchReg and add that.
  loadConstant(MBB, I, DL, ScratchReg, delta);
  if (BF::PRegClass.contains(Reg)) {
    assert(BF::PRegClass.contains(ScratchReg) &&
           "ScratchReg must be a P register");
    BuildMI(MBB, I, DL, TII.get(BF::ADDpp), Reg)
      .addReg(Reg, RegState::Kill)
      .addReg(ScratchReg, RegState::Kill);
  } else {
    assert(BF::DRegClass.contains(Reg) && "Reg must be a D or P register");
    assert(BF::DRegClass.contains(ScratchReg) &&
           "ScratchReg must be a D register");
    BuildMI(MBB, I, DL, TII.get(BF::ADD), Reg)
      .addReg(Reg, RegState::Kill)
      .addReg(ScratchReg, RegState::Kill);
  }
}

// Emit instructions to load a constant into D/P register
void BlackfinRegisterInfo::loadConstant(MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator I,
                                        DebugLoc DL,
                                        unsigned Reg,
                                        int value) const {
  if (isInt<7>(value)) {
    BuildMI(MBB, I, DL, TII.get(BF::LOADimm7), Reg).addImm(value);
    return;
  }

  if (isUint<16>(value)) {
    BuildMI(MBB, I, DL, TII.get(BF::LOADuimm16), Reg).addImm(value);
    return;
  }

  if (isInt<16>(value)) {
    BuildMI(MBB, I, DL, TII.get(BF::LOADimm16), Reg).addImm(value);
    return;
  }

  // We must split into halves
  BuildMI(MBB, I, DL,
          TII.get(BF::LOAD16i), getSubReg(Reg, bfin_subreg_hi16))
    .addImm((value >> 16) & 0xffff)
    .addReg(Reg, RegState::ImplicitDefine);
  BuildMI(MBB, I, DL,
          TII.get(BF::LOAD16i), getSubReg(Reg, bfin_subreg_lo16))
    .addImm(value & 0xffff)
    .addReg(Reg, RegState::ImplicitKill)
    .addReg(Reg, RegState::ImplicitDefine);
}

void BlackfinRegisterInfo::
eliminateCallFramePseudoInstr(MachineFunction &MF,
                              MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator I) const {
  if (!hasReservedCallFrame(MF)) {
    int64_t Amount = I->getOperand(0).getImm();
    if (Amount != 0) {
      assert(Amount%4 == 0 && "Unaligned call frame size");
      if (I->getOpcode() == BF::ADJCALLSTACKDOWN) {
        adjustRegister(MBB, I, I->getDebugLoc(), BF::SP, BF::P1, -Amount);
      } else {
        assert(I->getOpcode() == BF::ADJCALLSTACKUP &&
               "Unknown call frame pseudo instruction");
        adjustRegister(MBB, I, I->getDebugLoc(), BF::SP, BF::P1, Amount);
      }
    }
  }
  MBB.erase(I);
}

/// findScratchRegister - Find a 'free' register. Try for a call-clobbered
/// register first and then a spilled callee-saved register if that fails.
static unsigned findScratchRegister(MachineBasicBlock::iterator II,
                                    RegScavenger *RS,
                                    const TargetRegisterClass *RC,
                                    int SPAdj) {
  assert(RS && "Register scavenging must be on");
  unsigned Reg = RS->FindUnusedReg(RC);
  if (Reg == 0)
    Reg = RS->scavengeRegister(RC, II, SPAdj);
  return Reg;
}

unsigned
BlackfinRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                          int SPAdj, FrameIndexValue *Value,
                                          RegScavenger *RS) const {
  MachineInstr &MI = *II;
  MachineBasicBlock &MBB = *MI.getParent();
  MachineFunction &MF = *MBB.getParent();
  DebugLoc DL = MI.getDebugLoc();

  unsigned FIPos;
  for (FIPos=0; !MI.getOperand(FIPos).isFI(); ++FIPos) {
    assert(FIPos < MI.getNumOperands() &&
           "Instr doesn't have FrameIndex operand!");
  }
  int FrameIndex = MI.getOperand(FIPos).getIndex();
  assert(FIPos+1 < MI.getNumOperands() && MI.getOperand(FIPos+1).isImm());
  int Offset = MF.getFrameInfo()->getObjectOffset(FrameIndex)
    + MI.getOperand(FIPos+1).getImm();
  unsigned BaseReg = BF::FP;
  if (hasFP(MF)) {
    assert(SPAdj==0 && "Unexpected SP adjust in function with frame pointer");
  } else {
    BaseReg = BF::SP;
    Offset += MF.getFrameInfo()->getStackSize() + SPAdj;
  }

  bool isStore = false;

  switch (MI.getOpcode()) {
  case BF::STORE32fi:
    isStore = true;
  case BF::LOAD32fi: {
    assert(Offset%4 == 0 && "Unaligned i32 stack access");
    assert(FIPos==1 && "Bad frame index operand");
    MI.getOperand(FIPos).ChangeToRegister(BaseReg, false);
    MI.getOperand(FIPos+1).setImm(Offset);
    if (isUint<6>(Offset)) {
      MI.setDesc(TII.get(isStore
                         ? BF::STORE32p_uimm6m4
                         : BF::LOAD32p_uimm6m4));
      return 0;
    }
    if (BaseReg == BF::FP && isUint<7>(-Offset)) {
      MI.setDesc(TII.get(isStore
                         ? BF::STORE32fp_nimm7m4
                         : BF::LOAD32fp_nimm7m4));
      MI.getOperand(FIPos+1).setImm(-Offset);
      return 0;
    }
    if (isInt<18>(Offset)) {
      MI.setDesc(TII.get(isStore
                         ? BF::STORE32p_imm18m4
                         : BF::LOAD32p_imm18m4));
      return 0;
    }
    // Use RegScavenger to calculate proper offset...
    MI.dump();
    llvm_unreachable("Stack frame offset too big");
    break;
  }
  case BF::ADDpp: {
    assert(MI.getOperand(0).isReg() && "ADD instruction needs a register");
    unsigned DestReg = MI.getOperand(0).getReg();
    // We need to produce a stack offset in a P register. We emit:
    // P0 = offset;
    // P0 = BR + P0;
    assert(FIPos==1 && "Bad frame index operand");
    loadConstant(MBB, II, DL, DestReg, Offset);
    MI.getOperand(1).ChangeToRegister(DestReg, false, false, true);
    MI.getOperand(2).ChangeToRegister(BaseReg, false);
    break;
  }
  case BF::STORE16fi:
    isStore = true;
  case BF::LOAD16fi: {
    assert(Offset%2 == 0 && "Unaligned i16 stack access");
    assert(FIPos==1 && "Bad frame index operand");
    // We need a P register to use as an address
    unsigned ScratchReg = findScratchRegister(II, RS, &BF::PRegClass, SPAdj);
    assert(ScratchReg && "Could not scavenge register");
    loadConstant(MBB, II, DL, ScratchReg, Offset);
    BuildMI(MBB, II, DL, TII.get(BF::ADDpp), ScratchReg)
      .addReg(ScratchReg, RegState::Kill)
      .addReg(BaseReg);
    MI.setDesc(TII.get(isStore ? BF::STORE16pi : BF::LOAD16pi));
    MI.getOperand(1).ChangeToRegister(ScratchReg, false, false, true);
    MI.RemoveOperand(2);
    break;
  }
  case BF::STORE8fi: {
    // This is an AnyCC spill, we need a scratch register.
    assert(FIPos==1 && "Bad frame index operand");
    MachineOperand SpillReg = MI.getOperand(0);
    unsigned ScratchReg = findScratchRegister(II, RS, &BF::DRegClass, SPAdj);
    assert(ScratchReg && "Could not scavenge register");
    if (SpillReg.getReg()==BF::NCC) {
      BuildMI(MBB, II, DL, TII.get(BF::MOVENCC_z), ScratchReg)
        .addOperand(SpillReg);
      BuildMI(MBB, II, DL, TII.get(BF::BITTGL), ScratchReg)
        .addReg(ScratchReg).addImm(0);
    } else {
      BuildMI(MBB, II, DL, TII.get(BF::MOVECC_zext), ScratchReg)
        .addOperand(SpillReg);
    }
    // STORE D
    MI.setDesc(TII.get(BF::STORE8p_imm16));
    MI.getOperand(0).ChangeToRegister(ScratchReg, false, false, true);
    MI.getOperand(FIPos).ChangeToRegister(BaseReg, false);
    MI.getOperand(FIPos+1).setImm(Offset);
    break;
  }
  case BF::LOAD8fi: {
    // This is an restore, we need a scratch register.
    assert(FIPos==1 && "Bad frame index operand");
    MachineOperand SpillReg = MI.getOperand(0);
    unsigned ScratchReg = findScratchRegister(II, RS, &BF::DRegClass, SPAdj);
    assert(ScratchReg && "Could not scavenge register");
    MI.setDesc(TII.get(BF::LOAD32p_imm16_8z));
    MI.getOperand(0).ChangeToRegister(ScratchReg, true);
    MI.getOperand(FIPos).ChangeToRegister(BaseReg, false);
    MI.getOperand(FIPos+1).setImm(Offset);
    ++II;
    if (SpillReg.getReg()==BF::CC) {
      // CC = D
      BuildMI(MBB, II, DL, TII.get(BF::MOVECC_nz), BF::CC)
        .addReg(ScratchReg, RegState::Kill);
    } else {
      // Restore NCC (CC = D==0)
      BuildMI(MBB, II, DL, TII.get(BF::SETEQri_not), BF::NCC)
        .addReg(ScratchReg, RegState::Kill)
        .addImm(0);
    }
    break;
  }
  default:
    llvm_unreachable("Cannot eliminate frame index");
    break;
  }
  return 0;
}

void BlackfinRegisterInfo::
processFunctionBeforeCalleeSavedScan(MachineFunction &MF,
                                     RegScavenger *RS) const {
  MachineFrameInfo *MFI = MF.getFrameInfo();
  const TargetRegisterClass *RC = BF::DPRegisterClass;
  if (requiresRegisterScavenging(MF)) {
    // Reserve a slot close to SP or frame pointer.
    RS->setScavengingFrameIndex(MFI->CreateStackObject(RC->getSize(),
                                                       RC->getAlignment(),
                                                       false));
  }
}

void BlackfinRegisterInfo::
processFunctionBeforeFrameFinalized(MachineFunction &MF) const {
}

// Emit a prologue that sets up a stack frame.
// On function entry, R0-R2 and P0 may hold arguments.
// R3, P1, and P2 may be used as scratch registers
void BlackfinRegisterInfo::emitPrologue(MachineFunction &MF) const {
  MachineBasicBlock &MBB = MF.front();   // Prolog goes in entry BB
  MachineBasicBlock::iterator MBBI = MBB.begin();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  DebugLoc dl = (MBBI != MBB.end()
                 ? MBBI->getDebugLoc()
                 : DebugLoc::getUnknownLoc());

  int FrameSize = MFI->getStackSize();
  if (FrameSize%4) {
    FrameSize = (FrameSize+3) & ~3;
    MFI->setStackSize(FrameSize);
  }

  if (!hasFP(MF)) {
    assert(!MFI->hasCalls() &&
           "FP elimination on a non-leaf function is not supported");
    adjustRegister(MBB, MBBI, dl, BF::SP, BF::P1, -FrameSize);
    return;
  }

  // emit a LINK instruction
  if (FrameSize <= 0x3ffff) {
    BuildMI(MBB, MBBI, dl, TII.get(BF::LINK)).addImm(FrameSize);
    return;
  }

  // Frame is too big, do a manual LINK:
  // [--SP] = RETS;
  // [--SP] = FP;
  // FP = SP;
  // P1 = -FrameSize;
  // SP = SP + P1;
  BuildMI(MBB, MBBI, dl, TII.get(BF::PUSH))
    .addReg(BF::RETS, RegState::Kill);
  BuildMI(MBB, MBBI, dl, TII.get(BF::PUSH))
    .addReg(BF::FP, RegState::Kill);
  BuildMI(MBB, MBBI, dl, TII.get(BF::MOVE), BF::FP)
    .addReg(BF::SP);
  loadConstant(MBB, MBBI, dl, BF::P1, -FrameSize);
  BuildMI(MBB, MBBI, dl, TII.get(BF::ADDpp), BF::SP)
    .addReg(BF::SP, RegState::Kill)
    .addReg(BF::P1, RegState::Kill);

}

void BlackfinRegisterInfo::emitEpilogue(MachineFunction &MF,
                                        MachineBasicBlock &MBB) const {
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MachineBasicBlock::iterator MBBI = prior(MBB.end());
  DebugLoc dl = MBBI->getDebugLoc();

  int FrameSize = MFI->getStackSize();
  assert(FrameSize%4 == 0 && "Misaligned frame size");

  if (!hasFP(MF)) {
    assert(!MFI->hasCalls() &&
           "FP elimination on a non-leaf function is not supported");
    adjustRegister(MBB, MBBI, dl, BF::SP, BF::P1, FrameSize);
    return;
  }

  // emit an UNLINK instruction
  BuildMI(MBB, MBBI, dl, TII.get(BF::UNLINK));
}

unsigned BlackfinRegisterInfo::getRARegister() const {
  return BF::RETS;
}

unsigned
BlackfinRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return hasFP(MF) ? BF::FP : BF::SP;
}

unsigned BlackfinRegisterInfo::getEHExceptionRegister() const {
  llvm_unreachable("What is the exception register");
  return 0;
}

unsigned BlackfinRegisterInfo::getEHHandlerRegister() const {
  llvm_unreachable("What is the exception handler register");
  return 0;
}

int BlackfinRegisterInfo::getDwarfRegNum(unsigned RegNum, bool isEH) const {
  llvm_unreachable("What is the dwarf register number");
  return -1;
}

#include "BlackfinGenRegisterInfo.inc"

