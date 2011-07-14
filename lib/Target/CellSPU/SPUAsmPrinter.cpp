//===-- SPUAsmPrinter.cpp - Print machine instrs to Cell SPU assembly -------=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to Cell SPU assembly language. This printer
// is the output mechanism used by `llc'.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "asmprinter"
#include "SPU.h"
#include "SPUTargetMachine.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Target/Mangler.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetRegistry.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

namespace {
  class SPUAsmPrinter : public AsmPrinter {
  public:
    explicit SPUAsmPrinter(TargetMachine &TM, MCStreamer &Streamer) :
      AsmPrinter(TM, Streamer) {}

    virtual const char *getPassName() const {
      return "STI CBEA SPU Assembly Printer";
    }

    /// printInstruction - This method is automatically generated by tablegen
    /// from the instruction set description.
    void printInstruction(const MachineInstr *MI, raw_ostream &OS);
    static const char *getRegisterName(unsigned RegNo);


    void EmitInstruction(const MachineInstr *MI) {
      SmallString<128> Str;
      raw_svector_ostream OS(Str);
      printInstruction(MI, OS);
      OutStreamer.EmitRawText(OS.str());
    }
    void printOp(const MachineOperand &MO, raw_ostream &OS);

    void printOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      const MachineOperand &MO = MI->getOperand(OpNo);
      if (MO.isReg()) {
        O << getRegisterName(MO.getReg());
      } else if (MO.isImm()) {
        O << MO.getImm();
      } else {
        printOp(MO, O);
      }
    }

    bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                         unsigned AsmVariant, const char *ExtraCode,
                         raw_ostream &O);
    bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                               unsigned AsmVariant, const char *ExtraCode,
                               raw_ostream &O);


    void
    printU7ImmOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O)
    {
      unsigned int value = MI->getOperand(OpNo).getImm();
      assert(value < (1 << 8) && "Invalid u7 argument");
      O << value;
    }

    void
    printShufAddr(const MachineInstr *MI, unsigned OpNo, raw_ostream &O)
    {
      char value = MI->getOperand(OpNo).getImm();
      O << (int) value;
      O << "(";
      printOperand(MI, OpNo+1, O);
      O << ")";
    }

    void
    printS16ImmOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O)
    {
      O << (short) MI->getOperand(OpNo).getImm();
    }

    void
    printU16ImmOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O)
    {
      O << (unsigned short)MI->getOperand(OpNo).getImm();
    }

    void
    printMemRegReg(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      // When used as the base register, r0 reads constant zero rather than
      // the value contained in the register.  For this reason, the darwin
      // assembler requires that we print r0 as 0 (no r) when used as the base.
      const MachineOperand &MO = MI->getOperand(OpNo);
      O << getRegisterName(MO.getReg()) << ", ";
      printOperand(MI, OpNo+1, O);
    }

    void
    printU18ImmOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O)
    {
      unsigned int value = MI->getOperand(OpNo).getImm();
      assert(value <= (1 << 19) - 1 && "Invalid u18 argument");
      O << value;
    }

    void
    printS10ImmOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O)
    {
      short value = (short) (((int) MI->getOperand(OpNo).getImm() << 16)
                             >> 16);
      assert((value >= -(1 << 9) && value <= (1 << 9) - 1)
             && "Invalid s10 argument");
      O << value;
    }

    void
    printU10ImmOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O)
    {
      short value = (short) (((int) MI->getOperand(OpNo).getImm() << 16)
                             >> 16);
      assert((value <= (1 << 10) - 1) && "Invalid u10 argument");
      O << value;
    }

    void
    printDFormAddr(const MachineInstr *MI, unsigned OpNo, raw_ostream &O)
    {
      assert(MI->getOperand(OpNo).isImm() &&
             "printDFormAddr first operand is not immediate");
      int64_t value = int64_t(MI->getOperand(OpNo).getImm());
      int16_t value16 = int16_t(value);
      assert((value16 >= -(1 << (9+4)) && value16 <= (1 << (9+4)) - 1)
             && "Invalid dform s10 offset argument");
      O << (value16 & ~0xf) << "(";
      printOperand(MI, OpNo+1, O);
      O << ")";
    }

    void
    printAddr256K(const MachineInstr *MI, unsigned OpNo, raw_ostream &O)
    {
      /* Note: operand 1 is an offset or symbol name. */
      if (MI->getOperand(OpNo).isImm()) {
        printS16ImmOperand(MI, OpNo, O);
      } else {
        printOp(MI->getOperand(OpNo), O);
        if (MI->getOperand(OpNo+1).isImm()) {
          int displ = int(MI->getOperand(OpNo+1).getImm());
          if (displ > 0)
            O << "+" << displ;
          else if (displ < 0)
            O << displ;
        }
      }
    }

    void printCallOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      printOp(MI->getOperand(OpNo), O);
    }

    void printHBROperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      printOp(MI->getOperand(OpNo), O);
    }

    void printPCRelativeOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      // Used to generate a ".-<target>", but it turns out that the assembler
      // really wants the target.
      //
      // N.B.: This operand is used for call targets. Branch hints are another
      // animal entirely.
      printOp(MI->getOperand(OpNo), O);
    }

    void printSymbolHi(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      if (MI->getOperand(OpNo).isImm()) {
        printS16ImmOperand(MI, OpNo, O);
      } else {
        printOp(MI->getOperand(OpNo), O);
        O << "@h";
      }
    }

    void printSymbolLo(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      if (MI->getOperand(OpNo).isImm()) {
        printS16ImmOperand(MI, OpNo, O);
      } else {
        printOp(MI->getOperand(OpNo), O);
        O << "@l";
      }
    }

    /// Print local store address
    void printSymbolLSA(const MachineInstr *MI, unsigned OpNo, raw_ostream &O) {
      printOp(MI->getOperand(OpNo), O);
    }

    void printROTHNeg7Imm(const MachineInstr *MI, unsigned OpNo,
                          raw_ostream &O) {
      if (MI->getOperand(OpNo).isImm()) {
        int value = (int) MI->getOperand(OpNo).getImm();
        assert((value >= 0 && value < 16)
               && "Invalid negated immediate rotate 7-bit argument");
        O << -value;
      } else {
        llvm_unreachable("Invalid/non-immediate rotate amount in printRotateNeg7Imm");
      }
    }

    void printROTNeg7Imm(const MachineInstr *MI, unsigned OpNo, raw_ostream &O){
      assert(MI->getOperand(OpNo).isImm() &&
             "Invalid/non-immediate rotate amount in printRotateNeg7Imm");
      int value = (int) MI->getOperand(OpNo).getImm();
      assert((value >= 0 && value <= 32)
             && "Invalid negated immediate rotate 7-bit argument");
      O << -value;
    }
  };
} // end of anonymous namespace

// Include the auto-generated portion of the assembly writer
#include "SPUGenAsmWriter.inc"

void SPUAsmPrinter::printOp(const MachineOperand &MO, raw_ostream &O) {
  switch (MO.getType()) {
  case MachineOperand::MO_Immediate:
    report_fatal_error("printOp() does not handle immediate values");
    return;

  case MachineOperand::MO_MachineBasicBlock:
    O << *MO.getMBB()->getSymbol();
    return;
  case MachineOperand::MO_JumpTableIndex:
    O << MAI->getPrivateGlobalPrefix() << "JTI" << getFunctionNumber()
      << '_' << MO.getIndex();
    return;
  case MachineOperand::MO_ConstantPoolIndex:
    O << MAI->getPrivateGlobalPrefix() << "CPI" << getFunctionNumber()
      << '_' << MO.getIndex();
    return;
  case MachineOperand::MO_ExternalSymbol:
    // Computing the address of an external symbol, not calling it.
    if (TM.getRelocationModel() != Reloc::Static) {
      O << "L" << MAI->getGlobalPrefix() << MO.getSymbolName()
        << "$non_lazy_ptr";
      return;
    }
    O << *GetExternalSymbolSymbol(MO.getSymbolName());
    return;
  case MachineOperand::MO_GlobalAddress:
    // External or weakly linked global variables need non-lazily-resolved
    // stubs
    if (TM.getRelocationModel() != Reloc::Static) {
      const GlobalValue *GV = MO.getGlobal();
      if (((GV->isDeclaration() || GV->hasWeakLinkage() ||
            GV->hasLinkOnceLinkage() || GV->hasCommonLinkage()))) {
        O << *GetSymbolWithGlobalValueBase(GV, "$non_lazy_ptr");
        return;
      }
    }
    O << *Mang->getSymbol(MO.getGlobal());
    return;
  case MachineOperand::MO_MCSymbol:
    O << *(MO.getMCSymbol());
    return;
  default:
    O << "<unknown operand type: " << MO.getType() << ">";
    return;
  }
}

/// PrintAsmOperand - Print out an operand for an inline asm expression.
///
bool SPUAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                    unsigned AsmVariant,
                                    const char *ExtraCode, raw_ostream &O) {
  // Does this asm operand have a single letter operand modifier?
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0) return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default: return true;  // Unknown modifier.
    case 'L': // Write second word of DImode reference.
      // Verify that this operand has two consecutive registers.
      if (!MI->getOperand(OpNo).isReg() ||
          OpNo+1 == MI->getNumOperands() ||
          !MI->getOperand(OpNo+1).isReg())
        return true;
      ++OpNo;   // Return the high-part.
      break;
    }
  }

  printOperand(MI, OpNo, O);
  return false;
}

bool SPUAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                          unsigned OpNo, unsigned AsmVariant,
                                          const char *ExtraCode,
                                          raw_ostream &O) {
  if (ExtraCode && ExtraCode[0])
    return true; // Unknown modifier.
  printMemRegReg(MI, OpNo, O);
  return false;
}

// Force static initialization.
extern "C" void LLVMInitializeCellSPUAsmPrinter() { 
  RegisterAsmPrinter<SPUAsmPrinter> X(TheCellSPUTarget);
}
