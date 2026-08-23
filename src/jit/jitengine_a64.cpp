/* ES40 emulator -- JIT engine: the ARM64 (AArch64) codegen backend.
 * Copyright (C) 2026 by the ES40 Emulator Project
 * All rights reserved.
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * SPDX-License-Identifier: BSD-1-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS AND CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef ES40_JIT

#include "jitengine.h"        // defines ES40_JIT_X64 / ES40_JIT_A64 from the host arch
#include "jitengine_internal.h"

#ifdef ES40_JIT_A64

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <initializer_list>
#include <vector>
#define ASMJIT_STATIC
#include <asmjit/a64.h>

// register convention for both block and trace codegen:
//   x9 = transient expansion scratch; x19 = CAlphaCPU*, x20 = state.r[] base,
//   x21 = completed-instruction count, x22 = next guest PC / chain target,
//   x23-x28 = future guest-GPR pin bank.
struct CJitEngine::RegAlloc {
  static constexpr asmjit::a64::Gp kArgCpu = asmjit::a64::x0;
  static constexpr asmjit::a64::Gp kArgRegs = asmjit::a64::x1;
  static constexpr asmjit::a64::Gp kResultCount = asmjit::a64::w0;
  static constexpr asmjit::a64::Gp kScratch0 = asmjit::a64::x9;

  static constexpr asmjit::a64::Gp kCpu = asmjit::a64::x19;
  static constexpr asmjit::a64::Gp kRegs = asmjit::a64::x20;
  static constexpr asmjit::a64::Gp kChainCount = asmjit::a64::x21;
  static constexpr asmjit::a64::Gp kNextPc = asmjit::a64::x22;
  static constexpr asmjit::a64::Gp kGuestPin0 = asmjit::a64::x23;
  static constexpr asmjit::a64::Gp kGuestPin1 = asmjit::a64::x24;
  static constexpr asmjit::a64::Gp kGuestPin2 = asmjit::a64::x25;
  static constexpr asmjit::a64::Gp kGuestPin3 = asmjit::a64::x26;
  static constexpr asmjit::a64::Gp kGuestPin4 = asmjit::a64::x27;
  static constexpr asmjit::a64::Gp kGuestPin5 = asmjit::a64::x28;

  static constexpr asmjit::RegMask kPersistentGpMask =
      asmjit::Support::bit_mask<asmjit::RegMask>(
          kCpu.id(), kRegs.id(), kChainCount.id(), kNextPc.id(),
          kGuestPin0.id(), kGuestPin1.id(), kGuestPin2.id(),
          kGuestPin3.id(), kGuestPin4.id(), kGuestPin5.id());
};

static_assert((CJitEngine::RegAlloc::kPersistentGpMask
               & asmjit::Support::bit_mask<asmjit::RegMask>(8, 16, 17,
                     asmjit::a64::Gp::kIdOs, 29, 30, 31)) == 0,
              "A64 persistent registers must exclude ABI, linker, platform, frame, link, and stack registers");
static_assert((CJitEngine::RegAlloc::kPersistentGpMask
               & asmjit::Support::bit_mask<asmjit::RegMask>(
                     CJitEngine::RegAlloc::kScratch0.id())) == 0,
              "A64 expansion scratch must not overlap persistent registers");

namespace {

enum class A64OpKind : uint8_t {
  kUnsupported
};

enum class A64TerminatorKind : uint8_t {
  kNone,
  kDirect,
  kIndirect
};

enum class A64PlanStop : uint8_t {
  kInvalidSource,
  kEmptyBlock,
  kUnsupported,
  kBlockEnd,
  kPageBoundary,
  kInstructionLimit,
  kTerminator
};

struct A64OpClass {
  A64OpKind kind = A64OpKind::kUnsupported;
  A64TerminatorKind terminator = A64TerminatorKind::kNone;
};

struct A64DecodedOp {
  uint32_t ins = 0;
  A64OpKind kind = A64OpKind::kUnsupported;
  A64TerminatorKind terminator = A64TerminatorKind::kNone;
  uint8_t opcode = 0;
  uint8_t ra = 0;
  uint8_t rb = 0;
  uint8_t rc = 0;
  uint8_t literal = 0;
  bool is_literal = false;
};

static_assert(sizeof(A64DecodedOp) <= 16,
              "A64 block plans must keep their fixed instruction snapshot compact");

struct A64BlockPlan {
  static constexpr uint32_t kMaxOps = 64;

  std::array<A64DecodedOp, kMaxOps> ops{};
  uint32_t count = 0;
  uint32_t breaker_word = 0;
  A64PlanStop stop = A64PlanStop::kInvalidSource;
  A64TerminatorKind terminator = A64TerminatorKind::kNone;

  constexpr bool has_prefix() const noexcept { return count != 0; }
};

struct A64ScanLimit {
  uint32_t count;
  A64PlanStop stop;
};

// Classification stays fail-closed: each translation will add its classifier and emitter together.
static constexpr A64OpClass classify_a64_op(uint32_t, bool) noexcept
{
  return {};
}

static constexpr A64DecodedOp decode_a64_op(uint32_t ins, bool pal_block) noexcept
{
  const A64OpClass classification = classify_a64_op(ins, pal_block);
  A64DecodedOp decoded{};
  decoded.ins = ins;
  decoded.kind = classification.kind;
  decoded.terminator = classification.terminator;
  decoded.opcode = static_cast<uint8_t>(ins >> 26);
  decoded.ra = static_cast<uint8_t>((ins >> 21) & 31u);
  decoded.rb = static_cast<uint8_t>((ins >> 16) & 31u);
  decoded.rc = static_cast<uint8_t>(ins & 31u);
  decoded.literal = static_cast<uint8_t>((ins >> 13) & 0xffu);
  decoded.is_literal = ((ins >> 12) & 1u) != 0;
  return decoded;
}

static constexpr A64ScanLimit a64_scan_limit(uint32_t n_instr, uint64_t phys) noexcept
{
  constexpr uint32_t kGuestPageBytes = 0x2000;
  A64ScanLimit limit{n_instr, A64PlanStop::kBlockEnd};
  if (limit.count > A64BlockPlan::kMaxOps) {
    limit.count = A64BlockPlan::kMaxOps;
    limit.stop = A64PlanStop::kInstructionLimit;
  }

  const uint32_t page_offset = static_cast<uint32_t>(phys & (kGuestPageBytes - 1));
  const uint32_t page_words = (kGuestPageBytes - page_offset) / sizeof(uint32_t);
  if (limit.count > page_words) {
    limit.count = page_words;
    limit.stop = A64PlanStop::kPageBoundary;
  }
  return limit;
}

static uint32_t load_a64_guest_u32(const uint8_t* source) noexcept
{
  return static_cast<uint32_t>(source[0])
      | (static_cast<uint32_t>(source[1]) << 8)
      | (static_cast<uint32_t>(source[2]) << 16)
      | (static_cast<uint32_t>(source[3]) << 24);
}

static A64BlockPlan plan_a64_block(const CJitEngine::JitBlock& block,
    const uint8_t* dram, uint64_t dram_size) noexcept
{
  A64BlockPlan plan{};
  if (block.n_instr == 0) {
    plan.stop = A64PlanStop::kEmptyBlock;
    return plan;
  }
  if (dram == nullptr || (block.phys & 3u) != 0 || block.phys > dram_size
      || static_cast<uint64_t>(block.n_instr) > (dram_size - block.phys) / sizeof(uint32_t))
    return plan;

  const A64ScanLimit limit = a64_scan_limit(block.n_instr, block.phys);
  plan.stop = limit.stop;
  const uint8_t* const source = dram + static_cast<size_t>(block.phys);
  const bool pal_block = (block.tag & 1u) != 0;
  for (uint32_t i = 0; i < limit.count; ++i) {
    const uint32_t word = load_a64_guest_u32(source + static_cast<size_t>(i) * 4);
    const A64DecodedOp decoded = decode_a64_op(word, pal_block);
    if (decoded.kind == A64OpKind::kUnsupported) {
      plan.breaker_word = word;
      plan.stop = A64PlanStop::kUnsupported;
      return plan;
    }

    plan.ops[plan.count++] = decoded;
    if (decoded.terminator != A64TerminatorKind::kNone) {
      plan.terminator = decoded.terminator;
      plan.stop = A64PlanStop::kTerminator;
      return plan;
    }
  }
  return plan;
}

static constexpr uint32_t kA64DecodeProbe =
    (0x10u << 26) | (17u << 21) | (9u << 16) | 29u;
static constexpr uint32_t kA64LiteralProbe =
    (0x11u << 26) | (4u << 21) | (0xa5u << 13) | (1u << 12) | 7u;
static_assert(decode_a64_op(kA64DecodeProbe, false).opcode == 0x10
              && decode_a64_op(kA64DecodeProbe, false).ra == 17
              && decode_a64_op(kA64DecodeProbe, false).rb == 9
              && decode_a64_op(kA64DecodeProbe, false).rc == 29
              && !decode_a64_op(kA64DecodeProbe, false).is_literal,
              "A64 planning must decode Alpha register-form operands once");
static_assert(decode_a64_op(kA64LiteralProbe, false).is_literal
              && decode_a64_op(kA64LiteralProbe, false).literal == 0xa5,
              "A64 planning must decode Alpha literal operands once");
static_assert(a64_scan_limit(3, 0).count == 3
              && a64_scan_limit(3, 0).stop == A64PlanStop::kBlockEnd
              && a64_scan_limit(65, 0).count == 64
              && a64_scan_limit(65, 0).stop == A64PlanStop::kInstructionLimit
              && a64_scan_limit(3, 0x1ff8).count == 2
              && a64_scan_limit(3, 0x1ff8).stop == A64PlanStop::kPageBoundary
              && a64_scan_limit(1, 0x2000).count == 1,
              "A64 planning must honor block, instruction, and physical-page bounds");

static constexpr uint32_t a64_guest_gpr_slot(uint32_t raw_reg, bool pal_shadow) noexcept
{
  const uint32_t reg = raw_reg & 31u;
  return reg + ((pal_shadow && (reg & 0x0cu) == 0x04u) ? 32u : 0u);
}

static constexpr asmjit::a64::Mem a64_guest_gpr_mem(uint32_t raw_reg,
                                                     bool pal_shadow) noexcept
{
  return asmjit::a64::ptr(CJitEngine::RegAlloc::kRegs,
      static_cast<int32_t>(a64_guest_gpr_slot(raw_reg, pal_shadow) * sizeof(uint64_t)));
}

static_assert(a64_guest_gpr_slot(3, true) == 3
              && a64_guest_gpr_slot(4, true) == 36
              && a64_guest_gpr_slot(23, true) == 55
              && a64_guest_gpr_slot(24, true) == 24
              && a64_guest_gpr_slot(23, false) == 23,
              "A64 guest operands must match the PAL-shadow register mapping");
static_assert(a64_guest_gpr_mem(23, true).base_id() == CJitEngine::RegAlloc::kRegs.id()
              && a64_guest_gpr_mem(23, true).offset() == 55 * 8,
              "A64 guest GPR operands must be fixed offsets from the register-bank base");

static bool a64_is_real_x(const asmjit::a64::Gp& reg) noexcept
{
  return reg.is_gp64() && reg.id() < asmjit::a64::Gp::kIdSp;
}

static bool a64_is_address_x(const asmjit::a64::Gp& reg) noexcept
{
  return reg.is_gp64() && reg.id() <= asmjit::a64::Gp::kIdSp;
}

// AsmJit's MOV pseudo-op selects a logical immediate or the shortest MOVZ/MOVN/MOVK sequence.
static asmjit::Error emit_a64_mov_u64(asmjit::a64::Assembler& a,
                                      const asmjit::a64::Gp& dst, uint64_t value)
{
  if (!a64_is_real_x(dst)) return asmjit::Error::kInvalidArgument;
  return a.mov(dst, asmjit::imm(value));
}

static asmjit::Error emit_a64_add_offset(asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& dst, const asmjit::a64::Gp& base, int64_t delta,
    const asmjit::a64::Gp& scratch = CJitEngine::RegAlloc::kScratch0)
{
  using namespace asmjit;
  if (!a64_is_address_x(dst) || !a64_is_address_x(base)) return Error::kInvalidArgument;
  if (delta == 0)
    return dst.id() == base.id() ? Error::kOk : a.mov(dst, base);

  const bool negative = delta < 0;
  const uint64_t magnitude = negative ? uint64_t(0) - uint64_t(delta) : uint64_t(delta);
  auto emit_part = [&](const a64::Gp& part_dst, const a64::Gp& part_base,
                       uint64_t part) -> Error {
    return negative ? a.sub(part_dst, part_base, imm(part))
                    : a.add(part_dst, part_base, imm(part));
  };

  if (arm::Utils::is_add_sub_imm(magnitude))
    return emit_part(dst, base, magnitude);

  // Two immediate instructions cover every 24-bit magnitude without consuming scratch.
  if (magnitude <= 0x00ffffffu) {
    Error err = emit_part(dst, base, magnitude & 0xfffu);
    return err != Error::kOk ? err : emit_part(dst, dst, magnitude & 0xfff000u);
  }

  if (!a64_is_real_x(scratch) || scratch.id() == base.id())
    return Error::kInvalidArgument;
  Error err = emit_a64_mov_u64(a, scratch, magnitude);
  if (err != Error::kOk) return err;
  return negative ? a.sub(dst, base, scratch) : a.add(dst, base, scratch);
}

struct A64FrameLayout {
  static constexpr int32_t kCpuRegs = 16;
  static constexpr int32_t kCountPc = 32;
  static constexpr int32_t kPin01 = 48;
  static constexpr int32_t kPin23 = 64;
  static constexpr int32_t kPin45 = 80;
  static constexpr int32_t kHelperOut = 96;
  static constexpr int32_t kHelperScratch = 104;
  static constexpr int32_t kSize = 112;
};

static_assert(A64FrameLayout::kSize % 16 == 0,
              "A64 JIT frame must preserve 16-byte stack alignment");
static_assert(A64FrameLayout::kHelperScratch + 8 == A64FrameLayout::kSize,
              "A64 helper locals must fit inside the fixed frame");

static asmjit::Error emit_a64_prologue(asmjit::a64::Assembler& a)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;
  using namespace asmjit::a64;

  Error err = a.stp(x29, x30, ptr_pre(sp, -A64FrameLayout::kSize));
  if (err != Error::kOk) return err;
  err = emit_a64_add_offset(a, x29, sp, 0);
  if (err != Error::kOk) return err;
  err = a.stp(RA::kCpu, RA::kRegs, ptr(sp, A64FrameLayout::kCpuRegs));
  if (err != Error::kOk) return err;
  err = a.stp(RA::kChainCount, RA::kNextPc, ptr(sp, A64FrameLayout::kCountPc));
  if (err != Error::kOk) return err;
  err = a.stp(RA::kGuestPin0, RA::kGuestPin1, ptr(sp, A64FrameLayout::kPin01));
  if (err != Error::kOk) return err;
  err = a.stp(RA::kGuestPin2, RA::kGuestPin3, ptr(sp, A64FrameLayout::kPin23));
  if (err != Error::kOk) return err;
  err = a.stp(RA::kGuestPin4, RA::kGuestPin5, ptr(sp, A64FrameLayout::kPin45));
  if (err != Error::kOk) return err;

  err = a.mov(RA::kCpu, RA::kArgCpu);
  if (err != Error::kOk) return err;
  err = a.mov(RA::kRegs, RA::kArgRegs);
  if (err != Error::kOk) return err;
  return emit_a64_mov_u64(a, RA::kChainCount, 0);
}

static asmjit::Error emit_a64_epilogue(asmjit::a64::Assembler& a)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;
  using namespace asmjit::a64;

  Error err = a.ldp(RA::kGuestPin4, RA::kGuestPin5, ptr(sp, A64FrameLayout::kPin45));
  if (err != Error::kOk) return err;
  err = a.ldp(RA::kGuestPin2, RA::kGuestPin3, ptr(sp, A64FrameLayout::kPin23));
  if (err != Error::kOk) return err;
  err = a.ldp(RA::kGuestPin0, RA::kGuestPin1, ptr(sp, A64FrameLayout::kPin01));
  if (err != Error::kOk) return err;
  err = a.ldp(RA::kChainCount, RA::kNextPc, ptr(sp, A64FrameLayout::kCountPc));
  if (err != Error::kOk) return err;
  err = a.ldp(RA::kCpu, RA::kRegs, ptr(sp, A64FrameLayout::kCpuRegs));
  if (err != Error::kOk) return err;
  err = a.ldp(x29, x30, ptr_post(sp, A64FrameLayout::kSize));
  if (err != Error::kOk) return err;
  return a.ret(x30);
}

#ifndef NDEBUG
static bool a64_abi_compatible(const asmjit::Environment& environment)
{
  using namespace asmjit;
  if (!environment.is_family_aarch64()) return false;

  CallConv cc;
  if (cc.init(CallConvId::kCDecl, environment) != Error::kOk) return false;
  const uint8_t* const gp_args = cc.passed_order(RegGroup::kGp);
  if (gp_args[0] != CJitEngine::RegAlloc::kArgCpu.id()
      || gp_args[1] != CJitEngine::RegAlloc::kArgRegs.id()) return false;
  for (uint32_t i = 2; i < 8; ++i) if (gp_args[i] != i) return false;

  const RegMask preserved = cc.preserved_regs(RegGroup::kGp);
  return (preserved & CJitEngine::RegAlloc::kPersistentGpMask)
          == CJitEngine::RegAlloc::kPersistentGpMask
      && (preserved & Support::bit_mask<RegMask>(CJitEngine::RegAlloc::kScratch0.id())) == 0
      && cc.natural_stack_alignment() == 16;
}
#endif

} // namespace

void CJitEngine::emit_op(void*, const uint8_t*, void*, const HelperSet&,
                         bool, JitBlock*, uint32_t, uint32_t, RegAlloc&, void*, bool) {}

void CJitEngine::compile_block(JitBlock* b, const uint8_t* dram, uint64_t dram_size, void*, void*, void*,
    void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*,
    void*, void*, void*, void*)
{
  // Mark the attempt first so every setup failure leaves a permanent interpreter block.
  b->compiled = true;
  b->code = nullptr;
  b->jit_body = nullptr;
  b->prefix_len = 0;

  const A64BlockPlan plan = plan_a64_block(*b, dram, dram_size);
  // Avoid even dry-run codegen until a classifier and emitter agree on a supported prefix.
  if (!plan.has_prefix()) return;

  if (m_rt == nullptr) return;
  auto* const rt = static_cast<asmjit::JitRuntime*>(m_rt);
#ifndef NDEBUG
  static const bool abi_compatible =
      a64_abi_compatible(rt->environment());
  assert(abi_compatible);
#endif

  // Build a complete fixed frame, but deliberately discard the generated image.
  asmjit::CodeHolder code;
  if (code.init(rt->environment(), rt->cpu_features()) != asmjit::Error::kOk) return;

  asmjit::a64::Assembler a;
  if (code.attach(&a) != asmjit::Error::kOk) return;
#ifndef NDEBUG
  a.add_diagnostic_options(asmjit::DiagnosticOptions::kValidateAssembler);
#endif
  assert(a.is_initialized());

  if (emit_a64_prologue(a) != asmjit::Error::kOk) return;

  asmjit::Label done = a.new_label();
  asmjit::Label body = a.new_label();
  if (a.bind(body) != asmjit::Error::kOk) return;
  [[maybe_unused]] const size_t body_off = code.code_size();

  // Empty translated body: a cold call would return zero completed instructions.
  if (a.mov(RegAlloc::kResultCount, RegAlloc::kChainCount.w()) != asmjit::Error::kOk) return;
  if (a.bind(done) != asmjit::Error::kOk) return;
  if (emit_a64_epilogue(a) != asmjit::Error::kOk) return;
  if (a.finalize() != asmjit::Error::kOk) return;

  assert(body_off == 40);
  assert(code.code_size() == 72);
}

void CJitEngine::compile_trace(TraceFragment*, JitBlock**, uint32_t,
                               const uint8_t*, uint64_t, const HelperSet&) {}

#endif // ES40_JIT_A64

#endif // ES40_JIT

// Keeps this translation unit non-empty when ARM64 is not the build host (MSVC LNK4221).
extern const char jit_a64_backend_tu;
const char jit_a64_backend_tu = 0;
