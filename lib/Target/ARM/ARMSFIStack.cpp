//===-- ARMSFIStack.cpp - NaCl SFI Stack Pointer updates ---------*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file adds instructions needed for NaCl SFI in the case of stack pointer
// updates (if they were not already inserted).
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "arm-sfi"
#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "ARMSFIStack.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Function.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
  class ARMSFIStack : public MachineFunctionPass {
  public:
    static char ID;
    ARMSFIStack() : MachineFunctionPass(ID) {}

    const ARMBaseInstrInfo *TII;
    const TargetRegisterInfo *TRI;
    virtual void getAnalysisUsage(AnalysisUsage &AU) const;
    virtual bool runOnMachineFunction(MachineFunction &Fn);

    virtual const char *getPassName() const {
      return "ARM SFI stack pointer sandboxing";
    }

  private:
    void SandboxStackChange(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI);
    void LightweightVerify(MachineFunction &MF);
  };
  char ARMSFIStack::ID = 0;
}

void ARMSFIStack::getAnalysisUsage(AnalysisUsage &AU) const {
  // Slight (possibly unnecessary) efficiency tweak:
  // Promise not to modify the CFG.
  AU.setPreservesCFG();
  MachineFunctionPass::getAnalysisUsage(AU);
}


/**********************************************************************/
/* Debug */

static void DumpInstructionVerbose(const MachineInstr &MI) {
  dbgs() << MI;
  dbgs() << MI.getNumOperands() << " operands:" << "\n";
  for (unsigned i = 0; i < MI.getNumOperands(); ++i) {
    const MachineOperand& op = MI.getOperand(i);
    dbgs() << "  " << i << "(" << op.getType() << "):" << op << "\n";
  }
  dbgs() << "\n";
}

static void DumpBasicBlockVerbose(const MachineBasicBlock &MBB) {
  dbgs() << "\n<<<<< DUMP BASIC BLOCK START\n";
  for (MachineBasicBlock::const_iterator MBBI = MBB.begin(), MBBE = MBB.end();
       MBBI != MBBE;
       ++MBBI) {
    DumpInstructionVerbose(*MBBI);
  }
  dbgs() << "<<<<< DUMP BASIC BLOCK END\n\n";
}

static void DumpBasicBlockVerboseCond(const MachineBasicBlock &MBB, bool b) {
  if (b) {
    DumpBasicBlockVerbose(MBB);
  }
}

/*
 * A primitive validator to catch problems at compile time.
 * E.g., it could be used along with bugpoint to reduce a bitcode file.
 */
void ARMSFIStack::LightweightVerify(MachineFunction &MF) {

  for (MachineFunction::iterator MFI = MF.begin(), MFE = MF.end();
       MFI != MFE;
       ++MFI) {
    MachineBasicBlock &MBB = *MFI;
    for (MachineBasicBlock::iterator MBBI = MBB.begin(), MBBE = MBB.end();
         MBBI != MBBE;
         ++MBBI) {
      MachineInstr &MI = *MBBI;

      if (ARM_SFI::NeedSandboxStackChange(MI, TRI)) {
        dbgs() << "LightWeightVerify for function: "
               << MF.getFunction()->getName() << "  (BAD STACK CHANGE)\n";
        DumpInstructionVerbose(MI);
        DumpBasicBlockVerbose(MBB);
        //        assert(false && "LightweightVerify Failed");
      }
    }
  }
}

/**********************************************************************/
/* Check and sandbox. */

namespace ARM_SFI {

bool IsStackChange(const MachineInstr &MI, const TargetRegisterInfo *TRI) {
  return MI.modifiesRegister(ARM::SP, TRI);
}

bool NextInstrMasksSP(const MachineInstr &MI) {
  MachineBasicBlock::const_iterator It = &MI;
  const MachineBasicBlock *MBB = MI.getParent();

  MachineBasicBlock::const_iterator next = ++It;
  if (next == MBB->end()) {
    return false;
  }

  const MachineInstr &next_instr = *next;
  unsigned opcode = next_instr.getOpcode();
  return (opcode == ARM::SFI_DATA_MASK) &&
      (next_instr.getOperand(0).getReg() == ARM::SP);
}

bool IsSandboxedStackChange(const MachineInstr &MI) {
  unsigned opcode = MI.getOpcode();
  switch (opcode) {
    default: break;

    // These just bump SP by a little (and access the stack),
    // so that is okay due to guard pages.
    case ARM::STM_UPD:
    case ARM::VSTMD_UPD:
    case ARM::VSTMS_UPD:
      return true;

    // Similar, unless it is a load into SP...
    case ARM::LDM_UPD:
    case ARM::VLDMD_UPD:
    case ARM::VLDMS_UPD: {
      bool dest_SP = false;
      // Dest regs start at operand index 4.
      for (unsigned i = 4; i < MI.getNumOperands(); ++i) {
        const MachineOperand &DestReg = MI.getOperand(i);
        dest_SP = dest_SP || (DestReg.getReg() == ARM::SP);
      }
      if (dest_SP) {
        break;
      }
      return true;
    }

    // Some localmods *should* prevent selecting a reg offset
    // (see SelectAddrMode2 in ARMISelDAGToDAG.cpp).
    // Otherwise, the store is already a potential violation.
    case ARM::STR_PRE:
    case ARM::STRH_PRE:
    case ARM::STRB_PRE:
      return true;

    // Similar, unless it is a load into SP...
    case ARM::LDR_PRE:
    case ARM::LDRH_PRE:
    case ARM::LDRB_PRE:
    case ARM::LDRSH_PRE:
    case ARM::LDRSB_PRE: {
      const MachineOperand &DestReg = MI.getOperand(0);
      if (DestReg.getReg() == ARM::SP) {
        break;
      }
      return true;
    }

    // Here, if SP is the base / write-back reg, we need to check if
    // a reg is used as offset (otherwise it is not a small nudge).
    case ARM::STR_POST:
    case ARM::STRH_POST:
    case ARM::STRB_POST: {
      const MachineOperand &WBReg = MI.getOperand(0);
      const MachineOperand &OffReg = MI.getOperand(3);
      if (WBReg.getReg() == ARM::SP && OffReg.getReg() != 0) {
        break;
      }
      return true;
    }

    // Similar, but also check that DestReg is not SP.
    case ARM::LDR_POST:
    case ARM::LDRH_POST:
    case ARM::LDRB_POST:
    case ARM::LDRSH_POST:
    case ARM::LDRSB_POST: {
      const MachineOperand &DestReg = MI.getOperand(0);
      if (DestReg.getReg() == ARM::SP) {
        break;
      }
      const MachineOperand &WBReg = MI.getOperand(1);
      const MachineOperand &OffReg = MI.getOperand(3);
      if (WBReg.getReg() == ARM::SP && OffReg.getReg() != 0) {
        break;
      }
      return true;
    }
  }

  return (NextInstrMasksSP(MI));
}

bool NeedSandboxStackChange(const MachineInstr &MI,
                               const TargetRegisterInfo *TRI) {
  return (IsStackChange(MI, TRI) && !IsSandboxedStackChange(MI));
}

} // namespace ARM_SFI

void ARMSFIStack::SandboxStackChange(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator MBBI) {
  // (1) Ensure there is room in the bundle for a data mask instruction
  // (nop'ing to the next bundle if needed).
  // (2) Do a data mask on SP after the instruction that updated SP.
  MachineInstr &MI = *MBBI;

  // Use same predicate as current instruction.
  ARMCC::CondCodes Pred = TII->getPredicate(&MI);

  BuildMI(MBB, MBBI, MI.getDebugLoc(),
          TII->get(ARM::SFI_NOP_IF_AT_BUNDLE_END));

  // Get to next instr (one + to get the original, and one more + to get past)
  MachineBasicBlock::iterator MBBINext = (MBBI++);
  MachineBasicBlock::iterator MBBINext2 = (MBBI++);

  BuildMI(MBB, MBBINext2, MI.getDebugLoc(),
          TII->get(ARM::SFI_DATA_MASK))
      .addReg(ARM::SP)         // modify SP (as dst)
      .addReg(ARM::SP)         // start with SP (as src)
      .addImm((int64_t) Pred)  // predicate condition
      .addReg(ARM::CPSR);      // predicate source register (CPSR)

  return;
}

/**********************************************************************/

bool ARMSFIStack::runOnMachineFunction(MachineFunction &MF) {
  TII = static_cast<const ARMBaseInstrInfo*>(MF.getTarget().getInstrInfo());
  TRI = MF.getTarget().getRegisterInfo();

  bool Modified = false;
  for (MachineFunction::iterator MFI = MF.begin(), E = MF.end();
       MFI != E;
       ++MFI) {
    MachineBasicBlock &MBB = *MFI;
    bool ModifiedBB = false;
    for (MachineBasicBlock::iterator MBBI = MBB.begin(), MBBE = MBB.end();
         MBBI != MBBE;
         ++MBBI) {
      if (ARM_SFI::NeedSandboxStackChange(*MBBI, TRI)) {
        SandboxStackChange(MBB, MBBI);
        ModifiedBB |= true;
      }
    }

    DEBUG(DumpBasicBlockVerboseCond(MBB, ModifiedBB));
  }

  DEBUG(LightweightVerify(MF));
  return Modified;
}

/// createARMSFIStackPass - returns an instance of the SFI placement pass.
FunctionPass *llvm::createARMSFIStackPass() {
  return new ARMSFIStack();
}
