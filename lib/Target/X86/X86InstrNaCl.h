//===-- X86InstrNaCl.h - Prototype for CustomExpandInstNaCl    ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef X86_INSTRNACL_H
#define X86_INSTRNACL_H

namespace llvm {
  class MCInst;
  class MCStreamer;
  bool CustomExpandInstNaCl(const MCInst &Inst, MCStreamer &Out);
}

#endif
