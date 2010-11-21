//====- X86InstrNaCl.td - Describe NaCl Instructions ----*- tablegen -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file describes the modifications to the X86 instruction set needed for
// Native Client code generation.
//
//===----------------------------------------------------------------------===//


//===----------------------------------------------------------------------===//
//
// Native Client 32-bit
//
//===----------------------------------------------------------------------===//

let isTerminator = 1, isBarrier = 1, hasCtrlDep = 1 in {
def NACL_TRAP32  : I<0, Pseudo, (outs), (ins), "mov{l}\t$$0, 0", []>;
}


// TODO: indicate that we clobber rcx
let isTerminator = 1, isReturn = 1, isBarrier = 1,
    hasCtrlDep = 1, FPForm = SpecialFP in {

  def NACL_RET32   : I<0, Pseudo, (outs), (ins variable_ops),
                    "pop %ecx\n\tnacljmp %ecx",
                    []>, 
                    Requires<[IsNaCl32]>;
  def NACL_RETI32   : I<0, Pseudo, (outs), (ins i16imm:$amt, variable_ops),
                    "pop %ecx\n"
                    "\tadd $amt,%esp\n"
                    "\tnacljmp %ecx",
                    []>, 
                    Requires<[IsNaCl32]>;
}

let isBranch = 1, isTerminator = 1, isBarrier = 1, isIndirectBranch = 1 in {
    def NACL_JMP32r     : I<0, Pseudo, (outs), (ins GR32:$dst),
                       "nacljmp\t $dst",
                         []>, Requires<[IsNaCl32]>;
}

let isCall = 1 in
  // All calls clobber the non-callee saved registers. ESP is marked as
  // a use to prevent stack-pointer assignments that appear immediately
  // before calls from potentially appearing dead. Uses for argument
  // registers are added manually.
  let Defs = [EAX, ECX, EDX, FP0, FP1, FP2, FP3, FP4, FP5, FP6, ST0,
              MM0, MM1, MM2, MM3, MM4, MM5, MM6, MM7,
              XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
              XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15, EFLAGS],
      Uses = [ESP] in {

def NACL_CALLpcrel32 : Ii32PCRel<0, Pseudo,
                      (outs), (ins i32imm_pcrel:$dst,variable_ops),
                      "call\t$dst", []>,
                      Requires<[IsNaCl32]>;
def NACL_CALL32r     : I<0, Pseudo, (outs), (ins GR32:$dst, variable_ops),
                        "naclcall\t$dst", [(X86call GR32:$dst)]>,
                       Requires<[IsNaCl32]>;
}

// Normal calls, with various flavors of addresses.
def : Pat<(X86call (i32 tglobaladdr:$dst)),
          (NACL_CALLpcrel32 tglobaladdr:$dst)>, Requires<[IsNaCl32]>;
def : Pat<(X86call (i32 texternalsym:$dst)),
          (NACL_CALLpcrel32 texternalsym:$dst)>, Requires<[IsNaCl32]>;
def : Pat<(X86call (i32 imm:$dst)),
          (NACL_CALLpcrel32 imm:$dst)>, Requires<[CallImmAddr,IsNaCl32]>;


// Tail calls
let isCall = 1, isTerminator = 1, isReturn = 1, isBarrier = 1,
    isCodeGenOnly = 1 in
  let Defs = [EAX, ECX, EDX, FP0, FP1, FP2, FP3, FP4, FP5, FP6, ST0,
              MM0, MM1, MM2, MM3, MM4, MM5, MM6, MM7,
              XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
              XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15, EFLAGS],
      Uses = [ESP] in {

  def NACL_TCRETURNdi : I<0, Pseudo, (outs), 
                         (ins i32imm_pcrel:$dst, i32imm:$offset, variable_ops),
                         "#TC_RETURN $dst $offset", []>,
                        Requires<[IsNaCl32]>;
  def NACL_TCRETURNri : I<0, Pseudo, (outs), 
                          (ins GR32_TC:$dst, i32imm:$offset, variable_ops),
                         "#TC_RETURN $dst $offset", []>,
                        Requires<[IsNaCl32]>;

  def NACL_TAILJMPd : Ii32<0xE9, RawFrm, (outs),
                           (ins i32imm_pcrel:$dst, variable_ops),
                           "jmp\t$dst  # TAILCALL", []>,
                        Requires<[IsNaCl32]>;
  def NACL_TAILJMPr : I<0, Pseudo, (outs), (ins GR32_TC:$dst, variable_ops),
                   "nacljmp\t $dst  # TAILCALL",
                   []>, Requires<[IsNaCl32]>;
}

// More tail call stuff
def : Pat<(X86tcret GR32_TC:$dst, imm:$off),
          (NACL_TCRETURNri GR32_TC:$dst, imm:$off)>,
	  Requires<[IsNaCl32]>;

def : Pat<(X86tcret (i32 tglobaladdr:$dst), imm:$off),
          (NACL_TCRETURNdi texternalsym:$dst, imm:$off)>,
	  Requires<[IsNaCl32]>;

def : Pat<(X86tcret (i32 texternalsym:$dst), imm:$off),
          (NACL_TCRETURNdi texternalsym:$dst, imm:$off)>,
	  Requires<[IsNaCl32]>;

//===----------------------------------------------------------------------===//
//
// Native Client 64-bit
//
//===----------------------------------------------------------------------===//

let isTerminator = 1, isBarrier = 1, hasCtrlDep = 1 in {
def NACL_TRAP64  : I<0, Pseudo, (outs), (ins), "mov{l}\t$$0, (%r15)", []>;
}

let Defs = [RSP, EFLAGS], Uses = [RSP] in {
    def NACL_ADD_SP : I<0, Pseudo, (outs GR64:$dst),
                      (ins GR64:$src1, i64i32imm:$src2),
                      "naclasp{q}\t{$src2, %r15|%r15, $src2}",
                      []>, Requires<[IsNaCl64]>;

   def NACL_SUB_SP   : I<0, Pseudo, (outs GR64:$dst),
                      (ins GR64:$src1, i64i32imm:$src2),
                      "naclssp{q}\t{$src2, %r15|%r15, $src2}",
                      []>, Requires<[IsNaCl64]>;

   def NACL_ADJ_SP   : I<0, Pseudo, (outs GR64:$dst),
                      // We only need the offset. However, we are rewriting
                      // an LEA, so we should keep the num of operands the same.
                      (ins GR64:$base, i8imm:$scale, GR64:$index,
                       i32imm:$off, i8imm:$seg),
                      "naclspadj{q}\t{$off, %r15|%r15, $off}",
                      []>, Requires<[IsNaCl64]>;

}

let isCall = 1 in
  // All calls clobber the non-callee saved registers. RSP is marked as
  // a use to prevent stack-pointer assignments that appear immediately
  // before calls from potentially appearing dead. Uses for argument
  // registers are added manually.
  let Defs = [RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11,
              FP0, FP1, FP2, FP3, FP4, FP5, FP6, ST0, ST1,
              MM0, MM1, MM2, MM3, MM4, MM5, MM6, MM7,
              XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
              XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15, EFLAGS],
      Uses = [RSP] in {

    def NACL_CALL64pcrel32 : Ii32<0, Pseudo,
                          (outs), (ins i32imm_pcrel:$dst, variable_ops),
                          "call{q}\t$dst", []>,
                          Requires<[IsNaCl64]>;

    def NACL_CALL64r    : I<0, Pseudo, (outs), (ins GR32:$dst, variable_ops),
                          "naclcall\t$dst, %r15",
                          [(X86call GR32:$dst)]>,
                          Requires<[IsNaCl64]>;
}

def : Pat<(X86call (i32 tglobaladdr:$dst)),
          (NACL_CALL64pcrel32 tglobaladdr:$dst)>, Requires<[IsNaCl64]>;
def : Pat<(X86call (i32 texternalsym:$dst)),
          (NACL_CALL64pcrel32 texternalsym:$dst)>, Requires<[IsNaCl64]>;
def : Pat<(X86call (i32 imm:$dst)),
          (NACL_CALL64pcrel32 imm:$dst)>, Requires<[CallImmAddr,IsNaCl64]>;


// TODO: indicate that we clobber rcx
let isTerminator = 1, isReturn = 1, isBarrier = 1,
    hasCtrlDep = 1, FPForm = SpecialFP in {

  def NACL_RET64    : I<0, Pseudo, (outs), (ins variable_ops),
                     "pop %rcx\n\tnacljmp %ecx, %r15",
                     []>,
                     Requires<[IsNaCl64]>;
}

// Tail calls
let isCall = 1, isTerminator = 1, isReturn = 1, isBarrier = 1,
    isCodeGenOnly = 1 in
  let Defs = [RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11,
              FP0, FP1, FP2, FP3, FP4, FP5, FP6, ST0, ST1,
              MM0, MM1, MM2, MM3, MM4, MM5, MM6, MM7,
              XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
              XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15, EFLAGS],
      Uses = [RSP] in {

  def NACL_TCRETURNdi64 : I<0, Pseudo, (outs),
                           (ins i32imm_pcrel:$dst, i32imm:$offset, 
                            variable_ops),
                           "#TC_RETURN $dst $offset", []>,
                          Requires<[IsNaCl64]>;
  def NACL_TCRETURNri64 : I<0, Pseudo, (outs),
                          (ins GR32_TC_64:$dst, i32imm:$offset, variable_ops),
                          "#TC_RETURN $dst $offset", []>,
                          Requires<[IsNaCl64]>;

  def NACL_TAILJMPd64 : Ii32<0xE9, RawFrm, (outs),
                            (ins i32imm_pcrel:$dst, variable_ops),
                            "jmp\t$dst  # TAILCALL", []>,
                        Requires<[IsNaCl64]>;
  def NACL_TAILJMPr64 : I<0, Pseudo, (outs),
                          (ins GR32_TC_64:$dst, variable_ops),
                          "nacljmp\t$dst,%r15  # TAILCALL", []>,
                        Requires<[IsNaCl64]>;
}


// More tail call stuff

def : Pat<(X86tcret (i32 tglobaladdr:$dst), imm:$off),
          (NACL_TCRETURNdi64 tglobaladdr:$dst, imm:$off)>,
	  Requires<[IsNaCl64]>;

def : Pat<(X86tcret (i32 texternalsym:$dst), imm:$off),
          (NACL_TCRETURNdi64 texternalsym:$dst, imm:$off)>,
	  Requires<[IsNaCl64]>;

def : Pat<(X86tcret GR32_TC_64:$dst, imm:$off),
          (NACL_TCRETURNri64 GR32_TC_64:$dst, imm:$off)>,
	  Requires<[IsNaCl64]>;


// Special i32mem for addresses of load folding tail calls. These are not
// allowed to use callee-saved registers since they must be scheduled
// after callee-saved register are popped.
def i32mem_TC_64 : Operand<i32> {
  let PrintMethod = "printi32mem";
  let MIOperandInfo = (ops GR32_TC_64, i8imm, GR32_TC_64, i32imm, i8imm);
  let ParserMatchClass = X86MemAsmOperand;
}

let isBranch = 1, isTerminator = 1, isBarrier = 1, isIndirectBranch = 1 in {
  def NACL_JMP64r     : I<0, Pseudo, (outs), (ins GR32:$dst),
                        "nacljmp\t$dst, %r15",
                        [(brind GR32:$dst)]>,
                        Requires<[IsNaCl64]>;

}

let Defs = [RSP] in
def NACL_SET_SPr   : I<0, Pseudo,
                     (outs GR32:$esp), (ins GR32:$src),
                     "naclrestsp_noflags\t$src, %r15",
                     []>,
                     Requires<[IsNaCl64]>;

let Defs = [RSP] in
def NACL_SET_SPm  : I<0, Pseudo,
                     (outs GR32:$esp), (ins i32mem:$src),
                     "naclrestsp_noflags\t$src, %r15",
                     []>,
                     Requires<[IsNaCl64]>;
          

// Important: Be careful with what you clobber in NACL_POP_RBP.
// During tail calls, all parameter registers and callee saved
// registers are potentially live. (we clobber EFLAGS, but that seems ok)
def NACL_POP_RBP   : I<0, Pseudo,
                     (outs GR64:$reg), (ins),
                     "naclrestbp\t(%rsp), %r15\n\t"
                     "naclasp{q}\t{$$8, %r15|%r15, $$8}",
                     []>,
                     Requires<[IsNaCl64]>;

// The NaCl loader cannot set the FS or GS segmentation registers
// in Windows, so we must use a NaCl syscall.
let Defs = [RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11,
            FP0, FP1, FP2, FP3, FP4, FP5, FP6, ST0, ST1,
            MM0, MM1, MM2, MM3, MM4, MM5, MM6, MM7,
            XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
            XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15, EFLAGS],
    Uses = [RSP] in
def NACL_TLS_addr64 : I<0, Pseudo, (outs), (ins i32mem:$sym),
                      "movq\t$$$sym, %rdi; "
                      "call\t__tls_get_addr@PLT",
                     [(X86tlsaddr tls32addr:$sym)]>,
                     Requires<[IsNaCl64]>;

let usesCustomInserter = 1, Defs = [EFLAGS] in
def NACL_VAARG_64 : I<0, Pseudo,
                 (outs GR32:$dst),
                 (ins i8mem:$ap, i32imm:$size, i8imm:$mode, i32imm:$align),
                 "#NACL_VAARG_64 $dst, $ap, $size, $mode, $align",
                 [(set GR32:$dst,
                    (X86vaarg64 addr:$ap, imm:$size, imm:$mode, imm:$align)),
                  (implicit EFLAGS)]>,
                 Requires<[IsNaCl64]>;


