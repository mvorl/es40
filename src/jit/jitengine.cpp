/* ES40 emulator -- JIT engine implementation. */
#ifdef ES40_JIT

#include "jitengine.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <chrono>   // note_exec times its own stats-print I/O (excluded from the wall-clock RPCC)
#include <initializer_list>
#include <vector>   // deferred slow-path stubs (memop helper calls)
#include <cstddef>  // offsetof (chain guard)
#define ASMJIT_STATIC
// asmjit's x64 backend emits x86-64; asmjit's CallConv maps the host C ABI
// (Microsoft x64 or System V) from the build env. On ARM64 only asmjit's core is
// needed (JitRuntime for the engine plumbing) -- no a64 codegen is emitted yet.
// jitengine.h rejects any other host arch. I don't forsee ever adding 32-bit x86.
#ifdef ES40_JIT_X64
#include <asmjit/x86.h>
#else
#include <asmjit/core.h>
#endif

namespace {

#ifdef ES40_JIT_X64   // guest-ISA classification + emit support: only the x86 backend uses it

#ifdef JIT_DISASM
// Log/error any asmjit emit failure (badly formed instruction / bad operand) that the Assembler
// would otherwise accept and then ship as a silently truncated block. Records the failure so
// compile_block discards the block instead of adding it.
class JitErrorHandler : public asmjit::ErrorHandler {
public:
  FILE* fp = nullptr;   // disassembly trace file (falls back to stderr if unopened)
  int   cpu_id = -1;
  bool  failed = false;
  void handle_error(asmjit::Error err, const char* message, asmjit::BaseEmitter*) override {
    (void) err;
    failed = true;
    fprintf(fp ? fp : stderr, "[JIT][CPU%d][EMIT-ERROR] %s\n", cpu_id, message);
  }
};
#endif

enum SafeOp {
  OP_NONE,
  OP_ADDQ, OP_SUBQ, OP_ADDL, OP_SUBL,
  OP_S4ADDQ, OP_S8ADDQ, OP_S4SUBQ, OP_S8SUBQ,
  OP_S4ADDL, OP_S8ADDL, OP_S4SUBL, OP_S8SUBL,   // INTA (0x10) scaled longword: sext32((Ra*scale) +/- Rb)
  OP_CMPBGE,                                    // INTA (0x10) per-byte unsigned compare -> 8-bit mask
  OP_SEXTB, OP_SEXTW,                            // FPTI (0x1c) CIX/BWX: Rc = sign-extend byte/word of op2
  OP_CTPOP, OP_CTLZ, OP_CTTZ,                    // FPTI (0x1c) CIX bit-count of op2: popcount / leading / trailing zeros
  OP_ITOFS, OP_ITOFF, OP_ITOFT,                  // ITFP (0x14) int->FP reg moves: f[Fc] = fmt(Ra)
  OP_FTOIS, OP_FTOIT,                            // FPTI (0x1c) FP->int reg moves: Rc = fmt(f[Fa])
  OP_FLTL,                                       // FLTL (0x17) non-arithmetic: FPCR moves, CPYSx, FCMOVx, CVTLQ/QL
  OP_CVTQT, OP_CVTQS,                            // FLTI (0x16) int->IEEE convert: f[Fc] = (T/S)(s64)f[Fb] via SSE
  OP_ADDT, OP_SUBT, OP_MULT, OP_DIVT,            // FLTI (0x16) IEEE T-float arith: f[Fc] = f[Fa] op f[Fb] via SSE
  OP_CMPTUN, OP_CMPTEQ, OP_CMPTLT, OP_CMPTLE,    // FLTI (0x16) IEEE T-float compares: f[Fc] = (cmp) ? 2.0 : 0.0
  OP_ADDS, OP_SUBS, OP_MULS, OP_DIVS,            // FLTI (0x16) IEEE S-float arith: f[Fc] = round_single(f[Fa] op f[Fb])
  OP_CVTST, OP_CVTTS, OP_CVTTQ,                  // FLTI (0x16) IEEE converts: S->T widen, T->S narrow, T->Q to int
  OP_SQRTS, OP_SQRTT,                            // ITFP (0x14) IEEE sqrt: f[Fc] = sqrt(f[Fb]) via sqrtss/sqrtsd
  OP_FLTV,                                       // FLTV (0x15) VAX arith/convert/compare: f[Fc] via jit_fltv helper
  OP_AND, OP_BIS, OP_XOR, OP_BIC, OP_ORNOT, OP_EQV,
  OP_CMOV,                       // INTL (0x11) conditional moves CMOVxx: Rc = cond(Ra) ? op2 : Rc
  OP_AMASK, OP_IMPLVER,          // INTL (0x11) CPU feature probes: Rc = op2 & ~CPU_AMASK / Rc = CPU_IMPLVER
  OP_CMPEQ, OP_CMPLT, OP_CMPLE, OP_CMPULT, OP_CMPULE,
  OP_SLL, OP_SRL, OP_SRA, OP_MULQ,
  OP_MULL, OP_UMULH,             // INTM (0x13): MULL = sext32(Ra*op2); UMULH = hi64 of unsigned Ra*op2
  OP_EXTL, OP_EXTH, OP_INSL, OP_INSH, OP_MSKL, OP_MSKH, OP_ZAP,   // INTS (0x12) byte-manip (Rb&7 keyed)
  OP_NOP, OP_MFENCE,             // MISC (0x18): prefetch/cache hints (no-op), barriers (mfence)
  OP_RPCC, OP_RC, OP_RS,         // MISC (0x18) state reads (Ra dest): cycle counter / read-and-clear,set intr flag
  OP_LDQ, OP_LDL,                // memory-format loads: Ra = MEM[Rb + disp16]
  OP_LDBU, OP_LDWU,              // BWX byte/word loads (0x0a/0x0c): Ra = zero-extend MEM[Rb+disp16].{b,w}
  OP_LDL_L, OP_LDQ_L,            // load-locked (0x2a/0x2b): Ra = MEM[Rb+disp16] + establish LL/SC monitor
  OP_STL_C, OP_STQ_C,            // store-conditional (0x2e/0x2f): cond store Ra -> MEM; Ra = success(1)/fail(0)
  OP_STQ, OP_STL,                // memory-format stores: MEM[Rb + disp16] = Ra
  OP_STB, OP_STW,                // BWX byte/word stores (0x0e/0x0d): MEM[Rb + disp16].{b,w} = Ra
  OP_LDQ_U, OP_STQ_U,            // unaligned quad (0x0b/0x0f): MEM[(Rb+disp16) & ~7] load/store
  OP_LDT, OP_LDS, OP_STT, OP_STS, // FP memory (0x23/0x22/0x27/0x26): f[Fa] <-> MEM, LDT/STT raw, LDS/STS ieee conv
  OP_LDF, OP_LDG, OP_STF, OP_STG, // VAX FP memory (0x20/0x21/0x24/0x25): f[Fa] <-> MEM, F/G format conversion (helper)
  OP_LDA, OP_LDAH,               // load-address: Ra = Rb + disp16 (<<16 for LDAH); pure ALU
  OP_HW_MFPR,                    // HW_MFPR (0x19), PALmode only: Ra = IPR[(ins>>8)&0xff] via helper
  OP_HW_LDL, OP_HW_LDQ,          // HW_LD (0x1b) physical func 0/1, PALmode only: Ra = phys[Rb+disp12]
  OP_HW_LDQ_VPTE,                // HW_LD (0x1b) func 5: virtual PTE fetch, access-checked vs KERNEL mode
  OP_HW_LDL_WCHK,                // HW_LD (0x1b) func 0xa: longword virtual read + write-check (WrChk)
  OP_HW_MTPR,                    // HW_MTPR (0x1d) side-effect-free IPRs, PALmode only: IPR[fn] = Rb
  OP_HW_MTPR_TERM,               // HW_MTPR I_CTL (0x11): writes SDE/SPE/VA mode -> terminate, re-dispatch past it
  OP_HW_STL, OP_HW_STQ,          // HW_ST (0x1f) physical func 0/1, PALmode only: phys[Rb+disp12] = Ra
  OP_JMP,                        // JMP/JSR/RET (0x1a): Ra = PC+4; PC = Rb & ~3 (computed target)
  OP_HW_RET,                     // HW_RET (0x1e), PALmode: PC = Rb & ~2 (computed jump, the PAL return)
  OP_CALL_PAL,                   // CALL_PAL (0x00): save R23/exc_addr; PC = pal_base | entry offset
  // Branch-format terminators (contiguous; see is_branch). Conditional on Ra, plus BR/BSR.
  OP_BEQ, OP_BNE, OP_BLT, OP_BLE, OP_BGT, OP_BGE, OP_BLBC, OP_BLBS, OP_BR, OP_BSR,
  OP_FBEQ, OP_FBNE, OP_FBLT, OP_FBLE, OP_FBGT, OP_FBGE   // FP branches (0x31-0x37): branch on f[Fa] vs 0.0
};

static inline bool is_branch(SafeOp op) { return op >= OP_BEQ && op <= OP_FBGE; }
static inline bool is_fp_branch(SafeOp op) { return op >= OP_FBEQ && op <= OP_FBGE; }
static inline bool is_store(SafeOp op)  { return op == OP_STQ || op == OP_STL; }
// A terminator ends the block and writes its own next PC (branches + the computed jump).
static inline bool is_terminator(SafeOp op) { return op == OP_JMP || op == OP_HW_RET || op == OP_CALL_PAL || op == OP_HW_MTPR_TERM || is_branch(op); }

// POPCNT isn't baseline x86-64 (pre-2008 CPUs lack it); query the host once. CTLZ/CTTZ use
// baseline BSR/BSF, so only CTPOP is gated -- it stays interpreted when the host lacks POPCNT.
static bool host_has_popcnt() {
  static const bool ok = asmjit::CpuInfo::host().features().x86().has_popcnt();
  return ok;
}

// Safe = goto-free, register-only operate-format ops (no trap, memory, or branch).
// pal_block enables PALmode-only ops (HW_MFPR): outside PALmode they'd OPCDEC, so only
// compile them when the block is PALmode (the dispatcher keys blocks by PC bit 0).
SafeOp classify(uint32_t ins, bool pal_block)
{
  uint32_t opcode = ins >> 26;
  uint32_t func = (ins >> 5) & 0x7F;
  switch (opcode) {
    case 0x10: // INTA
      switch (func) {
        case 0x20: return OP_ADDQ;   case 0x29: return OP_SUBQ;
        case 0x00: return OP_ADDL;   case 0x09: return OP_SUBL;
        case 0x22: return OP_S4ADDQ; case 0x32: return OP_S8ADDQ;
        case 0x2b: return OP_S4SUBQ; case 0x3b: return OP_S8SUBQ;
        case 0x02: return OP_S4ADDL; case 0x12: return OP_S8ADDL;
        case 0x0b: return OP_S4SUBL; case 0x1b: return OP_S8SUBL;
        case 0x0f: return OP_CMPBGE;
        case 0x2d: return OP_CMPEQ;  case 0x4d: return OP_CMPLT;
        case 0x6d: return OP_CMPLE;  case 0x1d: return OP_CMPULT;
        case 0x3d: return OP_CMPULE;
        // ADDL/V (0x40), SUBL/V (0x49), ADDQ/V (0x60), SUBQ/V (0x69): overflow-trapping -> interpret
      }
      break;
    case 0x11: // INTL
      switch (func) {
        case 0x00: return OP_AND;   case 0x20: return OP_BIS;
        case 0x40: return OP_XOR;   case 0x08: return OP_BIC;
        case 0x28: return OP_ORNOT; case 0x48: return OP_EQV;
        case 0x14: case 0x16: case 0x24: case 0x26:   // CMOVLBS/LBC/EQ/NE
        case 0x44: case 0x46: case 0x64: case 0x66:   // CMOVLT/GE/LE/GT
          return OP_CMOV;
        case 0x61:                                    // AMASK: Ra must be R31 (else the interp's
          // DO_AMASK traps OPCDEC) -- compile only the architecturally valid form.
          if (((ins >> 21) & 0x1f) != 31) return OP_NONE;
          return OP_AMASK;
        case 0x6c: return OP_IMPLVER;                 // IMPLVER: Rc = implementation version constant
      }
      break;
    case 0x12: // INTS: shifts + byte-manipulation (extract / insert / mask / zap), Rb&7 keyed
      switch (func) {
        case 0x39: return OP_SLL; case 0x34: return OP_SRL; case 0x3c: return OP_SRA;
        case 0x06: case 0x16: case 0x26: case 0x36: return OP_EXTL;   // EXTBL/WL/LL/QL
        case 0x5a: case 0x6a: case 0x7a:            return OP_EXTH;   // EXTWH/LH/QH
        case 0x0b: case 0x1b: case 0x2b: case 0x3b: return OP_INSL;   // INSBL/WL/LL/QL
        case 0x57: case 0x67: case 0x77:            return OP_INSH;   // INSWH/LH/QH
        case 0x02: case 0x12: case 0x22: case 0x32: return OP_MSKL;   // MSKBL/WL/LL/QL
        case 0x52: case 0x62: case 0x72:            return OP_MSKH;   // MSKWH/LH/QH
        case 0x30: case 0x31:                       return OP_ZAP;    // ZAP / ZAPNOT
      }
      break;
    case 0x13: // INTM (integer multiply). MULL/V (0x40) + MULQ/V (0x60) overflow-trap -> interpret.
      if (func == 0x20) return OP_MULQ;
      if (func == 0x00) return OP_MULL;    // 32-bit multiply, low 32 sign-extended
      if (func == 0x30) return OP_UMULH;   // high 64 bits of the unsigned 128-bit product
      break;
    case 0x14: {  // ITFP: compile the int->FP register moves + IEEE SQRT*. Fc==31 is interpreted
      // (the interp zeroes f[31] per instruction -- a compiled write would desync).
      const uint32_t f14 = (ins >> 5) & 0x7ff;
      if ((ins & 0x1f) == 31) return OP_NONE;             // Fc==31 -> interp
      const uint32_t sb = f14 & 0x3f;                     // IEEE sqrt (source Fb): SQRTS 0x0b / SQRTT 0x2b
      if (sb == 0x0b || sb == 0x2b) {
        const uint32_t r = (f14 >> 6) & 3;
        if (r == 0 || r == 1) return OP_NONE;             // /C, /M: SSE rounds nearest -> interpret
        if (((f14 >> 8) & 7) == 7) return OP_NONE;        // /SUI -> interpret
        return (sb == 0x2b) ? OP_SQRTT : OP_SQRTS;
      }
      if (((ins >> 16) & 0x1f) != 31) return OP_NONE;     // ITOFx: Rb must be 31 (Ra is the int source)
      if (f14 == 0x004) return OP_ITOFS;
      if (f14 == 0x014) return OP_ITOFF;
      if (f14 == 0x024) return OP_ITOFT;
      return OP_NONE;
    }
    case 0x17: {  // FLTL: all non-arithmetic (FPCR moves, sign-copies, FCMOVs, CVTLQ/QL) via
      // jit_fltl; only the /V trap variants of CVTQL stay interpreted.
      const uint32_t f17 = (ins >> 5) & 0x7ff;
      const bool ok = f17 == 0x010 || (f17 >= 0x020 && f17 <= 0x022) || f17 == 0x024
                   || f17 == 0x025 || (f17 >= 0x02a && f17 <= 0x02f) || f17 == 0x030;
      if (!ok) return OP_NONE;
      // f31-dest gate (interp zeroes f[31] per instr): MF_FPCR writes f[Fa]; MT_FPCR has no FP dest
      if (f17 == 0x025)      { if (((ins >> 21) & 0x1f) == 31) return OP_NONE; }
      else if (f17 != 0x024) { if ((ins & 0x1f) == 31) return OP_NONE; }
      return OP_FLTL;
    }
    case 0x15: { // FLTV (VAX): helper-dispatched arith/convert/compare; bail to the interp on trap.
      if ((ins & 0x1f) == 31) return OP_NONE;       // Fc==31: interp zeroes f[31] per instr
      const uint32_t f = (ins >> 5) & 0x7ff;
      if (f == 0x0a5 || f == 0x4a5 || f == 0x0a6 || f == 0x4a6 || f == 0x0a7 || f == 0x4a7   // CMPGEQ/LT/LE
       || f == 0x03c || f == 0x0bc || f == 0x03e || f == 0x0be) return OP_FLTV;              // CVTQF/CVTQG
      if (f & 0x200) return OP_NONE;                 // invalid qualifier -> interp OPCDECs (UNKNOWN2)
      switch (f & 0x7f) {                            // base-op arith / G<->F<->D / G->Q convert
        case 0x000: case 0x001: case 0x002: case 0x003:   // ADDF/SUBF/MULF/DIVF
        case 0x01e:                                       // CVTDG
        case 0x020: case 0x021: case 0x022: case 0x023:   // ADDG/SUBG/MULG/DIVG
        case 0x02c: case 0x02d: case 0x02f:               // CVTGF/CVTGD/CVTGQ
          return OP_FLTV;
      }
      return OP_NONE;                                // other 0x15 funcs: interp OPCDECs
    }
    case 0x16: { // FLTI (IEEE): SSE-inline the int->float converts; bail to the interp on traps/edges.
      const uint32_t f = (ins >> 5) & 0x7ff;
      if ((ins & 0x1f) == 31) return OP_NONE;       // Fc==31: interp zeroes f[31] per instr (all 0x16)
      if (f == 0x2ac || f == 0x6ac) return OP_CVTST;  // CVTST (S->T): before the invalid gate (SRC bit set);
                                                      // S-denormal renormalizes / NaN quiets -> bail, else valid-T copy
      if ((f & 0x3f) == 0x2f) {                       // CVTTQ (T->Q int): /C chop is valid -> handle before round gate
        if (((f >> 6) & 3) == 1) return OP_NONE;      // /M -> interp
        if (((f >> 8) & 7) == 7) return OP_NONE;      // /SUI -> interp
        if (((f & 0x600) == 0x200) || ((f & 0x500) == 0x400)) return OP_NONE;  // invalid qualifier
        return OP_CVTTQ;
      }
      if (((f & 0x600) == 0x200) || ((f & 0x500) == 0x400)) return OP_NONE;  // invalid qualifier -> OPCDEC
      const uint32_t rnd = (f >> 6) & 3;            // 0=/C 1=/M 2=/N 3=/D (dynamic, runtime-checked)
      if (rnd == 0 || rnd == 1) return OP_NONE;     // chopped / minus-inf: SSE rounds to nearest -> interpret
      if (((f >> 8) & 7) == 7) return OP_NONE;      // /SUI: traps on every inexact -> interpret
      if (f == 0x0a4 || f == 0x5a4) return OP_CMPTUN;   // IEEE compares (None + /SU): f[Fc] = (cmp) ? 2.0 : 0.0
      if (f == 0x0a5 || f == 0x5a5) return OP_CMPTEQ;   // full-function; a NaN operand bails to the interp
      if (f == 0x0a6 || f == 0x5a6) return OP_CMPTLT;
      if (f == 0x0a7 || f == 0x5a7) return OP_CMPTLE;
      const uint32_t baseop = f & 0x3f;
      if (baseop == 0x00) return OP_ADDS;          // ADDS \  IEEE S-float (single) arith via SSE
      if (baseop == 0x01) return OP_SUBS;          // SUBS  >
      if (baseop == 0x02) return OP_MULS;          // MULS  |
      if (baseop == 0x03) return OP_DIVS;          // DIVS /
      if (baseop == 0x20) return OP_ADDT;          // ADDT \  IEEE T-float (double) arith via SSE:
      if (baseop == 0x21) return OP_SUBT;          // SUBT  > f[Fc] = f[Fa] op f[Fb]
      if (baseop == 0x22) return OP_MULT;          // MULT  |
      if (baseop == 0x23) return OP_DIVT;          // DIVT /
      if (baseop == 0x2c) return OP_CVTTS;         // CVTTS (T->S narrow): f[Fc] = round_single(f[Fb])
      if (baseop == 0x3e && (f & 0x300) != 0x100) return OP_CVTQT;  // CVTQT: f[Fc] = (double)(s64) f[Fb]
      if (baseop == 0x3c && (f & 0x300) != 0x100) return OP_CVTQS;  // CVTQS: f[Fc] = (single)(s64) f[Fb]
      return OP_NONE;                               // all other 0x16 funcs interpreted
    }
    case 0x1c: // FPTI (CIX/BWX/MVI/FP-moves): the sign-extends, bit-counts, and FP->int register
      // moves compile here (CTPOP needs host POPCNT). MVI packed media (PERR/MIN/MAX/PK/UNPK)
      // stays interpreted for now.
      if (func == 0x00) return OP_SEXTB;   // sign-extend byte of op2
      if (func == 0x01) return OP_SEXTW;   // sign-extend word of op2
      if (func == 0x32) return OP_CTLZ;    // count leading zeros of op2  (BSR; op2==0 -> 64)
      if (func == 0x33) return OP_CTTZ;    // count trailing zeros of op2 (BSF; op2==0 -> 64)
      if (func == 0x30 && host_has_popcnt()) return OP_CTPOP;   // popcount; interpret when host lacks POPCNT
      if (func == 0x70 || func == 0x78) {  // FTOIT / FTOIS: Rb must be 31; Rc==31 -> interpret
        if (((ins >> 16) & 0x1f) != 31 || (ins & 0x1f) == 31) return OP_NONE;
        return (func == 0x70) ? OP_FTOIT : OP_FTOIS;
      }
      break;
    case 0x18: // MISC: memory barriers -> mfence (keep MP ordering); prefetch/cache hints -> no-op
      switch (ins & 0xFFFF) {
        case 0x0000: case 0x0400:                    // TRAPB, EXCB
        case 0x4000: case 0x4400: return OP_MFENCE;  // MB, WMB
        case 0x8000: case 0xA000: case 0xE800:       // FETCH, FETCH_M, ECB
        case 0xF800: case 0xFC00: return OP_NOP;     // WH64, WH64EN
        // RPCC/RC/RS read time-varying / consumed state into Ra; the verify log+replays the read so the
        // two passes agree. RC/RS also side-effect the soft-interrupt flag, so the Ra==31 (no GPR dest,
        // flag-only) forms are compiled too -- the helper still runs, the store is skipped. RPCC Ra==31
        // is a pure no-op read (nothing to replay), so it stays interpreted.
        case 0xC000: if (((ins >> 21) & 0x1f) != 31) return OP_RPCC; break;  // RPCC (cycle counter)
        case 0xE000: return OP_RC;   // RC (read & clear soft-intr flag); Ra==31 -> clear-only
        case 0xF000: return OP_RS;   // RS (read & set soft-intr flag); Ra==31 -> set-only
      }
      break;
    case 0x00: {                // CALL_PAL: compile valid standard funcs (priv 0x00-0x3f, unpriv 0x80-0xbf)
      const uint32_t fn = ins & 0x1FFFFFFF;
      if (fn == 0x3E) break;    // WTINT: interpret -> DO_CALL_PAL (native idle completion when enabled)
      if (fn <= 0x3F || (fn >= 0x80 && fn <= 0xBF)) return OP_CALL_PAL;
      break;                    // SRM specials (0x1234xx) / invalid ranges -> interpret
    }
    case 0x08: return OP_LDA;   // load address (Ra = Rb + disp16) -- pure ALU, no memory
    case 0x09: return OP_LDAH;  // load address high (Ra = Rb + (disp16 << 16))
    case 0x19: {                // HW_MFPR: read IPR -> Ra. PALmode-only (else OPCDEC).
      // ISUM (fn 0x0d) reads async interrupt lines -- compiled via log/replay. Only the IPRs
      // jit_hw_mfpr implements compile: an unknown function must reach the interpreter's
      // UNKNOWN2 (OPCDEC trap), which the helper's keep-Ra default would silently skip.
      if (!pal_block) return OP_NONE;
      const uint32_t fn = (ins >> 8) & 0xff;
      const bool known = ((fn & 0xc0) == 0x40)                                   // PCTX group
                      || (fn >= 0x05 && fn <= 0x0d) || fn == 0x0f || fn == 0x10
                      || fn == 0x11 || fn == 0x14 || fn == 0x16 || fn == 0x27
                      || fn == 0x2a || fn == 0x2b || fn == 0xc0 || fn == 0xc2 || fn == 0xc3;
      return known ? OP_HW_MFPR : OP_NONE;
    }
    case 0x1b: {                // HW_LD: read phys[Rb+disp12] -> Ra. PALmode-only. Compile the
      // physical forms (func 0/1), the quad VPTE fetch (func 5, kernel-checked -- jit_read_vpte),
      // and the longword virtual WrChk (func 0xa, jit_read_wchk). Locked + alt forms interpret.
      if (!pal_block) return OP_NONE;
      const uint32_t f = (ins >> 12) & 0xf;
      if (f == 0) return OP_HW_LDL;
      if (f == 1) return OP_HW_LDQ;
      if (f == 5) {             // Ra==31: nothing to log/replay -> interpret
        if (((ins >> 21) & 0x1f) == 31) return OP_NONE;
        return OP_HW_LDQ_VPTE;
      }
      if (f == 10) {            // func 0xa (HRM TYPE 1012): longword virtual read + WrChk -- jit_read_wchk
        if (((ins >> 21) & 0x1f) == 31) return OP_NONE;   // Ra==31: probe-only, interpret for the fault
        return OP_HW_LDL_WCHK;
      }
      return OP_NONE;
    }
    case 0x1d: {                // HW_MTPR (PALmode): compile the pure-store IPRs, the TB fills
      // (idempotent add_tb_i/_d), IER (field stores + check_int kick), the ITB invalidates (idempotent
      // tbia/tbiap/tbis -> note_itb_invalidate), IC_FLUSH (lazy flush; reclaim deferred off the
      // compiled frame), I_CTL (terminator: writes SDE/SPE/VA mode), CM/SIRR (mode + soft-int fields,
      // check_int kick), and the 0x40-7f AST/FPEN/PPCEN stores. DTB invalidates (dpc coherence), ASN
      // writes, HW_INT_CLR, PAL_BASE, VA_CTL (translation/flush) stay interpreted.
      if (!pal_block) return OP_NONE;
      const uint32_t mfn = (ins >> 8) & 0xff;
      // 0x40-0x7f bitmask group: bit 0 writes ASN (dpc flush + asn-epoch bump) -> must terminate; the
      // rest (ASTER/ASTRR/PPCEN/FPEN) are pure field stores (+check_int), safe to compile.
      if ((mfn & 0xc0) == 0x40) return (mfn & 1) ? OP_NONE : OP_HW_MTPR;
      switch (mfn) {
        case 0x00: case 0x14: case 0x20: case 0x26:   // ITB_TAG, PCTR_CTL, DTB_TAG0, DTB_ALTMODE
        case 0x29: case 0xa0: case 0xc0:              // DC_CTL, DTB_TAG1, CC
        case 0x01: case 0x21: case 0xa1:              // ITB_PTE, DTB_PTE0, DTB_PTE1 (TB fills)
        case 0x02: case 0x03: case 0x04:              // ITB_IAP, ITB_IA, ITB_IS (idempotent ITB invalidates)
        case 0x13:                                    // IC_FLUSH (lazy gen-bump flush; reclaim deferred off-frame)
        case 0x0a:                                    // IER (interrupt enables + check_int kick)
        case 0x09: case 0x0b: case 0x0c:              // CM, IER_CM, SIRR (mode/soft-int fields + check_int)
          return OP_HW_MTPR;
        case 0x11:                                    // I_CTL: changes SDE (shadow remap)/SPE/VA mode -> terminate
          return OP_HW_MTPR_TERM;
        case 0x15: case 0x17: case 0x27:              // CLR_MAP, SLEEP, MM_STAT (no-ops)
        case 0x2b: case 0x2c: case 0x2d:              // C_DATA, C_SHIFT, M_FIX (no-ops)
          return OP_NOP;
      }
      return OP_NONE;
    }
    case 0x1f: {                // HW_ST (PALmode): compile physical longword/quadword (func 0/1).
      // Store-conditional (2/3) does LL/SC, virtual (4/5) and virtual-alt (12/13) translate with
      // side effects -> interpret. Mirrors HW_LD; the value is Ra, base Rb, displacement 12-bit.
      if (!pal_block) return OP_NONE;
      const uint32_t f = (ins >> 12) & 0xf;
      if (f == 0) return OP_HW_STL;
      if (f == 1) return OP_HW_STQ;
      return OP_NONE;
    }
    case 0x1a: return OP_JMP;   // JMP/JSR/RET: computed jump (target = Rb & ~3). Now compiled --
                                // chained in-frame via a block-cache lookup (jit_indirect), which
                                // handles varying targets without the old single-slot thrash.
    case 0x1e:                  // HW_RET (HWREI): a PAL return, also a simple computed jump (Rb & ~2).
      return pal_block ? OP_HW_RET : OP_NONE;
    case 0x28: return OP_LDL;   // memory-format loads (Ra = MEM[Rb+disp16])
    case 0x29: return OP_LDQ;
    case 0x0a: return OP_LDBU;  // BWX byte/word loads (Ra = zero-extend MEM[Rb+disp16].{b,w})
    case 0x0c: return OP_LDWU;
    case 0x0b: return OP_LDQ_U;  // unaligned quad load: Ra = MEM[(Rb+disp16) & ~7]
    case 0x2a:                  // LDL_L / LDQ_L: the load-locked half of LL/SC. Compile the value-
    case 0x2b:                  // returning forms only -- Ra==31 (lock without consuming the value)
      // leaves the loaded value out of the GPRs, so the verify can't replay it; interpret those.
      if (((ins >> 21) & 0x1f) == 31) return OP_NONE;
      return (opcode == 0x2a) ? OP_LDL_L : OP_LDQ_L;
    case 0x2c: return OP_STL;   // memory-format stores (MEM[Rb+disp16] = Ra)
    case 0x2d: return OP_STQ;
    case 0x0e: return OP_STB;   // BWX byte/word stores (MEM[Rb+disp16].{b,w} = Ra low bits)
    case 0x0d: return OP_STW;
    case 0x0f: return OP_STQ_U;  // unaligned quad store: MEM[(Rb+disp16) & ~7] = Ra
    case 0x23: return OP_LDT;   // FP loads (f[Fa] = MEM[Rb+disp16]): LDT raw 8B
    case 0x22: return OP_LDS;   //                                    LDS ieee_lds(4B)
    case 0x27: return OP_STT;   // FP stores (MEM[Rb+disp16] = f[Fa]): STT raw 8B
    case 0x26: return OP_STS;   //                                     STS ieee_sts(4B)
    case 0x20: return OP_LDF;   // VAX FP loads:  LDF vax_ldf(4B)
    case 0x21: return OP_LDG;   //                LDG vax_ldg(8B)
    case 0x24: return OP_STF;   // VAX FP stores: STF vax_stf(4B)
    case 0x25: return OP_STG;   //                STG vax_stg(8B)
    case 0x2e:                  // STL_C / STQ_C: the store-conditional half of LL/SC. Ra is both the
    case 0x2f:                  // value AND the success/fail dest. Compile Ra!=31 (Ra==31 discards the
      // result into R31, so the verify can't capture it from a GPR); interpret those.
      if (((ins >> 21) & 0x1f) == 31) return OP_NONE;
      return (opcode == 0x2e) ? OP_STL_C : OP_STQ_C;
    // Branch format: opcode | Ra | disp21. Conditional on Ra, plus BR/BSR.
    case 0x30: return OP_BR;    case 0x34: return OP_BSR;
    case 0x38: return OP_BLBC;  case 0x39: return OP_BEQ;
    case 0x3a: return OP_BLT;   case 0x3b: return OP_BLE;
    case 0x3c: return OP_BLBS;  case 0x3d: return OP_BNE;
    case 0x3e: return OP_BGE;   case 0x3f: return OP_BGT;
    case 0x31: return OP_FBEQ;  case 0x32: return OP_FBLT;   // FP branches: FPSTART, then f[Fa] vs 0.0
    case 0x33: return OP_FBLE;  case 0x35: return OP_FBNE;
    case 0x36: return OP_FBGE;  case 0x37: return OP_FBGT;
  }
  return OP_NONE;
}

#endif // ES40_JIT_X64

} // namespace

// Defined further down; forward-declared so compile_block's punch-list print can use it.
static const char* opcode_name(unsigned op);

CJitEngine::CJitEngine(int cpu_id) : m_cpu_id(cpu_id), m_recorded(0), m_code_bytes(0), m_rt(nullptr)
{
  memset(m_blocks, 0, sizeof(m_blocks));    // flush() is lazy (gen bump) -- zero the slots here
  memset(m_traces, 0, sizeof(m_traces));    // trace tier: empty until formation fills slots
  // Trace tier kill-switch (config_debug.h JIT_TRACES). OFF by default, 1-block traces preempt block
  // chaining = a net loss; re-enable when fusion closes loops in-trace.
#ifdef JIT_TRACES
  m_traces_enabled = true;
#else
  m_traces_enabled = false;
#endif
  m_rt = new asmjit::JitRuntime();
#ifdef JIT_VERIFY
  m_v_exec = m_v_fail = 0;
#endif
#ifdef JIT_STATS
  m_stat_native = m_stat_interp = m_stat_hot = m_stat_miss = 0;
  m_stat_compiled = m_stat_plen_sum = m_stat_code_bytes = 0;
  m_stat_wall_last_ns = (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  m_tsc_compiled = m_tsc_interp = 0;
  m_tsc_window_start = jit_rdtsc();
  m_bail_link = m_jmp_attempt = m_jmp_hit = 0;
  m_fresh_cold = m_fresh_tag = m_fresh_asn = m_fresh_phys = m_fresh_hash = 0;
  m_trace_formed = m_trace_entered = m_trace_exits = m_trace_stale = 0;
  memset(m_term_op, 0, sizeof(m_term_op));
  memset(m_pal_func, 0, sizeof(m_pal_func));
  memset(m_mtpr_func, 0, sizeof(m_mtpr_func));
  memset(m_hwld_func, 0, sizeof(m_hwld_func));
  memset(m_misc_func, 0, sizeof(m_misc_func));
  m_first_breaker_logged = false;
#endif
#ifdef JIT_DISASM
  char name[64];
  snprintf(name, sizeof name, "jit_disasm_cpu%d.txt", m_cpu_id);
  m_disasm_fp = fopen(name, "w");
  if (!m_disasm_fp)
    fprintf(stderr, "[JIT][CPU%d] could not open %s for the disassembly trace\n", m_cpu_id, name);
#endif
#ifdef JIT_VERIFY
  if (cpu_id == 0) trace_selftest();   // M0: validate trace_ok's source-coherence once (SMC/IMB/remap)
#endif
}

CJitEngine::~CJitEngine()
{
  delete (asmjit::JitRuntime*) m_rt;
#ifdef JIT_DISASM
  if (m_disasm_fp) fclose(m_disasm_fp);
#endif
}

// FNV-1a/64 over a block's source words -- record() uses it to revalidate kept code after a flush
// instead of recompiling, and to catch self-modifying code (changed bytes -> different hash).
static inline uint64_t src_hash(const uint8_t* p, uint32_t n_instr)
{
  const uint32_t* w = (const uint32_t*) p;
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t i = 0; i < n_instr; ++i) { h ^= w[i]; h *= 1099511628211ULL; }
  return h;
}

CJitEngine::JitBlock* CJitEngine::record(uint64_t virt_pc, uint64_t phys_pc, uint32_t asn, bool asm_global, uint32_t n_instr, const uint8_t* dram)
{
  JitBlock& b = m_blocks[index_of(virt_pc)];
  // record() is only reached after the dispatcher validated the live physical, and every returning
  // branch below verifies the code bytes (flush-fresh or hash), so stamp the chain epoch here.
  b.vgen = m_vgen_cur;
  // Still valid + matching + flush-fresh: nothing flushed us since last seen, so the code can't
  // have changed. (flush_gen-stale must NOT take this no-hash path -- post-IMB needs the hash.)
  if (b.valid && b.flush_gen == m_flush_gen && b.tag == virt_pc && (b.asm_global || b.asn == asn)
      && b.phys == phys_pc) {
    b.n_instr = n_instr;
    return &b;
  }
  // Revalidate: a flush dropped the block but kept the compiled code. If the bytes the prefix was
  // compiled from still hash the same, reuse it instead of recompiling. Hash over hash_len, NOT
  // the caller's n_instr -- interrupt-truncated cold passes make n vary for the same block.
  if (b.code && b.tag == virt_pc && (b.asm_global || b.asn == asn) && b.phys == phys_pc
      && b.src_sum == src_hash(dram + phys_pc, b.hash_len)) {
    b.valid = true;
    b.flush_gen = m_flush_gen;
    b.n_instr = n_instr;
    b.jit_body = (void*) ((uint8_t*) (void*) b.code + b.body_off);   // restore chained re-entry
    return &b;
  }
  // New block, page remap, or modified bytes: record fresh and force a recompile.
#ifdef JIT_STATS
  // Why is this a FRESH compile (steps 2+3 both failed)? Categorize the slot's prior occupant in the
  // same order step 3 checks, so we know whether the churn is cache aliasing (tag) -- which more slots
  // fix -- vs same-PC cross-process (asn) -- which needs asn in the index -- vs remap/self-mod/cold.
  if      (!b.code)                         m_fresh_cold++;   // empty / reclaimed slot (genuine cold or warmup)
  else if (b.tag != virt_pc)                m_fresh_tag++;    // a DIFFERENT block aliases this slot (cache-size lever)
  else if (!(b.asm_global || b.asn == asn)) m_fresh_asn++;    // same PC, different process (needs asn-in-index)
  else if (b.phys != phys_pc)               m_fresh_phys++;   // page remap
  else                                      m_fresh_hash++;   // source bytes changed (self-modifying code)
#endif
  b.tag = virt_pc;
  b.phys = phys_pc;
  b.asn = asn;
  b.asm_global = asm_global;
  b.n_instr = n_instr;
  b.valid = true;
  b.flush_gen = m_flush_gen;
  b.code = nullptr;
  b.jit_body = nullptr;   // not compiled yet -> cached links to us must miss until compile
  for (int i = 0; i < kLinkSlots; ++i) b.link[i] = {};   // no cached successors yet
#ifdef JIT_STATS
  b.link_misses = 0; b.link_fanout = 0;   // instrumentation: reset successor-fanout tracking on (re)use
#endif
  b.prefix_len = 0;
  b.compiled = false;
  b.hot = 0;              // fresh block: restart the trace-promotion counter 
#ifdef JIT_REGPROF
  b.rp_hits = 0;          // fresh block: restart the exec counter (resurrect/revalidate keep theirs)
#endif
  if (++m_recorded == 50000)
    printf("[JIT][CPU%d] block dispatcher active: 50000 blocks discovered.\n", m_cpu_id);
  return &b;
}

// Lazy-flush survivor: the dispatcher calls this on a lookup miss, with the LIVE physical it just
// translated. If the slot matches and its source bytes still hash the same, restamp and return it
// straight to the hot path 
CJitEngine::JitBlock* CJitEngine::revalidate_flushed(uint64_t virt_pc, uint32_t asn, uint64_t phys_pc, const uint8_t* dram)
{
  JitBlock& b = m_blocks[index_of(virt_pc)];
  // Resurrect BOTH lazy-flush survivors (flush(): valid, flush_gen-stale) AND flush_non_global() drops
  // (valid cleared). The source-hash below is the guard, matching record()'s revalidate path
  // so don't require valid flags; requiring it forced every flush_non_global'd block through an
  // interpret pass. tag/asn/phys/hash still guard collisions, cross-process aliasing, page remaps, 
  // and self-modifying code.
  if (!(b.code && b.tag == virt_pc && (b.asm_global || b.asn == asn)))
    return nullptr;
  if (b.phys != phys_pc || b.src_sum != src_hash(dram + phys_pc, b.hash_len))
    return nullptr;
  b.valid = true;          // flush_non_global() may have cleared it; the hash just re-validated the bytes
  b.flush_gen = m_flush_gen;
  b.vgen = m_vgen_cur;   // phys + code bytes just validated
  b.jit_body = (void*) ((uint8_t*) (void*) b.code + b.body_off);
  return &b;
}

// Trace tier - is a looked-up trace safe to enter? (review: per-segment source validation.)
// head_live_phys is the head's freshly resolved physical. The steps mirror the block dispatcher's phys
// check (jit_run ~line 832) + revalidate_flushed's hash:
//   1. head remap / ASN-recycle: the head's live physical no longer matches what we built from -> stale.
//   2. epoch fresh (nothing flushed/ITB-invalidated since build) -> enter directly.
//   3. epoch changed -> re-hash every fused segment. Unchanged bytes => the epoch bumped for an unrelated
//      reason (e.g. an IMB on another page): keep the trace + re-stamp. Changed bytes (SMC/remap): stale.
// Interior coherence: the source hash here checks BYTES at the cached phys -- it can't see an interior page
// remapped to a DIFFERENT physical with identical bytes. The CALLER closes that gap by re-resolving each
// segment's LIVE physical: trace_segs_live (trace entry) + the recorder's per-successor check (formation).
bool CJitEngine::trace_ok(TraceFragment* t, uint64_t head_live_phys, const uint8_t* dram)
{
  if (t->n_segs == 0 || t->segs[0].phys_pc != head_live_phys)
    { note_trace_stale(); return false; }             // head remap / ASN recycle
  // A 1-block trace mirrors its head block's compiled prefix, whose length can change with NO source-byte
  // or epoch change: a fault-truncated cold record shrinks prefix_len (n_instr oscillates), a later clean
  // record regrows it invisibly to the hash below. If the live head block compiled a different length,
  // the trace is stale; drop it so the dispatcher re-forms a consistent one.
  for (uint32_t s = 0; s < t->n_segs; ++s) {   // ANY fused block's prefix_len can oscillate with no source/epoch change (not just the head)
    const JitBlock& sb = m_blocks[index_of(t->segs[s].guest_pc)];
    if (sb.valid && sb.tag == t->segs[s].guest_pc && sb.prefix_len != t->segs[s].n_instr) { note_trace_stale(); return false; }
  }
  if (t->vgen == m_vgen_cur)
    return true;                                      // epoch fresh: nothing changed since build
  for (uint32_t i = 0; i < t->n_segs; ++i) {
    const SourceSeg& s = t->segs[i];
    if (s.src_sum != src_hash(dram + s.phys_pc, s.n_instr))
      { note_trace_stale(); return false; }            // a segment's source bytes changed -> stale
  }
  t->vgen = m_vgen_cur;                               // all segments re-validated: re-stamp the epoch
  t->flush_gen = m_flush_gen;
  return true;
}

#ifdef JIT_VERIFY
// unit-test trace_ok's source-coherence decision (SMC/IMB, ITB-remap, head-remap, multi-segment) 
// WITHOUT real traces or the emitter. Mutates m_itb_gen/m_flush_gen but saves/restores; runs 
// once at ctor when the engine is fresh (counters 0, no live blocks). A FAIL here means the
// trace tier's coherence is broken 
void CJitEngine::trace_selftest()
{
  const uint64_t save_itb = m_itb_gen, save_flush = m_flush_gen, save_vgen = m_vgen_cur;
  uint32_t mem[8] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444,
                      0x55555555, 0x66666666, 0x77777777, 0x88888888 };
  const uint8_t* d = (const uint8_t*) mem;

  TraceFragment t = {};
  t.valid = true; t.head_tag = 0x2000; t.asn = 1; t.n_segs = 2;
  t.segs[0] = { 0x2000, 0,  4, false, 1, src_hash(d + 0,  4) };   // words[0..3] at phys 0
  t.segs[1] = { 0x2010, 16, 4, false, 1, src_hash(d + 16, 4) };   // words[4..7] at phys 16
  t.vgen = m_vgen_cur;

  bool ok = true;
  ok &= ( trace_ok(&t, 0,   d) == true  );    // 1. fresh: epoch + head-phys match -> enter
  ok &= ( trace_ok(&t, 999, d) == false );    // 2. head remap / ASN-recycle: live head phys differs -> drop
  note_itb_invalidate();                       // 3. ITB-invalidate, bytes unchanged:
  ok &= ( trace_ok(&t, 0,   d) == true  );    //    epoch bumps, re-hash matches -> keep ...
  ok &= ( t.vgen == m_vgen_cur );              //    ... and re-stamped to the new epoch
  mem[5] = 0xDEADBEEF;                         // 4. SMC on interior seg1 + IMB (flush bump):
  ++m_flush_gen; ++m_vgen_cur;                 //    (flush()'s bump, minus its reclaim bookkeeping)
  ok &= ( trace_ok(&t, 0,   d) == false );    //    re-hash mismatch -> drop
  mem[5] = 0x66666666;                         // 5. restore the byte, epoch still bumped:
  ok &= ( trace_ok(&t, 0,   d) == true  );    //    re-hash matches again -> keep

  printf("[JIT][CPU%d] trace_ok self-test (SMC/IMB/ITB-remap/head-remap): %s\n",
         m_cpu_id, ok ? "PASS" : "*** FAIL ***");
  m_itb_gen = save_itb; m_flush_gen = save_flush; m_vgen_cur = save_vgen;
}
#endif

// Free ALL compiled code (delete+new of the runtime; reset()-and-reuse corrupted the JitAllocator
// block tree) and drop every slot's now-dangling pointers. Safe only from this CPU's cold path
// (never while its compiled code could be executing); runtimes are per-CPU.
void CJitEngine::reclaim_code()
{
  //printf("[JIT][CPU%d] code reclaim: %llu MB freed\n", m_cpu_id,
  //       (unsigned long long) (m_code_bytes >> 20));
  delete (asmjit::JitRuntime*) m_rt;
  m_rt = new asmjit::JitRuntime();
  m_code_bytes = 0;
  m_reclaim_pending = false;   // a reclaim (cold-path or deferred) satisfies any pending request
  for (int i = 0; i < kCacheEntries; ++i) {
    m_blocks[i].valid = false;
    m_blocks[i].code = nullptr;
    m_blocks[i].jit_body = nullptr;
    m_blocks[i].compiled = false;
    // Link snapshots hold raw body pointers into the runtime just freed. The epoch alone can't be
    // trusted.
    for (int sl = 0; sl < kLinkSlots; ++sl) m_blocks[i].link[sl] = {};
  }
  // Traces hold JitFns into the runtime we just deleted -- drop them too, or a post-reclaim trace
  // dispatch jumps through a freed pointer. trace_lookup keys on valid, so clearing it is enough.
  for (int i = 0; i < kTraceEntries; ++i) m_traces[i].valid = false;
}

void CJitEngine::flush()
{
  // LAZY:  don't walk 16K slots each time. Bump the generation instead: stale blocks miss in
  // lookup() and revalidate_flushed() re-hashes their source bytes before they run again.
  ++m_flush_gen;
  ++m_vgen_cur;
  if (m_rt && m_code_bytes >= kReclaimBytes)
    m_reclaim_pending = true;   // DEFER: reclaim frees all code -- unsafe from a compiled IC_FLUSH;
                                // reclaim_if_pending() does it at the next dispatch boundary.
}

// ASM-bit-clear icache flush (process/ASN switch): drop only !asm_global blocks. Global (ASM) PAL
// blocks are ASN-independent and must survive it. The drop is SOFT, revalidate_flushed() re-hashes
// and resurrects the compiled code on next use (no recompile, no interpret pass, unless the bytes
// actually changed)
void CJitEngine::flush_non_global()
{
  if (m_rt && m_code_bytes >= kReclaimBytes) { flush(); return; }
  // The chain guard reads link SNAPSHOTS, not the successor's live jit_body, so the soft drop
  // below is invisible to it so need to invalidate every cached edge by epoch instead. 
  ++m_vgen_cur;
  for (int i = 0; i < kCacheEntries; ++i) {
    if (!m_blocks[i].asm_global) {
      m_blocks[i].valid = false;
      m_blocks[i].jit_body = nullptr;
    }
  }
  for (int i = 0; i < kTraceEntries; ++i) {   // a trace spanning any !asm_global segment depends on a soft-dropped block -> drop it too
    if (!m_traces[i].valid) continue;
    for (uint32_t s = 0; s < m_traces[i].n_segs; ++s)
      if (!m_traces[i].segs[s].asm_global) { m_traces[i].valid = false; note_trace_stale(); break; }
  }
}

#ifdef ES40_JIT_X64   // ---- the x86-64 codegen backend (emit_op / compile_block / compile_trace) ----

// ZAP/ZAPNOT byte-expand: g_zapnot_mask[b] has byte i = 0xFF where bit i of b is set (ZAPNOT keeps
// those bytes; ZAP keeps the complement). Compiled ZAP indexes this instead of an 8-way bit test.
static uint64_t g_zapnot_mask[256];
static const bool g_zapnot_init = [] {
  for (int b = 0; b < 256; ++b) {
    uint64_t m = 0;
    for (int i = 0; i < 8; ++i) if (b & (1 << i)) m |= (uint64_t) 0xff << (i * 8);
    g_zapnot_mask[b] = m;
  }
  return true;
}();

#ifdef JIT_REGPROF
// Bitmask of the Alpha integer GPRs a block's prefix touches (read or written), for pin selection.
// Format-aware over the ops that drive the store-forward chains -- integer operate, memory, branch,
// JMP, and the MISC state reads; FP / HW / CALL_PAL are skipped (not the GPR forwarding path).
static uint32_t regprof_mask(const uint32_t* w, uint32_t n)
{
  uint32_t mask = 0;
  auto touch = [&](int r) { if (r != 31) mask |= (1u << r); };   // R31 is hardwired 0, never a pin
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t ins = w[i];
    const uint32_t op  = ins >> 26;
    const int ra = (ins >> 21) & 0x1f, rb = (ins >> 16) & 0x1f, rc = ins & 0x1f;
    const bool islit = ((ins >> 12) & 1) != 0;
    switch (op) {
      case 0x10: case 0x11: case 0x12: case 0x13: case 0x1c:     // integer operate: Ra, Rc, Rb(if reg)
        touch(ra); touch(rc); if (!islit) touch(rb); break;
      case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c:     // LDA/LDAH + BWX ld/st: Ra, Rb(base)
      case 0x0d: case 0x0e: case 0x0f:
      case 0x28: case 0x29: case 0x2a: case 0x2b:                // int LDL/LDQ/LDx_L
      case 0x2c: case 0x2d: case 0x2e: case 0x2f:                // int STL/STQ/STx_C
      case 0x1a:                                                 // JMP/JSR/RET: Ra(link), Rb(target)
        touch(ra); touch(rb); break;
      case 0x30: case 0x34: case 0x38: case 0x39: case 0x3a:     // integer branches (incl BR/BSR link): Ra
      case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
        touch(ra); break;
      case 0x18: {                                               // MISC: RPCC/RC/RS write Ra
        const uint32_t f = ins & 0xffff;
        if (f == 0xc000 || f == 0xe000 || f == 0xf000) touch(ra);
        break;
      }
      default: break;   // FP / HW / CALL_PAL: not the GPR forwarding path
    }
  }
  return mask;
}
#endif

// The 3 guest GPRs kept live in the callee-saved pins r12/r13/r15. compile_block uses the global hot
// set (RA/a0/PV); compile_trace can override with the trace's own hot regs (the M2 regalloc spike).
static const int kGlobalPins[3] = { 26, 16, 27 };

// memop slow path: the inline fast path's guards jump to `slow`, which is emitted in a
// cold tail after the block epilogue. The stub re-creates emit_call's marshalling + fault 
// bail from captured PODs, then jumps back to `join`.
struct ColdMemStub {
  enum Kind { LOAD, STORE, FPMEM };
  Kind          kind;
  asmjit::Label slow;       // stub entry 
  asmjit::Label join;       // fast-path merge point 
  asmjit::Label done;       // the block's shared bail exit 
  void*         helper;     // jit_read / jit_write / jit_fp_read / jit_fp_write
  int           size_bits;  // LOAD/STORE operand size 
  uint32_t      descr;      // FPMEM only: (fmt<<16)|size 
  int           ra;         // LOAD dest / STORE value guest reg 
  int           pin;        // host reg id bound to `ra`, or -1 = the regs[] memory slot
  int           slot;       // PALshadow-adjusted regs[] index for `ra` 
  uint32_t      i;          // op index in the block 
  uint64_t      fault_pc;   // resume PC on fault 
};

// Emit one cold stub. Arg marshalling mirrors emit_call: non-immediate sources are placed before
// immediates so a size/selector immediate can't overwrite RDX 
static void emit_cold_mem_stub(asmjit::x86::Assembler& a, const uint8_t* gpa,
                               const CJitEngine::JitOffsets& off, const ColdMemStub& s)
{
  using namespace asmjit;
  auto aq = [&](int k) { return x86::gpq(gpa[k]); };
  auto ad = [&](int k) { return x86::gpd(gpa[k]); };
  a.bind(s.slow);
  a.mov(aq(0), x86::rbp);                                       // cpu
  if (aq(1).id() != x86::rdx.id()) a.mov(aq(1), x86::rdx);      // va (precomputed in RDX)
  switch (s.kind) {
    case ColdMemStub::LOAD:                                     // jit_read(cpu, va, size, &out)
      a.lea(aq(3), x86::qword_ptr(x86::rsp, 32));               // &out slot
      a.mov(ad(2), imm((uint32_t) s.size_bits));
      break;
    case ColdMemStub::STORE:                                    // jit_write(cpu, va, size, value)
      if (s.ra == 31)      a.xor_(ad(3), ad(3));                // value (R31 == 0)
      else if (s.pin >= 0) a.mov(aq(3), x86::gpq((uint32_t) s.pin));
      else                 a.mov(aq(3), x86::qword_ptr(x86::rbx, s.slot * 8));
      a.mov(ad(2), imm((uint32_t) s.size_bits));
      break;
    case ColdMemStub::FPMEM:                                    // jit_fp_read/write(cpu, va, fa, descr)
      a.mov(ad(2), imm((uint32_t) s.ra));
      a.mov(ad(3), imm(s.descr));
      break;
  }
  a.mov(x86::rax, imm((uint64_t) s.helper));
  a.call(x86::rax);
  Label ok = a.new_label();
  a.test(x86::eax, x86::eax);
  a.jz(ok);
  a.mov(x86::r10, imm(s.fault_pc));                             // fault: resume at this op
  a.mov(x86::qword_ptr(x86::rbp, off.state_pc), x86::r10);
  a.mov(x86::eax, imm(s.i));                                    // this iteration: i instrs done
  a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));                // + earlier chained iterations
  a.jmp(s.done);
  a.bind(ok);
  if (s.kind == ColdMemStub::LOAD) {
    // Result extraction + dest write must leave RAX = r[ra] to uphold the fast path's
    // value-forward contract at `join` (the next op may reuse RAX as Ra).
    if      (s.size_bits == 64) a.mov(x86::rax, x86::qword_ptr(x86::rsp, 32));
    else if (s.size_bits == 32) a.movsxd(x86::rax, x86::dword_ptr(x86::rsp, 32));
    else if (s.size_bits == 16) a.movzx(x86::eax, x86::word_ptr(x86::rsp, 32));
    else                        a.movzx(x86::eax, x86::byte_ptr(x86::rsp, 32));
    if (s.pin >= 0) a.mov(x86::gpq((uint32_t) s.pin), x86::rax);
    else            a.mov(x86::qword_ptr(x86::rbx, s.slot * 8), x86::rax);
  }
  a.jmp(s.join);
}

void CJitEngine::emit_op(void* a_ptr, const uint8_t* gpa, void* done_ptr, const HelperSet& hs,
    bool pal_block, JitBlock* b, uint32_t ins, uint32_t i, RegAlloc& regalloc, void* cold_ptr)
{
    using namespace asmjit;
    x86::Assembler& a = *(x86::Assembler*)a_ptr;
    Label& done = *(Label*)done_ptr;
    std::vector<ColdMemStub>& cold = *(std::vector<ColdMemStub>*) cold_ptr;
    (void) cold;   // JIT_VERIFY builds keep every helper call inline and never record a stub
    // aliases so the moved if-chain references the helper names verbatim:
    void* read_helper = hs.read_helper;               void* write_helper = hs.write_helper;
    void* opcdec_helper = hs.opcdec_helper;           void* hw_mfpr_helper = hs.hw_mfpr_helper;
    void* hw_ld_helper = hs.hw_ld_helper;             void* hw_mtpr_helper = hs.hw_mtpr_helper;
    void* hw_st_helper = hs.hw_st_helper;             void* indirect_helper = hs.indirect_helper;
    void* read_locked_helper = hs.read_locked_helper; void* stc_helper = hs.stc_helper;
    void* misc_helper = hs.misc_helper;               void* read_vpte_helper = hs.read_vpte_helper;
    void* read_wchk_helper = hs.read_wchk_helper;     void* itof_helper = hs.itof_helper;
    void* ftoi_helper = hs.ftoi_helper;               void* fltl_helper = hs.fltl_helper;
    void* fp_read_helper = hs.fp_read_helper;         void* fp_write_helper = hs.fp_write_helper;
    void* fltv_helper = hs.fltv_helper;

    auto aq = [&](int k) { return x86::gpq(gpa[k]); };
    auto ad = [&](int k) { return x86::gpd(gpa[k]); };
    auto set_pc = [&](uint64_t pc_val) {
        a.mov(x86::r10, imm(pc_val));
        a.mov(x86::qword_ptr(x86::rbp, m_off.state_pc), x86::r10);
        };
    auto pin_id = [&](int r) -> int {        // r -> its bound host reg id, or -1 = the state.r[] memory slot
        return regalloc.host_of(r);
        };

    int ra = (ins >> 21) & 0x1F;
    int rb = (ins >> 16) & 0x1F;
    int rc = ins & 0x1F;
    bool islit = ((ins >> 12) & 1) != 0;
    uint32_t lit = (ins >> 13) & 0xFF;
    SafeOp op = classify(ins, pal_block);

    do {
        // MISC (0x18) barriers/hints: emit an mfence for TRAPB/EXCB/MB/WMB (x86's seq_cst fence, to
        // preserve the guest's MP memory ordering, matching DO_*'s atomic_thread_fence), nothing for
        // the prefetch/cache hints -- then keep going so the block extends straight past them.
        if (op == OP_NOP)    continue;
        if (op == OP_MFENCE) { a.mfence(); continue; }

        // Value-forwarding: rax may still hold the guest reg the previous op computed. Capture that for
        // op1_rax's reuse, then default-invalidate; only mov_to_reg(_, rax) below re-marks what rax holds.
        const int prev_rax = regalloc.rax_holds;
        regalloc.rax_holds = -1;

        auto reg = [&](int r) {
            // PALshadow (RREG, AlphaCPU.h): in a PALmode block with SDE set, R4-7 and R20-23 map to
            // the shadow bank r[r+32]. 
            int idx = (pal_block && ((r & 0xc) == 0x4)) ? r + 32 : r;
            return x86::qword_ptr(x86::rbx, idx * 8);
            };
        // 32-bit (low-dword) view with the SAME shadow remap, for the 32-bit ALU ops (ADDL/SUBL).
        auto reg32 = [&](int r) {
            int idx = (pal_block && ((r & 0xc) == 0x4)) ? r + 32 : r;
            return x86::dword_ptr(x86::rbx, idx * 8);
            };
        // Pin-aware reg access: route through the pinned x86 reg when r is pinned, else the regs[]
        // memory slot via reg()/reg32(). Callers handle r==31 (it varies per site: 0, a displacement,
        // or skip). mov_to_reg writes the slot for any non-pinned r (R31 included, matching the old
        // unconditional stores -- nothing reads r[31] back).
        auto mov_from_reg = [&](const x86::Gp& dst, int r) {        // dst is 64-bit
            int p = pin_id(r);
            if (p >= 0) a.mov(dst, x86::gpq((uint32_t)p));
            else        a.mov(dst, reg(r));
            };
        auto mov_from_reg32 = [&](const x86::Gp& dst, int r) {      // dst is 32-bit
            int p = pin_id(r);
            if (p >= 0) a.mov(dst, x86::gpd((uint32_t)p));
            else        a.mov(dst, reg32(r));
            };
        auto mov_to_reg = [&](int r, const x86::Gp& src) {          // src is 64-bit
            int p = pin_id(r);
            if (p >= 0) a.mov(x86::gpq((uint32_t)p), src);
            else        a.mov(reg(r), src);
            if (src.id() == x86::rax.id() && r != 31) regalloc.rax_holds = r;   // rax now mirrors r[r]; forward it
            };
        auto op1_rax = [&]() {
            if (ra != 31 && prev_rax == ra) return;   // value-forward: rax already holds Ra (prev op's result)
            if (ra == 31) a.xor_(x86::eax, x86::eax);
            else          mov_from_reg(x86::rax, ra);
            };
        auto op2_rcx = [&]() {                  // operand2 (literal, or r[Rb] with r31=0)
            if (islit)         a.mov(x86::rcx, imm(lit));
            else if (rb == 31) a.xor_(x86::ecx, x86::ecx);
            else               mov_from_reg(x86::rcx, rb);
            };
        // op2 as a DIRECT ALU source, folding away the mov-to-rcx: literal imm, R31 -> 0, a pinned host
        // reg, or the regs[] memory slot -- all valid `OP rax, <src>` sources. emit_alu2 uses it for the
        // simple accumulate-into-rax ops (rax stays the dest, so no aliasing concern).
        auto op2op = [&]() -> Operand {
            if (islit)    return imm(lit);
            if (rb == 31) return imm(0);
            int p = pin_id(rb);
            if (p >= 0)   return x86::gpq((uint32_t) p);
            return reg(rb);
            };
        auto emit_alu2 = [&](uint32_t instId) { op1_rax(); a.emit(instId, x86::rax, op2op()); };

        // ABI-native helper call.
        // Each JitArg names an argument source; the k # source is placed in arg register k (aq/ad).
        // Non-immediate sources are emitted before immediates so a size/selector immediate can't
        // overwrite RDX while JA_VA (the only register-sourced argument) still needs it.
        enum JitArgKind { JA_CPU, JA_GP, JA_GPZ, JA_VA, JA_OUT, JA_R10, JA_I32, JA_I64 };
        struct JitArg { JitArgKind k; uint64_t v; };
        auto emit_call = [&](void* fn, std::initializer_list<JitArg> as) {
            auto place = [&](int k, const JitArg& s) {
                switch (s.k) {
                case JA_CPU: a.mov(aq(k), x86::rbp);                              break;  // cpu
                case JA_GP:  mov_from_reg(aq(k), (int)s.v);                      break;  // r[v]
                case JA_GPZ: if (s.v == 31) a.xor_(ad(k), ad(k));                         // r[v], or 0 if R31
                           else           mov_from_reg(aq(k), (int)s.v);       break;
                case JA_VA:  if (aq(k).id() != x86::rdx.id()) a.mov(aq(k), x86::rdx); break;  // va: precomputed in RDX
                case JA_OUT: a.lea(aq(k), x86::qword_ptr(x86::rsp, 32));          break;  // &out slot
                case JA_R10: a.mov(aq(k), x86::r10);                             break;  // value already in R10
                case JA_I32: a.mov(ad(k), imm((uint32_t)s.v));                  break;
                case JA_I64: a.mov(aq(k), imm((uint64_t)s.v));                  break;
                }
                };
            int k = 0; for (const JitArg& s : as) { if (s.k != JA_I32 && s.k != JA_I64) place(k, s); ++k; }
            k = 0;     for (const JitArg& s : as) { if (s.k == JA_I32 || s.k == JA_I64) place(k, s); ++k; }
            a.mov(x86::rax, imm((uint64_t)fn));
            a.call(x86::rax);
            };

        // Memory-format loads: va = regs[Rb] + disp16.
        if (op == OP_LDQ || op == OP_LDL || op == OP_LDBU || op == OP_LDWU || op == OP_LDQ_U) {
            if (ra == 31) continue;            // LDx R31 is a NOP (interpreter skips the read)
            const int disp = (int)(int16_t)(ins & 0xFFFF);
            const int size_bits = (op == OP_LDQ || op == OP_LDQ_U) ? 64 : (op == OP_LDL) ? 32 : (op == OP_LDWU) ? 16 : 8;
            const int amask = (size_bits / 8) - 1;
            if (rb == 31)  a.mov(x86::rdx, imm(disp));         // va -> RDX
            else { mov_from_reg(x86::rdx, rb); if (disp) a.add(x86::rdx, imm(disp)); }
            if (op == OP_LDQ_U) a.and_(x86::rdx, imm(~(uint64_t)7));   // LDQ_U: force 8-byte alignment

#ifdef JIT_VERIFY
            // Slow path: jit_read(cpu, va, size, &out); on fault bail to `done` returning i
            // (0..i-1 committed). In JIT_VERIFY builds this is the ONLY path, so the helper's
            // replay keeps the differential check race-free.
            {
                emit_call(read_helper, { {JA_CPU, 0}, {JA_VA, 0}, {JA_I32, (uint64_t)size_bits}, {JA_OUT, 0} });
                Label ok = a.new_label();
                a.test(x86::eax, x86::eax);
                a.jz(ok);
                set_pc(b->tag + 4 * (uint64_t)i);               // resume at the faulting load
                a.mov(x86::eax, imm(i));                          // this iteration: i instrs done
                a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));                       // + earlier chained iterations
                a.jmp(done);
                a.bind(ok);
                if (op == OP_LDQ || op == OP_LDQ_U) a.mov(x86::rax, x86::qword_ptr(x86::rsp, 32));  // LDQ/LDQ_U: full quad
                else if (op == OP_LDL)  a.movsxd(x86::rax, x86::dword_ptr(x86::rsp, 32));
                else if (op == OP_LDWU) a.movzx(x86::eax, x86::word_ptr(x86::rsp, 32));   // BWX: zero-extend
                else                    a.movzx(x86::eax, x86::byte_ptr(x86::rsp, 32));   // LDBU
                mov_to_reg(ra, x86::rax);
            }
#else
            // Inline fast path: aligned + data_page_cache[0][dpc_index(va)] hit + DRAM. Falls to the
            // helper on misalign / cache miss / MMIO. Mirrors jit_read's data-cache path. RDX = va
            // (preserved); R11 = slot byte offset = dpc_index(va)*stride; RAX/R10 scratch.
            Label slow = a.new_label();
            Label ldone = a.new_label();
            a.test(x86::dl, imm(amask));                                      a.jnz(slow);
            a.mov(x86::r11, x86::rdx);
            a.shr(x86::r11, imm(13));
            a.and_(x86::r11, imm(m_off.dpc_mask));                            // r11 = dpc_index(va)
            a.imul(x86::r11, x86::r11, imm(m_off.dpc_stride));                // r11 = slot byte offset
            a.mov(x86::r10, x86::rdx);
            a.and_(x86::r10, imm(-0x2000));                                   // r10 = va & ~0x1FFF
            a.cmp(x86::qword_ptr(x86::rbp, x86::r11, 0, m_off.dpc_virt_page), x86::r10);  a.jne(slow);
            a.mov(x86::rax, x86::qword_ptr(x86::rbp, m_off.state_cm));                    // rax = {cm, asn0} (adjacent in state)
            a.cmp(x86::qword_ptr(x86::rbp, x86::r11, 0, m_off.dpc_cm), x86::rax);         a.jne(slow);   // vs slot {cm, asn}
            a.mov(x86::r10, x86::qword_ptr(x86::rbp, x86::r11, 0, m_off.dpc_host_base));  // r10 = host page base (0 = MMIO/none)
            a.test(x86::r10, x86::r10);                                      a.jz(slow);  // MMIO/straddle: device read via helper
            a.mov(x86::rax, x86::rdx);
            a.and_(x86::rax, imm(0x1FFF));                                   // rax = page offset
            if (op == OP_LDQ || op == OP_LDQ_U) a.mov(x86::rax, x86::qword_ptr(x86::r10, x86::rax));  // LDQ/LDQ_U: full quad
            else if (op == OP_LDL)  a.movsxd(x86::rax, x86::dword_ptr(x86::r10, x86::rax));
            else if (op == OP_LDWU) a.movzx(x86::eax, x86::word_ptr(x86::r10, x86::rax));   // BWX: zero-extend
            else                    a.movzx(x86::eax, x86::byte_ptr(x86::r10, x86::rax));   // LDBU
            mov_to_reg(ra, x86::rax);
            a.bind(ldone);   // fast path falls through; the slow path is a cold-tail stub jumping back here
            {
                ColdMemStub s{};
                s.kind = ColdMemStub::LOAD;  s.slow = slow;  s.join = ldone;  s.done = done;
                s.helper = read_helper;      s.size_bits = size_bits;
                s.ra = ra;  s.pin = pin_id(ra);
                s.slot = (pal_block && ((ra & 0xc) == 0x4)) ? ra + 32 : ra;   // PALshadow remap (see reg())
                s.i = i;    s.fault_pc = b->tag + 4 * (uint64_t) i;
                cold.push_back(s);
            }
#endif
            continue;
        }

        // Memory-format stores: MEM[regs[Rb] + disp16] = regs[Ra]. Inline fast path mirrors the load
        // path against the WRITE cache [1] (a hit already passed the write-permission + FOW check, so
        // the page is writable); falls to jit_write on misalign / cache miss / MMIO. In verify the
        // helper is the only path -- it compares the store against the interpreter's recorded one.
        if (op == OP_STL || op == OP_STQ || op == OP_STB || op == OP_STW || op == OP_STQ_U) {
            const int disp = (int)(int16_t)(ins & 0xFFFF);
            const int size_bits = (op == OP_STQ || op == OP_STQ_U) ? 64 : (op == OP_STL) ? 32 : (op == OP_STW) ? 16 : 8;
            const int amask = (size_bits / 8) - 1;
            if (rb == 31)  a.mov(x86::rdx, imm(disp));                       // va -> RDX (preserved for helper)
            else { mov_from_reg(x86::rdx, rb); if (disp) a.add(x86::rdx, imm(disp)); }
            if (op == OP_STQ_U) a.and_(x86::rdx, imm(~(uint64_t)7));        // STQ_U: force 8-byte alignment

#ifdef JIT_VERIFY
            {
                emit_call(write_helper, { {JA_CPU, 0}, {JA_VA, 0}, {JA_I32, (uint64_t)size_bits}, {JA_GPZ, (uint64_t)ra} });  // jit_write(cpu, va, size, value)
                Label ok = a.new_label();
                a.test(x86::eax, x86::eax);
                a.jz(ok);
                set_pc(b->tag + 4 * (uint64_t)i);                            // resume at the faulting store
                a.mov(x86::eax, imm(i));                                       // this iteration: i instrs done
                a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));                                    // + earlier chained iterations
                a.jmp(done);
                a.bind(ok);
            }
#else
            // Inline fast path: aligned + data_page_cache[1][dpc_index(va)] hit + DRAM. RDX = va
            // (preserved for the helper); R11 = write-cache slot byte offset; RAX/R10/R9 scratch.
            Label slow = a.new_label();
            Label sdone = a.new_label();
            a.test(x86::dl, imm(amask));                                      a.jnz(slow);
            a.mov(x86::r11, x86::rdx);
            a.shr(x86::r11, imm(13));
            a.and_(x86::r11, imm(m_off.dpc_mask));                            // r11 = dpc_index(va)
            a.imul(x86::r11, x86::r11, imm(m_off.dpc_stride));                // r11 = slot byte offset
            a.add(x86::r11, imm(m_off.dpc_write_row));                        // -> write cache [1]
            a.mov(x86::r10, x86::rdx);
            a.and_(x86::r10, imm(-0x2000));                                   // r10 = va & ~0x1FFF
            a.cmp(x86::qword_ptr(x86::rbp, x86::r11, 0, m_off.dpc_virt_page), x86::r10);  a.jne(slow);
            a.mov(x86::rax, x86::qword_ptr(x86::rbp, m_off.state_cm));                    // rax = {cm, asn0} (adjacent in state)
            a.cmp(x86::qword_ptr(x86::rbp, x86::r11, 0, m_off.dpc_cm), x86::rax);         a.jne(slow);   // vs slot {cm, asn}
            a.mov(x86::r10, x86::qword_ptr(x86::rbp, x86::r11, 0, m_off.dpc_host_base));  // r10 = host page base (0 = MMIO/none)
            a.test(x86::r10, x86::r10);                                      a.jz(slow);  // MMIO/straddle: device write via helper
            a.mov(x86::rax, x86::rdx);
            a.and_(x86::rax, imm(0x1FFF));                                   // rax = page offset
            if (ra == 31)  a.xor_(x86::r9d, x86::r9d);                        // value -> R9 (R31 == 0)
            else           mov_from_reg(x86::r9, ra);
            if (op == OP_STQ || op == OP_STQ_U) a.mov(x86::qword_ptr(x86::r10, x86::rax), x86::r9);   // full quad
            else if (op == OP_STL)                   a.mov(x86::dword_ptr(x86::r10, x86::rax), x86::r9d);
            else if (op == OP_STW)                   a.mov(x86::word_ptr(x86::r10, x86::rax), x86::r9w);
            else                                     a.mov(x86::byte_ptr(x86::r10, x86::rax), x86::r9b);   // STB
            a.bind(sdone);   // fast path falls through; the slow path is a cold-tail stub jumping back here
            {
                ColdMemStub s{};
                s.kind = ColdMemStub::STORE;  s.slow = slow;  s.join = sdone;  s.done = done;
                s.helper = write_helper;      s.size_bits = size_bits;
                s.ra = ra;  s.pin = pin_id(ra);
                s.slot = (pal_block && ((ra & 0xc) == 0x4)) ? ra + 32 : ra;   // PALshadow remap (see reg())
                s.i = i;    s.fault_pc = b->tag + 4 * (uint64_t) i;
                cold.push_back(s);
            }
#endif
            continue;
        }

        // FP memory (LDS/LDT/STS/STT + VAX LDF/LDG/STF/STG): f[Fa] <-> MEM[Rb+disp16] with FPSTART.
        // LDT/STT are raw 8B -> inline fast path; the converting forms (S/F/G) go through the helper. Verify uses the
        // helper only (FP loads replay the logged f-value, FP stores compare via jit_write's store-log).
        if (op == OP_LDT || op == OP_LDS || op == OP_STT || op == OP_STS ||
            op == OP_LDF || op == OP_LDG || op == OP_STF || op == OP_STG) {
            const bool isload = (op == OP_LDT || op == OP_LDS || op == OP_LDF || op == OP_LDG);
            const bool israw = (op == OP_LDT || op == OP_STT);   // T-format: no conversion -> inline-able
            const int  fa = ra;                               // Fa = (ins>>21)&0x1f (FP regs: no shadow remap)
            // fmt: 0=T raw, 1=S ieee, 2=F vax, 3=G vax. S/F are 32-bit in memory; T/G are 64-bit.
            const uint32_t fmt = (op == OP_LDS || op == OP_STS) ? 1u : (op == OP_LDF || op == OP_STF) ? 2u
                : (op == OP_LDG || op == OP_STG) ? 3u : 0u;
            const int  size_bits = (fmt == 1 || fmt == 2) ? 32 : 64;
            const int  disp = (int)(int16_t)(ins & 0xFFFF);
            const uint32_t descr = (fmt << 16) | (uint32_t)size_bits;   // fmt<<16 | size

            if (isload && fa == 31) continue;   // LDT/LDS f31: interp skips the read (NOP)

            if (rb == 31)  a.mov(x86::rdx, imm(disp));                       // va -> RDX (preserved for helper)
            else { mov_from_reg(x86::rdx, rb); if (disp) a.add(x86::rdx, imm(disp)); }

            auto emit_helper = [&]() {
                emit_call(isload ? fp_read_helper : fp_write_helper,
                    { {JA_CPU, 0}, {JA_VA, 0}, {JA_I32, (uint64_t)fa}, {JA_I32, (uint64_t)descr} });  // jit_fp_read/write -> 0/1
                Label ok = a.new_label();
                a.test(x86::eax, x86::eax);
                a.jz(ok);
                set_pc(b->tag + 4 * (uint64_t)i);                            // resume at the faulting FP mem op
                a.mov(x86::eax, imm(i));
                a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));
                a.jmp(done);
                a.bind(ok);
                };

#ifdef JIT_VERIFY
            emit_helper();
#else
            if (!israw) { emit_helper(); continue; }   // LDS/STS: ieee conversion only via the helper

            // Inline fast path for LDT/STT: FPSTART (fpen gate + exc_sum=0), then the data-cache hit.
            // misalign / cache miss / MMIO / fpen==0 fall to the helper. R11 = slot byte offset; RAX/R10/R9 scratch.
            Label slow = a.new_label();
            Label fdone = a.new_label();
            a.cmp(x86::byte_ptr(x86::rbp, m_off.fpen), imm(0));                          a.je(slow);   // fpen==0 -> FEN trap
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_sum), imm(0));                                    // exc_sum = 0
            a.test(x86::dl, imm(7));                                                     a.jnz(slow);  // 8-byte aligned
            a.mov(x86::r11, x86::rdx);
            a.shr(x86::r11, imm(13));
            a.and_(x86::r11, imm(m_off.dpc_mask));
            a.imul(x86::r11, x86::r11, imm(m_off.dpc_stride));
            if (!isload) a.add(x86::r11, imm(m_off.dpc_write_row));                                    // store -> write cache [1]
            a.mov(x86::r10, x86::rdx);
            a.and_(x86::r10, imm(-0x2000));
            a.cmp(x86::qword_ptr(x86::rbp, x86::r11, 0, m_off.dpc_virt_page), x86::r10); a.jne(slow);
            a.mov(x86::rax, x86::qword_ptr(x86::rbp, m_off.state_cm));                   // rax = {cm, asn0} (adjacent in state)
            a.cmp(x86::qword_ptr(x86::rbp, x86::r11, 0, m_off.dpc_cm), x86::rax);        a.jne(slow);   // vs slot {cm, asn}
            a.mov(x86::r10, x86::qword_ptr(x86::rbp, x86::r11, 0, m_off.dpc_host_base));  // r10 = host page base (0 = MMIO/none)
            a.test(x86::r10, x86::r10);                                                  a.jz(slow);   // MMIO/straddle -> helper
            a.mov(x86::rax, x86::rdx);
            a.and_(x86::rax, imm(0x1FFF));                                               // rax = page offset
            if (isload) {                                                               // LDT: f[fa] = MEM[phys]
                a.mov(x86::rax, x86::qword_ptr(x86::r10, x86::rax));
                a.mov(x86::qword_ptr(x86::rbp, m_off.f_base + fa * 8), x86::rax);
            }
            else {                                                                    // STT: MEM[phys] = f[fa]
                a.mov(x86::r9, x86::qword_ptr(x86::rbp, m_off.f_base + fa * 8));
                a.mov(x86::qword_ptr(x86::r10, x86::rax), x86::r9);
            }
            a.bind(fdone);   // fast path falls through; the slow path is a cold-tail stub jumping back here
            {
                ColdMemStub s{};
                s.kind = ColdMemStub::FPMEM;  s.slow = slow;  s.join = fdone;  s.done = done;
                s.helper = isload ? fp_read_helper : fp_write_helper;
                s.descr = descr;  s.ra = fa;  s.pin = -1;  s.slot = 0;   // helper owns the f[] access
                s.i = i;          s.fault_pc = b->tag + 4 * (uint64_t) i;
                cold.push_back(s);
            }
#endif
            continue;
        }

        // Store-conditional STL_C/STQ_C (0x2e/0x2f): conditionally store Ra; Ra = 1 (success) / 0 (fail).
        // jit_stc consumes the LL monitor + does the host CAS (production), or compares against the
        // interpreter's logged outcome (verify). 
        if (op == OP_STL_C || op == OP_STQ_C) {
            const int disp = (int)(int16_t)(ins & 0xFFFF);
            const int size_bits = (op == OP_STQ_C) ? 64 : 32;
            if (rb == 31)  a.mov(x86::rdx, imm(disp));                       // va -> RDX
            else { mov_from_reg(x86::rdx, rb); if (disp) a.add(x86::rdx, imm(disp)); }
            emit_call(stc_helper, { {JA_CPU, 0}, {JA_VA, 0}, {JA_I32, (uint64_t)size_bits}, {JA_GP, (uint64_t)ra} });  // jit_stc(cpu, va, size, value)
            Label nobail = a.new_label();
            a.test(x86::eax, imm(0x100));                                   // 0x100 = translation-fault bail
            a.jz(nobail);
            set_pc(b->tag + 4 * (uint64_t)i);                              // resume at the faulting STx_C
            a.mov(x86::eax, imm(i));
            a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));
            a.jmp(done);
            a.bind(nobail);
            mov_to_reg(ra, x86::rax);                                      // Ra = success(1) / fail(0)
            continue;
        }

        // HW_LD physical (PALmode func 0/1): Ra = phys[Rb + disp12], no translation. jit_read_phys
        // does the aligned DRAM read (or replays in verify, bails on MMIO so the interpreter does the
        // ordered device read). disp is 12-bit here, not the 16-bit memory-format displacement.
        if (op == OP_HW_LDL || op == OP_HW_LDQ || op == OP_HW_LDQ_VPTE || op == OP_HW_LDL_WCHK) {
            if (ra == 31) continue;                                  // R31 dest discards the read
            const int disp = (int)((int32_t)(ins << 20) >> 20);    // sign-extend 12-bit displacement
            const int size_bits = (op == OP_HW_LDL || op == OP_HW_LDL_WCHK) ? 32 : 64;
            if (rb == 31)  a.mov(x86::rdx, imm(disp));               // address (phys, or virtual for VPTE) -> RDX
            else { mov_from_reg(x86::rdx, rb); if (disp) a.add(x86::rdx, imm(disp)); }
            // func 5 -> jit_read_vpte (kernel-checked virtual read); else jit_read_phys
            emit_call(op == OP_HW_LDQ_VPTE ? read_vpte_helper : op == OP_HW_LDL_WCHK ? read_wchk_helper : hw_ld_helper,
                { {JA_CPU, 0}, {JA_VA, 0}, {JA_I32, (uint64_t)size_bits}, {JA_OUT, 0} });
            Label ok = a.new_label();
            a.test(x86::eax, x86::eax);
            a.jz(ok);
            set_pc(b->tag + 4 * (uint64_t)i);                       // resume at the faulting HW_LD
            a.mov(x86::eax, imm(i));                                  // this iteration: i instrs done
            a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));                               // + earlier chained iterations
            a.jmp(done);
            a.bind(ok);
            // HW_LDL SIGN-extends the longword to canonical form (QEMU gen_hw_ld uses MO_LESL; the EV68CB
            // HRM is silent but the Alpha longword-canonical rule applies, same as LDL). NOTE: the interp's
            // DO_HW_LDL zero-extends -- that is the bug, fixed in cpu_pal.h to match this.
            if (op == OP_HW_LDL || op == OP_HW_LDL_WCHK) a.movsxd(x86::rax, x86::dword_ptr(x86::rsp, 32));
            else                 a.mov(x86::rax, x86::qword_ptr(x86::rsp, 32));   // HW_LDQ / VPTE: full quad
            mov_to_reg(ra, x86::rax);
            continue;
        }

        // Load-locked LDL_L/LDQ_L (0x2a/0x2b): Ra = MEM[Rb + disp16] AND establish the LL/SC exclusive
        // monitor. 
        if (op == OP_LDL_L || op == OP_LDQ_L) {
            const int disp = (int)(int16_t)(ins & 0xFFFF);
            const int size_bits = (op == OP_LDQ_L) ? 64 : 32;
            if (rb == 31)  a.mov(x86::rdx, imm(disp));               // va -> RDX
            else { mov_from_reg(x86::rdx, rb); if (disp) a.add(x86::rdx, imm(disp)); }
            emit_call(read_locked_helper, { {JA_CPU, 0}, {JA_VA, 0}, {JA_I32, (uint64_t)size_bits}, {JA_OUT, 0} });  // jit_read_locked(cpu, va, size, &out)
            Label ok = a.new_label();
            a.test(x86::eax, x86::eax);
            a.jz(ok);
            set_pc(b->tag + 4 * (uint64_t)i);                       // resume at the faulting LDx_L
            a.mov(x86::eax, imm(i));
            a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));
            a.jmp(done);
            a.bind(ok);
            a.mov(x86::rax, x86::qword_ptr(x86::rsp, 32));           // *out already sign-extended by the helper
            mov_to_reg(ra, x86::rax);
            continue;
        }

        // HW_MTPR (PALmode, side-effect-free IPRs): jit_hw_mtpr(cpu, function, value=Rb) stores an IPR
        // field directly. The verify pass snapshots+compares those live-state writes. No fault/bail.
        if (op == OP_HW_MTPR) {
            const uint32_t function = (ins >> 8) & 0xff;
            emit_call(hw_mtpr_helper, { {JA_CPU, 0}, {JA_I32, (uint64_t)function}, {JA_GPZ, (uint64_t)rb} });  // jit_hw_mtpr(cpu, function, value)
            continue;
        }

        // HW_MTPR I_CTL (0x11), terminator: I_CTL writes SDE (the PALshadow R4-7/R20-23 remap), SPE and
        // VA mode -- assumptions the reg() remap and the MMU bake in -- so compiled code PAST it would be
        // wrong. Run the IPR write via the same helper, then end the block: set the next PC and re-dispatch,
        // so post-I_CTL code recompiles under the new SDE. 
        if (op == OP_HW_MTPR_TERM) {
            const uint32_t function = (ins >> 8) & 0xff;            // 0x11 (I_CTL)
            emit_call(hw_mtpr_helper, { {JA_CPU, 0}, {JA_I32, (uint64_t)function}, {JA_GPZ, (uint64_t)rb} });
            set_pc(b->tag + 4 * (uint64_t)(i + 1));                // next PC -> R10 + state.pc (PALmode bit preserved)
            continue;
        }

        // HW_ST physical (PALmode func 0/1): phys[Rb + disp12] = Ra, no translation. jit_write_phys
        // does the aligned DRAM write (or compares the logged store in verify, bails on MMIO). disp is
        // 12-bit here, not the 16-bit memory-format displacement.
        if (op == OP_HW_STL || op == OP_HW_STQ) {
            const int disp = (int)((int32_t)(ins << 20) >> 20);    // sign-extend 12-bit displacement
            const int size_bits = (op == OP_HW_STQ) ? 64 : 32;
            if (rb == 31)  a.mov(x86::rdx, imm(disp));               // phys addr -> RDX
            else { mov_from_reg(x86::rdx, rb); if (disp) a.add(x86::rdx, imm(disp)); }
            emit_call(hw_st_helper, { {JA_CPU, 0}, {JA_VA, 0}, {JA_I32, (uint64_t)size_bits}, {JA_GPZ, (uint64_t)ra} });  // jit_write_phys(cpu, phys, size, value)
            Label ok = a.new_label();
            a.test(x86::eax, x86::eax);
            a.jz(ok);
            set_pc(b->tag + 4 * (uint64_t)i);                       // resume at the faulting HW_ST
            a.mov(x86::eax, imm(i));
            a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));
            a.jmp(done);
            a.bind(ok);
            continue;
        }

        // Load-address: Ra = Rb + disp16 (LDA) or Rb + (disp16 << 16) (LDAH). Pure register
        // arithmetic. R31 dest discards the result (a NOP).
        if (op == OP_LDA || op == OP_LDAH) {
            if (ra == 31) continue;
            int64_t d = (int64_t)(int16_t)(ins & 0xFFFF);
            if (op == OP_LDAH) d <<= 16;
            if (rb == 31)  a.mov(x86::rax, imm(d));
            else { mov_from_reg(x86::rax, rb); if (d) a.add(x86::rax, imm(d)); }
            mov_to_reg(ra, x86::rax);
            continue;
        }

        // HW_MFPR (0x19, PALmode): read the IPR named by (ins>>8)&0xff into Ra. The helper is an
        // independent reimplementation of DO_HW_MFPR that RETURNS the value (it reads state only, never
        // writes it). pass the current Ra as `cur` so an unknown IPR returns it unchanged (matching interp),
        // and write reg(ra) here so the value lands in whichever regs[] array we hold. Every MFPR IPR is
        // a pure read
        if (op == OP_HW_MFPR) {
            if (ra != 31) {                                  // MFPR R31 discards the value (R31 is hardwired 0)
                emit_call(hw_mfpr_helper, { {JA_CPU, 0}, {JA_I32, (uint64_t)ins}, {JA_GP, (uint64_t)ra} });  // -> RAX = IPR value
                mov_to_reg(ra, x86::rax);                      // Ra = value (reg() applies the PALshadow remap)
            }
            continue;
        }

        // MISC state reads RPCC/RC/RS (0x18): Ra = jit_misc(cpu, sel). The helper reads the cycle
        // counter / interrupt flag (clearing or setting it) in production, and replays the interp
        // pass's value in verify. Dest is Ra (not Rc); classify gated Ra!=31, so a store always happens.
        if (op == OP_RPCC || op == OP_RC || op == OP_RS) {
            const int sel = (op == OP_RPCC) ? 0 : (op == OP_RC) ? 1 : 2;
            emit_call(misc_helper, { {JA_CPU, 0}, {JA_I32, (uint64_t)sel} });  // -> RAX = value (replayed in verify); RC/RS also side-effect the flag
            if (ra != 31) mov_to_reg(ra, x86::rax);          // Ra = value (reg() remap); Ra==31 RC/RS = flag side-effect only, discard
            continue;
        }

        // ITOFx (0x14): f[Fc] = fmt(Ra). jit_itof mirrors FPSTART (fpen -> FEN trap = bail, exc_sum=0);
        // the verify compares the FP file via its snapshot.
        if (op == OP_ITOFS || op == OP_ITOFF || op == OP_ITOFT) {
            emit_call(itof_helper, { {JA_CPU, 0}, {JA_I32, (uint64_t)rc}, {JA_GPZ, (uint64_t)ra},
                      {JA_I32, (uint64_t)(op == OP_ITOFS ? 1 : (op == OP_ITOFF) ? 2 : 0)} });  // jit_itof(cpu, fc, value, fmt)
            Label ok = a.new_label();
            a.test(x86::eax, x86::eax);
            a.jz(ok);
            set_pc(b->tag + 4 * (uint64_t)i);               // FEN trap: resume here in the interpreter
            a.mov(x86::eax, imm(i));
            a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));
            a.jmp(done);
            a.bind(ok);
            continue;
        }

        // FTOIx (0x1c): Rc = fmt(f[Fa]). Same FPSTART bail shape; dest is a GPR (verify-compared).
        if (op == OP_FTOIS || op == OP_FTOIT) {
            emit_call(ftoi_helper, { {JA_CPU, 0}, {JA_I32, (uint64_t)ra},
                      {JA_I32, (uint64_t)(op == OP_FTOIS ? 1 : 0)}, {JA_OUT, 0} });  // jit_ftoi(cpu, fa, fmt, &out)
            Label ok = a.new_label();
            a.test(x86::eax, x86::eax);
            a.jz(ok);
            set_pc(b->tag + 4 * (uint64_t)i);               // FEN trap: resume here in the interpreter
            a.mov(x86::eax, imm(i));
            a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));
            a.jmp(done);
            a.bind(ok);
            a.mov(x86::rax, x86::qword_ptr(x86::rsp, 32));
            mov_to_reg(rc, x86::rax);                        // Rc (reg() applies the PALshadow remap)
            continue;
        }

        // CVTQT/CVTQS (0x16): f[Fc] = (double|single)(s64) f[Fb] via SSE. Inline the steady-state
        // common case; bail to the interpreter (it owns rounding/traps/FPCR) on any edge that would
        // trap or need non-nearest rounding. The verify carries fpcr/exc_sum across its two passes, so
        // the inline path leaves them untouched -- correctness rests on the bails, not on FPCR upkeep.
        if (op == OP_CVTQT || op == OP_CVTQS) {
            const bool dyn = (((ins >> 11) & 3) == 3);      // /D: rounding is FPCR<59:58>, checked at runtime
            Label bail = a.new_label(), fdone = a.new_label(), cont = a.new_label();
            a.cmp(x86::byte_ptr(x86::rbp, m_off.fpen), imm(0));   // FPSTART: FP disabled -> FEN trap (interp)
            a.je(bail);
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_sum), imm(0));
            if (dyn) {                                      // dynamic rounding: only nearest is SSE's default
                a.mov(x86::rax, x86::qword_ptr(x86::rbp, m_off.fpcr));
                a.shr(x86::rax, imm(58)); a.and_(x86::eax, imm(3));
                a.cmp(x86::eax, imm(2)); a.jne(bail);         // FPCR<59:58> != round-to-nearest -> bail
            }
            if (rb == 31) a.xor_(x86::eax, x86::eax);        // val = (s64) f[Fb]  (Fb==31 -> 0)
            else          a.mov(x86::rax, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * rb));
            if (op == OP_CVTQT) { a.cvtsi2sd(x86::xmm0, x86::rax); a.cvttsd2si(x86::rcx, x86::xmm0); }
            else { a.cvtsi2ss(x86::xmm0, x86::rax); a.cvttss2si(x86::rcx, x86::xmm0); }
            a.cmp(x86::rcx, x86::rax);                       // round-trip equal -> exact (no inexact, no trap)
            a.je(fdone);
            a.bt(x86::qword_ptr(x86::rbp, m_off.fpcr), imm(56));   // inexact: FPCR.INE clear -> first one traps
            a.jnc(bail);
            a.bind(fdone);
            if (op == OP_CVTQS) a.cvtss2sd(x86::xmm0, x86::xmm0);  // widen single -> register (T-format) bits
            a.movq(x86::qword_ptr(x86::rbp, m_off.f_base + 8u * rc), x86::xmm0);
            a.jmp(cont);
            a.bind(bail);
            set_pc(b->tag + 4 * (uint64_t)i);              // resume this instruction in the interpreter
            a.mov(x86::eax, imm(i)); a.add(x86::eax, x86::dword_ptr(x86::rsp, 40)); a.jmp(done);
            a.bind(cont);
            continue;
        }

        // IEEE T-float arithmetic (0x16 ADDT/SUBT/MULT/DIVT): inline SSE on the double f-registers. 
        // FPCR.INE already sticky-set (so an inexact result won't trap). Every edge bails to the interpreter:
        // FP-off, /D-not-nearest, denormal operand, INE-clear (a first inexact would trap), or an Inf/NaN/
        // denormal result (overflow/invalid/div-zero/underflow). 
        if (op == OP_ADDT || op == OP_SUBT || op == OP_MULT || op == OP_DIVT) {
            const bool dyn = (((ins >> 11) & 3) == 3);      // /D: rounding is FPCR<59:58>, checked at runtime
            Label bail = a.new_label(), cont = a.new_label();
            auto class_bail = [&](const x86::Vec& x, bool chk_inf_nan) {   // bail if denormal (always) / Inf|NaN (if asked)
                Label ok = a.new_label();
                a.movq(x86::rax, x);
                a.mov(x86::rcx, x86::rax);
                a.shr(x86::rcx, imm(52)); a.and_(x86::ecx, imm(0x7ff));      // biased exponent
                if (chk_inf_nan) { a.cmp(x86::ecx, imm(0x7ff)); a.je(bail); }
                a.test(x86::ecx, x86::ecx); a.jnz(ok);                       // exp != 0 -> normal (or Inf/NaN)
                a.shl(x86::rax, imm(12)); a.jnz(bail);                       // exp == 0, mantissa != 0 -> denormal
                a.bind(ok);
                };
            a.cmp(x86::byte_ptr(x86::rbp, m_off.fpen), imm(0)); a.je(bail);          // FPSTART: FP disabled -> FEN trap
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_sum), imm(0));
            a.bt(x86::qword_ptr(x86::rbp, m_off.fpcr), imm(56)); a.jnc(bail);        // INE clear -> first inexact traps
            if (dyn) {                                                              // dynamic rounding: SSE gives nearest
                a.mov(x86::rax, x86::qword_ptr(x86::rbp, m_off.fpcr));
                a.shr(x86::rax, imm(58)); a.and_(x86::eax, imm(3));
                a.cmp(x86::eax, imm(2)); a.jne(bail);
            }
            a.movq(x86::xmm0, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)ra));   // f[Fa]
            a.movq(x86::xmm1, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rb));   // f[Fb]
            class_bail(x86::xmm0, false);                                           // denormal operand -> interp (DNZ/trap)
            class_bail(x86::xmm1, false);
            switch (op) {
            case OP_ADDT: a.addsd(x86::xmm0, x86::xmm1); break;
            case OP_SUBT: a.subsd(x86::xmm0, x86::xmm1); break;
            case OP_MULT: a.mulsd(x86::xmm0, x86::xmm1); break;
            default:      a.divsd(x86::xmm0, x86::xmm1); break;                   // OP_DIVT
            }
            class_bail(x86::xmm0, true);                                           // Inf/NaN/denormal result -> interp
            a.movq(x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rc), x86::xmm0);   // f[Fc]
            a.jmp(cont);
            a.bind(bail);
            set_pc(b->tag + 4 * (uint64_t)i);
            a.mov(x86::eax, imm(i)); a.add(x86::eax, x86::dword_ptr(x86::rsp, 40)); a.jmp(done);
            a.bind(cont);
            continue;
        }

        // IEEE T-float compares (0x16 CMPTUN/EQ/LT/LE): f[Fc] = (Fa cmp Fb) ? 2.0 : 0.0. No rounding or
        // inexact. A NaN operand (INV / quiet-NaN / SWC rules) or a denormal operand (unmaskable denormal
        // trap -- the interp's ieee_fcmp traps via ieee_unpack) goes to the interp; +/-Inf, zero and
        // normals compare inline via ucomisd. Two ordered operands make CMPTUN always false.
        if (op == OP_CMPTUN || op == OP_CMPTEQ || op == OP_CMPTLT || op == OP_CMPTLE) {
            Label bail = a.new_label(), cont = a.new_label();
            auto cmp_bail = [&](const x86::Vec& x) {                          // bail on NaN or denormal; Inf/zero/normal -> ok
                Label ok = a.new_label(), special = a.new_label();
                a.movq(x86::rax, x);
                a.mov(x86::rcx, x86::rax);
                a.shr(x86::rcx, imm(52)); a.and_(x86::ecx, imm(0x7ff));         // biased exponent
                a.cmp(x86::ecx, imm(0x7ff)); a.je(special);                     // exp all-ones -> Inf or NaN
                a.test(x86::ecx, x86::ecx); a.jnz(ok);                          // exp != 0 -> normal
                a.bind(special);                                                // exp == 0 (zero/denormal) or all-ones (Inf/NaN)
                a.shl(x86::rax, imm(12)); a.jnz(bail);                          // mantissa != 0 -> denormal or NaN -> bail
                a.bind(ok);                                                     // mantissa == 0 -> zero or Inf -> compare inline
                };
            a.cmp(x86::byte_ptr(x86::rbp, m_off.fpen), imm(0)); a.je(bail);   // FPSTART
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_sum), imm(0));
            a.movq(x86::xmm0, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)ra));   // f[Fa]
            a.movq(x86::xmm1, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rb));   // f[Fb]
            cmp_bail(x86::xmm0); cmp_bail(x86::xmm1);
            if (op == OP_CMPTUN) {
                a.xor_(x86::eax, x86::eax);                                     // both ordered -> unordered is false
            }
            else {
                a.ucomisd(x86::xmm0, x86::xmm1);
                if (op == OP_CMPTEQ) a.sete(x86::al);                      // ZF    -> Fa == Fb
                else if (op == OP_CMPTLT) a.setb(x86::al);                      // CF    -> Fa <  Fb
                else                      a.setbe(x86::al);                     // CF|ZF -> Fa <= Fb  (CMPTLE)
                a.movzx(x86::eax, x86::al);
                a.shl(x86::rax, imm(62));                                       // true -> 0x4000000000000000 (2.0)
            }
            a.mov(x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rc), x86::rax);     // f[Fc]
            a.jmp(cont);
            a.bind(bail);
            set_pc(b->tag + 4 * (uint64_t)i);
            a.mov(x86::eax, imm(i)); a.add(x86::eax, x86::dword_ptr(x86::rsp, 40)); a.jmp(done);
            a.bind(cont);
            continue;
        }

        // IEEE convert / sqrt / S-float class-check bails (used by the blocks below). <bl> is each
        // block's bail label; bail if denormal (always) or Inf|NaN (when chk). dbl = double, sgl = single.
        auto dbl_bail = [&](const x86::Vec& x, bool chk, const Label& bl) {
            Label ok = a.new_label();
            a.movq(x86::rax, x); a.mov(x86::rcx, x86::rax);
            a.shr(x86::rcx, imm(52)); a.and_(x86::ecx, imm(0x7ff));
            if (chk) { a.cmp(x86::ecx, imm(0x7ff)); a.je(bl); }
            a.test(x86::ecx, x86::ecx); a.jnz(ok);
            a.shl(x86::rax, imm(12)); a.jnz(bl);
            a.bind(ok);
            };
        auto sgl_bail = [&](const x86::Vec& x, bool chk, const Label& bl) {
            Label ok = a.new_label();
            a.movd(x86::eax, x); a.mov(x86::ecx, x86::eax);
            a.shr(x86::ecx, imm(23)); a.and_(x86::ecx, imm(0xff));
            if (chk) { a.cmp(x86::ecx, imm(0xff)); a.je(bl); }
            a.test(x86::ecx, x86::ecx); a.jnz(ok);
            a.shl(x86::eax, imm(9)); a.jnz(bl);
            a.bind(ok);
            };
        auto fp_dyn_bail = [&](const Label& bl) {     // /D: SSE rounds to nearest, so bail unless FPCR does too
            a.mov(x86::rax, x86::qword_ptr(x86::rbp, m_off.fpcr));
            a.shr(x86::rax, imm(58)); a.and_(x86::eax, imm(3));
            a.cmp(x86::eax, imm(2)); a.jne(bl);
            };

        // CVTST (S->T widen): zero/normal/Inf are already valid T bit patterns -> copy. S-denormal
        // (renormalizes) and NaN (quiets / may INV) go to the interpreter. No rounding or inexact.
        if (op == OP_CVTST) {
            Label bail = a.new_label(), cont = a.new_label();
            a.cmp(x86::byte_ptr(x86::rbp, m_off.fpen), imm(0)); a.je(bail);          // FPSTART
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_sum), imm(0));
            a.movq(x86::xmm0, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rb));   // f[Fb]
            dbl_bail(x86::xmm0, true, bail);                                        // denormal/Inf/NaN -> interp
            a.movq(x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rc), x86::xmm0);   // S bits are valid T
            a.jmp(cont);
            a.bind(bail);
            set_pc(b->tag + 4 * (uint64_t)i);
            a.mov(x86::eax, imm(i)); a.add(x86::eax, x86::dword_ptr(x86::rsp, 40)); a.jmp(done);
            a.bind(cont);
            continue;
        }

        // CVTTS (T->S narrow): round the double to single, re-widen for the S register format. Denormal
        // operand, an Inf/NaN/denormal single result (overflow/invalid/underflow), or a first inexact
        // (INE clear) bail.
        if (op == OP_CVTTS) {
            const bool dyn = (((ins >> 11) & 3) == 3);
            Label bail = a.new_label(), cont = a.new_label();
            a.cmp(x86::byte_ptr(x86::rbp, m_off.fpen), imm(0)); a.je(bail);          // FPSTART
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_sum), imm(0));
            a.bt(x86::qword_ptr(x86::rbp, m_off.fpcr), imm(56)); a.jnc(bail);        // INE clear -> first inexact traps
            if (dyn) fp_dyn_bail(bail);
            a.movq(x86::xmm0, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rb));   // f[Fb] (double)
            dbl_bail(x86::xmm0, false, bail);                                       // denormal operand -> interp
            a.cvtsd2ss(x86::xmm0, x86::xmm0);                                       // round to single
            sgl_bail(x86::xmm0, true, bail);                                        // Inf/NaN/denormal single -> interp
            a.cvtss2sd(x86::xmm0, x86::xmm0);                                       // -> double (register format)
            a.movq(x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rc), x86::xmm0);
            a.jmp(cont);
            a.bind(bail);
            set_pc(b->tag + 4 * (uint64_t)i);
            a.mov(x86::eax, imm(i)); a.add(x86::eax, x86::dword_ptr(x86::rsp, 40)); a.jmp(done);
            a.bind(cont);
            continue;
        }

        // CVTTQ (T->Q): double -> signed 64-bit integer (raw bits into f[Fc]). /C chops (cvttsd2si), else
        // round-to-nearest (cvtsd2si). Overflow / Inf / NaN -> the integer indefinite -> interp (covers
        // IOV). A non-integer source (inexact) with INE clear traps -> interp; exact converts run inline.
        if (op == OP_CVTTQ) {
            const bool chop = (((ins >> 11) & 3) == 0);
            const bool dyn = (((ins >> 11) & 3) == 3);
            Label bail = a.new_label(), cont = a.new_label(), exact = a.new_label();
            a.cmp(x86::byte_ptr(x86::rbp, m_off.fpen), imm(0)); a.je(bail);          // FPSTART
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_sum), imm(0));
            if (dyn) fp_dyn_bail(bail);
            a.movq(x86::xmm0, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rb));   // f[Fb] (double)
            dbl_bail(x86::xmm0, false, bail);                                       // denormal operand -> interp
            if (chop) a.cvttsd2si(x86::rax, x86::xmm0); else a.cvtsd2si(x86::rax, x86::xmm0);
            a.mov(x86::ecx, imm(1)); a.shl(x86::rcx, imm(63));                      // rcx = INT64_MIN (indefinite)
            a.cmp(x86::rax, x86::rcx); a.je(bail);                                  // overflow / Inf / NaN -> interp
            a.cvtsi2sd(x86::xmm1, x86::rax);                                        // round-trip to test exactness
            a.ucomisd(x86::xmm1, x86::xmm0); a.je(exact);                           // round-trip == source -> exact
            a.bt(x86::qword_ptr(x86::rbp, m_off.fpcr), imm(56)); a.jnc(bail);       // inexact + INE clear -> trap
            a.bind(exact);
            a.mov(x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rc), x86::rax);     // store s64 bits
            a.jmp(cont);
            a.bind(bail);
            set_pc(b->tag + 4 * (uint64_t)i);
            a.mov(x86::eax, imm(i)); a.add(x86::eax, x86::dword_ptr(x86::rsp, 40)); a.jmp(done);
            a.bind(cont);
            continue;
        }

        // IEEE SQRT (T/S): sqrtsd / sqrtss. Denormal operand, an Inf/NaN/denormal result (a negative
        // operand yields NaN), or a first inexact (INE clear) bail to the interpreter.
        if (op == OP_SQRTT || op == OP_SQRTS) {
            const bool dyn = (((ins >> 11) & 3) == 3);
            Label bail = a.new_label(), cont = a.new_label();
            a.cmp(x86::byte_ptr(x86::rbp, m_off.fpen), imm(0)); a.je(bail);          // FPSTART
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_sum), imm(0));
            a.bt(x86::qword_ptr(x86::rbp, m_off.fpcr), imm(56)); a.jnc(bail);        // INE clear -> first inexact traps
            if (dyn) fp_dyn_bail(bail);
            a.movq(x86::xmm0, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rb));   // f[Fb]
            dbl_bail(x86::xmm0, false, bail);                                       // denormal operand -> interp
            if (op == OP_SQRTT) {
                a.sqrtsd(x86::xmm0, x86::xmm0);
                dbl_bail(x86::xmm0, true, bail);                                      // Inf/NaN(neg)/denormal result -> interp
            }
            else {
                a.cvtsd2ss(x86::xmm0, x86::xmm0);
                a.sqrtss(x86::xmm0, x86::xmm0);
                sgl_bail(x86::xmm0, true, bail);                                      // Inf/NaN(neg)/denormal result -> interp
                a.cvtss2sd(x86::xmm0, x86::xmm0);
            }
            a.movq(x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rc), x86::xmm0);
            a.jmp(cont);
            a.bind(bail);
            set_pc(b->tag + 4 * (uint64_t)i);
            a.mov(x86::eax, imm(i)); a.add(x86::eax, x86::dword_ptr(x86::rsp, 40)); a.jmp(done);
            a.bind(cont);
            continue;
        }

        // IEEE S-float arith (ADDS/SUBS/MULS/DIVS): compute in single precision (narrow the operands,
        // op, re-widen). Same bail policy as the T-float arith -- FP-off, /D-not-nearest, denormal
        // operand, INE clear, or an Inf/NaN/denormal single result.
        if (op == OP_ADDS || op == OP_SUBS || op == OP_MULS || op == OP_DIVS) {
            const bool dyn = (((ins >> 11) & 3) == 3);
            Label bail = a.new_label(), cont = a.new_label();
            a.cmp(x86::byte_ptr(x86::rbp, m_off.fpen), imm(0)); a.je(bail);          // FPSTART
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_sum), imm(0));
            a.bt(x86::qword_ptr(x86::rbp, m_off.fpcr), imm(56)); a.jnc(bail);        // INE clear -> first inexact traps
            if (dyn) fp_dyn_bail(bail);
            a.movq(x86::xmm0, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)ra));   // f[Fa]
            a.movq(x86::xmm1, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rb));   // f[Fb]
            dbl_bail(x86::xmm0, false, bail);                                       // denormal operands -> interp
            dbl_bail(x86::xmm1, false, bail);
            a.cvtsd2ss(x86::xmm0, x86::xmm0);                                       // operands -> single (exact)
            a.cvtsd2ss(x86::xmm1, x86::xmm1);
            switch (op) {
            case OP_ADDS: a.addss(x86::xmm0, x86::xmm1); break;
            case OP_SUBS: a.subss(x86::xmm0, x86::xmm1); break;
            case OP_MULS: a.mulss(x86::xmm0, x86::xmm1); break;
            default:      a.divss(x86::xmm0, x86::xmm1); break;                   // OP_DIVS
            }
            sgl_bail(x86::xmm0, true, bail);                                        // Inf/NaN/denormal single result -> interp
            a.cvtss2sd(x86::xmm0, x86::xmm0);                                       // -> double (register format)
            a.movq(x86::qword_ptr(x86::rbp, m_off.f_base + 8u * (uint32_t)rc), x86::xmm0);
            a.jmp(cont);
            a.bind(bail);
            set_pc(b->tag + 4 * (uint64_t)i);
            a.mov(x86::eax, imm(i)); a.add(x86::eax, x86::dword_ptr(x86::rsp, 40)); a.jmp(done);
            a.bind(cont);
            continue;
        }

        // FLTL non-arithmetic (0x17): all effects in state.f / fpcr via jit_fltl(cpu, ins).
        if (op == OP_FLTL) {
            emit_call(fltl_helper, { {JA_CPU, 0}, {JA_I32, (uint64_t)ins} });  // jit_fltl(cpu, ins)
            Label ok = a.new_label();
            a.test(x86::eax, x86::eax);
            a.jz(ok);
            set_pc(b->tag + 4 * (uint64_t)i);               // FEN trap: resume here in the interpreter
            a.mov(x86::eax, imm(i));
            a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));
            a.jmp(done);
            a.bind(ok);
            continue;
        }

        // FLTV VAX arith (0x15): jit_fltv runs the op into f[Fc]. Return 0 ok / 1 FPSTART bail (op not run
        // -> set_pc, interp re-runs) / 2 arith trap (op ran + GO_PAL already set state.pc -> return as-is).
        if (op == OP_FLTV) {
            emit_call(fltv_helper, { {JA_CPU, 0}, {JA_I32, (uint64_t)ins} });  // jit_fltv(cpu, ins)
            Label ok = a.new_label(), trapped = a.new_label();
            a.test(x86::eax, x86::eax);
            a.jz(ok);                                        // 0: no trap -> continue the block
            a.cmp(x86::eax, imm(2));
            a.je(trapped);                                   // 2: arith trap -- GO_PAL already set state.pc
            set_pc(b->tag + 4 * (uint64_t)i);               // 1: FEN trap (op not run) -> resume this instr
            a.mov(x86::eax, imm(i));
            a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));
            a.jmp(done);
            a.bind(trapped);                                 // op ran then diverted: count it, keep state.pc
            a.mov(x86::eax, imm(i + 1));
            a.add(x86::eax, x86::dword_ptr(x86::rsp, 40));
            a.jmp(done);
            a.bind(ok);
            continue;
        }

        // Computed jump JMP/JSR/RET (0x1a): Ra = PC+4 (return address); PC = Rb & ~3. A
        // terminator like a branch, but the target is a register -- left in R10 so the epilogue's
        // cached link validates it (succ->tag == target), chaining calls/returns/dispatch.
        if (op == OP_JMP) {
            const uint64_t ret = b->tag + 4 * (uint64_t)(i + 1);
            if (rb == 31) a.xor_(x86::r10d, x86::r10d);
            else          mov_from_reg(x86::r10, rb);
            a.and_(x86::r10, imm(~(uint64_t)3));                       // target = Rb & ~3 (clear low 2)
            if (b->tag & 3) a.or_(x86::r10, imm(b->tag & 3));           // DO_JMP: mode bits come from the current pc
            if (ra != 31) { a.mov(x86::rax, imm(ret & ~(uint64_t)3)); mov_to_reg(ra, x86::rax); }  // return addr = PC & ~3 (DO_JMP)
            a.mov(x86::qword_ptr(x86::rbp, m_off.state_pc), x86::r10);  // state.pc = target
            continue;
        }

        // HW_RET (HWREI, 0x1e): a PAL return -- a simple computed jump, target = Rb & ~2, no return-
        // address write. Like OP_JMP, leaves R10 = target for the epilogue's in-frame chain.
        if (op == OP_HW_RET) {
            if (rb == 31) a.xor_(x86::r10d, x86::r10d);
            else          mov_from_reg(x86::r10, rb);
            a.and_(x86::r10, imm(~(uint64_t)2));                       // target = Rb & ~2 (clear bit 1)
            a.mov(x86::qword_ptr(x86::rbp, m_off.state_pc), x86::r10);  // state.pc = target
            continue;
        }

        // CALL_PAL (0x00): vector to the PALcode entry, saving the return address in R23 and the
        // faulting PC in EXC_ADDR (per ENTER_NATIVE_CALL_PAL). 
        if (op == OP_CALL_PAL) {
            const uint32_t func = ins & 0x1FFFFFFF;
            const uint64_t cpc = b->tag + 4 * (uint64_t)i;                          // CALL_PAL address
            const uint64_t ret = (b->tag + 4 * (uint64_t)(i + 1)) & ~(uint64_t)2;  // return addr (PC & ~2)
            const uint64_t voff = (uint64_t)0x2000 | ((uint64_t)(func & 0x80) << 5)
                | ((uint64_t)(func & 0x3f) << 6) | (uint64_t)1;     // PAL entry offset
            Label do_vector = a.new_label();
            if (func < 0x40) {                          // privileged: OPCDEC trap if in user mode (cm != 0)
                a.cmp(x86::dword_ptr(x86::rbp, m_off.state_cm), imm(0));
                a.je(do_vector);
                emit_call(opcdec_helper, { {JA_CPU, 0}, {JA_I64, cpc} });  // jit_opcdec: sets state.pc/exc_addr, clears lock
                a.add(x86::qword_ptr(x86::rsp, 40), imm(i + 1));   // count the block; helper already wrote state.pc
                a.mov(x86::eax, x86::dword_ptr(x86::rsp, 40));
                a.jmp(done);                               // trap path exits (does not chain)
            }
            a.bind(do_vector);
            a.mov(x86::rax, imm(cpc));                                  // EXC_ADDR = CALL_PAL address
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_addr), x86::rax);
            a.movzx(x86::eax, x86::byte_ptr(x86::rbp, m_off.sde));      // SDE (0/1)
            a.shl(x86::eax, imm(5));                                    // * 32
            a.add(x86::eax, imm(23));                                   // R23 index: 23, or 55 if SDE
            a.mov(x86::rcx, imm(ret));
            a.mov(x86::qword_ptr(x86::rbx, x86::rax, 3), x86::rcx);     // r[idx] = return address
            a.mov(x86::r10, x86::qword_ptr(x86::rbp, m_off.pal_base));
            a.or_(x86::r10, imm(voff));                                 // r10 = pal_base | entry offset
            a.mov(x86::qword_ptr(x86::rbp, m_off.state_pc), x86::r10);  // state.pc = PAL entry
            continue;                                                  // -> terminator epilogue chains via r10
        }

        // Branch terminators: compute the target into state.pc, then end the block. The
        // branch is at index i, so the PC of the next instruction is b->tag + 4*(i+1).
        // FP conditional branches (0x31-0x37): FPSTART, then branch on f[Fa] vs 0.0 (sign-magnitude).
        // Map the bits to a monotonic signed s = sign ? -magnitude : magnitude so the integer signed
        // cmov conditions apply directly; -0 -> s=0 (matches the interp's zero handling), NaN by bits.
        if (is_fp_branch(op)) {
            const int64_t  bdisp = (int64_t)((uint64_t)(ins & 0x1FFFFF) << 43) >> 43;  // sext disp21
            const uint64_t fall = b->tag + 4 * (uint64_t)(i + 1);
            const uint64_t tgt = fall + (uint64_t)(bdisp * 4);
            Label bail = a.new_label(), cont = a.new_label();
            a.cmp(x86::byte_ptr(x86::rbp, m_off.fpen), imm(0));   // FPSTART: FP disabled -> FEN trap (interp)
            a.je(bail);
            a.mov(x86::qword_ptr(x86::rbp, m_off.exc_sum), imm(0));
            if (ra == 31) a.xor_(x86::eax, x86::eax);             // f[31] = 0 -> s = 0
            else {
                a.mov(x86::rax, x86::qword_ptr(x86::rbp, m_off.f_base + 8u * ra));
                a.mov(x86::rcx, x86::rax);
                a.sar(x86::rcx, imm(63));                           // rcx = sign mask (0 or -1)
                a.btr(x86::rax, imm(63));                           // rax = magnitude (sign bit cleared)
                a.xor_(x86::rax, x86::rcx); a.sub(x86::rax, x86::rcx);   // rax = s = sign ? -magnitude : magnitude
            }
            a.mov(x86::r10, imm(fall));
            a.mov(x86::r11, imm(tgt));
            a.test(x86::rax, x86::rax);
            switch (op) {
            case OP_FBEQ: a.cmovz(x86::r10, x86::r11);  break;
            case OP_FBNE: a.cmovnz(x86::r10, x86::r11); break;
            case OP_FBLT: a.cmovs(x86::r10, x86::r11);  break;
            case OP_FBGE: a.cmovns(x86::r10, x86::r11); break;
            case OP_FBLE: a.cmovle(x86::r10, x86::r11); break;
            case OP_FBGT: a.cmovg(x86::r10, x86::r11);  break;
            default: break;
            }
            a.mov(x86::qword_ptr(x86::rbp, m_off.state_pc), x86::r10);
            a.jmp(cont);
            a.bind(bail);
            set_pc(b->tag + 4 * (uint64_t)i);                    // resume this instruction in the interpreter
            a.mov(x86::eax, imm(i)); a.add(x86::eax, x86::dword_ptr(x86::rsp, 40)); a.jmp(done);
            a.bind(cont);
            continue;
        }

        if (is_branch(op)) {
            const int64_t  bdisp = (int64_t)((uint64_t)(ins & 0x1FFFFF) << 43) >> 43;  // sext disp21
            const uint64_t fall = b->tag + 4 * (uint64_t)(i + 1);
            const uint64_t tgt = fall + (uint64_t)(bdisp * 4);
            if (op == OP_BR || op == OP_BSR) {                 // Ra = return address; PC = target
                if (ra != 31) { a.mov(x86::r10, imm(fall & ~(uint64_t)3)); mov_to_reg(ra, x86::r10); }  // link = PC & ~3 (DO_BR)
                a.mov(x86::r10, imm(tgt));
            }
            else {                                           // conditional: PC = cond ? target : fall
                if (ra == 31) a.xor_(x86::eax, x86::eax);
                else          mov_from_reg(x86::rax, ra);
                a.mov(x86::r10, imm(fall));
                a.mov(x86::r11, imm(tgt));
                if (op == OP_BLBC || op == OP_BLBS) a.test(x86::rax, imm(1));
                else                                a.test(x86::rax, x86::rax);
                switch (op) {
                case OP_BEQ:  a.cmovz(x86::r10, x86::r11);  break;
                case OP_BNE:  a.cmovnz(x86::r10, x86::r11); break;
                case OP_BLT:  a.cmovs(x86::r10, x86::r11);  break;
                case OP_BGE:  a.cmovns(x86::r10, x86::r11); break;
                case OP_BLE:  a.cmovle(x86::r10, x86::r11); break;
                case OP_BGT:  a.cmovg(x86::r10, x86::r11);  break;
                case OP_BLBC: a.cmovz(x86::r10, x86::r11);  break;
                case OP_BLBS: a.cmovnz(x86::r10, x86::r11); break;
                default: break;
                }
            }
            a.mov(x86::qword_ptr(x86::rbp, m_off.state_pc), x86::r10);
            continue;
        }

        // INTL conditional moves (CMOVxx): Rc = cond(Ra) ? op2 : Rc. Same condition tests as the
        // matching branches; op2 (Rb or literal) moves into Rc only when the condition holds, so the
        // current Rc is read and kept otherwise. R31 dest discards the result.
        if (op == OP_CMOV) {
            if (rc == 31) continue;
            const uint32_t f = (ins >> 5) & 0x7f;
            op1_rax();                                  // rax = Ra (condition operand)
            op2_rcx();                                  // rcx = op2 (Rb or literal -- the moved value)
            mov_from_reg(x86::r10, rc);                 // r10 = current Rc (kept when the condition fails)
            if (f == 0x14 || f == 0x16) a.test(x86::rax, imm(1));    // CMOVLBS/LBC: test the low bit
            else                        a.test(x86::rax, x86::rax);  // others: test the full value
            switch (f) {
            case 0x24: a.cmovz(x86::r10, x86::rcx);  break;   // CMOVEQ  (Ra == 0)
            case 0x26: a.cmovnz(x86::r10, x86::rcx); break;   // CMOVNE  (Ra != 0)
            case 0x44: a.cmovs(x86::r10, x86::rcx);  break;   // CMOVLT  (Ra <  0)
            case 0x46: a.cmovns(x86::r10, x86::rcx); break;   // CMOVGE  (Ra >= 0)
            case 0x64: a.cmovle(x86::r10, x86::rcx); break;   // CMOVLE  (Ra <= 0)
            case 0x66: a.cmovg(x86::r10, x86::rcx);  break;   // CMOVGT  (Ra >  0)
            case 0x14: a.cmovnz(x86::r10, x86::rcx); break;   // CMOVLBS (Ra & 1)
            case 0x16: a.cmovz(x86::r10, x86::rcx);  break;   // CMOVLBC (!(Ra & 1))
            }
            mov_to_reg(rc, x86::r10);
            continue;
        }

        // INTS (0x12) byte-manipulation: extract/insert/mask/zap. Rc = a shift+mask of Ra keyed on the
        // byte position pos = Rb&7 (the selector V_2). Pure ALU -> verify-checked by the GPR compare.
        // RAX = Ra/result, CL = the variable shift, R10/R11 scratch, EDX preserves pos for the H-form
        // pos==0 edge cases. Mirrors cpu_bwx.h; size (0=B 1=W 2=L 3=Q) and mask come from the function.
        if (op == OP_EXTL || op == OP_EXTH || op == OP_INSL || op == OP_INSH
            || op == OP_MSKL || op == OP_MSKH || op == OP_ZAP) {
            if (rc == 31) continue;
            const uint32_t f = (ins >> 5) & 0x7f;
            const int size = (f >> 4) & 3;
            const uint64_t mask = (size == 0) ? (uint64_t)0xff : (size == 1) ? (uint64_t)0xffff
                : (size == 2) ? (uint64_t)0xffffffff : ~(uint64_t)0;
            op1_rax();                                                  // rax = Ra (data)
            op2_rcx();                                                  // rcx = Rb / literal (selector)
            switch (op) {
            case OP_EXTL:                                             // (Ra >> pos*8) & mask
                a.and_(x86::ecx, imm(7)); a.shl(x86::ecx, imm(3));
                a.shr(x86::rax, x86::cl);
                if (size != 3) { a.mov(x86::r10, imm(mask)); a.and_(x86::rax, x86::r10); }
                break;
            case OP_EXTH:                                             // (Ra << ((64-pos*8)&63)) & mask
                a.and_(x86::ecx, imm(7)); a.shl(x86::ecx, imm(3));
                a.neg(x86::ecx); a.and_(x86::ecx, imm(63));
                a.shl(x86::rax, x86::cl);
                if (size != 3) { a.mov(x86::r10, imm(mask)); a.and_(x86::rax, x86::r10); }
                break;
            case OP_INSL:                                             // (Ra & mask) << pos*8
                if (size != 3) { a.mov(x86::r10, imm(mask)); a.and_(x86::rax, x86::r10); }
                a.and_(x86::ecx, imm(7)); a.shl(x86::ecx, imm(3));
                a.shl(x86::rax, x86::cl);
                break;
            case OP_INSH:                                             // pos ? ((Ra&mask) >> ((64-pos*8)&63)) : 0
                if (size != 3) { a.mov(x86::r10, imm(mask)); a.and_(x86::rax, x86::r10); }
                a.and_(x86::ecx, imm(7)); a.mov(x86::edx, x86::ecx);
                a.shl(x86::ecx, imm(3)); a.neg(x86::ecx); a.and_(x86::ecx, imm(63));
                a.shr(x86::rax, x86::cl);
                a.xor_(x86::r11d, x86::r11d); a.test(x86::edx, x86::edx); a.cmovz(x86::rax, x86::r11);
                break;
            case OP_MSKL:                                             // Ra & ~(mask << pos*8)
                a.and_(x86::ecx, imm(7)); a.shl(x86::ecx, imm(3));
                a.mov(x86::r10, imm(mask)); a.shl(x86::r10, x86::cl);
                a.not_(x86::r10); a.and_(x86::rax, x86::r10);
                break;
            case OP_MSKH:                                             // pos ? (Ra & ~(mask >> ((64-pos*8)&63))) : Ra
                a.and_(x86::ecx, imm(7)); a.mov(x86::edx, x86::ecx);
                a.shl(x86::ecx, imm(3)); a.neg(x86::ecx); a.and_(x86::ecx, imm(63));
                a.mov(x86::r10, imm(mask)); a.shr(x86::r10, x86::cl); a.not_(x86::r10);
                a.mov(x86::r11, x86::rax); a.and_(x86::r11, x86::r10);
                a.test(x86::edx, x86::edx); a.cmovnz(x86::rax, x86::r11);
                break;
            case OP_ZAP:                                              // Ra & byte_expand(selector); ZAP inverts
                a.movzx(x86::ecx, x86::cl);
                a.mov(x86::r11, imm((uint64_t)&g_zapnot_mask[0]));
                a.mov(x86::r10, x86::qword_ptr(x86::r11, x86::rcx, 3));   // g_zapnot_mask[selector & 0xff]
                if (f == 0x30) a.not_(x86::r10);                        // ZAP keeps bytes whose bit is CLEAR
                a.and_(x86::rax, x86::r10);
                break;
            default: break;
            }
            mov_to_reg(rc, x86::rax);
            continue;
        }

        switch (op) {
        case OP_ADDQ:  emit_alu2(x86::Inst::kIdAdd); break;
        case OP_SUBQ:  emit_alu2(x86::Inst::kIdSub); break;
        case OP_AND:   emit_alu2(x86::Inst::kIdAnd); break;
        case OP_BIS:   emit_alu2(x86::Inst::kIdOr);  break;
        case OP_XOR:   emit_alu2(x86::Inst::kIdXor); break;
        case OP_BIC:   op1_rax(); op2_rcx(); a.not_(x86::rcx); a.and_(x86::rax, x86::rcx); break;
        case OP_ORNOT: op1_rax(); op2_rcx(); a.not_(x86::rcx); a.or_(x86::rax, x86::rcx); break;
        case OP_EQV:   op1_rax(); op2_rcx(); a.not_(x86::rcx); a.xor_(x86::rax, x86::rcx); break;
        case OP_MULQ:  op1_rax(); op2_rcx(); a.imul(x86::rax, x86::rcx); break;
        case OP_UMULH: op1_rax(); op2_rcx(); a.mul(x86::rcx); a.mov(x86::rax, x86::rdx); break;  // RDX:RAX = Ra*op2; hi64 = RDX
        case OP_MULL:                                          // 32-bit multiply, low 32 sign-extended
        {
            if (ra == 31) a.xor_(x86::eax, x86::eax);
            else          mov_from_reg32(x86::eax, ra);
            if (islit)         a.mov(x86::ecx, imm(lit));
            else if (rb == 31) a.xor_(x86::ecx, x86::ecx);
            else               mov_from_reg32(x86::ecx, rb);
            a.imul(x86::eax, x86::ecx);                          // eax = (Ra*op2)[31:0]
            a.movsxd(x86::rax, x86::eax);
            break;
        }

        case OP_S4ADDQ: op1_rax(); a.shl(x86::rax, imm(2)); op2_rcx(); a.add(x86::rax, x86::rcx); break;
        case OP_S8ADDQ: op1_rax(); a.shl(x86::rax, imm(3)); op2_rcx(); a.add(x86::rax, x86::rcx); break;
        case OP_S4SUBQ: op1_rax(); a.shl(x86::rax, imm(2)); op2_rcx(); a.sub(x86::rax, x86::rcx); break;
        case OP_S8SUBQ: op1_rax(); a.shl(x86::rax, imm(3)); op2_rcx(); a.sub(x86::rax, x86::rcx); break;

        case OP_SLL: op1_rax(); op2_rcx(); a.shl(x86::rax, x86::cl); break;
        case OP_SRL: op1_rax(); op2_rcx(); a.shr(x86::rax, x86::cl); break;
        case OP_SRA: op1_rax(); op2_rcx(); a.sar(x86::rax, x86::cl); break;

        case OP_SEXTB: op2_rcx(); a.movsx(x86::rax, x86::cl); break;   // Rc = sign-extend low byte of op2 (V_2)
        case OP_SEXTW: op2_rcx(); a.movsx(x86::rax, x86::cx); break;   // Rc = sign-extend low word of op2 (V_2)

        case OP_CTPOP: op2_rcx(); a.popcnt(x86::rax, x86::rcx); break; // Rc = popcount(op2); POPCNT(0)==0 == CTPOP(0)
        case OP_CTLZ: {                                                // Rc = (op2==0) ? 64 : 63 - BSR(op2)
            op2_rcx();
            Label zero = a.new_label(), done = a.new_label();
            a.bsr(x86::rax, x86::rcx);            // ZF=1 if op2==0; else rax = index of MSB (0..63)
            a.jz(zero);
            a.xor_(x86::rax, imm(63));            // 63 - bsr  (bsr in [0,63], so 63-n == n^63)
            a.jmp(done);
            a.bind(zero); a.mov(x86::eax, imm(64));
            a.bind(done);
            break;
        }
        case OP_CTTZ:                                                  // Rc = (op2==0) ? 64 : BSF(op2)
            op2_rcx();
            a.bsf(x86::rax, x86::rcx);            // ZF=1 if op2==0; else rax = index of LSB
            a.mov(x86::r10d, imm(64));            // MOV preserves ZF
            a.cmovz(x86::rax, x86::r10);          // op2==0 -> 64 (BSF leaves rax undefined)
            break;

        case OP_AMASK:    // Rc = op2 & ~CPU_AMASK -- EV68 feature mask 0x1307 (keep in sync w/ cpu_defs.h);
            op2_rcx();      // classify enforced Ra==31 (the Ra!=31 form traps OPCDEC in the interpreter)
            a.mov(x86::rax, imm(~(uint64_t)0x1307));
            a.and_(x86::rax, x86::rcx);
            break;
        case OP_IMPLVER:  // Rc = CPU_IMPLVER (2 = EV6 family; keep in sync w/ cpu_defs.h)
            a.mov(x86::eax, imm(2));
            break;

        case OP_CMPEQ:  op1_rax(); op2_rcx(); a.cmp(x86::rax, x86::rcx); a.sete(x86::al);  a.movzx(x86::eax, x86::al); break;
        case OP_CMPLT:  op1_rax(); op2_rcx(); a.cmp(x86::rax, x86::rcx); a.setl(x86::al);  a.movzx(x86::eax, x86::al); break;
        case OP_CMPLE:  op1_rax(); op2_rcx(); a.cmp(x86::rax, x86::rcx); a.setle(x86::al); a.movzx(x86::eax, x86::al); break;
        case OP_CMPULT: op1_rax(); op2_rcx(); a.cmp(x86::rax, x86::rcx); a.setb(x86::al);  a.movzx(x86::eax, x86::al); break;
        case OP_CMPULE: op1_rax(); op2_rcx(); a.cmp(x86::rax, x86::rcx); a.setbe(x86::al); a.movzx(x86::eax, x86::al); break;

        case OP_CMPBGE:   // 8 parallel unsigned byte compares (Ra.byte[i] >= op2.byte[i]) -> bit i; bits 63:8 = 0
            op1_rax(); op2_rcx();
            a.movq(x86::xmm0, x86::rax);             // Ra (8 bytes)
            a.movq(x86::xmm1, x86::rcx);             // op2 (8 bytes; literal in byte 0, 0 elsewhere)
            a.movdqa(x86::xmm2, x86::xmm0);          // copy of Ra
            a.pmaxub(x86::xmm2, x86::xmm1);          // per-byte unsigned max(Ra, op2)
            a.pcmpeqb(x86::xmm2, x86::xmm0);         // 0xff where max == Ra, i.e. Ra >= op2
            a.pmovmskb(x86::eax, x86::xmm2);         // gather byte-sign bits -> low 8 bits = the mask
            a.movzx(x86::eax, x86::al);              // discard the high (zero-padded) lane bits
            break;

        case OP_ADDL: case OP_SUBL:                                    // 32-bit, result sign-extended to 64
        case OP_S4ADDL: case OP_S8ADDL: case OP_S4SUBL: case OP_S8SUBL: // scaled longword: sext32((Ra*scale) +/- Rb)
        {
            const bool issub = (op == OP_SUBL || op == OP_S4SUBL || op == OP_S8SUBL);
            const int  sh = (op == OP_S4ADDL || op == OP_S4SUBL) ? 2     // Ra*4
                : (op == OP_S8ADDL || op == OP_S8SUBL) ? 3     // Ra*8
                : 0;
            if (ra == 31) a.xor_(x86::eax, x86::eax);
            else          mov_from_reg32(x86::eax, ra);   // shadow-remapped (was a raw rbx read)
            if (sh) a.shl(x86::eax, imm(sh));            // scale in 32-bit: (Ra<<sh)[31:0] == ((RAV<<sh)+..)[31:0]
            if (islit)         a.mov(x86::ecx, imm(lit));
            else if (rb == 31) a.xor_(x86::ecx, x86::ecx);
            else               mov_from_reg32(x86::ecx, rb);
            if (issub) a.sub(x86::eax, x86::ecx);
            else       a.add(x86::eax, x86::ecx);
            a.movsxd(x86::rax, x86::eax);
            break;
        }
        default: break;
        }

        if (rc != 31) mov_to_reg(rc, x86::rax);
    } while (0);
}

void CJitEngine::compile_block(JitBlock* b, const uint8_t* dram, uint64_t dram_size, void* read_helper, void* write_helper, void* opcdec_helper, void* hw_mfpr_helper, void* hw_ld_helper, void* hw_mtpr_helper, void* hw_st_helper, void* indirect_helper, void* read_locked_helper, void* stc_helper, void* misc_helper, void* read_vpte_helper, void* read_wchk_helper, void* itof_helper, void* ftoi_helper, void* fltl_helper, void* fp_read_helper, void* fp_write_helper, void* fltv_helper)
{
  using namespace asmjit;
  // Reclaim must self-trigger here, NOT only in flush(): flush() runs when the guest executes
  // IMB/IC_FLUSH, and a compute-heavy phase can go minutes without one while recompiles keep
  // allocating -- code memory grew unbounded (multi-GB). Safe: we're in this CPU's cold path.
  if (m_rt && m_code_bytes >= kReclaimBytes) {
    reclaim_code();
    b->valid = true;   // b was just (re)validated by record(); restore it after the wipe
  }
  b->compiled = true;

  uint64_t phys = b->phys;
  if (b->n_instr == 0 || phys + (uint64_t) b->n_instr * 4 > dram_size) return;
  const uint32_t* words = (const uint32_t*) (dram + phys);   // x86 LE == Alpha LE

  // PALmode blocks (PC bit 0) remap R4-7/R20-23 to the shadow bank (see RREG); reg() applies it.
  const bool pal_block = (b->tag & 1) != 0;

  // Stop at the 8 KB page boundary: past it the next instruction's physical
  // address need not be phys+4 (the next virtual page maps elsewhere), so words[]
  // there would be the wrong instructions. (The page-crossing case verify caught.)
  const uint64_t page_end = (phys & ~(uint64_t) 0x1FFF) + 0x2000;
  uint32_t plen = 0;
  bool terminator_branch = false;       // last instruction is a compiled terminator (sets its own PC)
  bool terminator_jmp = false;          // ...and it's a computed jump (don't chain: targets vary)
  while (plen < b->n_instr && plen < 64
         && (phys + (uint64_t) plen * 4) < page_end)
  {
    SafeOp sop = classify(words[plen], pal_block);
    if (sop == OP_NONE) {               // uncompilable op ends the straight-line prefix
#ifdef JIT_STATS
      const uint32_t bop = words[plen] >> 26;
      m_term_op[bop]++;                 // tally what cut this block (the coverage gap to chase)
      if (bop == 0x00)                  // CALL_PAL: also tally the function code (low 8 bits)
        m_pal_func[words[plen] & 0xFF]++;
      else if (bop == 0x1d)             // HW_MTPR: tally the IPR index -- which writes break blocks
        m_mtpr_func[(words[plen] >> 8) & 0xFF]++;
      else if (bop == 0x1b)             // HW_LD: tally the form (phys/virt/lock/vpte/chk, ins[15:12])
        m_hwld_func[(words[plen] >> 12) & 0xF]++;
      else if (bop == 0x18)             // MISC: tally the Ra==31 form (ins[15:12]: 0xc RPCC / 0xe RC / 0xf RS)
        m_misc_func[(words[plen] >> 12) & 0xF]++;
      // Punch list: one-shot print of the first ACTIONABLE breaker -- skip the opcodes whose
      // compilable subset is already settled, so it points at the next NEW target rather than a
      // decided one: 0x00 CALL_PAL (terminator), 0x1b HW_LD / 0x1f HW_ST (physical done;
      // conditional/virtual forms side-effecting), 0x1d HW_MTPR (pure-store IPRs done; rest
      // side-effecting), 0x10 INTA + 0x13 INTM (non-trapping ops done; only /V overflow-trap variants left),
      // 0x18 MISC (barriers/hints + RPCC/RC/RS via log/replay done; only the rare Ra==31 RC/RS forms interpret),
      // 0x14 ITFP (ITOF* moves done; SQRT* = the deferred FP-math class: FPCR/rounding/traps),
      // 0x17 FLTL (non-arithmetic done; only CVTQL/V trap variants left).
      // JMP (0x1a) + HW_RET (0x1e) are now compiled+chained. Stats count all.
      if (!m_first_breaker_logged && bop != 0x00 && bop != 0x1b && bop != 0x1d && bop != 0x1f && bop != 0x10 && bop != 0x18 && bop != 0x13 && bop != 0x14 && bop != 0x17) {
        m_first_breaker_logged = true;
        printf("[JIT][PUNCH][CPU%d] first unhandled breaker: %s(0x%02x) ins=%08x at pc=%016llx%s\n",
               m_cpu_id, opcode_name(bop), bop, words[plen],
               (unsigned long long) ((b->tag & ~(uint64_t) 1) + (uint64_t) plen * 4),
               pal_block ? "  [PALmode]" : "");
      }
#endif
      break;
    }
    plen++;
    if (is_terminator(sop)) {           // branch or computed jump ends the block
      terminator_branch = true;
      if (sop == OP_JMP || sop == OP_HW_RET) terminator_jmp = true;
      break;
    }
  }

  if (plen == 0) return;
#ifdef JIT_REGPROF
  b->rp_mask = regprof_mask(words, plen);   // GPR-access fingerprint; exec-weighted at report time
#endif

  // Emit  uint32_t fn(CAlphaCPU* cpu, uint64_t* regs)  (Win64: cpu=RCX, regs=RDX).
  // Keep cpu in RBP and regs in RBX (callee-saved, so they survive helper calls);
  // reserve a 40-byte frame (32 shadow + 8 load-out slot) that keeps RSP 16-aligned
  // for calls. RAX = op1/result, RCX = operand2 (CL for variable shifts).
  CodeHolder code;
  if (code.init(((JitRuntime*) m_rt)->environment()) != Error::kOk) return;
#ifdef JIT_DISASM
  // Dev: capture this block's disassembly, validate each emitted instruction, and trap any
  // emit failure (dumped + bailed near rt->add() below). Logging formats every instruction.
  StringLogger logger;
  code.set_logger(&logger);
  JitErrorHandler eh; eh.cpu_id = m_cpu_id; eh.fp = m_disasm_fp;
  code.set_error_handler(&eh);
#endif
  x86::Assembler a(&code);
#ifdef JIT_DISASM
  a.add_diagnostic_options(DiagnosticOptions::kValidateAssembler);
#endif

  // Host integer-argument registers:
  // Win64 {rcx,rdx,r8,r9}; System V {rdi,rsi,rdx,rcx}. aq(i)/ad(i) = the i argument as a
  // 64-/32-bit register; emit_call (below) marshals into them, replacing out usage of 
  // #ifdef _WIN32. Helpers limited to <= 4 integer arguments.
  CallConv cc;
  (void) cc.init(CallConvId::kCDecl, ((JitRuntime*) m_rt)->environment());
  const uint8_t* gpa = cc.passed_order(RegGroup::kGp);
  auto aq = [&](int i) { return x86::gpq(gpa[i]); };
  auto ad = [&](int i) { return x86::gpd(gpa[i]); };
  // cpu (RBP), regs (RBX), the chain counter (R14) and the basic Alpha-GPR pins (R12/R13/R15)
  // all hold live values across helper calls, so they must be callee-saved under the host ABI.
  // Verified at dev time (compiled out in NDEBUG builds).
  [[maybe_unused]] const uint32_t kPinnedGp =
      (1u << x86::rbp.id()) | (1u << x86::rbx.id()) | (1u << x86::r14.id())
    | (1u << x86::r12.id()) | (1u << x86::r13.id()) | (1u << x86::r15.id());
  assert((((uint32_t) cc.preserved_regs(RegGroup::kGp)) & kPinnedGp) == kPinnedGp);

  a.push(x86::rbx);
  a.push(x86::rbp);
  a.push(x86::r14);            // callee-saved: now a pin for Alpha R30 (SP); chain count moved to [rsp+40]
  a.push(x86::r12);            // callee-saved: pin for Alpha R26 (RA)
  a.push(x86::r13);            // callee-saved: pin for Alpha R16 (a0)
  a.push(x86::r15);            // callee-saved: pin for Alpha R27 (PV)
#ifdef _WIN32
  a.push(x86::rsi);            // callee-saved on Win64: pin for Alpha R29 (GP)
  a.push(x86::rdi);            // callee-saved on Win64: pin for Alpha R0 (v0)
#endif
  a.sub(x86::rsp, imm(56));    // 32 shadow + out slot + chain-count slot [rsp+40]; 6/8 pushes -> 56 keeps RSP 16-aligned
  a.mov(x86::rbp, aq(0));      // cpu  (arg 0)
  a.mov(x86::rbx, aq(1));      // regs (arg 1)
  a.mov(x86::qword_ptr(x86::rsp, 40), imm(0));   // chain instruction count := 0 (reclaimed r14 -> stack slot)
  // Load the global pins from regs[] on cold entry. Chained re-entry jumps to `body` below,
  // skipping this -- the pins stay live in x86 across the whole chain, synced back at `done`.
  a.mov(x86::r12, x86::qword_ptr(x86::rbx, 26 * 8));   // R26 (RA)
  a.mov(x86::r13, x86::qword_ptr(x86::rbx, 16 * 8));   // R16 (a0)
  a.mov(x86::r15, x86::qword_ptr(x86::rbx, 27 * 8));   // R27 (PV)
  a.mov(x86::r14, x86::qword_ptr(x86::rbx, 30 * 8));   // R30 (SP) -- reclaimed r14
#ifdef _WIN32
  a.mov(x86::rsi, x86::qword_ptr(x86::rbx, 29 * 8));   // R29 (GP)
  a.mov(x86::rdi, x86::qword_ptr(x86::rbx,  0 * 8));   // R0 (v0)
#endif

  Label done = a.new_label();  // shared exit: restore frame + ret (EAX preset by caller)
  Label body = a.new_label();  // chained re-entry (after the prologue; preserves R14)
  a.bind(body);
  const size_t body_off = code.code_size();   // byte offset of the chained entry from fn
#ifdef JIT_REGPROF
  a.mov(x86::rax, imm((uint64_t) &b->rp_hits));   // REGPROF: count every execution (cold entry + chained re-entry)
  a.inc(x86::qword_ptr(x86::rax));                // RAX is dead at body entry -- the first op reloads it
#endif

  // The compiled block computes its own next PC into state.pc at every exit (the
  // foundation for branch compilation and block linking). R10 is scratch here.
  auto set_pc = [&](uint64_t pc_val) {
    a.mov(x86::r10, imm(pc_val));
    a.mov(x86::qword_ptr(x86::rbp, m_off.state_pc), x86::r10);
  };

  // Block register allocator: the 3 global pins (R26/R16/R27 -> r12/r13/r15, callee-saved, live across the
  // chain) are the static binding today. Dynamic next. 
  RegAlloc ra;
  for (int r = 0; r < 32; ++r) ra.host[r] = -1;
  ra.rax_holds = -1;
  ra.host[kGlobalPins[0]] = (int) x86::r12.id();
  ra.host[kGlobalPins[1]] = (int) x86::r13.id();
  ra.host[kGlobalPins[2]] = (int) x86::r15.id();
  ra.host[30] = (int) x86::r14.id();                  // SP (reclaimed r14), all platforms
#ifdef _WIN32
  ra.host[29] = (int) x86::rsi.id();                  // GP (Win64)
  ra.host[0]  = (int) x86::rdi.id();                  // v0 (Win64)
#endif

  const HelperSet hs = { read_helper, write_helper, opcdec_helper, hw_mfpr_helper, hw_ld_helper,
                       hw_mtpr_helper, hw_st_helper, indirect_helper, read_locked_helper, stc_helper,
                       misc_helper, read_vpte_helper, read_wchk_helper, itof_helper, ftoi_helper,
                       fltl_helper, fp_read_helper, fp_write_helper, fltv_helper };

  std::vector<ColdMemStub> cold;   // outlined memop slow paths, emitted after the epilogue
  for (uint32_t i = 0; i < plen; ++i)
      emit_op(&a, gpa, &done, hs, pal_block, b, words[i], i, ra, &cold);

  // Epilogue. Count this block's instructions, then chain into the next block (staying
  // in native code) or return to the dispatcher.
#ifndef JIT_VERIFY
  // Gate the chain: stop if we've hit the budget ceiling or an interrupt/timer is pending
  auto emit_gate = [&](Label& lbl) {
    a.mov(x86::rax, x86::qword_ptr(x86::rsp, 40)); a.cmp(x86::rax, x86::qword_ptr(x86::rbp, m_off.jit_budget)); a.jge(lbl);
    a.cmp(x86::byte_ptr(x86::rbp, m_off.check_int), imm(0));     a.jne(lbl);
    a.cmp(x86::byte_ptr(x86::rbp, m_off.check_timers), imm(0));  a.jne(lbl);
  };
  // Cached direct link: tail straight into our cached successor's body via its SNAPSHOT
  // {tag, vgen, body} in OUR link slots so one cache line, no dereference into the successor's
  // JitBlock. A slot hits when it maps this exit's PC (tag == R10) and was patched under the
  // current epoch. Otherwise record a patch request (m_link_from = this block) and fall back. 
  auto emit_chain = [&](Label& lbl) {
    Label miss = a.new_label();
    // PAL/SDE guard once -- depends on the target (R10) + SDE, not the slot: a PALmode target's shadow
    // remap assumes SDE, so if R10 is PAL and !SDE no cached slot may chain in.
    { Label ok = a.new_label();
      a.test(x86::r10, imm(1));                            a.jz(ok);
      a.cmp(x86::byte_ptr(x86::rbp, m_off.sde), imm(0));   a.je(miss);   // PALmode + !SDE: don't
      a.bind(ok); }
    // Current epoch once, reused by every slot: ONE maintained qword (m_vgen_cur).
    a.mov(x86::rdx, imm((uint64_t) &m_vgen_cur));
    a.mov(x86::r11, x86::qword_ptr(x86::rdx));                 // r11 = current epoch
    // Poly-link: walk the cached successor snapshots, 2-successor cache both
    for (int sl = 0; sl < kLinkSlots; ++sl) {
      Label nxt = (sl + 1 < kLinkSlots) ? a.new_label() : miss;
      if (sl == 0) a.mov(x86::rax, imm((uint64_t) &b->link[0]));
      else         a.add(x86::rax, imm((uint32_t) sizeof(LinkSlot)));
      a.mov(x86::rcx, x86::qword_ptr(x86::rax, (int32_t) offsetof(LinkSlot, body)));   // snapshot body
      a.test(x86::rcx, x86::rcx);                                               a.jz(nxt);   // empty slot
      a.cmp(x86::qword_ptr(x86::rax, (int32_t) offsetof(LinkSlot, tag)),  x86::r10); a.jne(nxt);   // not this exit's target
      a.cmp(x86::qword_ptr(x86::rax, (int32_t) offsetof(LinkSlot, vgen)), x86::r11); a.jne(nxt);   // stale: re-patch via dispatcher
      a.jmp(x86::rcx);                                                 // HIT: tail in (shared frame)
      if (sl + 1 < kLinkSlots) a.bind(nxt);
    }
    a.bind(miss);
    a.mov(x86::rax, imm((uint64_t) b));
    a.mov(x86::qword_ptr(x86::rbp, m_off.link_from), x86::rax);   // request a successor-cache patch
    // fall through to lbl (return to dispatcher)
  };
#endif
  if (terminator_jmp) {
    // Computed jump (JMP / HW_RET): R10 holds the register target (already written to state.pc).
    // Chain in-frame via jit_indirect -- the dispatcher's own block-cache lookup -- tailing into
    // the target's compiled body when it's live + runnable here. Unlike the old single-slot link
    // this keys on the ACTUAL target, so it handles all targets with no thrash on varying jumps.
    a.add(x86::qword_ptr(x86::rsp, 40), imm(plen));
#ifndef JIT_VERIFY
    Label exit_chain = a.new_label();
    emit_gate(exit_chain);                                        // budget/interrupt: bail to dispatcher
    a.mov(aq(0), x86::rbp);                                       // cpu    (arg 0)
    a.mov(aq(1), x86::r10);                                       // target (arg 1) == state.pc
    a.mov(x86::rax, imm((uint64_t) indirect_helper));
    a.call(x86::rax);                                             // jit_indirect(cpu, target) -> body | 0
    a.test(x86::rax, x86::rax);                              a.jz(exit_chain);
    a.jmp(x86::rax);                                              // HIT: tail into the target's body
    a.bind(exit_chain);
#endif
  } else if (terminator_branch) {
    a.add(x86::qword_ptr(x86::rsp, 40), imm(plen));   // R10 still holds the next PC (branch wrote state.pc + R10)
#ifndef JIT_VERIFY
    Label exit_chain = a.new_label();
    Label not_self   = a.new_label();
    emit_gate(exit_chain);
    // Self-loop fast path: a taken branch back to our own start (r10 == b->tag) jumps
    // straight into the body, skipping the resolver call entirely.
    a.mov(x86::rax, imm(b->tag));                               // tag may exceed imm32
    a.cmp(x86::r10, x86::rax);                                  a.jne(not_self);
    a.jmp(body);
    a.bind(not_self);
    emit_chain(exit_chain);
    a.bind(exit_chain);
#endif
  } else {
    set_pc(b->tag + 4 * (uint64_t) plen);   // straight-line fall-through to the next block
    a.add(x86::qword_ptr(x86::rsp, 40), imm(plen));
#ifndef JIT_VERIFY
    Label exit_chain = a.new_label();
    emit_gate(exit_chain);
    emit_chain(exit_chain);
    a.bind(exit_chain);
#endif
  }
  a.mov(x86::eax, x86::dword_ptr(x86::rsp, 40));   // total instructions completed across the chain
  a.bind(done);                 // bail jumps here with EAX already set
  // Sync the pins back to regs[] -- rbx still = regs (restored last), and every dispatcher exit
  // (fall-through or mid-block bail) reaches here, so regs[] is live when we return.
  a.mov(x86::qword_ptr(x86::rbx, 26 * 8), x86::r12);   // R26 (RA)
  a.mov(x86::qword_ptr(x86::rbx, 16 * 8), x86::r13);   // R16 (a0)
  a.mov(x86::qword_ptr(x86::rbx, 27 * 8), x86::r15);   // R27 (PV)
  a.mov(x86::qword_ptr(x86::rbx, 30 * 8), x86::r14);   // R30 (SP)
#ifdef _WIN32
  a.mov(x86::qword_ptr(x86::rbx, 29 * 8), x86::rsi);   // R29 (GP)
  a.mov(x86::qword_ptr(x86::rbx,  0 * 8), x86::rdi);   // R0 (v0)
#endif
  a.add(x86::rsp, imm(56));
#ifdef _WIN32
  a.pop(x86::rdi);             // Win64 pins pop first (reverse push order)
  a.pop(x86::rsi);
#endif
  a.pop(x86::r15);              // pins pop in reverse push order
  a.pop(x86::r13);
  a.pop(x86::r12);
  a.pop(x86::r14);
  a.pop(x86::rbp);
  a.pop(x86::rbx);
  a.ret();

  // Cold tail: the outlined memop slow paths (dead 99.8% of the time -- dpc hit rate). 
  for (const ColdMemStub& s : cold)
    emit_cold_mem_stub(a, gpa, m_off, s);

  const size_t csz = code.code_size();
  JitFn fn = nullptr;
#ifdef JIT_DISASM
  {
    FILE* out = m_disasm_fp ? m_disasm_fp : stderr;
    fprintf(out, "[JIT][CPU%d] block @ %016llx%s  (%u instr, %llu bytes)\n%s\n",
            m_cpu_id, (unsigned long long) (b->tag & ~(uint64_t) 1),
            (b->tag & 1) ? " PAL" : "", plen, (unsigned long long) csz, logger.data());
    fflush(out);   // per-block flush: preserve the trace if JIT'd code later crashes
  }
  if (eh.failed) return;   // emit error already reported -- don't ship a broken block
#endif
  if (((JitRuntime*) m_rt)->add(&fn, &code) != Error::kOk) return;
  b->code = fn;
  b->jit_body = (void*) ((uint8_t*) (void*) fn + body_off);   // chained re-entry (past prologue)
  b->body_off = (uint32_t) body_off;                          // to restore jit_body on revalidate
  b->src_sum  = src_hash(dram + phys, b->n_instr);            // source fingerprint (revalidate vs self-mod)
  b->hash_len = b->n_instr;                                   // freeze the hash extent (n_instr drifts)
  b->prefix_len = plen;
  m_code_bytes += csz;   // track for the reclaim threshold (see flush())
#ifdef JIT_STATS
  m_stat_compiled++;
  m_stat_plen_sum += plen;
  m_stat_code_bytes += csz;
#endif
#ifdef JIT_REGPROF
  b->rp_csz = (uint32_t) csz;   // exec-weighted expansion: sum(rp_hits*rp_csz) / sum(rp_hits*prefix_len)
#endif
}

// Compile an N-block trace. Reuses the shared emit_op for each block's per-op codegen (so the body
// is the exact one the block path already verifies). Blocks are fused with a GUARD between them: after a
// block's terminator (which left R10 = its next PC), check R10 == the next fused block's tag; on a hit fall
// through in-trace, on a miss SIDE-EXIT to the dispatcher at the real next PC. n_blocks==1 = the single-
// block trace. Fills the caller-provided slot t (code + per-segment source descriptors + coherence epoch).
void CJitEngine::compile_trace(TraceFragment* t, JitBlock** blocks, uint32_t n_blocks,
                               const uint8_t* dram, uint64_t dram_size, const HelperSet& hs)
{
  using namespace asmjit;
  if (n_blocks == 0 || n_blocks > kMaxTraceSegs) return;
  // Validate every segment up front: a compiled prefix that fits in DRAM. (prefix_len, NOT n_instr --
  // ops past the prefix never passed the safe-to-compile scan; emitting them runs code PAST the terminator.)
  for (uint32_t bi = 0; bi < n_blocks; ++bi) {
    const JitBlock* b = blocks[bi];
    if (b->prefix_len == 0 || b->phys + (uint64_t) b->prefix_len * 4 > dram_size) return;
  }

  CodeHolder code;
  if (code.init(((JitRuntime*) m_rt)->environment()) != Error::kOk) return;
  x86::Assembler a(&code);
  CallConv cc;
  (void) cc.init(CallConvId::kCDecl, ((JitRuntime*) m_rt)->environment());
  const uint8_t* gpa = cc.passed_order(RegGroup::kGp);

  a.push(x86::rbx); a.push(x86::rbp); a.push(x86::r14);
  a.push(x86::r12); a.push(x86::r13); a.push(x86::r15);
#ifdef _WIN32
  a.push(x86::rsi); a.push(x86::rdi);
#endif
  a.sub(x86::rsp, imm(56));
  a.mov(x86::rbp, x86::gpq(gpa[0]));                   // cpu
  a.mov(x86::rbx, x86::gpq(gpa[1]));                   // regs
  a.mov(x86::qword_ptr(x86::rsp, 40), imm(0));         // chain count := 0 (reclaimed r14 -> stack slot)
  a.mov(x86::r12, x86::qword_ptr(x86::rbx, 26 * 8));   // R26 (RA) pin
  a.mov(x86::r13, x86::qword_ptr(x86::rbx, 16 * 8));   // R16 (a0) pin
  a.mov(x86::r15, x86::qword_ptr(x86::rbx, 27 * 8));   // R27 (PV) pin
  a.mov(x86::r14, x86::qword_ptr(x86::rbx, 30 * 8));   // R30 (SP) pin
#ifdef _WIN32
  a.mov(x86::rsi, x86::qword_ptr(x86::rbx, 29 * 8));   // R29 (GP) pin
  a.mov(x86::rdi, x86::qword_ptr(x86::rbx,  0 * 8));   // R0 (v0) pin
#endif

  Label done = a.new_label();   // shared side-exit/return: EAX preset to the instr count, state.pc live
  Label body = a.new_label();   // loop re-entry (after the prologue; pins + count stay live across iterations)
  a.bind(body);

  // Block register allocator: the 3 global pins (static, live across the trace). dynamic pool in future.
  RegAlloc ra;
  for (int r = 0; r < 32; ++r) ra.host[r] = -1;
  ra.rax_holds = -1;
  ra.host[kGlobalPins[0]] = (int) x86::r12.id();
  ra.host[kGlobalPins[1]] = (int) x86::r13.id();
  ra.host[kGlobalPins[2]] = (int) x86::r15.id();
  ra.host[30] = (int) x86::r14.id();                  // SP (reclaimed r14), all platforms
#ifdef _WIN32
  ra.host[29] = (int) x86::rsi.id();                  // GP (Win64)
  ra.host[0]  = (int) x86::rdi.id();                  // v0 (Win64)
#endif

  std::vector<ColdMemStub> cold;   // outlined memop slow paths, emitted after the epilogue
  for (uint32_t bi = 0; bi < n_blocks; ++bi) {
    JitBlock* b = blocks[bi];
    const uint32_t plen = b->prefix_len;
    const uint32_t* words = (const uint32_t*) (dram + b->phys);
    const bool pal_block = (b->tag & 1) != 0;

    // Default R10 + state.pc = this block's sequential next (the fall-through exit). emit_op's branch/jump
    // terminator overwrites both with its target; a fault bail writes the fault PC. For an intermediate
    // block this also makes the guard below see R10 == the sequential successor when it falls through.
    // (note: no compiled op currently READS state.pc mid-block, so default-before-emit is equivalent.)
    a.mov(x86::r10, imm(b->tag + 4 * (uint64_t) plen));
    a.mov(x86::qword_ptr(x86::rbp, m_off.state_pc), x86::r10);

    for (uint32_t i = 0; i < plen; ++i)
      emit_op(&a, gpa, &done, hs, pal_block, b, words[i], i, ra, &cold);

    a.add(x86::qword_ptr(x86::rsp, 40), imm(plen));   // count this block
    a.mov(x86::eax, x86::dword_ptr(x86::rsp, 40));    // EAX = instrs completed so far (preset for `done` -- a side-exit or return)

    if (bi + 1 < n_blocks) {
      // Guard: did this block actually flow to the next fused block? R10 = its next PC; a mismatch means
      // the path diverged from what we fused -> side-exit to the dispatcher at the real next PC (state.pc).
      a.mov(x86::rcx, imm(blocks[bi + 1]->tag));   // 64-bit tag may exceed imm32; rcx scratch (not EAX)
      a.cmp(x86::r10, x86::rcx);
      a.jne(done);
    }
  }
#ifndef JIT_VERIFY
  // Loop closure: if the last block branches back to the trace head, close the loop in compiled code with
  // the budget/interrupt gate ON the back-edge (risk #1). Verify builds omit this -> the trace runs one
  // iteration and exits at state.pc, so the side-exit verify validates the body unchanged.
  { JitBlock* lb = blocks[n_blocks - 1];
    const uint32_t* lw = (const uint32_t*) (dram + lb->phys);
    const uint32_t lop = lw[lb->prefix_len - 1], lopc = lop >> 26;
    if (lopc == 0x30 || lopc == 0x34 || (lopc >= 0x38 && lopc <= 0x3f)) {   // PC-relative branch terminator
      const int64_t disp = (int64_t) ((uint64_t) (lop & 0x1FFFFF) << 43) >> 43;
      const uint64_t tgt = (((lb->tag & ~(uint64_t) 1) + 4 * (uint64_t) (lb->prefix_len - 1)) + 4 + (uint64_t) (disp * 4)) | (lb->tag & 1);
      if (tgt == blocks[0]->tag) {   // taken target == head -> a closable loop
        a.mov(x86::rcx, imm(blocks[0]->tag));
        a.cmp(x86::r10, x86::rcx); a.jne(done);                                    // not looping now -> exit
        a.mov(x86::rax, x86::qword_ptr(x86::rsp, 40)); a.cmp(x86::rax, x86::qword_ptr(x86::rbp, m_off.jit_budget)); a.jge(done);   // budget ceiling
        a.cmp(x86::byte_ptr(x86::rbp, m_off.check_int), imm(0));     a.jne(done);   // interrupt pending
        a.cmp(x86::byte_ptr(x86::rbp, m_off.check_timers), imm(0));  a.jne(done);   // timer pending
        a.jmp(body);                                                               // loop in compiled code
      }
    }
  }
#endif
  a.bind(done);
  a.mov(x86::qword_ptr(x86::rbx, 26 * 8), x86::r12);   // sync pins back to regs[] before returning
  a.mov(x86::qword_ptr(x86::rbx, 16 * 8), x86::r13);
  a.mov(x86::qword_ptr(x86::rbx, 27 * 8), x86::r15);
  a.mov(x86::qword_ptr(x86::rbx, 30 * 8), x86::r14);   // R30 (SP)
#ifdef _WIN32
  a.mov(x86::qword_ptr(x86::rbx, 29 * 8), x86::rsi);   // R29 (GP)
  a.mov(x86::qword_ptr(x86::rbx,  0 * 8), x86::rdi);   // R0 (v0)
#endif
  a.add(x86::rsp, imm(56));
#ifdef _WIN32
  a.pop(x86::rdi); a.pop(x86::rsi);
#endif
  a.pop(x86::r15); a.pop(x86::r13); a.pop(x86::r12);
  a.pop(x86::r14); a.pop(x86::rbp); a.pop(x86::rbx);
  a.ret();

  // Cold tail: the outlined memop slow paths
  for (const ColdMemStub& s : cold)
    emit_cold_mem_stub(a, gpa, m_off, s);

  const size_t csz = code.code_size();
  JitFn fn = nullptr;
  if (((JitRuntime*) m_rt)->add(&fn, &code) != Error::kOk) return;
  uint32_t total = 0;
  for (uint32_t bi = 0; bi < n_blocks; ++bi) {
    JitBlock* b = blocks[bi];
    t->segs[bi] = { b->tag, b->phys, b->prefix_len, b->asm_global, b->asn,
                    src_hash(dram + b->phys, b->prefix_len) };
    total += b->prefix_len;
  }
  t->code       = fn;
  t->head_tag   = blocks[0]->tag;
  t->asn        = blocks[0]->asn;
  t->asm_global = blocks[0]->asm_global;
  t->valid      = true;
  t->vgen       = m_vgen_cur;
  t->flush_gen  = m_flush_gen;
  t->n_blocks   = n_blocks;
  t->n_instr    = total;
  t->n_segs     = n_blocks;
  t->n_exits    = 0;
  m_code_bytes += csz;
#ifdef JIT_STATS
  m_trace_formed++;
#endif
}

#else // !ES40_JIT_X64 -- ARM64 scaffolding: the engine's bookkeeping (record/lookup/flush/
      // coherence) compiles and runs, but no code is emitted. The dispatcher only enters
      // b->code / t->code when non-null, so every block falls back to the interpreter.

void CJitEngine::emit_op(void*, const uint8_t*, void*, const HelperSet&,
                         bool, JitBlock*, uint32_t, uint32_t, RegAlloc&, void*) {}

void CJitEngine::compile_block(JitBlock* b, const uint8_t*, uint64_t, void*, void*, void*,
    void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*,
    void*, void*, void*, void*)
{
  // Mark the attempt so record()'s hot path never re-requests a compile; code stays
  // null, so this block is interpreted permanently (no recompile churn).
  b->compiled = true;
  b->code = nullptr;
  b->jit_body = nullptr;
  b->prefix_len = 0;
}

void CJitEngine::compile_trace(TraceFragment*, JitBlock**, uint32_t,
                               const uint8_t*, uint64_t, const HelperSet&) {}

#endif // ES40_JIT_X64

#ifdef JIT_VERIFY
uint64_t CJitEngine::verify_compare(uint64_t blk_virt, const uint64_t* interp, const uint64_t* jit,
                                    const uint32_t* words, uint32_t nwords)
{
  m_v_exec++;
  for (int r = 0; r < 63; ++r) {   // r0..r30 main bank + r32..r62 PALshadow bank
    if (r == 31) continue;         // zero register
    if (interp[r] != jit[r]) {
      m_v_fail++;
      printf("[JIT][VERIFY] MISMATCH at block pc=%016llx: r%d interp=%016llx jit=%016llx\n",
             (unsigned long long) blk_virt, r,
             (unsigned long long) interp[r], (unsigned long long) jit[r]);
      printf("   words:");   // the compiled prefix, to decode which instruction diverged
      for (uint32_t w = 0; w < nwords && w < 16; ++w) printf(" %08x", words[w]);
      printf("\n");
      break;
    }
  }
  if ((m_v_exec % 500000) == 0) {
    const auto t0 = std::chrono::steady_clock::now();   // exclude this print's I/O stall from the
    printf("[JIT][VERIFY] %llu compiled-block execs, %llu mismatches\n",   // wall-clock-pinned RPCC
           (unsigned long long) m_v_exec, (unsigned long long) m_v_fail);
    return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now() - t0).count();
  }
  return 0;
}
#endif

#ifdef JIT_STATS
// Short mnemonic for the opcode that ended a block's compiled prefix (the coverage gap).
static const char* opcode_name(unsigned op)
{
  switch (op) {
    case 0x00: return "CALL_PAL";
    case 0x08: return "LDA";   case 0x09: return "LDAH"; case 0x0a: return "LDBU";  case 0x0b: return "LDQ_U";
    case 0x0c: return "LDWU";  case 0x0d: return "STW";  case 0x0e: return "STB";   case 0x0f: return "STQ_U";
    case 0x10: return "INTA";  case 0x11: return "INTL"; case 0x12: return "INTS";  case 0x13: return "INTM";
    case 0x14: return "ITFP";  case 0x15: return "FLTV"; case 0x16: return "FLTI";  case 0x17: return "FLTL";
    case 0x18: return "MISC";  case 0x19: return "HWMFPR";case 0x1a: return "JMP";   case 0x1b: return "HWLD";
    case 0x1c: return "FPTI";  case 0x1d: return "HWMTPR";case 0x1e: return "HWREI"; case 0x1f: return "HWST";
    case 0x20: return "LDF";   case 0x21: return "LDG";  case 0x22: return "LDS";   case 0x23: return "LDT";
    case 0x24: return "STF";   case 0x25: return "STG";  case 0x26: return "STS";   case 0x27: return "STT";
    case 0x2a: return "LDL_L"; case 0x2b: return "LDQ_L";case 0x2e: return "STL_C"; case 0x2f: return "STQ_C";
    default:   return "op";
  }
}

uint64_t CJitEngine::note_exec(uint32_t native_instr, uint32_t interp_instr, uint64_t comp_tsc, uint64_t interp_tsc)
{
  m_stat_native += native_instr;
  m_stat_interp += interp_instr;
  m_tsc_compiled += comp_tsc;         // host cycles spent in b->code() (compiled chains)
  m_tsc_interp   += interp_tsc;       // host cycles spent in the interp fallback loop
  if (native_instr) m_stat_hot++;     // one compiled-chain dispatch
  if (interp_instr) m_stat_miss++;    // one interpreted (cold/uncompilable) block
  const uint64_t total = m_stat_native + m_stat_interp;
  if (total < 100000000) return 0;    // report every 100M instructions

  // Time this report's own blocking I/O so the caller can exclude it from the wall-clock-pinned
  // RPCC -- else the printf stall is billed to the guest cycle counter as a forward jump.
  const auto stat_t0 = std::chrono::steady_clock::now();
  const uint64_t win_tsc = jit_rdtsc() - m_tsc_window_start;   // window's host cycles (time-split denominator)
#ifdef JIT_REGPROF
  regprof_report();   // exec-weighted GPR histogram for pin selection (I/O timed within this window)
#endif
  // Build each line in a buffer and print it with ONE call: 4 CPU threads print concurrently,
  // and per-item printf loops interleave mid-line. The [CPU%d] tag attributes each line.
  const double chain = m_stat_hot ? (double) m_stat_native / (double) m_stat_hot : 0.0;
  const double avgpl = m_stat_compiled ? (double) m_stat_plen_sum / (double) m_stat_compiled : 0.0;
  char buf[512];
  int  len;
  printf("[JIT][STATS][CPU%d] native %.1f%% (%llu/%llu) | chain avg %.1f instr over %llu dispatches | interp %llu blks\n",
         m_cpu_id, 100.0 * (double) m_stat_native / (double) total,
         (unsigned long long) m_stat_native, (unsigned long long) total,
         chain, (unsigned long long) m_stat_hot, (unsigned long long) m_stat_miss);
  // Throughput + code expansion: the in-JIT proxy for the codegen-quality diagnosis. MIPS is
  // wall-clock (clock-independent: cycles/instr = host_GHz / MIPS); x86-bytes/instr is the static
  // average emitted expansion. Read these off the WORKER CPU; the first window includes warmup.
  const uint64_t now_ns = (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
      stat_t0.time_since_epoch()).count();
  const uint64_t dt_ns  = (now_ns > m_stat_wall_last_ns) ? now_ns - m_stat_wall_last_ns : 0;
  const double   mips   = dt_ns ? (double) total * 1000.0 / (double) dt_ns : 0.0;
  const double   expand = m_stat_plen_sum ? (double) m_stat_code_bytes / (double) m_stat_plen_sum : 0.0;
  printf("[JIT][STATS][CPU%d] throughput %.0f MIPS (%llu instr / %.1f ms) | %.1f x86-bytes/instr (static avg)\n",
         m_cpu_id, mips, (unsigned long long) total, (double) dt_ns / 1e6, expand);
  // Wall-time split (host TSC): where the window actually spent time -- compiled execution vs interp
  // fallback vs the dispatcher/chaining remainder. Separates the two bottleneck candidates the
  // instruction counts can't (chain length and interp rate both just track how hot/covered the code is).
  if (win_tsc) {
    const double cf  = 100.0 * (double) m_tsc_compiled / (double) win_tsc;
    const double itf = 100.0 * (double) m_tsc_interp   / (double) win_tsc;
    printf("[JIT][STATS][CPU%d] time-split: compiled %.1f%% | interp %.1f%% | dispatch %.1f%%\n",
           m_cpu_id, cf, itf, (cf + itf < 100.0) ? (100.0 - cf - itf) : 0.0);
  }
  // Bail-cause: of the compiled-chain dispatches (m_stat_hot), how many bailed via a cached-link miss
  // (branches/fall-through) vs a jit_indirect miss (computed jumps) vs gate/other (budget/interrupt/
  // mid-block fault = remainder). Pinpoints whether the branch link is the dispatch lever.
  if (m_stat_hot) {
    const uint64_t jmp_bail = (m_jmp_attempt > m_jmp_hit) ? (m_jmp_attempt - m_jmp_hit) : 0;
    const uint64_t other = (m_stat_hot > m_bail_link + jmp_bail) ? (m_stat_hot - m_bail_link - jmp_bail) : 0;
    printf("[JIT][STATS][CPU%d] bail-cause: link %.0f%% | jump %.0f%% | gate/other %.0f%% (of %llu) | jump chain-rate %.0f%%\n",
           m_cpu_id, 100.0 * (double) m_bail_link / (double) m_stat_hot,
           100.0 * (double) jmp_bail / (double) m_stat_hot, 100.0 * (double) other / (double) m_stat_hot,
           (unsigned long long) m_stat_hot,
           m_jmp_attempt ? 100.0 * (double) m_jmp_hit / (double) m_jmp_attempt : 0.0);
  }
  // Fresh-compile reason (per window): tag=cache aliasing, asn=cross-process same-PC, phys=remap,
  // hash=self-mod, cold=genuine new/warmup. Sums to the window's `recorded` growth (the churn cost).
  const uint64_t fresh = m_fresh_tag + m_fresh_asn + m_fresh_phys + m_fresh_hash + m_fresh_cold;
  if (fresh)
    printf("[JIT][STATS][CPU%d] fresh-cause: tag %llu | asn %llu | phys %llu | hash %llu | cold %llu (of %llu recompiled)\n",
           m_cpu_id, (unsigned long long) m_fresh_tag, (unsigned long long) m_fresh_asn,
           (unsigned long long) m_fresh_phys, (unsigned long long) m_fresh_hash,
           (unsigned long long) m_fresh_cold, (unsigned long long) fresh);
  if (m_trace_formed || m_trace_entered)
    printf("[JIT][STATS][CPU%d] traces: formed %llu | entered %llu | exits %llu | stale %llu (windowed)\n",
           m_cpu_id, (unsigned long long) m_trace_formed, (unsigned long long) m_trace_entered,
           (unsigned long long) m_trace_exits, (unsigned long long) m_trace_stale);
  { uint64_t fh[6] = {0}, mh[6] = {0}, tm = 0;   // link-fanout: thrashing source blocks + cumulative misses, bucketed by #distinct successors
    for (int i = 0; i < kCacheEntries; ++i) {
      if (!m_blocks[i].valid || m_blocks[i].link_misses == 0) continue;
      int f = m_blocks[i].link_fanout > 5 ? 5 : m_blocks[i].link_fanout;
      fh[f]++; mh[f] += m_blocks[i].link_misses; tm += m_blocks[i].link_misses;
    }
    if (tm) printf("[JIT][STATS][CPU%d] link-fanout (srcs/misses by #distinct successors): f1=%llu/%llu f2=%llu/%llu f3=%llu/%llu f4=%llu/%llu f5+=%llu/%llu | total miss %llu\n",
      m_cpu_id, (unsigned long long) fh[1], (unsigned long long) mh[1], (unsigned long long) fh[2], (unsigned long long) mh[2], (unsigned long long) fh[3], (unsigned long long) mh[3], (unsigned long long) fh[4], (unsigned long long) mh[4], (unsigned long long) fh[5], (unsigned long long) mh[5], (unsigned long long) tm); }
  len = snprintf(buf, sizeof(buf), "[JIT][STATS][CPU%d] %llu recorded, %llu compiled (avg %.1f instr) | block-breakers:",
                 m_cpu_id, (unsigned long long) m_recorded, (unsigned long long) m_stat_compiled, avgpl);
  uint64_t hist[64];
  memcpy(hist, m_term_op, sizeof(hist));
  for (int rank = 0; rank < 8 && len < (int) sizeof(buf) - 32; ++rank) {
    int best = -1; uint64_t bestv = 0;
    for (int op = 0; op < 64; ++op) if (hist[op] > bestv) { bestv = hist[op]; best = op; }
    if (best < 0) break;
    len += snprintf(buf + len, sizeof(buf) - len, " %s(0x%02x)=%llu", opcode_name(best), best, (unsigned long long) bestv);
    hist[best] = 0;
  }
  printf("%s\n", buf);
  if (m_term_op[0]) {   // CALL_PAL cut blocks -- show which function codes dominate (chain targets)
    len = snprintf(buf, sizeof(buf), "[JIT][STATS][CPU%d]   CALL_PAL by func:", m_cpu_id);
    uint64_t ph[256];
    memcpy(ph, m_pal_func, sizeof(ph));
    for (int rank = 0; rank < 8 && len < (int) sizeof(buf) - 32; ++rank) {
      int best = -1; uint64_t bestv = 0;
      for (int f = 0; f < 256; ++f) if (ph[f] > bestv) { bestv = ph[f]; best = f; }
      if (best < 0) break;
      len += snprintf(buf + len, sizeof(buf) - len, " 0x%02x=%llu", best, (unsigned long long) bestv);
      ph[best] = 0;
    }
    printf("%s\n", buf);
  }
  if (m_term_op[0x1d]) {   // HW_MTPR cut blocks -- which IPR writes dominate (the side-effecting set)
    len = snprintf(buf, sizeof(buf), "[JIT][STATS][CPU%d]   HW_MTPR by IPR:", m_cpu_id);
    uint64_t mh[256];
    memcpy(mh, m_mtpr_func, sizeof(mh));
    for (int rank = 0; rank < 8 && len < (int) sizeof(buf) - 32; ++rank) {
      int best = -1; uint64_t bestv = 0;
      for (int f = 0; f < 256; ++f) if (mh[f] > bestv) { bestv = mh[f]; best = f; }
      if (best < 0) break;
      len += snprintf(buf + len, sizeof(buf) - len, " 0x%02x=%llu", best, (unsigned long long) bestv);
      mh[best] = 0;
    }
    printf("%s\n", buf);
  }
  if (m_term_op[0x1b]) {   // HW_LD cut blocks -- which forms dominate (virt/lock/vpte/chk)
    len = snprintf(buf, sizeof(buf), "[JIT][STATS][CPU%d]   HW_LD by form:", m_cpu_id);
    uint64_t lh[16];
    memcpy(lh, m_hwld_func, sizeof(lh));
    for (int rank = 0; rank < 8 && len < (int) sizeof(buf) - 32; ++rank) {
      int best = -1; uint64_t bestv = 0;
      for (int f = 0; f < 16; ++f) if (lh[f] > bestv) { bestv = lh[f]; best = f; }
      if (best < 0) break;
      len += snprintf(buf + len, sizeof(buf) - len, " 0x%x=%llu", best, (unsigned long long) bestv);
      lh[best] = 0;
    }
    printf("%s\n", buf);
  }
  if (m_term_op[0x18]) {   // MISC cut blocks -- which Ra==31 form dominates (RPCC degenerate-noop / RC,RS soft-intr-flag)
    printf("[JIT][STATS][CPU%d]   MISC by form: RPCC=%llu RC=%llu RS=%llu\n", m_cpu_id,
           (unsigned long long) m_misc_func[0xc], (unsigned long long) m_misc_func[0xe],
           (unsigned long long) m_misc_func[0xf]);
  }
  m_stat_native = m_stat_interp = m_stat_hot = m_stat_miss = 0;   // reset the window
  m_tsc_compiled = m_tsc_interp = 0;
  m_tsc_window_start = jit_rdtsc();   // next window's split denominator starts after this report's I/O
  m_bail_link = m_jmp_attempt = m_jmp_hit = 0;
  m_fresh_cold = m_fresh_tag = m_fresh_asn = m_fresh_phys = m_fresh_hash = 0;
  m_trace_formed = m_trace_entered = m_trace_exits = m_trace_stale = 0;
  const auto stat_end = std::chrono::steady_clock::now();
  m_stat_wall_last_ns = (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
      stat_end.time_since_epoch()).count();   // next window's throughput delta starts after this I/O
  return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(stat_end - stat_t0).count();
}
#endif

#ifdef JIT_REGPROF
// Pin-selection report: attribute each live block's executions (rp_hits) to every GPR it touches
// (rp_mask), then print the heaviest. A trailing '*' marks regs that CANNOT be pinned -- R4-7/R20-23,
// the PALshadow remap targets (which also covers R23's CALL_PAL save). R31 is excluded by the mask.
void CJitEngine::regprof_report()
{
  uint64_t hist[32] = { 0 };
  uint64_t exec_instr = 0, exec_bytes = 0;   // exec-weighted: hot-path Alpha instrs and emitted x86 bytes
  for (int s = 0; s < kCacheEntries; ++s) {
    const JitBlock& b = m_blocks[s];
    if (!b.valid || b.rp_hits == 0) continue;
    for (int r = 0; r < 31; ++r) if (b.rp_mask & (1u << r)) hist[r] += b.rp_hits;
    exec_instr += b.rp_hits * (uint64_t) b.prefix_len;
    exec_bytes += b.rp_hits * (uint64_t) b.rp_csz;
  }
  // Execution-weighted code expansion -- the HOT path, not the cold-block-skewed static average.
  // x86-instrs/instr ~= this / ~3.5; with cycles/instr from the throughput line -> hot-path IPC.
  printf("[JIT][REGPROF][CPU%d] exec-weighted expansion: %.1f x86-bytes/instr (hot path)\n",
         m_cpu_id, exec_instr ? (double) exec_bytes / (double) exec_instr : 0.0);
  char buf[256];
  int  len = snprintf(buf, sizeof(buf), "[JIT][REGPROF][CPU%d] hot GPRs (exec x accesses):", m_cpu_id);
  for (int rank = 0; rank < 8 && len < (int) sizeof(buf) - 24; ++rank) {
    int best = -1; uint64_t bestv = 0;
    for (int r = 0; r < 31; ++r) if (hist[r] > bestv) { bestv = hist[r]; best = r; }
    if (best < 0) break;
    const bool pinnable = (best & 0xc) != 0x4;   // exclude R4-7 / R20-23 (shadow remap; covers R23)
    len += snprintf(buf + len, sizeof(buf) - len, " R%d=%llu%s", best, (unsigned long long) bestv, pinnable ? "" : "*");
    hist[best] = 0;
  }
  printf("%s   (* = not pin-eligible)\n", buf);
}
#endif

#endif // ES40_JIT
