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
//   x23-x28 = guest-GPR pin bank.
struct CJitEngine::RegAlloc {
  static constexpr uint32_t kGuestPinCount = 6;

  static constexpr asmjit::a64::Gp kArgCpu = asmjit::a64::x0;
  static constexpr asmjit::a64::Gp kArgRegs = asmjit::a64::x1;
  static constexpr asmjit::a64::Gp kResultCount = asmjit::a64::w0;
  static constexpr asmjit::a64::Gp kScratch0 = asmjit::a64::x9;
  static constexpr asmjit::a64::Gp kCallTarget = asmjit::a64::x16;

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

  // Bindings are explicit and ordered: entry i maps to host x23+i. Keeping the
  // policy out of this table lets blocks and traces share the same routing machinery.
  std::array<uint8_t, kGuestPinCount> guest_by_pin{};
  uint8_t pin_count = 0;

  static constexpr bool guest_is_shadowable(uint32_t raw_guest) noexcept {
    return raw_guest < 32 && (raw_guest & 0x0cu) == 0x04u;
  }

  // Dynamic bindings exclude variant-dependent slots. Fixed block bindings may use
  // bind_main_guest() for an explicitly main-bank value that PAL-shadow routing bypasses.
  static constexpr bool guest_pin_candidate(uint32_t raw_guest) noexcept {
    return raw_guest < 31 && !guest_is_shadowable(raw_guest);
  }

  constexpr bool bind_main_guest(uint32_t raw_guest) noexcept {
    if (raw_guest >= 31 || pin_count >= kGuestPinCount
        || pin_index_of(raw_guest) >= 0) return false;
    guest_by_pin[pin_count++] = static_cast<uint8_t>(raw_guest);
    return true;
  }

  constexpr bool bind_guest(uint32_t raw_guest) noexcept {
    return guest_pin_candidate(raw_guest) && bind_main_guest(raw_guest);
  }

  constexpr int pin_index_of(uint32_t raw_guest) const noexcept {
    if (raw_guest >= 32) return -1;
    for (uint32_t i = 0; i < pin_count; ++i)
      if (guest_by_pin[i] == raw_guest) return static_cast<int>(i);
    return -1;
  }

  constexpr int guest_of_pin(uint32_t pin_index) const noexcept {
    return pin_index < pin_count ? guest_by_pin[pin_index] : -1;
  }

  constexpr int host_of(uint32_t raw_guest, bool pal_shadow) const noexcept {
    if (raw_guest >= 31 || (pal_shadow && guest_is_shadowable(raw_guest))) return -1;
    const int pin_index = pin_index_of(raw_guest);
    return pin_index < 0 ? -1 : static_cast<int>(kGuestPin0.id()) + pin_index;
  }
};

static_assert(CJitEngine::RegAlloc::kGuestPin1.id()
                  == CJitEngine::RegAlloc::kGuestPin0.id() + 1
              && CJitEngine::RegAlloc::kGuestPin2.id()
                  == CJitEngine::RegAlloc::kGuestPin0.id() + 2
              && CJitEngine::RegAlloc::kGuestPin3.id()
                  == CJitEngine::RegAlloc::kGuestPin0.id() + 3
              && CJitEngine::RegAlloc::kGuestPin4.id()
                  == CJitEngine::RegAlloc::kGuestPin0.id() + 4
              && CJitEngine::RegAlloc::kGuestPin5.id()
                  == CJitEngine::RegAlloc::kGuestPin0.id() + 5,
              "A64 guest pin indices must map contiguously to x23-x28");

static_assert((CJitEngine::RegAlloc::kPersistentGpMask
               & asmjit::Support::bit_mask<asmjit::RegMask>(8, 16, 17,
                     asmjit::a64::Gp::kIdOs, 29, 30, 31)) == 0,
              "A64 persistent registers must exclude ABI, linker, platform, frame, link, and stack registers");
static_assert((CJitEngine::RegAlloc::kPersistentGpMask
               & asmjit::Support::bit_mask<asmjit::RegMask>(
                     CJitEngine::RegAlloc::kScratch0.id())) == 0,
              "A64 expansion scratch must not overlap persistent registers");

namespace {

// Every block body shares this exact mapping so a chained entry can inherit the
// live pin bank without an adapter. R22/R23 name the main-bank slots; PAL-shadow
// accesses to those architectural registers continue to route through memory.
static constexpr std::array<uint8_t, CJitEngine::RegAlloc::kGuestPinCount>
    kA64BlockGuestPins{{1, 16, 30, 22, 23, 27}};

static constexpr CJitEngine::RegAlloc make_a64_block_regalloc() noexcept
{
  CJitEngine::RegAlloc regs{};
  for (const uint8_t guest : kA64BlockGuestPins)
    if (!regs.bind_main_guest(guest)) return {};
  return regs;
}

enum A64GprOperandMask : uint8_t {
  kA64GprRa = 1u << 0,
  kA64GprRb = 1u << 1,
  kA64GprRc = 1u << 2
};

enum class A64OpKind : uint8_t {
  kUnsupported
};

enum class A64TerminatorKind : uint8_t {
  kNone,
  kDirect,
  kIndirect,
  kCallPal,
  kRedispatch
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
  uint8_t gpr_reads = 0;
  uint8_t gpr_writes = 0;
};

struct A64DecodedOp {
  uint32_t ins = 0;
  A64OpKind kind = A64OpKind::kUnsupported;
  A64TerminatorKind terminator = A64TerminatorKind::kNone;
  uint8_t gpr_reads = 0;
  uint8_t gpr_writes = 0;
  uint8_t opcode = 0;
  uint8_t ra = 0;
  uint8_t rb = 0;
  uint8_t rc = 0;
  uint8_t literal = 0;
  bool is_literal = false;
};

static_assert(sizeof(A64DecodedOp) <= 16,
              "A64 block plans must keep their fixed instruction snapshot compact");

struct A64GprUsage {
  std::array<uint16_t, 32> refs{};
  uint32_t reads = 0;
  uint32_t writes = 0;
};

static constexpr void note_a64_gpr(A64GprUsage& usage, uint32_t raw_guest,
                                   bool write) noexcept
{
  if (raw_guest >= 31) return;
  ++usage.refs[raw_guest];
  if (write) usage.writes |= 1u << raw_guest;
  else       usage.reads  |= 1u << raw_guest;
}

static constexpr void note_a64_op_gprs(A64GprUsage& usage,
                                       const A64DecodedOp& op) noexcept
{
  if (op.gpr_reads & kA64GprRa) note_a64_gpr(usage, op.ra, false);
  if (op.gpr_reads & kA64GprRb) note_a64_gpr(usage, op.rb, false);
  if (op.gpr_reads & kA64GprRc) note_a64_gpr(usage, op.rc, false);
  if (op.gpr_writes & kA64GprRa) note_a64_gpr(usage, op.ra, true);
  if (op.gpr_writes & kA64GprRb) note_a64_gpr(usage, op.rb, true);
  if (op.gpr_writes & kA64GprRc) note_a64_gpr(usage, op.rc, true);
}

static constexpr uint64_t a64_advance_pc(uint64_t pc, uint32_t n_instr) noexcept
{
  // Alpha PC arithmetic wraps modulo 2^64; adding whole words preserves its mode bits.
  return pc + static_cast<uint64_t>(n_instr) * sizeof(uint32_t);
}

static constexpr int64_t a64_branch_displacement(uint32_t ins) noexcept
{
  const uint32_t raw = ins & 0x1fffffu;
  return (raw & 0x100000u) != 0
      ? static_cast<int64_t>(raw) - 0x200000
      : static_cast<int64_t>(raw);
}

static constexpr uint64_t a64_branch_target(uint64_t fallthrough_pc,
                                            uint32_t ins) noexcept
{
  const int64_t byte_disp = a64_branch_displacement(ins) * 4;
  return fallthrough_pc + static_cast<uint64_t>(byte_disp);
}

static constexpr uint64_t a64_jmp_target(uint64_t source_pc, uint64_t rb) noexcept
{
  return (rb & ~uint64_t(3)) | (source_pc & 3u);
}

static constexpr uint64_t a64_hw_ret_target(uint64_t rb) noexcept
{
  return rb & ~uint64_t(2);
}

enum class A64ExitKind : uint8_t {
  kNone,
  kFallthrough,
  kDirect,
  kIndirect,
  kCallPal,
  kRedispatch
};

enum class A64PcAuthority : uint8_t {
  kNextPc,
  kCpuState
};

struct A64BlockExit {
  // Deltas are added exactly once to x21, which carries prior chained-block progress.
  uint64_t fallthrough_pc = 0;
  uint32_t completed_delta = 0;
  A64ExitKind kind = A64ExitKind::kNone;

  constexpr bool valid() const noexcept { return kind != A64ExitKind::kNone; }
};

struct A64OpExit {
  uint64_t next_pc = 0;
  uint32_t completed_delta = 0;
  A64PcAuthority pc_authority = A64PcAuthority::kNextPc;
};

static constexpr A64BlockExit plan_a64_exit(uint64_t start_pc, uint32_t count,
                                            A64TerminatorKind terminator) noexcept
{
  if (count == 0) return {start_pc, 0, A64ExitKind::kNone};

  A64ExitKind kind = A64ExitKind::kFallthrough;
  switch (terminator) {
    case A64TerminatorKind::kDirect:     kind = A64ExitKind::kDirect; break;
    case A64TerminatorKind::kIndirect:   kind = A64ExitKind::kIndirect; break;
    case A64TerminatorKind::kCallPal:    kind = A64ExitKind::kCallPal; break;
    case A64TerminatorKind::kRedispatch: kind = A64ExitKind::kRedispatch; break;
    case A64TerminatorKind::kNone:       break;
  }
  return {a64_advance_pc(start_pc, count), count, kind};
}

static constexpr A64OpExit a64_retry_exit(uint64_t start_pc,
                                          uint32_t op_index) noexcept
{
  return {a64_advance_pc(start_pc, op_index), op_index, A64PcAuthority::kNextPc};
}

static constexpr A64OpExit a64_completed_trap_exit(uint32_t op_index) noexcept
{
  return {0, op_index + 1, A64PcAuthority::kCpuState};
}

static constexpr bool a64_publish_pc_before_chain(const A64BlockExit& exit,
                                                  bool pal_block) noexcept
{
  return pal_block || exit.kind == A64ExitKind::kCallPal
      || exit.kind == A64ExitKind::kRedispatch;
}

struct A64BlockPlan {
  static constexpr uint32_t kMaxOps = 64;

  std::array<A64DecodedOp, kMaxOps> ops{};
  A64GprUsage gpr_usage{};
  uint32_t count = 0;
  uint32_t breaker_word = 0;
  A64PlanStop stop = A64PlanStop::kInvalidSource;
  A64TerminatorKind terminator = A64TerminatorKind::kNone;
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
  decoded.gpr_reads = classification.gpr_reads;
  decoded.gpr_writes = classification.gpr_writes;
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
    note_a64_op_gprs(plan.gpr_usage, decoded);
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

static constexpr bool a64_exit_contract_probe() noexcept
{
  const A64BlockExit none = plan_a64_exit(0x1001, 0, A64TerminatorKind::kNone);
  const A64BlockExit fall = plan_a64_exit(0x1001, 3, A64TerminatorKind::kNone);
  const A64BlockExit direct = plan_a64_exit(0x1001, 2, A64TerminatorKind::kDirect);
  const A64BlockExit indirect = plan_a64_exit(0x1001, 2, A64TerminatorKind::kIndirect);
  const A64BlockExit call_pal = plan_a64_exit(0x1000, 1, A64TerminatorKind::kCallPal);
  const A64BlockExit redispatch =
      plan_a64_exit(0x1001, 1, A64TerminatorKind::kRedispatch);
  const A64OpExit retry = a64_retry_exit(0x1001, 63);
  const A64OpExit trapped = a64_completed_trap_exit(63);

  return !none.valid() && none.completed_delta == 0 && none.fallthrough_pc == 0x1001
      && fall.kind == A64ExitKind::kFallthrough
      && fall.completed_delta == 3 && fall.fallthrough_pc == 0x100d
      && direct.kind == A64ExitKind::kDirect
      && indirect.kind == A64ExitKind::kIndirect
      && call_pal.kind == A64ExitKind::kCallPal
      && redispatch.kind == A64ExitKind::kRedispatch
      && retry.completed_delta == 63 && retry.next_pc == 0x10fd
      && retry.pc_authority == A64PcAuthority::kNextPc
      && trapped.completed_delta == 64
      && trapped.pc_authority == A64PcAuthority::kCpuState
      && !a64_publish_pc_before_chain(fall, false)
      && a64_publish_pc_before_chain(fall, true)
      && a64_publish_pc_before_chain(call_pal, false)
      && a64_publish_pc_before_chain(redispatch, false);
}

static_assert(a64_advance_pc(~uint64_t(0) - 2, 1) == 1
              && a64_branch_displacement(0x000fffffu) == 1048575
              && a64_branch_displacement(0x00100000u) == -1048576
              && a64_branch_displacement(0x001fffffu) == -1
              && a64_branch_target(0x1005, 0x001fffffu) == 0x1001
              && a64_jmp_target(0x1001, 0x2007) == 0x2005
              && a64_hw_ret_target(0x2007) == 0x2005,
              "A64 exit planning must preserve Alpha PC arithmetic and target masks");
static_assert(a64_exit_contract_probe(),
              "A64 exits must distinguish normal, retry, trap, and terminator bookkeeping");

static constexpr bool a64_gpr_usage_probe() noexcept
{
  A64GprUsage usage{};
  A64DecodedOp register_form{};
  register_form.ra = 1;
  register_form.rb = 1;
  register_form.rc = 2;
  register_form.gpr_reads = kA64GprRa | kA64GprRb;
  register_form.gpr_writes = kA64GprRc;
  note_a64_op_gprs(usage, register_form);

  // Operand roles, not bit 12 by itself, decide whether Rb is a register.
  A64DecodedOp literal_form{};
  literal_form.ra = 2;
  literal_form.rb = 16;
  literal_form.rc = 31;
  literal_form.is_literal = true;
  literal_form.gpr_reads = kA64GprRa;
  literal_form.gpr_writes = kA64GprRc;
  note_a64_op_gprs(usage, literal_form);

  return usage.refs[1] == 2 && usage.refs[2] == 2
      && usage.refs[16] == 0 && usage.refs[31] == 0
      && usage.reads == ((1u << 1) | (1u << 2))
      && usage.writes == (1u << 2);
}

static_assert(a64_gpr_usage_probe(),
              "A64 GPR analysis must follow semantic roles and exclude architectural R31");

static constexpr uint32_t a64_guest_gpr_slot(uint32_t raw_reg, bool pal_shadow) noexcept
{
  const uint32_t reg = raw_reg & 31u;
  return reg + ((pal_shadow && CJitEngine::RegAlloc::guest_is_shadowable(reg)) ? 32u : 0u);
}

enum class A64GprRouteKind : uint8_t {
  kInvalid,
  kMemory,
  kPinned,
  kZero,
  kDiscard
};

struct A64GprRoute {
  A64GprRouteKind kind;
  uint8_t slot;
  int8_t host;
};

// A route selects operand storage only. In particular, kDiscard suppresses the
// R31 destination write, never the instruction's faults or other side effects.

static constexpr A64GprRoute a64_guest_gpr_read_route(
    const CJitEngine::RegAlloc& regs, uint32_t raw_reg, bool pal_shadow) noexcept
{
  if (raw_reg >= 32) return {A64GprRouteKind::kInvalid, 0, -1};
  const uint32_t reg = raw_reg;
  if (reg == 31) return {A64GprRouteKind::kZero, 31, -1};
  const uint8_t slot = static_cast<uint8_t>(a64_guest_gpr_slot(reg, pal_shadow));
  const int host = regs.host_of(reg, pal_shadow);
  return host < 0 ? A64GprRoute{A64GprRouteKind::kMemory, slot, -1}
                  : A64GprRoute{A64GprRouteKind::kPinned, slot,
                                static_cast<int8_t>(host)};
}

static constexpr A64GprRoute a64_guest_gpr_write_route(
    const CJitEngine::RegAlloc& regs, uint32_t raw_reg, bool pal_shadow) noexcept
{
  if (raw_reg >= 32) return {A64GprRouteKind::kInvalid, 0, -1};
  const uint32_t reg = raw_reg;
  if (reg == 31) return {A64GprRouteKind::kDiscard, 31, -1};
  const uint8_t slot = static_cast<uint8_t>(a64_guest_gpr_slot(reg, pal_shadow));
  const int host = regs.host_of(reg, pal_shadow);
  return host < 0 ? A64GprRoute{A64GprRouteKind::kMemory, slot, -1}
                  : A64GprRoute{A64GprRouteKind::kPinned, slot,
                                static_cast<int8_t>(host)};
}

static constexpr bool a64_gpr_route_probe() noexcept
{
  CJitEngine::RegAlloc regs{};
  if (regs.host_of(1, false) != -1
      || !regs.bind_guest(1)
      || !regs.bind_guest(16)
      || regs.bind_guest(1)
      || regs.bind_guest(22)
      || regs.bind_guest(31)
      || !regs.bind_guest(0)
      || !regs.bind_guest(2)
      || !regs.bind_guest(3)
      || !regs.bind_guest(30)
      || regs.bind_guest(27)) return false;

  const A64GprRoute r1 = a64_guest_gpr_read_route(regs, 1, false);
  const A64GprRoute r16 = a64_guest_gpr_read_route(regs, 16, false);
  const A64GprRoute p22 = a64_guest_gpr_read_route(regs, 22, true);
  const A64GprRoute r24 = a64_guest_gpr_read_route(regs, 24, true);
  const A64GprRoute r31 = a64_guest_gpr_read_route(regs, 31, false);
  const A64GprRoute w31 = a64_guest_gpr_write_route(regs, 31, false);
  const A64GprRoute bad = a64_guest_gpr_read_route(regs, 32, false);
  return regs.guest_of_pin(0) == 1 && regs.guest_of_pin(1) == 16
      && r1.kind == A64GprRouteKind::kPinned
      && r1.host == CJitEngine::RegAlloc::kGuestPin0.id()
      && r16.kind == A64GprRouteKind::kPinned
      && r16.host == CJitEngine::RegAlloc::kGuestPin1.id()
      && p22.kind == A64GprRouteKind::kMemory && p22.slot == 54
      && r24.kind == A64GprRouteKind::kMemory && r24.slot == 24
      && r31.kind == A64GprRouteKind::kZero
      && w31.kind == A64GprRouteKind::kDiscard
      && bad.kind == A64GprRouteKind::kInvalid
      && CJitEngine::RegAlloc::guest_pin_candidate(3)
      && !CJitEngine::RegAlloc::guest_pin_candidate(4)
      && !CJitEngine::RegAlloc::guest_pin_candidate(20)
      && CJitEngine::RegAlloc::guest_pin_candidate(24)
      && !CJitEngine::RegAlloc::guest_pin_candidate(31);
}

static_assert(a64_gpr_route_probe(),
              "A64 GPR routing must preserve pin, PAL-shadow, and R31 semantics");

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

static constexpr bool a64_block_pin_contract_probe() noexcept
{
  const CJitEngine::RegAlloc regs = make_a64_block_regalloc();
  return regs.pin_count == kA64BlockGuestPins.size()
      && regs.guest_of_pin(0) == 1 && regs.host_of(1, false) == 23
      && regs.guest_of_pin(1) == 16 && regs.host_of(16, false) == 24
      && regs.guest_of_pin(2) == 30 && regs.host_of(30, false) == 25
      && regs.guest_of_pin(3) == 22 && regs.host_of(22, false) == 26
      && regs.guest_of_pin(4) == 23 && regs.host_of(23, false) == 27
      && regs.guest_of_pin(5) == 27 && regs.host_of(27, false) == 28
      && regs.host_of(22, true) == -1 && regs.host_of(23, true) == -1
      && a64_guest_gpr_slot(22, true) == 54
      && a64_guest_gpr_slot(23, true) == 55
      && !CJitEngine::RegAlloc::guest_pin_candidate(22)
      && !CJitEngine::RegAlloc::guest_pin_candidate(23);
}

static_assert(a64_block_pin_contract_probe(),
              "A64 block bodies must share one PAL-safe guest-pin convention");

static asmjit::Error emit_a64_load_guest_pins(asmjit::a64::Assembler& a,
                                               const CJitEngine::RegAlloc& regs)
{
  using namespace asmjit;
  for (uint32_t i = 0; i < regs.pin_count; ++i) {
    const int guest = regs.guest_of_pin(i);
    if (guest < 0) return Error::kInvalidArgument;
    const Error err = a.ldr(a64::x(CJitEngine::RegAlloc::kGuestPin0.id() + i),
                            a64_guest_gpr_mem(static_cast<uint32_t>(guest), false));
    if (err != Error::kOk) return err;
  }
  return Error::kOk;
}

static asmjit::Error emit_a64_sync_guest_pins(asmjit::a64::Assembler& a,
                                               const CJitEngine::RegAlloc& regs)
{
  using namespace asmjit;
  for (uint32_t i = 0; i < regs.pin_count; ++i) {
    const int guest = regs.guest_of_pin(i);
    if (guest < 0) return Error::kInvalidArgument;
    const Error err = a.str(a64::x(CJitEngine::RegAlloc::kGuestPin0.id() + i),
                            a64_guest_gpr_mem(static_cast<uint32_t>(guest), false));
    if (err != Error::kOk) return err;
  }
  return Error::kOk;
}

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
static_assert(A64FrameLayout::kHelperOut % alignof(uint64_t) == 0
              && A64FrameLayout::kHelperOut + 8 <= A64FrameLayout::kHelperScratch,
              "A64 helper out storage must be aligned and disjoint from call scratch");
static_assert(A64FrameLayout::kHelperScratch + 8 == A64FrameLayout::kSize,
              "A64 helper locals must fit inside the fixed frame");

enum class A64CallArgKind : uint8_t {
  kCpu,
  kGuest,
  kGuestOrZero,
  kHost,
  kOut,
  kImm32,
  kImm64
};

struct A64CallArg {
  A64CallArgKind kind = A64CallArgKind::kImm64;
  uint64_t value = 0;
};

static constexpr uint32_t kA64MaxCallArgs = 8;

static constexpr bool a64_valid_helper_host_source(uint32_t host) noexcept
{
  // x16 is the call target, x17 is linker scratch, x18 is platform-reserved,
  // and x29/x30/SP/ZR are fixed framee, not helper
  return host < 16 || (host >= 19 && host <= 28);
}

static constexpr bool a64_valid_call_arg(const A64CallArg& arg) noexcept
{
  switch (arg.kind) {
    case A64CallArgKind::kCpu:
    case A64CallArgKind::kOut:
    case A64CallArgKind::kImm64:
      return true;
    case A64CallArgKind::kGuest:
      return arg.value < 31;
    case A64CallArgKind::kGuestOrZero:
      return arg.value < 32;
    case A64CallArgKind::kHost:
      return arg.value <= UINT32_MAX
          && a64_valid_helper_host_source(static_cast<uint32_t>(arg.value));
    case A64CallArgKind::kImm32:
      return arg.value <= UINT32_MAX;
  }
  return false;
}

static_assert(kA64MaxCallArgs == 8
              && a64_valid_call_arg({A64CallArgKind::kGuest, 30})
              && !a64_valid_call_arg({A64CallArgKind::kGuest, 31})
              && a64_valid_call_arg({A64CallArgKind::kGuestOrZero, 31})
              && a64_valid_helper_host_source(0)
              && a64_valid_helper_host_source(15)
              && !a64_valid_helper_host_source(16)
              && !a64_valid_helper_host_source(17)
              && !a64_valid_helper_host_source(18)
              && a64_valid_helper_host_source(19)
              && a64_valid_helper_host_source(28)
              && !a64_valid_helper_host_source(29),
              "A64 helper arguments must respect AAPCS64 register roles");

static int a64_helper_index(const CJitEngine::HelperSet& helpers,
                            const void* helper) noexcept
{
  constexpr size_t kHelperCount = 19;
  static_assert(sizeof(CJitEngine::HelperSet) == kHelperCount * sizeof(void*),
                "HelperSet must remain the CPU helper table's pure pointer layout");
  if (helper == nullptr) return -1;
  std::array<void*, kHelperCount> table{};
  std::memcpy(table.data(), &helpers, sizeof(helpers));
  for (size_t i = 0; i < table.size(); ++i)
    if (table[i] == helper) return static_cast<int>(i);
  return -1;
}

static constexpr asmjit::a64::Mem a64_helper_out_mem() noexcept
{
  return asmjit::a64::ptr(asmjit::a64::sp, A64FrameLayout::kHelperOut);
}

static constexpr asmjit::a64::Mem a64_helper_scratch_mem() noexcept
{
  return asmjit::a64::ptr(asmjit::a64::sp, A64FrameLayout::kHelperScratch);
}

static_assert(a64_helper_out_mem().base_id() == asmjit::a64::Gp::kIdSp
              && a64_helper_out_mem().offset() == A64FrameLayout::kHelperOut
              && a64_helper_scratch_mem().offset() == A64FrameLayout::kHelperScratch,
              "A64 helper locals must remain fixed SP-relative operands");

// Preserve the incoming values of x0-x7 when helper arguments change them.
static asmjit::Error emit_a64_parallel_arg_moves(asmjit::a64::Assembler& a,
    const std::array<A64CallArg, kA64MaxCallArgs>& args, uint32_t count)
{
  using namespace asmjit;
  constexpr int8_t kNoSource = -1;
  constexpr int8_t kStackSource = -2;
  std::array<int8_t, kA64MaxCallArgs> source{};
  std::array<bool, kA64MaxCallArgs> pending{};
  source.fill(kNoSource);

  uint32_t remaining = 0;
  for (uint32_t dst = 0; dst < count; ++dst) {
    const A64CallArg& arg = args[dst];
    if (arg.kind != A64CallArgKind::kHost || arg.value >= kA64MaxCallArgs
        || arg.value == dst) continue;
    source[dst] = static_cast<int8_t>(arg.value);
    pending[dst] = true;
    ++remaining;
  }

  while (remaining != 0) {
    bool progressed = false;
    for (uint32_t dst = 0; dst < count; ++dst) {
      if (!pending[dst]) continue;
      bool old_dst_needed = false;
      for (uint32_t other = 0; other < count; ++other) {
        if (pending[other] && source[other] == static_cast<int8_t>(dst)) {
          old_dst_needed = true;
          break;
        }
      }
      if (old_dst_needed) continue;

      const Error err = source[dst] == kStackSource
          ? a.ldr(a64::x(dst), a64_helper_scratch_mem())
          : a.mov(a64::x(dst), a64::x(static_cast<uint32_t>(source[dst])));
      if (err != Error::kOk) return err;
      pending[dst] = false;
      --remaining;
      progressed = true;
    }
    if (progressed) continue;

    uint32_t cycle_dst = 0;
    while (cycle_dst < count && !pending[cycle_dst]) ++cycle_dst;
    if (cycle_dst == count) return Error::kInvalidState;
    for (uint32_t i = 0; i < count; ++i)
      if (pending[i] && source[i] == kStackSource) return Error::kInvalidState;

    Error err = a.str(a64::x(cycle_dst), a64_helper_scratch_mem());
    if (err != Error::kOk) return err;
    for (uint32_t i = 0; i < count; ++i)
      if (pending[i] && source[i] == static_cast<int8_t>(cycle_dst))
        source[i] = kStackSource;
  }
  return Error::kOk;
}

static asmjit::Error emit_a64_call_arg(asmjit::a64::Assembler& a,
    const CJitEngine::RegAlloc& regs, bool pal_shadow, uint32_t index,
    const A64CallArg& arg)
{
  using namespace asmjit;
  const a64::Gp dst = a64::x(index);
  switch (arg.kind) {
    case A64CallArgKind::kCpu:
      return a.mov(dst, CJitEngine::RegAlloc::kCpu);
    case A64CallArgKind::kGuest:
    case A64CallArgKind::kGuestOrZero: {
      const A64GprRoute route = a64_guest_gpr_read_route(
          regs, static_cast<uint32_t>(arg.value), pal_shadow);
      switch (route.kind) {
        case A64GprRouteKind::kMemory:
          return a.ldr(dst, a64::ptr(CJitEngine::RegAlloc::kRegs,
              static_cast<int32_t>(route.slot * sizeof(uint64_t))));
        case A64GprRouteKind::kPinned:
          return a.mov(dst, a64::x(static_cast<uint32_t>(route.host)));
        case A64GprRouteKind::kZero:
          return a.mov(dst, a64::xzr);
        default:
          return Error::kInvalidArgument;
      }
    }
    case A64CallArgKind::kHost:
      // Sources in x0-x7 were resolved as a parallel copy above.
      return arg.value < kA64MaxCallArgs
          ? Error::kOk
          : a.mov(dst, a64::x(static_cast<uint32_t>(arg.value)));
    case A64CallArgKind::kOut:
      return emit_a64_add_offset(a, dst, a64::sp, A64FrameLayout::kHelperOut);
    case A64CallArgKind::kImm32:
      return a.mov(dst.w(), imm(static_cast<uint32_t>(arg.value)));
    case A64CallArgKind::kImm64:
      return emit_a64_mov_u64(a, dst, arg.value);
  }
  return Error::kInvalidArgument;
}

[[maybe_unused]] static asmjit::Error emit_a64_helper_call(
    asmjit::a64::Assembler& a, const CJitEngine::JitOffsets& offsets,
    const CJitEngine::HelperSet& helpers, const CJitEngine::RegAlloc& regs,
    bool pal_shadow, const void* helper, std::initializer_list<A64CallArg> call_args)
{
  using namespace asmjit;
  if (helper == nullptr || call_args.size() > kA64MaxCallArgs)
    return Error::kInvalidArgument;

  std::array<A64CallArg, kA64MaxCallArgs> args{};
  uint32_t count = 0;
  for (const A64CallArg& arg : call_args) {
    if (!a64_valid_call_arg(arg)) return Error::kInvalidArgument;
    args[count++] = arg;
  }

  Error err = emit_a64_parallel_arg_moves(a, args, count);
  if (err != Error::kOk) return err;
  for (uint32_t i = 0; i < count; ++i) {
    err = emit_a64_call_arg(a, regs, pal_shadow, i, args[i]);
    if (err != Error::kOk) return err;
  }

  const int helper_slot = a64_helper_index(helpers, helper);
  if (helper_slot >= 0 && offsets.helpers != 0) {
    const uint64_t table_offset = static_cast<uint64_t>(offsets.helpers)
        + static_cast<uint64_t>(helper_slot) * sizeof(void*);
    err = emit_a64_add_offset(a, CJitEngine::RegAlloc::kScratch0,
        CJitEngine::RegAlloc::kCpu, static_cast<int64_t>(table_offset));
    if (err != Error::kOk) return err;
    err = a.ldr(CJitEngine::RegAlloc::kCallTarget,
                a64::ptr(CJitEngine::RegAlloc::kScratch0));
  }
  else {
    err = emit_a64_mov_u64(a, CJitEngine::RegAlloc::kCallTarget,
                           reinterpret_cast<uintptr_t>(helper));
  }
  if (err != Error::kOk) return err;
  return a.blr(CJitEngine::RegAlloc::kCallTarget);
}

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

void CJitEngine::compile_block(JitBlock* b, const uint8_t* dram, uint64_t dram_size,
    void* read_helper, void* write_helper, void* opcdec_helper, void* hw_mfpr_helper,
    void* hw_ld_helper, void* hw_mtpr_helper, void* hw_st_helper, void* indirect_helper,
    void* read_locked_helper, void* stc_helper, void* misc_helper, void* read_vpte_helper,
    void* read_wchk_helper, void* itof_helper, void* ftoi_helper, void* fltl_helper,
    void* fp_read_helper, void* fp_write_helper, void* fltv_helper)
{
  // Mark the attempt first so every setup failure leaves a permanent interpreter block.
  b->compiled = true;
  b->code = nullptr;
  b->jit_body = nullptr;
  b->prefix_len = 0;

  const A64BlockPlan plan = plan_a64_block(*b, dram, dram_size);
  const A64BlockExit exit = plan_a64_exit(b->tag, plan.count, plan.terminator);
  // Avoid even dry-run codegen until a classifier and emitter agree on a supported prefix.
  if (!exit.valid()) return;

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

  RegAlloc regalloc = make_a64_block_regalloc();
  [[maybe_unused]] const HelperSet helpers = {
      read_helper, write_helper, opcdec_helper, hw_mfpr_helper, hw_ld_helper,
      hw_mtpr_helper, hw_st_helper, indirect_helper, read_locked_helper, stc_helper,
      misc_helper, read_vpte_helper, read_wchk_helper, itof_helper, ftoi_helper,
      fltl_helper, fp_read_helper, fp_write_helper, fltv_helper};
  if (emit_a64_prologue(a) != asmjit::Error::kOk) return;
  // Cold entry materializes the shared block convention. Chained entries target
  // body directly and inherit these live values plus x19-x22 from their predecessor.
  if (emit_a64_load_guest_pins(a, regalloc) != asmjit::Error::kOk) return;

  asmjit::Label done = a.new_label();
  asmjit::Label body = a.new_label();
  if (a.bind(body) != asmjit::Error::kOk) return;
  [[maybe_unused]] const size_t body_off = code.code_size();

  // Empty translated body: a cold call would return zero completed instructions.
  if (a.mov(RegAlloc::kResultCount, RegAlloc::kChainCount.w()) != asmjit::Error::kOk) return;
  if (a.bind(done) != asmjit::Error::kOk) return;
  // Every dispatcher or bailout exit converges here while x20 still names state.r[].
  if (emit_a64_sync_guest_pins(a, regalloc) != asmjit::Error::kOk) return;
  if (emit_a64_epilogue(a) != asmjit::Error::kOk) return;
  if (a.finalize() != asmjit::Error::kOk) return;

  assert(body_off == 40 + kA64BlockGuestPins.size() * 4);
  assert(code.code_size() == 72 + kA64BlockGuestPins.size() * 8);
}

void CJitEngine::compile_trace(TraceFragment*, JitBlock**, uint32_t,
                               const uint8_t*, uint64_t, const HelperSet&) {}

#endif // ES40_JIT_A64

#endif // ES40_JIT

// Keeps this translation unit non-empty when ARM64 is not the build host (MSVC LNK4221).
extern const char jit_a64_backend_tu;
const char jit_a64_backend_tu = 0;
