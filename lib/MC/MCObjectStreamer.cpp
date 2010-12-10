//===- lib/MC/MCObjectStreamer.cpp - Object File MCStreamer Interface -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCObjectStreamer.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCSection.h" // @LOCALMOD
#include "llvm/Target/TargetAsmBackend.h"
using namespace llvm;

MCObjectStreamer::MCObjectStreamer(MCContext &Context, TargetAsmBackend &TAB,
                                   raw_ostream &_OS, MCCodeEmitter *_Emitter,
                                   bool _PadSectionToAlignment)
  : MCStreamer(Context), Assembler(new MCAssembler(Context, TAB,
                                                   *_Emitter,
                                                   _PadSectionToAlignment,
                                                   _OS)),
    CurSectionData(0)
{
}

MCObjectStreamer::~MCObjectStreamer() {
  delete &Assembler->getBackend();
  delete &Assembler->getEmitter();
  delete Assembler;
}

MCFragment *MCObjectStreamer::getCurrentFragment() const {
  assert(getCurrentSectionData() && "No current section!");

  if (!getCurrentSectionData()->empty())
    return &getCurrentSectionData()->getFragmentList().back();

  return 0;
}

void MCObjectStreamer::EmitBundlePadding() const {
  MCSectionData *SD = getCurrentSectionData();

  if (SD->isBundlingEnabled() && !SD->isBundleLocked()) {
    MCBundlePaddingFragment *BPF = new MCBundlePaddingFragment(SD);
    BPF->setBundleAlign(SD->getBundleAlignNext());
    SD->setBundleAlignNext(MCBundlePaddingFragment::BundleAlignNone);
  }
}

MCDataFragment *MCObjectStreamer::getOrCreateDataFragment() const {
  // @LOCALMOD-BEGIN
  EmitBundlePadding();
  // @LOCALMOD-END

  MCDataFragment *F = dyn_cast_or_null<MCDataFragment>(getCurrentFragment());
  if (!F)
    F = new MCDataFragment(getCurrentSectionData());
  return F;
}

const MCExpr *MCObjectStreamer::AddValueSymbols(const MCExpr *Value) {
  switch (Value->getKind()) {
  case MCExpr::Target: llvm_unreachable("Can't handle target exprs yet!");
  case MCExpr::Constant:
    break;

  case MCExpr::Binary: {
    const MCBinaryExpr *BE = cast<MCBinaryExpr>(Value);
    AddValueSymbols(BE->getLHS());
    AddValueSymbols(BE->getRHS());
    break;
  }

  case MCExpr::SymbolRef:
    Assembler->getOrCreateSymbolData(cast<MCSymbolRefExpr>(Value)->getSymbol());
    break;

  case MCExpr::Unary:
    AddValueSymbols(cast<MCUnaryExpr>(Value)->getSubExpr());
    break;
  }

  return Value;
}

void MCObjectStreamer::EmitULEB128Value(const MCExpr *Value,
                                        unsigned AddrSpace) {
  new MCLEBFragment(*Value, false, getCurrentSectionData());
}

void MCObjectStreamer::EmitSLEB128Value(const MCExpr *Value,
                                        unsigned AddrSpace) {
  new MCLEBFragment(*Value, true, getCurrentSectionData());
}

void MCObjectStreamer::EmitWeakReference(MCSymbol *Alias,
                                         const MCSymbol *Symbol) {
  report_fatal_error("This file format doesn't support weak aliases.");
}

// @LOCALMOD-BEGIN ========================================================

void MCObjectStreamer::EmitBundleAlignStart() {
  MCSectionData *SD = getCurrentSectionData();
  assert(SD->isBundlingEnabled() &&
         ".bundle_align_start called, but bundling disabled!");
  assert(!SD->isBundleLocked() &&
         ".bundle_align_start while bundle locked");
  SD->setBundleAlignNext(MCBundlePaddingFragment::BundleAlignStart);
}

void MCObjectStreamer::EmitBundleAlignEnd() {
  MCSectionData *SD = getCurrentSectionData();
  assert(SD->isBundlingEnabled() &&
         ".bundle_align_end called, but bundling disabled!");
  assert(!SD->isBundleLocked() &&
         ".bundle_align_end while bundle locked");
  SD->setBundleAlignNext(MCBundlePaddingFragment::BundleAlignEnd);
}

void MCObjectStreamer::EmitBundleLock() {
  MCSectionData *SD = getCurrentSectionData();
  assert(SD->isBundlingEnabled() &&
         ".bundle_lock called, but bundling disabled!");
  assert(!SD->isBundleLocked() &&
         ".bundle_lock issued when bundle already locked");
  EmitBundlePadding();
  SD->setBundleLocked(true);
}

void MCObjectStreamer::EmitBundleUnlock() {
  MCSectionData *SD = getCurrentSectionData();
  assert(SD->isBundlingEnabled() &&
         ".bundle_unlock called, but bundling disabled!");
  assert(SD->isBundleLocked() &&
         ".bundle_unlock called when bundle not locked");
  SD->setBundleLocked(false);
}
// @LOCALMOD-END ==========================================================

void MCObjectStreamer::SwitchSection(const MCSection *Section) {
  assert(Section && "Cannot switch to a null section!");

  // If already in this section, then this is a noop.
  if (Section == CurSection) return;

  PrevSection = CurSection;
  CurSection = Section;
  CurSectionData = &getAssembler().getOrCreateSectionData(*Section);
}

void MCObjectStreamer::EmitInstruction(const MCInst &Inst) {

  // @LOCALMOD-BEGIN
  if (getAssembler().getBackend().CustomExpandInst(Inst, *this)) {
    return;
  }
  // @LOCALMOD-END

  // Scan for values.
  for (unsigned i = Inst.getNumOperands(); i--; )
    if (Inst.getOperand(i).isExpr())
      AddValueSymbols(Inst.getOperand(i).getExpr());

  getCurrentSectionData()->setHasInstructions(true);

  // Now that a machine instruction has been assembled into this section, make
  // a line entry for any .loc directive that has been seen.
  MCLineEntry::Make(this, getCurrentSection());

  // If this instruction doesn't need relaxation, just emit it as data.
  if (!getAssembler().getBackend().MayNeedRelaxation(Inst)) {
    EmitInstToData(Inst);
    return;
  }

  // Otherwise, if we are relaxing everything, relax the instruction as much as
  // possible and emit it as data.
  if (getAssembler().getRelaxAll()) {
    MCInst Relaxed;
    getAssembler().getBackend().RelaxInstruction(Inst, Relaxed);
    while (getAssembler().getBackend().MayNeedRelaxation(Relaxed))
      getAssembler().getBackend().RelaxInstruction(Relaxed, Relaxed);
    EmitInstToData(Relaxed);
    return;
  }

  // Otherwise emit to a separate fragment.
  EmitInstToFragment(Inst);
}

void MCObjectStreamer::Finish() {
  getAssembler().Finish();
}
